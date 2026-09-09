#include "Arduino.h"
#include <WiFi.h>
#include <esp_task_wdt.h>
#include "FS.h"
#include "WiFiManager.h"

#include "DeviceController.h"
#include "SensorController.h"
#include "ServiceController.h"
#include "ActuatorController.h"
#include "../Logic/EpochPlausibility.h"

// Heating holds its last state across a NaN reading (staying off risks freezing while the sensor is briefly down) but not forever - past this many seconds of continuous staleness the risk flips (a genuinely dead sensor with the heater stuck on is its own hazard), so it forces off instead. Only the Hold policy (HEATING_FAIL_SAFE_HOLD) uses this ceiling at all - see deviceConfig.configController.heatingFailSafePolicy's own remarks.
static const int MAX_HEATING_SENSOR_STALE_SECONDS = 30 * 60;

// Must match api.Shared.Models.HeatingFailSafePolicyType exactly.
enum HeatingFailSafePolicy { HEATING_FAIL_SAFE_HOLD = 0, HEATING_FAIL_SAFE_OFF = 1, HEATING_FAIL_SAFE_SCHEDULE_ONLY = 2 };

SemaphoreHandle_t deviceStateMutex = nullptr;

ActuatorStateLock::ActuatorStateLock() { xSemaphoreTakeRecursive(deviceStateMutex, portMAX_DELAY); }
ActuatorStateLock::~ActuatorStateLock() { xSemaphoreGiveRecursive(deviceStateMutex); }

// Not yet measured on real hardware (no dedicated high-water-mark log for this task yet, unlike NETWORK_TASK_STACK_SIZE) - initController()'s own call depth (rule tree walk, relay/PWM writes, no network/heap-heavy work) is comparable to or shallower than the network task's OTA path, so this starts at half that budget; revisit once a real uxTaskGetStackHighWaterMark reading is available.
static const uint32_t RELAY_TASK_STACK_SIZE = 8192;
// Independent of the network/config cycle on purpose - keeps relay decisions responsive to a changed EmergencyStop/manual-override/rule even while apiConfig() is mid-round-trip, without waiting for sensorData to be re-read this often (relaySnapshotGet() just returns the latest completed reading).
static const uint32_t RELAY_TASK_TICK_MS = 2000;
static TaskHandle_t relayTask = nullptr;

static void relayTaskLoop(void *)
{
    for (;;)
    {
        esp_task_wdt_reset();
        // A single bool read, not wrapped in ActuatorStateLock - see deviceStateMutex's own comment: a torn read of one bool costs at most one stale tick, not a crash, same tolerance main.cpp's loop() already has reading deviceConfig fields directly.
        if (deviceConfig.enabled)
        {
            SensorData snapshot = sensor.relaySnapshotGet();
            controller.initController(snapshot, device.getEpochSeconds());
        }
        else
        {
            controller.forceAllRelaysOff();
        }
        vTaskDelay(pdMS_TO_TICKS(RELAY_TASK_TICK_MS));
    }
}

void ActuatorController::beginRelayTask()
{
    // Mutex always created, even for a sensor-only device with no relay task - every ActuatorController
    // public method takes it unconditionally (forceAllRelaysOff() in particular is called from
    // main.cpp's loop() disabled/backoff branch regardless of device type), so a null handle here would
    // crash the very first such call on a sensor-only node instead of hitting its existing, harmless
    // empty-relayCount no-op.
    deviceStateMutex = xSemaphoreCreateRecursiveMutex();
    if (deviceConfig.deviceControllerEnabled)
    {
        xTaskCreate(relayTaskLoop, "relay", RELAY_TASK_STACK_SIZE, nullptr, 1, &relayTask);
    }
}

TaskHandle_t ActuatorController::relayTaskHandle()
{
    return relayTask;
}

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

