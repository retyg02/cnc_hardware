#include <iostream>
#include <vector>
#include <memory>
#include <thread>
#include <chrono>
#include <fstream>
#include <sstream>
#include <cstdint>
#include <winsock2.h>
#include "device.h"
#include "modbus_crc.h"


#pragma comment(lib, "ws2_32.lib")

#pragma comment(lib, "gcode_validator.dll.lib")


extern "C" __declspec(dllimport) bool validate_gcode_line(const char* raw_line);

bool isFactoryRunning = true;
uint16_t modbus_registers[60] = { 0 };
std::shared_ptr<CNC_Machine> global_cnc = nullptr;


void runConveyorLifecycle(std::shared_ptr<Conveyor_Machine> conveyor)
{
    double timeStep = 0.1;
    while (isFactoryRunning)
    {
        if (conveyor->getState() == MachineState::IDLE && !conveyor->isItemAtStation())
        {
            conveyor->executeMoveProgramm();
        }
        conveyor->updatePhysics(timeStep);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

void runCNCLifecycle(std::shared_ptr<CNC_Machine> cnc, std::shared_ptr<Conveyor_Machine> conveyor)
{
    double timeStep = 0.1;
    std::ifstream file;
    std::string currentLine;
    bool fileOpened = false;
    bool needNextLine = true;

    double lastX = 0.0, lastY = 0.0, lastZ = 0.0;
    double currentFeed = 120.0, currentSpeed = 0.0;
    int frameTimer = 0;
    int tickCounter = 0;

    std::cout << "[СИСТЕМА ЧПУ] Поток контроля цеха запущен. Ожидание заготовки...\n";

    while (isFactoryRunning)
    {
        tickCounter++;

        // 📥 ШАГ 1: МОНИТОРИНГ КОНВЕЙЕРА И СЕТЕВОГО ТРИГГЕРА
        if (!fileOpened && cnc->getState() == MachineState::IDLE)
        {
            if (modbus_registers[0] == 1 || modbus_registers[0] == 256)
            {
                // ⏳ Даем C# шлюзу 50 мс полностью заполнить буфер
                std::this_thread::sleep_for(std::chrono::milliseconds(50));

                // 📂 ЧИТАЕМ ИМЯ ФАЙЛА С ПРАВИЛЬНЫМ ПОРЯДКОМ БАЙТ
                std::string fileName = "";
                for (int i = 10; i <= 50; ++i)
                {
                    uint16_t regValue = modbus_registers[i];
                    if (regValue == 0) break;

                    // Меняем байты местами для правильной склейки букв
                    char lowChar = static_cast<char>(regValue & 0xFF);
                    char highChar = static_cast<char>((regValue >> 8) & 0xFF);

                    if (lowChar != '\0') fileName += lowChar;
                    if (highChar != '\0') fileName += highChar;
                }

                if (fileName.empty()) fileName = "test.nc";

                // Проверяем датчик заготовки (Регистр 9)
                if (modbus_registers[9] == 0)
                {
                    static bool conveyorLogSent = false;
                    if (!conveyorLogSent) {
                        std::cout << "\n[ТРАНСПОРТЕР] Сигнал получен. Конвейер везет заготовку '" << fileName << "' к ЧПУ...\n";
                        conveyorLogSent = true;
                    }
                }
                else // Заготовка доехала на позицию
                {
                    std::cout << "\n[ДАТЧИК ПЛК] Заготовка зафиксирована в рабочей зоне!\n";

                    cnc->setState(MachineState::WORKING);
                    modbus_registers[FactorySettings::MB_CNC_STATUS] = static_cast<uint16_t>(MachineState::WORKING);

                    std::string dynamicPath = "C:/Users/User/Desktop/all_git/g-code/" + fileName;

                    file.open(dynamicPath);
                    if (!file.is_open())
                    {
                        // 🛠️ РЕЗЕРВНЫЙ ФАЙЛ: Если динамический не нашелся, мягко подхватываем test.nc
                        std::cout << "[СИСТЕМА ЧПУ WARNING] Файл '" << fileName << "' не найден. Запуск резервной программы test.nc...\n";
                        fileName = "test.nc";
                        dynamicPath = "C:/Users/User/Desktop/all_git/g-code/" + fileName;
                        file.open(dynamicPath);
                    }

                    if (file.is_open())
                    {
                        std::cout << "[СИСТЕМА ЧПУ] Программа успешно запущена! Файл: " << fileName << "\n";
                        fileOpened = true;
                        needNextLine = true;
                        frameTimer = 0;
                    }
                    else
                    {
                        std::cout << "[КРИТИЧЕСКАЯ ОШИБКА] Не удалось открыть даже резервный файл test.nc!\n";
                        cnc->setState(MachineState::FAULT);
                        modbus_registers[FactorySettings::MB_CNC_STATUS] = static_cast<uint16_t>(MachineState::FAULT);
                    }
                }
            }
        }

        // ⚙️ ШАГ 2: ПОШАГОВАЯ РЕЗКА ДЕТАЛИ И ТЕХНОЛОГИЧЕСКИЙ РЕПОРТ
        if (cnc->getState() == MachineState::WORKING && fileOpened)
        {
            if (needNextLine)
            {
                if (std::getline(file, currentLine))
                {
                    // 1. Убираем символ возврата каретки \r (если файл сохранен в Windows формате)
                    if (!currentLine.empty() && currentLine.back() == '\r') {
                        currentLine.pop_back();
                    }

                    // 2. СРАЗУ ОТРЕЗАЕМ КОММЕНТАРИИ В C++
                    if (currentLine.empty()) continue;
                    size_t commentPos = currentLine.find(';');
                    if (commentPos != std::string::npos) {
                        currentLine = currentLine.substr(0, commentPos);
                    }

                    // 3. Тримминг (удаляем случайные пробелы в начале и конце строки)
                    size_t first = currentLine.find_first_not_of(" \t");
                    if (first == std::string::npos) continue; // Строка состояла только из пробелов/комментария
                    size_t last = currentLine.find_last_not_of(" \t");
                    currentLine = currentLine.substr(first, (last - first + 1));

                    // 4. И ВОТ ТЕПЕРЬ ОТПРАВЛЯЕМ ЧИСТЫЙ ТЕХНОЛОГИЧЕСКИЙ КАДР В RUST!
                    if (!validate_gcode_line(currentLine.c_str()))
                    {
                        std::cout << "\n[🚨 АВАРИЙНЫЙ СТОП] Rust Validator заблокировал опасный кадр: " << currentLine << "\n";
                        cnc->setState(MachineState::FAULT);
                        modbus_registers[FactorySettings::MB_CNC_STATUS] = static_cast<uint16_t>(MachineState::FAULT);
                        file.close(); fileOpened = false;
                        continue;
                    }

                    // 5. ДАЛЬШЕ ИДЕТ ТВОЙ РОДНОЙ ПАРСЕР ТОКЕНОВ (Он получит уже чистую строку)
                    std::stringstream ss(currentLine); std::string token;
                    int gMode = 1; double radius = 0.0; bool motionFrame = false;


                    while (ss >> token)
                    {
                        if (token.length() < 2) continue;
                        char letter = token[0]; double value = std::stod(token.substr(1));

                        if (letter == 'G') {
                            int gVal = static_cast<int>(value);
                            if (gVal >= 0 && gVal <= 3) { gMode = gVal; motionFrame = true; }
                        }
                        else if (letter == 'X') { lastX = value; motionFrame = true; }
                        else if (letter == 'Y') { lastY = value; motionFrame = true; }
                        else if (letter == 'Z') { lastZ = value; motionFrame = true; }
                        else if (letter == 'F') { currentFeed = value; }
                        else if (letter == 'S') { currentSpeed = value; }
                        else if (letter == 'R') { radius = value; }
                        else if (letter == 'M') {
                            int mVal = static_cast<int>(value);
                            if (mVal == 3) {
                                std::cout << "  [КОМАНДА ПЛК] M03: Запуск шпинделя стойки ЧПУ.\n";
                            }
                            else if (mVal == 5) {
                                std::cout << "  [КОМАНДА ПЛК] M05: Останов вращения шпинделя.\n";
                            }
                            else if (mVal == 30) {
                                std::cout << "\n[ПЛК СИГНАЛ] Команда M30: Управляющая программа ЧПУ успешно завершена.\n";
                                std::cout << "[СИСТЕМА] Сброс датчиков цеха. Ожидание следующей детали.\n";
                                cnc->setState(MachineState::IDLE);
                                modbus_registers[FactorySettings::MB_CNC_STATUS] = static_cast<uint16_t>(MachineState::IDLE);
                                conveyor->setSensor(false);
                                modbus_registers[9] = 0;
                                file.close(); fileOpened = false;
                            }
                        }
                    }

                    if (cnc->getState() == MachineState::WORKING && motionFrame)
                    {
                        cnc->setTargetCoordinates(lastX, lastY, lastZ, currentFeed, currentSpeed, gMode, radius);
                        needNextLine = false;

                        std::string modeText = (gMode == 0) ? "Холостой ход (G00)" : ((gMode == 1) ? "Линейная резка (G01)" : "Круговая интерполяция (G02/G03)");
                        std::cout << "  [ОБРАБОТКА] " << modeText << " -> X:" << lastX << " Y:" << lastY << " Z:" << lastZ << "\n";
                    }
                }
                else
                {
                    std::cout << "\n[СИСТЕМА ЧПУ] Программа завершена. Перевод стойки в IDLE.\n";
                    cnc->setState(MachineState::IDLE);
                    modbus_registers[FactorySettings::MB_CNC_STATUS] = static_cast<uint16_t>(MachineState::IDLE);
                    file.close(); fileOpened = false;
                }
            }

            // 🎯 ЧЕСТНЫЙ ИНДУСТРИАЛЬНЫЙ ТРИГГЕР ПО ДИСТАНЦИИ
            double distanceToTarget = std::sqrt(
                std::pow(cnc->getCurrentX() - lastX, 2) +
                std::pow(cnc->getCurrentY() - lastY, 2) +
                std::pow(cnc->getCurrentZ() - lastZ, 2)
            );

            // Если фреза физически доехала до цели кадра — просим следующую строку!
            if (!needNextLine && distanceToTarget < 0.1)
            {
                std::cout << "  [СИСТЕМА ЧПУ] Кадр выполнен. Переход к следующему...\n";
                needNextLine = true;
            }

        }

        cnc->updatePhysics(timeStep);
        conveyor->updatePhysics(timeStep);

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}




void runModbusServer()
{
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) return;

    SOCKET serverSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (serverSocket == INVALID_SOCKET) { WSACleanup(); return; }

    // Чиним залипание порта 502 при перезапусках симулятора
    int optval = 1;
    setsockopt(serverSocket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&optval), sizeof(optval));

    sockaddr_in serverService;
    serverService.sin_family = AF_INET;
    serverService.sin_addr.s_addr = htonl(INADDR_ANY);
    serverService.sin_port = htons(502);

    if (bind(serverSocket, (SOCKADDR*)&serverService, sizeof(serverService)) == SOCKET_ERROR) { closesocket(serverSocket); WSACleanup(); return; }
    if (listen(serverSocket, 3) == SOCKET_ERROR) { closesocket(serverSocket); WSACleanup(); return; }

    std::cout << "[СЕТЬ] Стабильный Modbus TCP Сервер слушает порт 502...\n";
    unsigned char netBuffer[512] = { 0 };

    while (isFactoryRunning)
    {
        SOCKET acceptSocket = accept(serverSocket, NULL, NULL);
        if (acceptSocket != INVALID_SOCKET && isFactoryRunning)
        {
            //std::cout << "[СЕТЬ] Промышленный шлюз C# подключился. Сессия обмена открыта.\n";

            // 🔄 ВНУТРЕННИЙ ЦИКЛ: Выкачиваем пакеты из одного сокета до победного!
            while (isFactoryRunning)
            {
                int bytesReceived = recv(acceptSocket, reinterpret_cast<char*>(netBuffer), sizeof(netBuffer), 0);

                // Если C# отключился или таймаут — закрываем этот сокет и идем к accept()
                if (bytesReceived <= 0)
                {
                    //std::cout << "[СЕТЬ] Соединение со шлюзом закрыто. Ожидание нового подключения...\n";
                    closesocket(acceptSocket);
                    break; // Выход во внешний цикл к функции accept()
                }
                if (bytesReceived >= 12)
                {
                    // 1. АСЕМБЛЕРНАЯ ВАЛИДАЦИЯ ПАКЕТА (Твой фирменный блок)
                    uint32_t dataLengthWithoutCRC = static_cast<uint32_t>(bytesReceived - 2);
                    uint16_t computedCRC = calculateCRC16_Asm(netBuffer, dataLengthWithoutCRC);
                    uint16_t receivedCRC = (static_cast<uint16_t>(netBuffer[bytesReceived - 2]) << 8) |
                        static_cast<uint16_t>(netBuffer[bytesReceived - 1]);

                    // Раскомментируй этот блок защиты, когда синхронизируешь CRC на C# шлюзе
                    // if (computedCRC != receivedCRC) {
                    //     std::cout << "[ОШИБКА СВЯЗИ] Критический сбой CRC16! Пакет уничтожен.\n";
                    //     continue;
                    // }

                    unsigned char fCode = netBuffer[7];
                    int startReg = (static_cast<int>(netBuffer[8]) << 8) | static_cast<int>(netBuffer[9]);

                    // --- ФУНКЦИЯ 0x03: ЧТЕНИЕ РЕГИСТРОВ (Отдача телеметрии в C#) ---
                    if (fCode == 0x03 && startReg >= 0 && startReg < 60)
                    {
                        int regCount = (static_cast<int>(netBuffer[10]) << 8) | static_cast<int>(netBuffer[11]);
                        unsigned char response[260] = { 0 };
                        for (int i = 0; i < 6; ++i) response[i] = netBuffer[i];
                        response[6] = 0; response[7] = 0x03; response[8] = static_cast<unsigned char>(regCount * 2);

                        int byteIdx = 9;
                        for (int i = 0; i < regCount; ++i) {
                            uint16_t regValue = modbus_registers[startReg + i];
                            response[byteIdx++] = static_cast<unsigned char>((regValue >> 8) & 0xFF);
                            response[byteIdx++] = static_cast<unsigned char>(regValue & 0xFF);
                        }
                        int totalLength = byteIdx; int pduLength = totalLength - 6;
                        response[4] = static_cast<unsigned char>((pduLength >> 8) & 0xFF);
                        response[5] = static_cast<unsigned char>(pduLength & 0xFF);
                        send(acceptSocket, reinterpret_cast<char*>(response), totalLength, 0);
                    }
                    // --- ФУНКЦИЯ 0x06: ЗАПИСЬ ОДНОГО РЕГИСТРА (Будильник ЧПУ станка) ---
                    else if (fCode == 0x06 && startReg >= 0 && startReg < 60)
                    {
                        uint16_t valueToWrite = (static_cast<int>(netBuffer[10]) << 8) | static_cast<int>(netBuffer[11]);
                        modbus_registers[startReg] = valueToWrite;

                        if (startReg == 0) {
                            std::cout << "[СЕТЬ ПЛК] Прилетела команда в регистр 0! Значение: " << valueToWrite << "\n";
                        }

                        //std::cout << "[TRACE 4.1] C++ поймал функцию 0x06! Регистр: " << startReg << " = " << valueToWrite << "\n";

                        send(acceptSocket, reinterpret_cast<char*>(netBuffer), bytesReceived, 0);
                    }
                    // --- ФУНКЦИЯ 0x10: ГРУППОВАЯ ЗАПИСЬ РЕГИСТРОВ (ASCII Имя файла) ---
                    else if (fCode == 0x10 && startReg >= 0 && startReg < 60)
                    {
                        int regCount = (static_cast<int>(netBuffer[10]) << 8) | static_cast<int>(netBuffer[11]);
                        int dataIdx = 13;
                        for (int i = 0; i < regCount; ++i) {
                            uint16_t val = (static_cast<int>(netBuffer[dataIdx]) << 8) | static_cast<int>(netBuffer[dataIdx + 1]);
                            modbus_registers[startReg + i] = val; dataIdx += 2;
                        }
                        unsigned char response[12] = { 0 };
                        for (int i = 0; i < 6; ++i) response[i] = netBuffer[i];
                        response[5] = 6; response[6] = netBuffer[6]; response[7] = 0x10;
                        response[8] = netBuffer[8]; response[9] = netBuffer[9];
                        response[10] = netBuffer[10]; response[11] = netBuffer[11];

                        //std::cout << "[TRACE 4.2] C++ поймал функцию 0x10! Стартовый регистр: " << startReg << ", Количество: " << regCount << "\n";

                        send(acceptSocket, reinterpret_cast<char*>(response), 12, 0);
                    }
                }
            } // Конец внутреннего цикла while (чтение пакетов из живого сокета)
        }
    } // Конец внешнего цикла сокет-сервера
    closesocket(serverSocket);
    WSACleanup();
}


int main()
{
    system("chcp 65001 > nul");
    std::cout << "=========================================================\n";
    std::cout << " ПРОМЫШЛЕННЫЙ СЕРВЕР ЦЕХА ЧПУ ПОДНЯТ В СЕТИ (C++)\n";
    std::cout << "=========================================================\n\n";

    auto cncStation = std::make_shared<CNC_Machine>(1, "Фрезер ЧПУ HAAS VF2");
    auto conveyorBelt = std::make_shared<Conveyor_Machine>(2, "Линия транспортера");

    global_cnc = cncStation; // Теперь сетевой поток сокетов видит этот живой объект!


    conveyorBelt->setSensor(true); // Взводим датчик заготовки при старте сервера


    std::thread conveyorThread(runConveyorLifecycle, conveyorBelt);
    std::thread cncThread(runCNCLifecycle, cncStation, conveyorBelt);
    std::thread modbusThread(runModbusServer);

    std::cout << "[SYSTEM] Все потоки запущены. Ожидание команды запуска из сети...\n\n";

    int uptimeSeconds = 0;
    while (isFactoryRunning)
    {
        uptimeSeconds++;
        if (uptimeSeconds % 5 == 0) {
            //std::cout << "[СЕРВЕР ЖИВ] Аптайм: " << uptimeSeconds << " сек | Статус ЧПУ: "
              //  << modbus_registers[FactorySettings::MB_CNC_STATUS] << " | X: "
                //<< (static_cast<double>(modbus_registers[FactorySettings::MB_CNC_X]) / 10.0) << " | Износ фрезы: "
                //<< (static_cast<double>(modbus_registers[FactorySettings::MB_CNC_WEAR]) / 1000.0) << "\n";
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    isFactoryRunning = false;
    if (conveyorThread.joinable()) conveyorThread.join();
    if (cncThread.joinable()) cncThread.join();
    if (modbusThread.joinable()) modbusThread.join();
    return 0;
}
