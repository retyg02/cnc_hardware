using FluentModbus;
using Newtonsoft.Json;
using System;
using System.Linq;
using System.Net.Http;
using System.Net.Http.Json;
using System.Text;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Forms;
using System.Windows.Media;
using System.Windows.Threading;
using System.Text.Json.Serialization;



namespace FactoryModbusDriver
{
    public partial class MainWindow : Window
    {
        private System.Windows.Forms.NotifyIcon _notifyIcon;

        // Сетевые клиенты
        private ModbusTcpClient _modbusClient;
        private static readonly HttpClient _httpClient = new HttpClient();

        // Высокочастотные таймеры для разделения потоков данных
        private DispatcherTimer _pollTimer;     // Опрос телеметрии (100 мс)
        private DispatcherTimer _commandTimer;  // Проверка команд из БД/FastAPI (500 мс)

        // Сетевая конфигурация
        private const string CppServerIp = "127.0.0.1";
        private const int ModbusPort = 502;

        // Твои точные эндпоинты FastAPI из Swagger
        private const string FastApiSubmitUrl = "http://127.0.0.1:8000/telemetry";
        private const string FastApiCncCommandUrl = "http://127.0.0.1:8000/telemetry/machines/1/set_command";
        private const string FastApiGetCommandUrl = "http://127.0.0.1:8000/telemetry/machines/1/command"; // Роут проверки команд фронтенда

        // Светодиодная палитра
        private readonly SolidColorBrush _ledRed = new SolidColorBrush(System.Windows.Media.Color.FromRgb(255, 79, 79));
        private readonly SolidColorBrush _ledGreen = new SolidColorBrush(System.Windows.Media.Color.FromRgb(76, 224, 179));


        private string _lastCncStateForLogs = "idle";
        private bool _isOverheatAlertSent = false;

        // Пропиши в самом верху класса MainWindow
        private string _activeSessionId = null;




        public MainWindow()
        {
            InitializeComponent();
            InitGateWay();
        }

        private void InitGateWay()
        {
            _modbusClient = new ModbusTcpClient();

            _modbusClient.ReadTimeout = 200;
            _modbusClient.WriteTimeout = 200;

            // 1. Таймер сбора телеметрии (Вверх: C++ -> C# -> FastAPI)
            _pollTimer = new DispatcherTimer();
            _pollTimer.Interval = TimeSpan.FromMilliseconds(500);
            _pollTimer.Tick += OnPollTick;

            // 2. Таймер спуска команд (Вниз: Фронтенд React -> FastAPI БД -> C# -> C++ Modbus)
            _commandTimer = new DispatcherTimer();
            _commandTimer.Interval = TimeSpan.FromMilliseconds(500); // Опрашиваем базу раз в полсекунды
            _commandTimer.Tick += OnCheckCommandsTick;

            Task.Run(() => TryConnectModbus());



            _notifyIcon = new System.Windows.Forms.NotifyIcon();

            var iconStream = System.Windows.Application.GetResourceStream(new Uri("pack://application:,,,/gear.ico")).Stream;
            _notifyIcon.Icon = new System.Drawing.Icon(iconStream);


            _notifyIcon.Text = "IIoT Промышленный Шлюз Автоматизации";

            // Возврат окна на экран при двойном клике по иконке в трее
            _notifyIcon.DoubleClick += (s, args) =>
            {
                this.Show();
                this.WindowState = WindowState.Normal;
            };

        }