// Roadmap #231 - only slots the server actually assigned arrive in deviceConfig.configController.pwmSlots[0..pwmSlotCount); a slot whose board has no real PWM pin at that position (PWM_PINS[slot-1] == -1) is silently skipped, same convention as collectPinsForFunction's relay-pin check above.
int ActuatorController::collectPwmSlotsForFunction(RelayFunctionType relayFunction, int pins[MAX_PWM_SLOTS], int intensities[MAX_PWM_SLOTS]) const
{
    int count = 0;
    for (int i = 0; i < deviceConfig.configController.pwmSlotCount; i++)
    {
        const PwmSlot &pwmSlot = deviceConfig.configController.pwmSlots[i];
        if (pwmSlot.relayFunction == (int)relayFunction && pwmSlot.slot >= 1 && pwmSlot.slot <= MAX_PWM_SLOTS)
        {
            int pin = deviceConfig.configPin.PWM_PINS[pwmSlot.slot - 1];
            if (pin >= 0)
            {
                pins[count] = pin;
                intensities[count] = pwmSlot.intensityPercent;
                count++;
            }
        }
    }
    return count;
}

bool isPositionalRelayFunction(RelayFunctionType function)
{
    return function == RelayFunctionType::Screen || function == RelayFunctionType::Vent;
}

// Interval/Schedule are ignored below this point when nested deep in a tree by anything other than these two leaf types themselves - a boundary can come from ANY node inside ANY rule, regardless of its position in that rule's AND/OR tree, so this walks every node recursively rather than just top-level ones (roadmap #396(4) made nesting possible). 30s floor avoids excessive wake-cycle thrashing right next to a boundary, especially for battery devices.
namespace
{
    void collectWakeBoundary(const ConditionNode nodes[], int nodeIndex, int localWeekday, int localSecondsOfDay, time_t epochSeconds, int &best)
    {
        const ConditionNode &node = nodes[nodeIndex];
        int candidate = -1;
        if (node.type == NODE_SCHEDULE)
        {
            candidate = secondsUntilScheduleBoundary(node.daysOfWeek, node.start, node.duration, localWeekday, localSecondsOfDay);
        }
        else if (node.type == NODE_INTERVAL)
        {
            candidate = secondsUntilIntervalBoundary(node.interval, node.intervalLength, epochSeconds);
        }
        else if (node.type == NODE_GROUP)
        {
            for (int i = 0; i < node.childCount; i++)
            {
                collectWakeBoundary(nodes, node.childIndices[i], localWeekday, localSecondsOfDay, epochSeconds, best);
            }
        }
        if (candidate >= 0 && candidate < best)
        {
            best = candidate;
        }
    }
}

int ActuatorController::computeNextWakeSeconds(time_t epochSeconds, int defaultSleepSeconds) const
{
    ActuatorStateLock lock; // reads deviceConfig.configController.rules[] below
    time_t localEpoch = epochSeconds + deviceConfig.utcOffsetSeconds;
    struct tm *localTm = gmtime(&localEpoch);
    int localWeekday = localTm->tm_wday;
    int localSecondsOfDay = localTm->tm_hour * 3600 + localTm->tm_min * 60 + localTm->tm_sec;

    int best = defaultSleepSeconds;
    for (int i = 0; i < deviceConfig.configController.ruleCount; i++)
    {
        const Rule &rule = deviceConfig.configController.rules[i];
        if (rule.nodeCount > 0)
        {
            collectWakeBoundary(rule.nodes, rule.rootIndex, localWeekday, localSecondsOfDay, epochSeconds, best);
        }
    }
    return clampToSleepFloor(best, MIN_SLEEP_SECONDS);
}

MetricReadings ActuatorController::collectMetricReadings(const SensorData &sensorData)
{
    MetricReadings readings;
    readings.temperature = sensorData.temperature;
    readings.soilTemperature = sensorData.temperatureSoil;
    readings.humidity = sensorData.humidity;
    readings.moisture = sensorData.moisture;
    readings.light = sensorData.light;
    readings.co2 = sensorData.co2;
    readings.tvoc = sensorData.tvoc;
    readings.barometer = sensorData.barometer;
    readings.liquidPH = sensorData.liquidPH;
    readings.rainLevel = sensorData.rainLevel;
    readings.waterLevel = sensorData.waterLevel;
    readings.wind = sensorData.wind;
    return readings;
}

