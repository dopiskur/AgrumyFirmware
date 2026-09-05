#include "Arduino.h"
#include <WiFi.h>
#include <EEPROM.h>
#include "FS.h"
#include "WiFiManager.h"

#include "DeviceController.h"
#include "ServiceController.h"
#include "ActuatorController.h"

void ActuatorController::setupController(){


}

// Only slots the server actually assigned arrive in deviceConfig.configController.relays[0..relayCount) - an unlisted slot is unassigned, nothing to collect for it.
int ActuatorController::collectPinsForFunction(RelayFunctionType relayFunction, int pins[MAX_RELAY_SLOTS]) const
{
    int count = 0;
    for (int i = 0; i < deviceConfig.configController.relayCount; i++)
    {
        const RelaySlot &relaySlot = deviceConfig.configController.relays[i];
        if (relaySlot.relayFunction == (int)relayFunction && relaySlot.slot >= 1 && relaySlot.slot <= MAX_RELAY_SLOTS)
        {
            pins[count++] = deviceConfig.configPin.RELAY_PINS[relaySlot.slot - 1];
        }
    }
    return count;
}

// Threshold rules are ignored - they're re-evaluated every poll regardless of timing, no boundary to sleep toward. 30s floor avoids excessive wake-cycle thrashing right next to a boundary, especially for battery devices.
int ActuatorController::computeNextWakeSeconds(time_t epochSeconds, int defaultSleepSeconds) const
{
    const int FLOOR_SECONDS = 30;

    time_t localEpoch = epochSeconds + deviceConfig.utcOffsetSeconds;
    struct tm *localTm = gmtime(&localEpoch);
    int localWeekday = localTm->tm_wday;
    int localSecondsOfDay = localTm->tm_hour * 3600 + localTm->tm_min * 60 + localTm->tm_sec;

    int best = defaultSleepSeconds;
    for (int i = 0; i < deviceConfig.configController.ruleCount; i++)
    {
        const Rule &rule = deviceConfig.configController.rules[i];
        int candidate = -1;
        if (rule.type == CONDITION_SCHEDULE)
        {
            candidate = secondsUntilScheduleBoundary(rule.daysOfWeek, rule.start, rule.duration, localWeekday, localSecondsOfDay);
        }
        else if (rule.type == CONDITION_INTERVAL)
        {
            candidate = secondsUntilIntervalBoundary(rule.interval, rule.intervalLength, epochSeconds);
        }
        if (candidate >= 0 && candidate < best)
        {
            best = candidate;
        }
    }
    return clampToSleepFloor(best, FLOOR_SECONDS);
}

// Ventilation reacts to humidity and is the only function whose "on" direction is inverted (exhausting excess humidity, not replenishing a deficit); Light/Heating/WaterPump all turn on BELOW their threshold and off above threshold+hysteresis. isCurrentlyOn is needed only for this dead-zone math - interval/schedule ignore it entirely.
bool ActuatorController::evaluateRule(const Rule &rule, SensorData sensorData, time_t epochSeconds,
                                       int localWeekday, int localSecondsOfDay, bool isCurrentlyOn) const
{
    switch (rule.type)
    {
    case CONDITION_THRESHOLD:
    {
        double reading;
        bool turnsOnAboveThreshold;
        switch ((RelayFunctionType)rule.targetFunction)
        {
        case RelayFunctionType::Ventilation:
            reading = sensorData.humidity;
            turnsOnAboveThreshold = true;
            break;
        case RelayFunctionType::Light:
            reading = sensorData.light;
            turnsOnAboveThreshold = false;
            break;
        case RelayFunctionType::Heating:
            reading = sensorData.temperature;
            turnsOnAboveThreshold = false;
            break;
        case RelayFunctionType::WaterPump:
            reading = sensorData.waterLevel;
            turnsOnAboveThreshold = false;
            break;
        default:
            return false; // rule somehow targets no function - never on
        }
        // NAN means no reading this cycle (sensor absent/disabled/failed) - must not be evaluated as a phantom threshold-crossing value (e.g. Heating turning on for a false 0C). Roadmap #324/#326.
        if (isnan(reading))
        {
            return false;
        }
        return computeThresholdState(isCurrentlyOn, reading, rule.threshold, rule.hysteresis, turnsOnAboveThreshold);
    }
    case CONDITION_INTERVAL:
        return rule.interval > 0 && computeIntervalState(rule.interval, rule.intervalLength, epochSeconds);
    case CONDITION_SCHEDULE:
        return computeScheduleState(rule.daysOfWeek, rule.start, rule.duration, localWeekday, localSecondsOfDay);
    default:
        return false; // unrecognized type - ConfigParser already skips these at parse time, belt and suspenders
    }
}

