#include "device.h"
#include <iostream>
#include <cmath>
#include <cstdlib>


#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

BaseMachine::BaseMachine(int id, const std::string& name)
{
    this->id = id;
    this->name = name;
    this->state = MachineState::IDLE;
    this->current_command = MachineCommand::STOP;
    this->load_percent = 0.0;
    this->error_count = 0;
    this->temperature = FactorySettings::ROOM_TEMPERATURE;
}

void BaseMachine::setState(MachineState newState) { state = newState; }
void BaseMachine::setCommand(MachineCommand newCommand) { current_command = newCommand; }
int BaseMachine::getId() const { return id; }
std::string BaseMachine::getName() const { return name; }
MachineState BaseMachine::getState() const { return state; }
MachineCommand BaseMachine::getCommand() const { return current_command; }
double BaseMachine::getPercentLoad() const { return load_percent; }
unsigned int BaseMachine::getErrors() const { return error_count; }
double BaseMachine::getTemp() const { return temperature; }

MachineTelemetry BaseMachine::getFullTelemetry() const
{
    MachineTelemetry t;
    t.machineId = id;
    t.machineName = name;
    t.state = state;
    t.loadPercent = load_percent;
    t.temperature = temperature;
    t.errorCount = error_count;
    return t;
}



CNC_Machine::CNC_Machine(int id, const std::string& name)
    : BaseMachine(id, name)
{
    currentX = currentY = currentZ = 0.0;
    targetX = targetY = targetZ = 0.0;
    startX = startY = 0.0;
    spindelSpeed = 0.0;
    isCutting = false;
    wearing = 1.0;
    rotateRadius = 0.0;
    feedRate = 0.0;
    currentGCodeMode = 0;
    arcAngle = targetAngle = centerX = centerY = 0.0;
}

void CNC_Machine::setTargetCoordinates(double x, double y, double z, double feed, double speed, int gMode, double radius)
{
    startX = currentX;
    startY = currentY;
    targetX = x;
    targetY = y;
    targetZ = z;
    feedRate = feed;
    spindelSpeed = speed;
    currentGCodeMode = gMode;
    rotateRadius = radius;

    isCutting = (gMode == 1 || gMode == 2 || gMode == 3);

    if ((gMode == 2 || gMode == 3) && radius > 0.0)
    {
        double dx = targetX - startX;
        double dy = targetY - startY;
        double dist = std::sqrt(dx * dx + dy * dy);

        if (dist > 0.0 && dist <= 2.0 * radius)
        {
            double midX = (startX + targetX) / 2.0;
            double midY = (startY + targetY) / 2.0;
            double h = std::sqrt(radius * radius - (dist * dist) / 4.0);

            if (gMode == 2) {
                centerX = midX + h * dy / dist;
                centerY = midY - h * dx / dist;
            }
            else {
                centerX = midX - h * dy / dist;
                centerY = midY + h * dx / dist;
            }

            arcAngle = std::atan2(startY - centerY, startX - centerX);
            targetAngle = std::atan2(targetY - centerY, targetX - centerX);

            if (gMode == 2 && targetAngle > arcAngle) targetAngle -= 2.0 * M_PI;
            if (gMode == 3 && targetAngle < arcAngle) targetAngle += 2.0 * M_PI;
        }
    }
}