// Heating's own NaN-temperature fail-safe policy is applied HERE, once per rule, before handing off to
// RelayLogic::evaluateNode's pure recursive tree-walk - per-zone configurable (deviceConfig.
// configController.heatingFailSafePolicy) between Hold (last state, then force off past a grace
// window), Off (force off immediately), and ScheduleOnly (skip this branch, fall through to the
// generic per-node NaN handling every other function already gets). Every non-Heating NaN-metric case
// is always handled generically inside evaluateNode itself (fails that one comparison).
bool ActuatorController::evaluateRule(const Rule &rule, SensorData sensorData, time_t epochSeconds,
                                       int localWeekday, int localSecondsOfDay, bool isCurrentlyOn) const
{
    // ScheduleOnly deliberately skips this whole special-cased branch - it falls straight through to
    // the generic evaluateNode() below, same treatment as every other function's NaN metric (a
    // Comparison node fails that one comparison; Schedule/Interval nodes are unaffected).
    if ((RelayFunctionType)rule.targetFunction == RelayFunctionType::Heating && isnan(sensorData.temperature)
        && deviceConfig.configController.heatingFailSafePolicy != HEATING_FAIL_SAFE_SCHEDULE_ONLY)
    {
        if (deviceConfig.configController.heatingFailSafePolicy == HEATING_FAIL_SAFE_OFF)
        {
            reportSensorStale("Heating forced off - fail-safe policy is Off, no hold grace period");
            return false;
        }
        // Hold policy (default, and the fallback for any unrecognized value): hold last state up to the ceiling, then force off.
        if (heatingSensorStaleSinceEpoch == 0)
        {
            heatingSensorStaleSinceEpoch = epochSeconds;
        }
        if (runTimeCeilingHit(epochSeconds, heatingSensorStaleSinceEpoch, MAX_HEATING_SENSOR_STALE_SECONDS))
        {
            reportSensorStale("Heating forced off - temperature sensor has been stale for over " + String(MAX_HEATING_SENSOR_STALE_SECONDS) + "s");
            return false;
        }
        reportSensorStale("Heating rule has no temperature reading this cycle (sensor absent/disabled/failed) - holding last state");
        return isCurrentlyOn;
    }
    if ((RelayFunctionType)rule.targetFunction == RelayFunctionType::Heating)
    {
        heatingSensorStaleSinceEpoch = 0;
    }

    if (rule.nodeCount == 0)
    {
        return false; // ConfigParser rejects an empty tree at parse time - belt and suspenders.
    }
    // Epoch plausibility (before the first successful NTP sync) only matters to Interval/Schedule nodes - evaluateNode itself gates those, a Comparison-only tree is unaffected by clock state, same distinction as before #396.
    MetricReadings readings = collectMetricReadings(sensorData);
    return evaluateNode(rule.nodes, rule.rootIndex, isCurrentlyOn, readings, epochSeconds, localWeekday, localSecondsOfDay);
}

