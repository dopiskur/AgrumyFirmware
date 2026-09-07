#include "Arduino.h"
#include <WiFi.h>
#include <EEPROM.h>
#include "FS.h"
#include "WiFiManager.h"

#include "DeviceController.h"
#include "ServiceController.h"
#include "ActuatorController.h"

// 2023-11-14 UTC, safely before any real deployment - distinguishes a genuine epoch from the 0 (or near-0) value NTPClient reports before its first successful sync.
static const time_t MIN_PLAUSIBLE_EPOCH = 1700000000;

// Heating holds its last state across a NaN reading (staying off risks freezing while the sensor is briefly down) but not forever - past this many seconds of continuous staleness the risk flips (a genuinely dead sensor with the heater stuck on is its own hazard), so it forces off instead.
static const int MAX_HEATING_SENSOR_STALE_SECONDS = 30 * 60;

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
            int pin = deviceConfig.configPin.RELAY_PINS[relaySlot.slot - 1];
            if (pin >= 0) // -1 means this board has no physical pin at this slot - a misconfigured server assignment, not a real relay
            {
                pins[count++] = pin;
            }
        }
    }
    return count;
}

// Threshold conditions are ignored - they're re-evaluated every poll regardless of timing, no boundary to sleep toward. 30s floor avoids excessive wake-cycle thrashing right next to a boundary, especially for battery devices.
int ActuatorController::computeNextWakeSeconds(time_t epochSeconds, int defaultSleepSeconds) const
{
    time_t localEpoch = epochSeconds + deviceConfig.utcOffsetSeconds;
    struct tm *localTm = gmtime(&localEpoch);
    int localWeekday = localTm->tm_wday;
    int localSecondsOfDay = localTm->tm_hour * 3600 + localTm->tm_min * 60 + localTm->tm_sec;

    // Roadmap #212: a boundary can come from ANY condition inside ANY rule, regardless of its position in that rule's AND/OR fold - the fold's boolean RESULT doesn't matter here, only "does this condition's own state flip soon".
    int best = defaultSleepSeconds;
    for (int i = 0; i < deviceConfig.configController.ruleCount; i++)
    {
        const Rule &rule = deviceConfig.configController.rules[i];
        for (int j = 0; j < rule.conditionCount; j++)
        {
            const Condition &condition = rule.conditions[j];
            int candidate = -1;
            if (condition.type == CONDITION_SCHEDULE)
            {
                candidate = secondsUntilScheduleBoundary(condition.daysOfWeek, condition.start, condition.duration, localWeekday, localSecondsOfDay);
            }
            else if (condition.type == CONDITION_INTERVAL)
            {
                candidate = secondsUntilIntervalBoundary(condition.interval, condition.intervalLength, epochSeconds);
            }
            if (candidate >= 0 && candidate < best)
            {
                best = candidate;
            }
        }
    }
    return clampToSleepFloor(best, MIN_SLEEP_SECONDS);
}

// Ventilation reacts to humidity and is the only function whose "on" direction is inverted (exhausting excess humidity, not replenishing a deficit); Light/Heating/WaterPump all turn on BELOW their threshold and off above threshold+hysteresis. isCurrentlyOn is needed only for this dead-zone math - interval/schedule ignore it entirely.
bool ActuatorController::evaluateCondition(const Condition &condition, int targetFunction, SensorData sensorData, time_t epochSeconds,
                                            int localWeekday, int localSecondsOfDay, bool isCurrentlyOn) const
{
    switch (condition.type)
    {
    case CONDITION_THRESHOLD:
    {
        double reading;
        bool turnsOnAboveThreshold;
        switch ((RelayFunctionType)targetFunction)
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
        // NAN means no reading this cycle (sensor absent/disabled/failed) - must not be evaluated as a phantom threshold-crossing value (e.g. Heating turning on for a false 0C).
        if (isnan(reading))
        {
            reportSensorStale("Function " + String(targetFunction) + " threshold condition has no reading this cycle (sensor absent/disabled/failed)");
            // fail-OFF (WaterPump/Light/Ventilation - a false-positive "on" is worse than staying off) is the wrong direction for Heating, where staying off risks freezing while the sensor is down. Hold whatever this condition last contributed, but only up to MAX_HEATING_SENSOR_STALE_SECONDS - a sensor that never recovers must not leave a heater stuck on indefinitely.
            if ((RelayFunctionType)targetFunction == RelayFunctionType::Heating)
            {
                if (heatingSensorStaleSinceEpoch == 0)
                {
                    heatingSensorStaleSinceEpoch = epochSeconds;
                }
                if (runTimeCeilingHit(epochSeconds, heatingSensorStaleSinceEpoch, MAX_HEATING_SENSOR_STALE_SECONDS))
                {
                    reportSensorStale("Heating forced off - temperature sensor has been stale for over " + String(MAX_HEATING_SENSOR_STALE_SECONDS) + "s");
                    return false;
                }
                return isCurrentlyOn;
            }
            return false;
        }
        if ((RelayFunctionType)targetFunction == RelayFunctionType::Heating)
        {
            heatingSensorStaleSinceEpoch = 0;
        }
        return computeThresholdState(isCurrentlyOn, reading, condition.threshold, condition.hysteresis, turnsOnAboveThreshold);
    }
    case CONDITION_INTERVAL:
        // epoch is 0 (or otherwise implausible) before the first successful NTP sync - evaluating against that computes nonsense (Jan 1 1970) rather than skipping until real time is known.
        return epochSeconds >= MIN_PLAUSIBLE_EPOCH && condition.interval > 0 && computeIntervalState(condition.interval, condition.intervalLength, epochSeconds);
    case CONDITION_SCHEDULE:
        return epochSeconds >= MIN_PLAUSIBLE_EPOCH && computeScheduleState(condition.daysOfWeek, condition.start, condition.duration, localWeekday, localSecondsOfDay);
    default:
        return false; // unrecognized type - ConfigParser already skips these at parse time, belt and suspenders
    }
}