        private void TryConnectModbus()
        {
            try
            {
                UpdateStatus("Подключение к C++ Modbus серверу...");
                _modbusClient.Connect(new System.Net.IPEndPoint(System.Net.IPAddress.Parse(CppServerIp), ModbusPort));

                Dispatcher.Invoke(() => {
                    LedModbus.Fill = _ledGreen;
                    _pollTimer.Start();
                    _commandTimer.Start(); // Запускаем мониторинг команд диспетчера
                });

                UpdateStatus("Драйвер запущен. Связь с ПЛК установлена.");
                LogMessage("[SYSTEM] Успешное подключение к Modbus TCP Серверу (C++).");
            }
            catch (Exception ex)
            {
                UpdateStatus("Ошибка связи с C++!");
                LogMessage($"[MODBUS ERROR] Не удалось подключиться: {ex.Message}. Ожидание перезапуска...");
                Task.Delay(3000).ContinueWith(_ => TryConnectModbus());
            }
        }

        // 🔄 ГЛАВНЫЙ ЦИКЛ СБОРА ТЕЛЕМЕТРИИ (100 мс)
        private async void OnPollTick(object? sender, EventArgs e)
        {
            // Запускаем опрос асинхронно в фоновом потоке, чтобы не вешать UI
            await Task.Run(async () =>
            {
                // Задел под масштабируемость: текущий ID комплекса в базе и сети
                int currentMachineId = 1;

                try
                {
                    // =========================================================================
                    // ШАГ А: ПОЛИНГ БАЗЫ ДАННЫХ FASTAPI (Слушаем команды RESET/session_id от Vue)
                    // =========================================================================
                    string getMachineUrl = "http://127.0.0.1:8000/telemetry/machines";

                    // Делаем аккуратный GET запрос к твоей таблице машин
                    var httpResponse = await _httpClient.GetAsync(getMachineUrl);
                    
                    //Dispatcher.Invoke(() => LogMessage($"httpResponse: {httpResponse}"));

                    if (httpResponse.IsSuccessStatusCode)
                    {
                        Dispatcher.Invoke(() => LedWeb.Fill = _ledGreen);

                        string rawJson = await httpResponse.Content.ReadAsStringAsync();
                        //Dispatcher.Invoke(() => LogMessage($"[DEBUG JSON] Пришло от FastAPI: {rawJson}"));

                        if (rawJson.Contains("\"current_command\":\"RESET\""))
                        {
                            Dispatcher.Invoke(() => LogMessage("[🚀 TRACE] ТЕКСТ RESET ОБНАРУЖЕН В СЫРОМ JSON!"));

                            // Текстом вырезаем session_id из строки, чтобы не зависеть от десериализатора
                            // Ищем где начинается "session_id":"
                            int sessionIdx = rawJson.IndexOf("\"session_id\":");


                            if (sessionIdx != -1)
                            {
                                int start = sessionIdx + "\"session_id\":\"".Length;
                                int end = rawJson.IndexOf("\"", start);
                                _activeSessionId = rawJson.Substring(start, end - start);
                            }
                            else
                            {
                                // Если сессия не вырезалась из текста, генерируем уникальный fallback по дате
                                _activeSessionId = "session_auto_" + DateTime.UtcNow.ToString("yyyyMMdd_HHmm");

                            }

                            Dispatcher.Invoke(() => LogMessage($"[🚀 TRACE] Успешно извлекли сессию из текста: '{_activeSessionId}'"));

                            // Будим C++ симулятор (Твой рабочий сокет)
                            if (!_modbusClient.IsConnected)
                            {
                                _modbusClient.Connect(new System.Net.IPEndPoint(System.Net.IPAddress.Parse(CppServerIp), ModbusPort));
                            }

                            // 1. АКТИВИРУЕМ СТАНОК (пишем 1 в регистр 0)
                            _modbusClient.WriteSingleRegister(0, 0, (short)1);
                            _modbusClient.Disconnect();

                            Dispatcher.Invoke(() => LogMessage("[🚀 TRACE] Modbus пакет активации отправлен в C++."));

                            // Динамически вырезаем ID машины из сырого JSON текста, чтобы сохранить масштабируемость
                            int machineId = 1; // Дефолтное значение
                            int idIdx = rawJson.IndexOf("\"id\":");
                            if (idIdx != -1)
                            {
                                int startId = idIdx + "\"id\":".Length;
                                int endId = rawJson.IndexOf(",", startId);
                                if (int.TryParse(rawJson.Substring(startId, endId - startId).Trim(), out int parsedId))
                                {
                                    machineId = parsedId; // Переменная сама подставит нужный ID станка (1, 2, 3...)
                                }
                            }

                            // Отправляем квитирование команды на правильный, динамический ID машины
                            var resetCmdPayload = new { command = "STOP" };
                            await SendToFastApiAsync($"http://127.0.0.1:8000/telemetry/machines/{machineId}/set_command", resetCmdPayload);

                        }


                        // Парсим список машин и находим нашу (id: 1)

                        var options = new System.Text.Json.JsonSerializerOptions { PropertyNameCaseInsensitive = true };
                        var machinesList = await httpResponse.Content.ReadFromJsonAsync<List<MachineDataDto>>(options);

                        //Dispatcher.Invoke(() => LogMessage($"machinesList: {machinesList}"));

                        var dbMachine = machinesList?.Find(m => m.id == currentMachineId);

                        //Dispatcher.Invoke(() => LogMessage($"dbMachine: {dbMachine}"));

                        if (dbMachine != null)
                        {
                            //Dispatcher.Invoke(() => LogMessage($"[TRACE 3.1] Распарсили станок 1 из базы -> Команда: '{dbMachine.current_command}', Сессия: '{dbMachine.session_id}'"));
                            // Если диспетчер во Vue нажал СТАРТ (загрузил файл и улетела команда RESET)
                            if (dbMachine.current_command == "RESET")
                            {
                                Dispatcher.Invoke(() => LogMessage("[TRACE 3.2] Условие совпало! Инициализирую динамический запуск ПЛК."));

                                // Железно фиксируем session_id, сгенерированный во Vue
                                _activeSessionId = dbMachine.session_id;
                                string targetGCodePath = dbMachine.gcode_path ?? "test.nc";

                                // Если сокет почему-то закрылся, аккуратно восстанавливаем связь
                                if (!_modbusClient.IsConnected)
                                {
                                    try { _modbusClient.Connect(new System.Net.IPEndPoint(System.Net.IPAddress.Parse(CppServerIp), ModbusPort)); } catch { }
                                }

                                if (_modbusClient.IsConnected)
                                {
                                    try
                                    {
                                        // ШАГ 1: Сначала пакуем имя файла в ASCII-регистры 10-50
                                        WriteAsciiStringToModbus(10, targetGCodePath);
                                        Dispatcher.Invoke(() => LogMessage($"[DRV LOG] Имя файла '{targetGCodePath}' успешно записано в регистры 10-50."));

                                        // ШАГ 2: И только потом пишем 1 в регистр 0, чтобы С++ увидел команду старта!
                                        _modbusClient.WriteSingleRegister(0, 0, (short)1);
                                        Dispatcher.Invoke(() => LogMessage("[DRV LOG] Сетевой триггер старта (регистр 0 = 1) отправлен."));
                                    }
                                    catch (Exception modbusEx)
                                    {
                                        Dispatcher.Invoke(() => LogMessage($"[❌ MODBUS ERROR] Ошибка записи: {modbusEx.Message}"));
                                    }
                                }

                                // ⚠️ ВНИМАНИЕ: Строку _modbusClient.Disconnect() мы ОТСЮДА ПОЛНОСТЬЮ УБРАЛИ! 
                                // Сокет должен оставаться открытым для передачи координат!

                                // КВИТИРОВАНИЕ: Сбрасываем команду в базе обратно в "STOP"
                                var resetCmdPayload = new { command = "STOP" };
                                await SendToFastApiAsync($"http://127.0.0.1:8000/telemetry/machines/{currentMachineId}/set_command", resetCmdPayload);

                            }
                        }
                    }
                    else
                    {
                        Dispatcher.Invoke(() => LedWeb.Fill = _ledRed);
                    }

                    // =========================================================================
                    // ФИНАЛЬНЫЙ ИСПРАВЛЕННЫЙ ШАГ Б: РАЗВОРOT БАЙТ И СТАБИЛИЗАЦИЯ СЕТИ
                    // =========================================================================
                    if (!_modbusClient.IsConnected)
                    {
                        try { _modbusClient.Connect(new System.Net.IPEndPoint(System.Net.IPAddress.Parse(CppServerIp), ModbusPort)); } catch { }
                    }

                    short[] registers = null;
                    try
                    {
                        registers = _modbusClient.ReadHoldingRegisters<short>(0, 0, 8).ToArray();
                    }
                    catch (Exception modbusExc)
                    {
                        // При ЛЮБОЙ сетевой ошибке или сбое протокола — мягко переподключаем сокет, очищая буфер!
                        try
                        {
                            _modbusClient.Disconnect();
                            _modbusClient.Connect(new System.Net.IPEndPoint(System.Net.IPAddress.Parse(CppServerIp), ModbusPort));
                        }
                        catch { }
                        return;
                    }

                    // ЛОКАЛЬНАЯ ФУНКЦИЯ РАЗВОРOТА СЕТЕВЫХ БАЙТ (Магия Intel Little-Endian)
                    Func<short, short> reverseBytes = (val) => {
                        ushort uVal = (ushort)val;
                        return (short)((uVal >> 8) | (uVal << 8));
                    };

                    // Применяем разворот байт ко всей телеметрии из лога!
                    // // Применяем разворот байт ко всей телеметрии из лога!
                    int cncStateRaw = reverseBytes(registers[0]); // MB_CNC_STATUS = 0
                    double cncX = reverseBytes(registers[1]) / 10.0; // MB_CNC_X = 1
                    double cncY = reverseBytes(registers[2]) / 10.0; // MB_CNC_Y = 2
                    double cncZ = reverseBytes(registers[3]) / 10.0; // MB_CNC_Z = 3
                    double cncTemp = reverseBytes(registers[4]); // MB_CNC_TEMP = 4

                    // Нагрузка больше не будет 0! 1536 превратится в честные 6%
                    double cncLoad = reverseBytes(registers[5]); // MB_CNC_LOAD = 5

                    double cncWear = reverseBytes(registers[6]) / 1000.0; // MB_CNC_WEAR = 6
                    double convPos = reverseBytes(registers[7]) / 100.0; // MB_CONVEYOR_POS = 7


                    // Переводим статус в текст для базы данных
                    string cncStateText = (cncStateRaw == 1) ? "working" : (cncStateRaw == 2 ? "error" : "idle");



                    // Выводим в интерфейс
                    Dispatcher.Invoke(() =>
                    {
                        LedModbus.Fill = _ledGreen;
                        //LogMessage($"[TELEMETRY] ЧПУ: {cncStateText} | X:{cncX:F1} Y:{cncY:F1} Z:{cncZ:F1} | Нагрузка: {cncLoad}% | T: {cncTemp}°C || Конвейер: {convPos:F1}м");
                    });

                    // Формируем JSON и пуляем базовую телеметрию в PostgreSQL через твой роут FastAPI
                    var cncPayload = new
                    {
                        machine_id = currentMachineId,
                        status = cncStateText,
                        load_percent = (int)cncLoad,
                        details = $"X: {cncX:F1}; Y: {cncY:F1}; Z: {cncZ:F1}; Temp: {cncTemp:F1}°C; Load: {cncLoad}%"
                    };
                    //Dispatcher.Invoke(() => LogMessage($"cncLoad: {cncLoad}"));
                    await SendToFastApiAsync(FastApiSubmitUrl, cncPayload);

                    // =========================================================================
                    // ШАГ В: ОТПРАВКА СЫРЫХ КООРДИНАТ ДЛЯ ВЕБ-АНИМАЦИИ ВО Vue (В MongoDB через FastAPI)
                    // =========================================================================
                    string fastApiCoordinatesUrl = "http://127.0.0.1:8000/telemetry/machines/coords";

                    // Если сессия еще не считана из Postgres (станок запустился локально), страхуемся дефолтом
                    string sessionToSend = _activeSessionId ?? ("session_idle_" + DateTime.UtcNow.ToString("yyyyMMdd"));

                    var animationCoordsPayload = new
                    {
                        machine_id = currentMachineId,
                        x = cncX,
                        y = cncY,
                        z = cncZ,
                        is_cutting = (cncStateRaw == 1),
                        session_id = sessionToSend, // <-- ТЕПЕРЬ СЕССИЯ ЖЕСТКО ЗАФИКСИРОВАНА И НЕ ПЛЫВЕТ!
                        timestamp = DateTime.UtcNow.ToString("o")
                    };

                    await SendToFastApiAsync(fastApiCoordinatesUrl, animationCoordsPayload);

                    // =========================================================================
                    // ШАГ Г: ГЕНЕРАЦИЯ ФИЗИЧЕСКИХ ЛОГОВ ДЛЯ Django-ПРЕДИКТОРА (В PostgreSQL)
                    // =========================================================================
                    string fastApiLogsUrl = "http://127.0.0.1:8000/telemetry/machines/log";

                    // 1. Фиксируем изменение режима работы станка комплекса
                    if (cncStateText != _lastCncStateForLogs)
                    {
                        string logMessageText = cncStateText == "working"
                            ? "Запуск чистовой обработки детали по G-коду."
                            : "Обработка успешно завершена. Комплекс переведен в режим ожидания.";

                        var stateChangeLogPayload = new
                        {
                            machine_id = currentMachineId,
                            action_text = logMessageText
                        };

                        await SendToFastApiAsync(fastApiLogsUrl, stateChangeLogPayload);
                        _lastCncStateForLogs = cncStateText;
                    }

                    // 2. Логируем опасный перегрев подшипникового узла для Predictive Maintenance
                    if (cncTemp >= 85.0 && !_isOverheatAlertSent)
                    {
                        var overheatLogPayload = new
                        {
                            machine_id = currentMachineId,
                            action_text = $"ВНИМАНИЕ: Превышен температурный порог шпинделя. Текущая: {cncTemp:F1}°C"
                        };

                        await SendToFastApiAsync(fastApiLogsUrl, overheatLogPayload);
                        _isOverheatAlertSent = true;
                    }
                    else if (cncTemp < 80.0 && _isOverheatAlertSent)
                    {
                        _isOverheatAlertSent = false;
                    }

                    // =========================================================================
                    // ШАГ Д: АВТОМАТИЧЕСКИЙ СБРОС СЕССИИ В NULL ПРИ ЗАВЕРШЕНИИ (Команда M30 в C++)
                    // =========================================================================
                    if (cncStateRaw == 0 && _activeSessionId != null)
                    {
                        _activeSessionId = null; // Очищаем локальную память в C#

                        // Запрос к FastAPI для обнуления колонки session_id в PostgreSQL в NULL
                        var clearSessionPayload = new { session_id = (string)null };
                        await SendToFastApiAsync($"http://127.0.0.1:8000/telemetry/machines/{currentMachineId}/set_session", clearSessionPayload);
                    }
                }
                catch (Exception ex)
                {
                    // Если C++ сервер занят или не ответил — просто пишем в лог без падения сессии
                    Dispatcher.Invoke(() =>
                    {
                        LedModbus.Fill = _ledRed;
                        //LogMessage($"[MODBUS TIMEOUT] ПЛК занят, ожидание следующего такта... ({ex.Message})");
                    });

                    // На всякий случай сбрасываем сокет при ошибке
                    //try { _modbusClient.Disconnect(); } catch { }
                }
            resurrection_line:;
            });
        }