// Order matters: cooldown is evaluated against the OLD offSinceEpoch BEFORE anything below touches onSinceEpoch/offSinceEpoch, so an ON request arriving mid-cooldown can never reset its own clock into a permanent lockout.
void ActuatorController::applyWaterPumpSafetyLimits(int slotIndex, int pin, time_t epochSeconds, double waterLevel)
{
    int i2cAddr = deviceConfig.configPin.RELAY_I2C_ADDRESS;
    int i2cSda = deviceConfig.configPin.RELAY_I2C_SDA;
    int i2cScl = deviceConfig.configPin.RELAY_I2C_SCL;
    bool activeLow = deviceConfig.configPin.RELAY_ACTIVE_LOW;
    bool desiredState = relayRead(pin, i2cAddr, i2cSda, i2cScl, activeLow); // whatever threshold/interval/schedule already wrote this tick
    int maxRunSeconds = deviceConfig.configController.waterPumpMaxRunSeconds;
    int cooldownSeconds = deviceConfig.configController.waterPumpCooldownSeconds;

    bool blockedByCooldown = desiredState && cooldownActive(epochSeconds, waterPumpOffSinceEpoch[slotIndex], cooldownSeconds);
    // Catches Interval/Schedule/Manual too, not just Threshold - those modes never consult waterLevel on their own, so without this check a scheduled run would proceed on an empty tank.
    bool blockedByLowTank = desiredState && !blockedByCooldown
                             && waterPumpBlockedByLowTank(waterLevel, deviceConfig.configController.waterLevelRawEmpty,
                                                           deviceConfig.configController.waterLevelRawFull,
                                                           deviceConfig.configController.waterPumpMinLevel);

    if (desiredState && !blockedByCooldown && !blockedByLowTank && waterPumpOnSinceEpoch[slotIndex] == 0)
    {
        waterPumpOnSinceEpoch[slotIndex] = epochSeconds;
    }

    bool ceilingHit = desiredState && !blockedByCooldown && !blockedByLowTank
                       && runTimeCeilingHit(epochSeconds, waterPumpOnSinceEpoch[slotIndex], maxRunSeconds);

    bool finalState = desiredState && !blockedByCooldown && !blockedByLowTank && !ceilingHit;

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
        else if (blockedByLowTank)
        {
            reportSafetyLimitTripped("WaterPump blocked - tank below minimum level (dry-run protection)");
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
    ActuatorStateLock lock;
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
    ActuatorStateLock lock;
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
    ActuatorStateLock lock;
    if (pendingSensorStaleMessage.length() == 0)
    {
        return false;
    }
    outMessage = pendingSensorStaleMessage;
    pendingSensorStaleMessage = "";
    return true;
}

// Forces every currently-assigned relay slot (and, roadmap #231, PWM slot) off with no sensor reading or rule evaluation - shared by initController()'s EmergencyStop/relayEnabled branch and the public forceAllRelaysOff() below.
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

    // EmergencyStop/relayEnabled=false must silence PWM outputs too, not just relays - a proportional signal left at its last duty cycle would keep driving a fan/light at speed while the admin believes everything is off.
    for (int i = 0; i < deviceConfig.configController.pwmSlotCount; i++)
    {
        const PwmSlot &pwmSlot = deviceConfig.configController.pwmSlots[i];
        if (pwmSlot.slot < 1 || pwmSlot.slot > MAX_PWM_SLOTS)
        {
            continue;
        }
        int pin = deviceConfig.configPin.PWM_PINS[pwmSlot.slot - 1];
        if (pin < 0)
        {
            continue;
        }
        pwmPinMode(pin);
        pwmWrite(pin, 0);
    }

    // The Fleet page must reflect EmergencyStop/relayEnabled=false immediately, not keep showing whatever was last decided before this tick forced everything off.
    const RelayFunctionType allFunctions[MAX_REPORTED_FUNCTIONS] = {
        RelayFunctionType::Ventilation, RelayFunctionType::Light, RelayFunctionType::Heating,
        RelayFunctionType::WaterPump, RelayFunctionType::Screen, RelayFunctionType::Vent,
    };
    for (RelayFunctionType function : allFunctions)
    {
        recordControllerState(function, false, 0);
    }
}

// Called by the relay task whenever deviceConfig.enabled is false, so relays stop freezing in whatever state they were last driven to.
void ActuatorController::forceAllRelaysOff() const
{
    ActuatorStateLock lock;
    driveEveryAssignedRelayOff();
}