// Order matters: cooldown is evaluated against the OLD offSinceEpoch BEFORE anything below touches onSinceEpoch/offSinceEpoch, so an ON request arriving mid-cooldown can never reset its own clock into a permanent lockout.
void ActuatorController::applyWaterPumpSafetyLimits(int slotIndex, int pin, time_t epochSeconds)
{
    int i2cAddr = deviceConfig.configPin.RELAY_I2C_ADDRESS;
    int i2cSda = deviceConfig.configPin.RELAY_I2C_SDA;
    int i2cScl = deviceConfig.configPin.RELAY_I2C_SCL;
    bool desiredState = relayRead(pin, i2cAddr, i2cSda, i2cScl); // whatever threshold/interval/schedule already wrote this tick
    int maxRunSeconds = deviceConfig.configController.waterPumpMaxRunSeconds;
    int cooldownSeconds = deviceConfig.configController.waterPumpCooldownSeconds;

    bool blockedByCooldown = desiredState && cooldownActive(epochSeconds, waterPumpOffSinceEpoch[slotIndex], cooldownSeconds);

    if (desiredState && !blockedByCooldown && waterPumpOnSinceEpoch[slotIndex] == 0)
    {
        waterPumpOnSinceEpoch[slotIndex] = epochSeconds;
    }

    bool ceilingHit = desiredState && !blockedByCooldown
                       && runTimeCeilingHit(epochSeconds, waterPumpOnSinceEpoch[slotIndex], maxRunSeconds);

    bool finalState = desiredState && !blockedByCooldown && !ceilingHit;

    if (!finalState && waterPumpOnSinceEpoch[slotIndex] != 0)
    {
        // Start the cooldown clock on every pump-off (not just safety-limit-caused ones) - water needs time to drain regardless of why the pump stopped.
        waterPumpOffSinceEpoch[slotIndex] = epochSeconds;
        waterPumpOnSinceEpoch[slotIndex] = 0;
    }

    if (finalState != desiredState)
    {
        relayWrite(pin, finalState, i2cAddr, i2cSda, i2cScl);
        if (ceilingHit)
        {
            reportSafetyLimitTripped("WaterPump max run time exceeded (" + String(maxRunSeconds) + "s)");
        }
        else if (blockedByCooldown)
        {
            reportSafetyLimitTripped("WaterPump cooldown active, restart blocked");
        }
    }
}

void ActuatorController::reportSafetyLimitTripped(const String &message)
{
    Serial.println("[Safety limit] " + message);
    pendingSafetyEventMessage = message; // last one wins if several slots trip the same tick
}

bool ActuatorController::consumeSafetyLimitEvent(String &outMessage)
{
    if (pendingSafetyEventMessage.length() == 0)
    {
        return false;
    }
    outMessage = pendingSafetyEventMessage;
    pendingSafetyEventMessage = "";
    return true;
}