        // 📥 ФОНОВЫЙ МОНИТОРИНГ КОМАНД ИЗ БД (Спуск вниз к C++ раз в 500 мс)
        private async void OnCheckCommandsTick(object sender, EventArgs e)
        {
            if (!_modbusClient.IsConnected) return;

            try
            {
                // Запрашиваем у FastAPI, есть ли в базе новые необработанные команды от фронтенда
                HttpResponseMessage response = await _httpClient.GetAsync(FastApiGetCommandUrl);
                if (response.IsSuccessStatusCode)
                {
                    string jsonResponse = await response.Content.ReadAsStringAsync();

                    // Парсим ответ бэкенда (ожидаем структуру вроде { "command": "STOP" } или { "command": "RESET" })
                    var commandData = JsonConvert.DeserializeAnonymousType(jsonResponse, new { command = "" });

                    if (commandData != null && !string.IsNullOrEmpty(commandData.command))
                    {
                        string cmd = commandData.command.ToUpper();
                        if (cmd == "STOP")
                        {
                            _modbusClient.WriteSingleRegister(0, 7, (short)3); // Пишем 3 (STOP) в регистр 40008
                            //LogMessage("[WEB COMMAND] Фронтенд запросил ЭКСТРЕННЫЙ СТОП. Команда передана в C++ ПЛК.");
                        }
                        else if (cmd == "RESET")
                        {
                            _modbusClient.WriteSingleRegister(0, 7, (short)2); // Пишем 2 (RESET) в регистр 40008
                            //LogMessage("[WEB COMMAND] Фронтенд запросил СБРОС ОШИБОК. Команда передана в C++ ПЛК.");
                        }
                    }
                }
            }
            catch (Exception ex)
            {
                // Не спамим логами, если бэкенд временно недоступен
            }
        }