bool ActuatorController::isRelayOn(RelayFunctionType relayFunction) const
{
    ActuatorStateLock lock;
    int pins[MAX_RELAY_SLOTS];
    int pinCount = collectPinsForFunction(relayFunction, pins);
    if (pinCount == 0)
    {
        return false;
    }
    int i2cAddr = deviceConfig.configPin.RELAY_I2C_ADDRESS;
    int i2cSda = deviceConfig.configPin.RELAY_I2C_SDA;
    int i2cScl = deviceConfig.configPin.RELAY_I2C_SCL;
    bool activeLow = deviceConfig.configPin.RELAY_ACTIVE_LOW;
    relayPinMode(pins[0], i2cAddr, i2cSda, i2cScl);
    return relayRead(pins[0], i2cAddr, i2cSda, i2cScl, activeLow);
}

void ActuatorController::recordControllerState(RelayFunctionType function, bool isOn, int percent) const
{
    int idx = (int)function - 1;
    if (idx < 0 || idx >= MAX_REPORTED_FUNCTIONS || pendingControllerDataChangeCount >= MAX_REPORTED_FUNCTIONS)
    {
        return;
    }
    bool positional = isPositionalRelayFunction(function);
    if (isOn == lastReportedOn[idx] && (!positional || percent == lastReportedPercent[idx]))
    {
        return; // no change since the last report - the wire contract only sends a CHANGE, not a periodic state dump
    }
    lastReportedOn[idx] = isOn;
    lastReportedPercent[idx] = percent;

    ControllerDataChange &entry = pendingControllerDataChanges[pendingControllerDataChangeCount++];
    entry.relayFunction = (int)function;
    entry.isOn = isOn;
    entry.isPositional = positional;
    entry.percent = percent;
}

int ActuatorController::consumeControllerDataChanges(ControllerDataChange changes[]) const
{
    ActuatorStateLock lock;
    int count = pendingControllerDataChangeCount;
    for (int i = 0; i < count; i++)
    {
        changes[i] = pendingControllerDataChanges[i];
    }
    pendingControllerDataChangeCount = 0;
    return count;
}

