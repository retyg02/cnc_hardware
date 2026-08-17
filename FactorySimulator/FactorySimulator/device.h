#pragma once
#include <string>
#include <vector>
#include <memory>


namespace FactorySettings
{
    constexpr double ROOM_TEMPERATURE = 25.0;
    constexpr double CRITICAL_TEMPERATURE = 100.0;
    constexpr double CONVEYOR_MAX_LENGTH = 5.0;
    constexpr double CONVEYOR_DEFAULT_SPEED = 0.8;
}

enum class MachineState
{
    IDLE,
    WORKING,
    ERROR
};

enum class MachineCommand
{
    RESET,
    STOP
};

struct MachineTelemetry
{
    int machineId;
    std::string machineName;
    MachineState state;
    double loadPercent;
    double temperature;
    unsigned int errorCount;
};

class BaseMachine
{
protected:
    int id;
    std::string name;
    MachineState state;
    MachineCommand current_command;
    double load_percent;
    unsigned int error_count;
    double temperature;
public:
    BaseMachine(int id, const std::string& name);
    virtual ~BaseMachine() = default;

    void setState(MachineState newState);
    void setCommand(MachineCommand newCommand);
    int getId() const;
    std::string getName() const;
    MachineState getState() const;
    MachineCommand getCommand() const;
    double getPercentLoad() const;
    unsigned int getErrors() const;
    double getTemp() const;

    MachineTelemetry getFullTelemetry() const; 

    virtual void updatePhysics(double deltaTime) = 0;
};

class CNC_Machine : public BaseMachine
{
private:
    double currentX, currentY, currentZ;
    double targetX, targetY, targetZ;
    double startX, startY;

    double spindelSpeed;
    bool isCutting;
    double wearing;
    double rotateRadius;
    double feedRate;

    int currentGCodeMode; 
    double arcAngle;      
    double targetAngle;   
    double centerX, centerY; 

public:
    CNC_Machine(int id, const std::string& name);

    void executeCuttingProgramm(std::string filePath);
    void updatePhysics(double deltaTime) override;

    void setTargetCoordinates(double x, double y, double z, double feed, double speed, int gMode, double radius);

    double getCurrentX() const;
    double getCurrentY() const;
    double getCurrentZ() const;
    double getSpindleSpeed() const;
    bool getIsCutting() const;
    double getWearing() const;
};

class Conveyor_Machine : public BaseMachine
{
private:
    double currentPosition;
    double targetPosition;
    double beltSpeed;
    bool itemDetected;
public:
    Conveyor_Machine(int id, const std::string& name);

    void executeMoveProgramm();
    void updatePhysics(double deltaTime) override;

    double getCurrentPosition() const;
    double getBeltSpeed() const;
    bool isItemAtStation() const;
    void setSensor(bool detected);
};