        private async Task SendToFastApiAsync(string url, object payload)
        {
            try
            {
                string json = JsonConvert.SerializeObject(payload);
                var content = new StringContent(json, Encoding.UTF8, "application/json");

                HttpResponseMessage response = await _httpClient.PostAsync(url, content);

                if (response.IsSuccessStatusCode)
                {
                    Dispatcher.Invoke(() => LedWeb.Fill = _ledGreen);
                }
                else
                {
                    Dispatcher.Invoke(() => LedWeb.Fill = _ledRed);
                }
            }
            catch
            {
                Dispatcher.Invoke(() => LedWeb.Fill = _ledRed);
            }
        }

        // 🚨 КНОПКА ЛОКАЛЬНОГО ЭКСТРЕННОГО СТОПА С ПАНЕЛИ ДРАЙВЕРА
        private async void StopButton_Click(object sender, RoutedEventArgs e)
        {
            LogMessage("[COMMAND] Инициация процедуры экстренного останова цеха...");
            UpdateStatus("Принудительный останов системы...");

            // 1. Асинхронно отправляем команду STOP на твой FastAPI роут set_command
            try
            {
                var commandPayload = new { command = "STOP" };
                string json = JsonConvert.SerializeObject(commandPayload);
                var content = new StringContent(json, Encoding.UTF8, "application/json");

                // Бьем прямо в FastAPI, не дожидаясь Modbus
                HttpResponseMessage response = await _httpClient.PostAsync(FastApiCncCommandUrl, content);

                if (response.IsSuccessStatusCode)
                {
                    LogMessage("[SYSTEM] Команда аварийного останова успешно зарегистрирована на бэкенде FastAPI.");
                }
                else
                {
                    LogMessage($"[HTTP ERROR] FastAPI ответил кодом ошибки: {response.StatusCode}");
                }
            }
            catch (Exception ex)
            {
                LogMessage($"[HTTP FAILED] Не удалось связаться с FastAPI: {ex.Message}");
            }

            // 2. Параллельно пробиваемся в C++ по Modbus, чтобы заглушить физический движок
            await Task.Run(() =>
            {
                try
                {
                    // Открываем выделенный независимый сокет для отправки стоп-сигнала
                    using (var quickClient = new ModbusTcpClient())
                    {
                        quickClient.Connect(new System.Net.IPEndPoint(System.Net.IPAddress.Parse(CppServerIp), ModbusPort));
                        quickClient.WriteSingleRegister(0, 7, (short)3); // Пишем 3 (STOP) в регистр 40008
                        quickClient.Disconnect();
                    }
                    Dispatcher.Invoke(() => LogMessage("[COMMAND] Аварийный сигнал СТОП успешно записан в Modbus регистр 40008 C++."));
                }
                catch (Exception ex)
                {
                    Dispatcher.Invoke(() => LogMessage($"[MODBUS FAILED] Не удалось передать стоп-сигнал в C++ ПЛК: {ex.Message}"));
                }
            });
        }