void ActuatorController::initController(SensorData sensorData, time_t epochSeconds)
{
    // Routes through RelayIO so an I2C-expander kit (KC868-A6) works the same as a direct-GPIO one.
    int i2cAddr = deviceConfig.configPin.RELAY_I2C_ADDRESS;
    int i2cSda = deviceConfig.configPin.RELAY_I2C_SDA;
    int i2cScl = deviceConfig.configPin.RELAY_I2C_SCL;

    // Densify the sparse relays[] list into a per-physical-slot lookup - waterPump*SinceEpoch/lastConfiguredType below are indexed by physical slot (0..MAX_RELAY_SLOTS-1), not by position in relays[].
    int configuredType[MAX_RELAY_SLOTS] = {0};
    for (int i = 0; i < deviceConfig.configController.relayCount; i++)
    {
        const RelaySlot &relaySlot = deviceConfig.configController.relays[i];
        if (relaySlot.slot >= 1 && relaySlot.slot <= MAX_RELAY_SLOTS)
        {
            configuredType[relaySlot.slot - 1] = relaySlot.relayFunction;
        }
    }
    const int *relayPin = deviceConfig.configPin.RELAY_PINS;

    // A slot whose function assignment changed since last tick can't trust its old WaterPump on/off-since history, even if it isn't WaterPump now (a later remap back to WaterPump would otherwise reuse it).
    for (int i = 0; i < MAX_RELAY_SLOTS; i++)
    {
        if (configuredType[i] != lastConfiguredType[i])
        {
            waterPumpOnSinceEpoch[i] = 0;
            waterPumpOffSinceEpoch[i] = 0;
            lastConfiguredType[i] = configuredType[i];
        }
    }

    // Master safety switches - either one lets the server force every relay off regardless of what the rules below would otherwise decide. emergencyStop is tenant-wide and fail-closed (roadmap #230); relayEnabled is this device's own per-controller toggle.
    if (deviceConfig.emergencyStop || !deviceConfig.configController.relayEnabled)
    {
        for (int i = 0; i < MAX_RELAY_SLOTS; i++)
        {
            if ((RelayFunctionType)configuredType[i] == RelayFunctionType::None)
            {
                continue;
            }
            relayPinMode(relayPin[i], i2cAddr, i2cSda, i2cScl);
            relayWrite(relayPin[i], false, i2cAddr, i2cSda, i2cScl);
        }
        return;
    }

    // gmtime() on a pre-shifted epoch yields LOCAL wall-clock calendar fields with no timezone database needed. Computed once here, not once per rule evaluated below.
    time_t localEpoch = epochSeconds + deviceConfig.utcOffsetSeconds;
    struct tm *localTm = gmtime(&localEpoch);
    int localWeekday = localTm->tm_wday;      // 0=Sunday..6=Saturday
    int localSecondsOfDay = localTm->tm_hour * 3600 + localTm->tm_min * 60 + localTm->tm_sec;

    // ONE pass per relay function: every rule targeting it is OR'd together (any rule saying "on" wins), then the single result is written to every pin assigned to it.
    const RelayFunctionType functions[4] = {
        RelayFunctionType::Ventilation, RelayFunctionType::Light,
        RelayFunctionType::Heating, RelayFunctionType::WaterPump,
    };
    for (RelayFunctionType function : functions)
    {
        int pins[MAX_RELAY_SLOTS];
        int pinCount = collectPinsForFunction(function, pins);
        if (pinCount == 0)
        {
            continue; // no relay slot assigned to this function
        }

        // Threshold rules need the function's CURRENT physical state for hysteresis math - read once from the first assigned pin; every pin sharing one function is kept in sync by the write below, so any one is representative.
        relayPinMode(pins[0], i2cAddr, i2cSda, i2cScl);
        bool isCurrentlyOn = relayRead(pins[0], i2cAddr, i2cSda, i2cScl);

        bool shouldBeOn = false;
        for (int i = 0; i < deviceConfig.configController.ruleCount; i++)
        {
            const Rule &rule = deviceConfig.configController.rules[i];
            if (rule.targetFunction == (int)function &&
                evaluateRule(rule, sensorData, epochSeconds, localWeekday, localSecondsOfDay, isCurrentlyOn))
            {
                shouldBeOn = true;
            }
        }

        // Final AND-NOT gate applied AFTER the OR above - a Weather condition can't be a Rule like Threshold/Interval/Schedule, since OR-combining rules means it could only ever ADD a reason to turn WaterPump on, never suppress one.
        if (function == RelayFunctionType::WaterPump && deviceConfig.configController.skipWaterPumpForRain)
        {
            shouldBeOn = false;
        }

        for (int i = 0; i < pinCount; i++)
        {
            relayPinMode(pins[i], i2cAddr, i2cSda, i2cScl);
            relayWrite(pins[i], shouldBeOn, i2cAddr, i2cSda, i2cScl);
        }
    }

    // Safety limits are applied per PHYSICAL SLOT (not once for the function, unlike the loop above) - each relay slot sharing the WaterPump function keeps its own independent on/off-since history. Reuses configuredType/relayPin declared at the top of this function.
    for (int i = 0; i < MAX_RELAY_SLOTS; i++)
    {
        if ((RelayFunctionType)configuredType[i] == RelayFunctionType::WaterPump)
        {
            applyWaterPumpSafetyLimits(i, relayPin[i], epochSeconds);
        }
    }
}