void ActuatorController::initController(SensorData sensorData, time_t epochSeconds)
{
    // Whole-tick lock, not just the deviceConfig reads below - isRelayOn() called near the end of this function takes it again (recursive-safe, same task), and the on/off decisions written throughout must be internally consistent with each other, not just individually torn-read-safe.
    ActuatorStateLock lock;
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

    // ONE pass per relay function: every rule targeting it is OR'd together (any rule saying "on" wins) for a
    // binary function, or MAX'd to a target percent for a positional one (Screen/Vent) - either way, the single
    // result is written to every pin/PWM output assigned to it.
    const RelayFunctionType functions[6] = {
        RelayFunctionType::Ventilation, RelayFunctionType::Light,
        RelayFunctionType::Heating, RelayFunctionType::WaterPump,
        RelayFunctionType::Screen, RelayFunctionType::Vent,
    };
    for (RelayFunctionType function : functions)
    {
        int pins[MAX_RELAY_SLOTS];
        int pinCount = collectPinsForFunction(function, pins);
        // Mirrors this function's decision onto any dedicated PWM output assigned to it. Collected
        // BEFORE the empty-check below, since a positional function (Screen/Vent) may be PWM-only with no plain
        // relay slot at all - Inert on every board today, PWM_PINS ships all-UNASSIGNED until a real schematic
        // confirms free GPIOs.
        int pwmPins[MAX_PWM_SLOTS];
        int pwmIntensities[MAX_PWM_SLOTS];
        int pwmCount = collectPwmSlotsForFunction(function, pwmPins, pwmIntensities);
        if (pinCount == 0 && pwmCount == 0)
        {
            continue; // no relay slot and no PWM output assigned to this function
        }

        // Threshold rules need the function's CURRENT physical state for hysteresis math - read once from the
        // first assigned relay pin; every pin sharing one function is kept in sync by the write below, so any one
        // is representative. A PWM-only positional function (no relay pin at all) has no physical state to read
        // back yet - starts every tick from "was off", a known limitation shared with PWM_PINS itself being unwired.
        bool isCurrentlyOn = false;
        if (pinCount > 0)
        {
            relayPinMode(pins[0], i2cAddr, i2cSda, i2cScl);
            isCurrentlyOn = relayRead(pins[0], i2cAddr, i2cSda, i2cScl, activeLow);
        }

        bool shouldBeOn = false;
        int targetPercent = 0;
        if (isPositionalRelayFunction(function))
        {
            // A positional function's rules don't OR to a plain bool, they MAX to a target percent (foldTargetPercent) - the highest-demanding currently-true rule wins.
            int targetPercents[MAX_RULES];
            bool ruleIsTrue[MAX_RULES];
            int matchingRuleCount = 0;
            for (int i = 0; i < deviceConfig.configController.ruleCount; i++)
            {
                const Rule &rule = deviceConfig.configController.rules[i];
                if (rule.targetFunction != (int)function)
                {
                    continue;
                }
                targetPercents[matchingRuleCount] = rule.targetPercent;
                ruleIsTrue[matchingRuleCount] = evaluateRule(rule, sensorData, epochSeconds, localWeekday, localSecondsOfDay, isCurrentlyOn);
                matchingRuleCount++;
            }
            targetPercent = foldTargetPercent(targetPercents, ruleIsTrue, matchingRuleCount);
            shouldBeOn = targetPercent > 0;
        }
        else
        {
            for (int i = 0; i < deviceConfig.configController.ruleCount; i++)
            {
                const Rule &rule = deviceConfig.configController.rules[i];
                if (rule.targetFunction == (int)function &&
                    evaluateRule(rule, sensorData, epochSeconds, localWeekday, localSecondsOfDay, isCurrentlyOn))
                {
                    shouldBeOn = true;
                }
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

        // A positional function drives its PWM output straight to the rule-commanded targetPercent (bypassing the
        // slot's own intensityPercent dial entirely - "open to 30%" means 30%, not 30% of some other admin-set
        // brightness); a binary function keeps mirroring shouldBeOn onto intensityPercent unchanged.
        for (int i = 0; i < pwmCount; i++)
        {
            pwmPinMode(pwmPins[i]);
            int duty = isPositionalRelayFunction(function) ? computePwmDutyPercent(shouldBeOn, targetPercent) : computePwmDutyPercent(shouldBeOn, pwmIntensities[i]);
            pwmWrite(pwmPins[i], duty);
        }

        // WaterPump is recorded separately below, AFTER its own safety-limit pass - that pass can still force it off this same tick, so recording shouldBeOn here would misreport a pump the safety limit is about to override. No such later override exists for any other function.
        if (function != RelayFunctionType::WaterPump)
        {
            recordControllerState(function, shouldBeOn, targetPercent);
        }
    }

    // Safety limits are applied per PHYSICAL SLOT (not once for the function, unlike the loop above) - each relay slot sharing the WaterPump function keeps its own independent on/off-since history. Reuses configuredType/relayPin declared at the top of this function.
    for (int i = 0; i < MAX_RELAY_SLOTS; i++)
    {
        if ((RelayFunctionType)configuredType[i] == RelayFunctionType::WaterPump && relayPin[i] >= 0)
        {
            applyWaterPumpSafetyLimits(i, relayPin[i], epochSeconds, sensorData.waterLevel);
        }
    }
    // Real pin read, not the pre-safety-limit shouldBeOn decided above - the safety-limit pass just run may have forced WaterPump off regardless of what the rules wanted. Reports false (no change from the default) for a WaterPump-less device, same as any other unassigned function.
    recordControllerState(RelayFunctionType::WaterPump, isRelayOn(RelayFunctionType::WaterPump), 0);

    // relayI2CFaulted() reflects the LAST i2cWriteShadow() call, so checking once here (after every relayWrite this tick) catches a bus fault regardless of which function's write hit it.
    if (relayI2CFaulted())
    {
        reportHardwareFault("I2C write to relay expander failed - physical relay state may not match commanded state");
    }
}