        private void TrayButton_Click(object sender, RoutedEventArgs e)
        {
            this.Hide(); // Полностью скрываем окно с экрана и панели задач
            
            _notifyIcon.Visible = true;
            LogMessage("[UI] Приложение свернуто в системный трей.");
        }


        private void StopTimers() { _pollTimer.Stop(); _commandTimer.Stop(); }

        private void LogMessage(string message)
        {
            Dispatcher.BeginInvoke(new Action(() =>
            {
                LogBox.AppendText($"[{DateTime.Now:HH:mm:ss}] {message}\r\n");

                // Если мышка СЕЙЧАС парит над окном логов — НЕ скроллим, даем пользователю почитать
                if (!LogBox.IsMouseOver)
                {
                    LogBox.ScrollToEnd();
                }

                // Предотвращаем утечку памяти
                if (LogBox.Text.Length > 10000)
                {
                    LogBox.Text = LogBox.Text.Substring(5000);
                }
            }));
        }



        private void UpdateStatus(string status) { Dispatcher.BeginInvoke(new Action(() => { StatusLabel.Text = $"Статус драйвера: {status}"; })); }


        protected override void OnClosing(System.ComponentModel.CancelEventArgs e)
        {
            if (_notifyIcon != null)
            {
                _notifyIcon.Visible = false;
                _notifyIcon.Dispose();
            }
            base.OnClosing(e);
        }