void CNC_Machine::updatePhysics(double deltaTime)
{
    if (state == MachineState::ERROR)
    {
        load_percent = 0.0;
        spindelSpeed = 0.0;
        isCutting = false;
        return;
    }

    if (state == MachineState::WORKING)
    {
        wearing += 0.0005 * deltaTime;
        double speedMMPerSec = feedRate / 60.0;

        if (currentGCodeMode == 0 || currentGCodeMode == 1)
        {
            double dx = targetX - currentX;
            double dy = targetY - currentY;
            double dz = targetZ - currentZ;
            double distance = std::sqrt(dx * dx + dy * dy + dz * dz);

            if (distance > 0.0)
            {
                double step = (currentGCodeMode == 0 ? 30.0 : speedMMPerSec) * deltaTime; 
                if (step >= distance) {
                    currentX = targetX;
                    currentY = targetY;
                    currentZ = targetZ;
                }
                else {
                    currentX += (dx / distance) * step;
                    currentY += (dy / distance) * step;
                    currentZ += (dz / distance) * step;
                }
            }
        }

        else if ((currentGCodeMode == 2 || currentGCodeMode == 3) && rotateRadius > 0.0)
        {
            double angularSpeed = speedMMPerSec / rotateRadius; 
            double angleStep = angularSpeed * deltaTime;

            if (currentGCodeMode == 2) {
                arcAngle -= angleStep;
                if (arcAngle <= targetAngle) arcAngle = targetAngle;
            }
            else {
                arcAngle += angleStep;
                if (arcAngle >= targetAngle) arcAngle = targetAngle;
            }

            currentX = centerX + rotateRadius * std::cos(arcAngle);
            currentY = centerY + rotateRadius * std::sin(arcAngle);

            if (currentZ != targetZ) {
                double dz = targetZ - currentZ;
                currentZ += dz * 0.1;
            }
        }

        if (isCutting)
        {
            double arcFactor = (currentGCodeMode == 2 || currentGCodeMode == 3) ? 1.25 : 1.0;
            load_percent = (feedRate * 0.03) * (std::abs(currentZ) + 0.5) * wearing * arcFactor;
            if (load_percent > 100.0) load_percent = 100.0;
        }
        else
        {
            load_percent = spindelSpeed > 0 ? 6.0 : 0.0; 
        }

        if (spindelSpeed > 0)
        {
            temperature += (spindelSpeed * 0.00004) * deltaTime;
            if (temperature >= FactorySettings::CRITICAL_TEMPERATURE)
            {
                setState(MachineState::ERROR);
                error_count++;
                std::cout << "\n[АВАРИЙНЫЙ ТРИГГЕР ПЛК] Превышен лимит нагрева шпинделя: " << temperature << "°C!\n";
            }
        }
    }
    else
    {
        load_percent = 0.0;
        if (temperature > FactorySettings::ROOM_TEMPERATURE) {
            temperature -= 0.3 * deltaTime;
        }
    }
}

void CNC_Machine::executeCuttingProgramm(std::string filePath)
{
    setState(MachineState::WORKING);
}

double CNC_Machine::getCurrentX() const { return currentX; }
double CNC_Machine::getCurrentY() const { return currentY; }
double CNC_Machine::getCurrentZ() const { return currentZ; }
double CNC_Machine::getSpindleSpeed() const { return spindelSpeed; }
bool CNC_Machine::getIsCutting() const { return isCutting; }
double CNC_Machine::getWearing() const { return wearing; }



Conveyor_Machine::Conveyor_Machine(int id, const std::string& name)
    : BaseMachine(id, name)
{
    currentPosition = 0.0;
    targetPosition = FactorySettings::CONVEYOR_MAX_LENGTH;
    beltSpeed = 0.0;
    itemDetected = false;
}

void Conveyor_Machine::executeMoveProgramm()
{
    if (state == MachineState::IDLE && !itemDetected)
    {
        beltSpeed = FactorySettings::CONVEYOR_DEFAULT_SPEED;
        currentPosition = 0.0;
        setState(MachineState::WORKING);
    }
}

void Conveyor_Machine::updatePhysics(double deltaTime)
{
    if (state == MachineState::ERROR)
    {
        beltSpeed = 0.0;
        load_percent = 0.0;
        return;
    }

    if (state == MachineState::WORKING)
    {
        currentPosition += beltSpeed * deltaTime;
        load_percent = itemDetected ? 40.0 : 15.0;

        if (currentPosition >= targetPosition)
        {
            currentPosition = targetPosition;
            setSensor(true);
            beltSpeed = 0.0;
            setState(MachineState::IDLE);
        }
    }
}

void Conveyor_Machine::setSensor(bool detected) { itemDetected = detected; }
double Conveyor_Machine::getCurrentPosition() const { return currentPosition; }
double Conveyor_Machine::getBeltSpeed() const { return beltSpeed; }
bool Conveyor_Machine::isItemAtStation() const { return itemDetected; }
