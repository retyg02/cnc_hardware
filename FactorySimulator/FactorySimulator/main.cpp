#include <iostream>
#include <vector>
#include <memory>
#include <thread>
#include <chrono>
#include <fstream>
#include <sstream>
#include "device.h"

bool isFactoryRunning = true;

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

void runCNCLifecycle(std::shared_ptr<CNC_Machine> cnc, std::shared_ptr<Conveyor_Machine> conveyor, std::string filePath)
{
    double timeStep = 0.1;
    std::ifstream file;
    std::string currentLine;
    bool fileOpened = false;
    bool needNextLine = true;

    double lastX = 0.0, lastY = 0.0, lastZ = 0.0;
    double currentFeed = 120.0;
    double currentSpeed = 0.0;

    while (isFactoryRunning)
    {
        if (cnc->getState() == MachineState::IDLE)
        {
            if (conveyor->isItemAtStation() && !fileOpened)
            {
                std::cout << "\n[АВТОМАТИКА ПЛК] Деталь на позиции. Загрузка файла УП...\n";
                file.open(filePath);
                if (file.is_open())
                {
                    cnc->executeCuttingProgramm(filePath);
                    fileOpened = true;
                    needNextLine = true;
                }
                else
                {
                    std::cout << "[ОШИБКА ИНФРАСТРУКТУРЫ] Файл " << filePath << " не найден!\n";
                    cnc->setState(MachineState::ERROR);
                }
            }
            cnc->updatePhysics(timeStep);
        }

        if (cnc->getState() == MachineState::WORKING && fileOpened)
        {
            if (needNextLine)
            {
                if (std::getline(file, currentLine))
                {
                    if (currentLine.empty() || currentLine[0] == ';') continue;

                    size_t commentPos = currentLine.find(';');
                    if (commentPos != std::string::npos) {
                        currentLine = currentLine.substr(0, commentPos);
                    }

                    std::cout << "  [ПРОЦЕССОР СТОЙКИ] Шаг УП -> " << currentLine << "\n";

                    std::stringstream ss(currentLine);
                    std::string token;

                    int gMode = 1; 
                    double radius = 0.0;
                    bool motionFrame = false;

                    while (ss >> token)
                    {
                        char letter = token[0];
                        if (token.length() < 2) continue;
                        double value = std::stod(token.substr(1));

                        if (letter == 'G')
                        {
                            int gVal = static_cast<int>(value);
                            if (gVal == 0 || gVal == 1 || gVal == 2 || gVal == 3) {
                                gMode = gVal;
                                motionFrame = true;
                            }
                        }
                        else if (letter == 'X') { lastX = value; motionFrame = true; }
                        else if (letter == 'Y') { lastY = value; motionFrame = true; }
                        else if (letter == 'Z') { lastZ = value; motionFrame = true; }
                        else if (letter == 'F') { currentFeed = value; }
                        else if (letter == 'S') { currentSpeed = value; }
                        else if (letter == 'R') { radius = value; }
                        else if (letter == 'M')
                        {
                            int mVal = static_cast<int>(value);
                            if (mVal == 3) currentSpeed = 1500.0; 
                            else if (mVal == 5) currentSpeed = 0.0; 
                            else if (mVal == 30)
                            {
                                std::cout << "[ЧПУ СИГНАЛ] Команда M30 (Конец УП). Переход в сон.\n";
                                cnc->setState(MachineState::IDLE);
                                conveyor->setSensor(false); 
                                file.close();
                                fileOpened = false;
                            }
                        }
                    }

                    if (cnc->getState() == MachineState::WORKING && motionFrame)
                    {
                        cnc->setTargetCoordinates(lastX, lastY, lastZ, currentFeed, currentSpeed, gMode, radius);
                        needNextLine = false;
                    }
                }
                else
                {
                    cnc->setState(MachineState::IDLE);
                    file.close();
                    fileOpened = false;
                }
            }

            cnc->updatePhysics(timeStep);

            static int frameTimer = 0;
            frameTimer++;
            if (frameTimer >= 30)
            {
                needNextLine = true;
                frameTimer = 0;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

int main()
{
    system("chcp 65001 > nul");

    std::cout << "=========================================================\n";
    std::cout << " ЗАПУСК АСИНХРОННОГО ЦИФРОВОГО ДВОЙНИКА ЦЕХА (C++17/20)\n";
    std::cout << "=========================================================\n\n";

    auto cncStation = std::make_shared<CNC_Machine>(1, "Фрезер ЧПУ HAAS VF2");
    auto conveyorBelt = std::make_shared<Conveyor_Machine>(2, "Линия транспортера");

    std::thread conveyorThread(runConveyorLifecycle, conveyorBelt);
    std::thread cncThread(runCNCLifecycle, cncStation, conveyorBelt, "test.nc");

    for (int i = 0; i < 20; ++i)
    {
        std::cout << "\n--- МОНИТОРИНГ ЦЕХА (Секунда " << i + 1 << ") ---\n";

        std::cout << "[" << conveyorBelt->getName() << "] Позиция: " << conveyorBelt->getCurrentPosition()
            << " м | Скорость: " << conveyorBelt->getBeltSpeed() << " м/с | Датчик: "
            << (conveyorBelt->isItemAtStation() ? "ЕСТЬ ДЕТАЛЬ" : "ПУСТО") << "\n";

        std::cout << "[" << cncStation->getName() << "] Статус: "
            << (cncStation->getState() == MachineState::WORKING ? "WORKING" : "IDLE")
            << " | X: " << cncStation->getCurrentX()
            << " Y: " << cncStation->getCurrentY()
            << " Z: " << cncStation->getCurrentZ()
            << " | Temp: " << cncStation->getTemp()
            << "°C | Нагрузка: " << cncStation->getPercentLoad() << "%\n";

        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    isFactoryRunning = false;
    if (conveyorThread.joinable()) conveyorThread.join();
    if (cncThread.joinable()) cncThread.join();

    std::cout << "=== СИМУЛЯЦИЯ УСПЕШНО ЗАВЕРШЕНА. ВСЕ ПОТОКИ ЗАКРЫТЫ. ===\n";
    return 0;
}