        private void WriteAsciiStringToModbus(int startRegister, string text)
        {
            // Стандартный Modbus TCP вмещает по 2 ASCII-символа (байта) в один 16-битный регистр
            byte[] bytes = System.Text.Encoding.ASCII.GetBytes(text);

            // Перебираем байты парами и пишем в C++ симулятор
            for (int i = 0; i < bytes.Length && i < 40; i += 2)
            {
                byte high = bytes[i];
                byte low = (i + 1 < bytes.Length) ? bytes[i + 1] : (byte)0;

                // Склеиваем два байта в одно число short (uint16)
                short registerValue = (short)((high << 8) | low);

                // Записываем в текущую ячейку памяти C++ сервера через твою библиотеку
                _modbusClient.WriteSingleRegister(0, (ushort)(startRegister + (i / 2)), registerValue);
            }
        }


        private ushort CalculateModbusCrc16(byte[] buffer, int length)
        {
            ushort crc = 0xFFFF;
            for (int i = 0; i < length; i++)
            {
                crc ^= buffer[i];
                for (int j = 0; j < 8; j++)
                {
                    if ((crc & 0x0001) != 0)
                    {
                        crc >>= 1;
                        crc ^= 0xA001; // Полином Modbus
                    }
                    else
                    {
                        crc >>= 1;
                    }
                }
            }
            return crc;
        }

    }



}

public class MachineDataDto
{
    public int id { get; set; }
    public string status { get; set; }

    [JsonPropertyName("current_command")]
    public string current_command { get; set; }

    [JsonPropertyName("session_id")]
    public string session_id { get; set; }

    [JsonPropertyName("gcode_path")]
    public string gcode_path { get; set; }
}