// Roadmap #212. Evaluates every condition, then hands the results to RelayLogic's foldConditions for
// the actual strict left-to-right AND/OR combine - array-building split out here so the fold logic
// itself stays a pure, natively-testable function independent of Arduino/SensorData/this class.
bool ActuatorController::evaluateRule(const Rule &rule, SensorData sensorData, time_t epochSeconds,
                                       int localWeekday, int localSecondsOfDay, bool isCurrentlyOn) const
{
    bool results[MAX_CONDITIONS_PER_RULE];
    int ops[MAX_CONDITIONS_PER_RULE];
    for (int i = 0; i < rule.conditionCount; i++)
    {
        results[i] = evaluateCondition(rule.conditions[i], rule.targetFunction, sensorData, epochSeconds, localWeekday, localSecondsOfDay, isCurrentlyOn);
        ops[i] = rule.conditions[i].operatorBefore;
    }
    return foldConditions(results, ops, rule.conditionCount);
}

// Order matters: cooldown is evaluated against the OLD offSinceEpoch BEFORE anything below touches onSinceEpoch/offSinceEpoch, so an ON request arriving mid-cooldown can never reset its own clock into a permanent lockout.
void ActuatorController::applyWaterPumpSafetyLimits(int slotIndex, int pin, time_t epochSeconds)
{
    int i2cAddr = deviceConfig.configPin.RELAY_I2C_ADDRESS;
    int i2cSda = deviceConfig.configPin.RELAY_I2C_SDA;
    int i2cScl = deviceConfig.configPin.RELAY_I2C_SCL;
    bool activeLow = deviceConfig.configPin.RELAY_ACTIVE_LOW;
    bool desiredState = relayRead(pin, i2cAddr, i2cSda, i2cScl, activeLow); // whatever threshold/interval/schedule already wrote this tick
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
        relayWrite(pin, finalState, i2cAddr, i2cSda, i2cScl, activeLow);
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

const ManualOverride *ActuatorController::findManualOverride(RelayFunctionType relayFunction) const
{
    for (int i = 0; i < deviceConfig.configController.manualOverrideCount; i++)
    {
        if (deviceConfig.configController.manualOverrides[i].relayFunction == (int)relayFunction)
        {
            return &deviceConfig.configController.manualOverrides[i];
        }
    }
    return nullptr;
}

double ActuatorController::readingForTargetMetric(int targetMetric, const SensorData &sensorData) const
{
    switch (targetMetric)
    {
    case TARGET_METRIC_TEMPERATURE:
        return sensorData.temperature;
    case TARGET_METRIC_HUMIDITY:
        return sensorData.humidity;
    case TARGET_METRIC_MOISTURE:
        return sensorData.moisture;
    default:
        return NAN;
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

void ActuatorController::reportHardwareFault(const String &message) const
{
    Serial.println("[Hardware fault] " + message);
    pendingHardwareFaultMessage = message;
}

bool ActuatorController::consumeHardwareFaultEvent(String &outMessage)
{
    if (pendingHardwareFaultMessage.length() == 0)
    {
        return false;
    }
    outMessage = pendingHardwareFaultMessage;
    pendingHardwareFaultMessage = "";
    return true;
}

void ActuatorController::reportSensorStale(const String &message) const
{
    Serial.println("[Sensor stale] " + message);
    pendingSensorStaleMessage = message; // last one wins if several functions hit NaN the same tick
}

bool ActuatorController::consumeSensorStaleEvent(String &outMessage)
{
    if (pendingSensorStaleMessage.length() == 0)
    {
        return false;
    }
    outMessage = pendingSensorStaleMessage;
    pendingSensorStaleMessage = "";
    return true;
}

// Forces every currently-assigned relay slot off with no sensor reading or rule evaluation - shared by initController()'s EmergencyStop/relayEnabled branch and the public forceAllRelaysOff() below.
void ActuatorController::driveEveryAssignedRelayOff() const
{
    int i2cAddr = deviceConfig.configPin.RELAY_I2C_ADDRESS;
    int i2cSda = deviceConfig.configPin.RELAY_I2C_SDA;
    int i2cScl = deviceConfig.configPin.RELAY_I2C_SCL;
    bool activeLow = deviceConfig.configPin.RELAY_ACTIVE_LOW;

    for (int i = 0; i < deviceConfig.configController.relayCount; i++)
    {
        const RelaySlot &relaySlot = deviceConfig.configController.relays[i];
        if (relaySlot.slot < 1 || relaySlot.slot > MAX_RELAY_SLOTS)
        {
            continue;
        }
        int pin = deviceConfig.configPin.RELAY_PINS[relaySlot.slot - 1];
        if (pin < 0)
        {
            continue;
        }
        relayPinMode(pin, i2cAddr, i2cSda, i2cScl);
        relayWrite(pin, false, i2cAddr, i2cSda, i2cScl, activeLow);
    }

    // See initController()'s matching check for why this is checked right after every relayWrite pass.
    if (relayI2CFaulted())
    {
        reportHardwareFault("I2C write to relay expander failed while forcing relays off - physical relay state may not match commanded state");
    }
}

// Called from main.cpp's loop() whenever a disabled/backoff cycle skips buildSensorData()/initController() entirely, so relays stop freezing in whatever state they were last driven to.
void ActuatorController::forceAllRelaysOff() const
{
    driveEveryAssignedRelayOff();
}

void ActuatorController::initController(SensorData sensorData, time_t epochSeconds)
{
    // Routes through RelayIO so an I2C-expander kit (KC868-A6) works the same as a direct-GPIO one.
    int i2cAddr = deviceConfig.configPin.RELAY_I2C_ADDRESS;
    int i2cSda = deviceConfig.configPin.RELAY_I2C_SDA;
    int i2cScl = deviceConfig.configPin.RELAY_I2C_SCL;
    bool activeLow = deviceConfig.configPin.RELAY_ACTIVE_LOW;

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
        driveEveryAssignedRelayOff();
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
        bool isCurrentlyOn = relayRead(pins[0], i2cAddr, i2cSda, i2cScl, activeLow);

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

        // Roadmap #219: a manual command WINS over both the automated rules' OR result above and the rain veto - an admin explicitly asking for this function to run right now is a deliberate bypass of automation, not another vote in it.
        if (const ManualOverride *manualOverride = findManualOverride(function))
        {
            double reading = readingForTargetMetric(manualOverride->targetMetric, sensorData);
            bool turnsOnAboveThreshold = (function == RelayFunctionType::Ventilation);
            if (manualOverride->mode != MANUAL_OVERRIDE_TARGET || !isnan(reading))
            {
                shouldBeOn = evaluateManualOverride(manualOverride->mode, epochSeconds, manualOverride->expiresAtEpoch,
                                                     isCurrentlyOn, reading, manualOverride->targetThreshold, manualOverride->targetHysteresis,
                                                     turnsOnAboveThreshold);
            }
            // else: Target mode but the metric is missing this cycle (sensor absent/disabled) - fall through, keep whatever the automated rules above already decided, same NAN-safety convention as evaluateCondition.
        }

        for (int i = 0; i < pinCount; i++)
        {
            relayPinMode(pins[i], i2cAddr, i2cSda, i2cScl);
            relayWrite(pins[i], shouldBeOn, i2cAddr, i2cSda, i2cScl, activeLow);
        }
    }

    // Safety limits are applied per PHYSICAL SLOT (not once for the function, unlike the loop above) - each relay slot sharing the WaterPump function keeps its own independent on/off-since history. Reuses configuredType/relayPin declared at the top of this function.
    for (int i = 0; i < MAX_RELAY_SLOTS; i++)
    {
        if ((RelayFunctionType)configuredType[i] == RelayFunctionType::WaterPump && relayPin[i] >= 0)
        {
            applyWaterPumpSafetyLimits(i, relayPin[i], epochSeconds);
        }
    }

    // relayI2CFaulted() reflects the LAST i2cWriteShadow() call, so checking once here (after every relayWrite this tick) catches a bus fault regardless of which function's write hit it.
    if (relayI2CFaulted())
    {
        reportHardwareFault("I2C write to relay expander failed - physical relay state may not match commanded state");
    }
}
