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
// One physical slot's outputKind dispatch, after this function's rate-limit-agnostic decisions
// (rain veto, manual override, the 0/100 reconciliation) already finalized rawTargetPercent. elapsedSeconds is
// derived from this SLOT's own lastDispatchEpoch, not shared across slots, so a slot assigned for the first time
// this tick sees elapsedSeconds 0 (no rate-limit/travel step at all - the caller of applyRateLimit/
// computeRelayPairStep already treats elapsedSeconds<=0 as "hold position").
void ActuatorController::dispatchSlot(int slotIndex, const RelaySlot &slot, int rawTargetPercent, time_t epochSeconds) const
{
    int i2cAddr = deviceConfig.configPin.RELAY_I2C_ADDRESS;
    int i2cSda = deviceConfig.configPin.RELAY_I2C_SDA;
    int i2cScl = deviceConfig.configPin.RELAY_I2C_SCL;
    bool activeLow = deviceConfig.configPin.RELAY_ACTIVE_LOW;

    time_t lastEpoch = lastDispatchEpoch[slotIndex];
    int elapsedSeconds = lastEpoch == 0 ? 0 : (int)(epochSeconds - lastEpoch);
    lastDispatchEpoch[slotIndex] = epochSeconds;

    int appliedPercent = rawTargetPercent;

    switch (slot.outputKind)
    {
    case OUTPUT_KIND_RELAY:
    {
        int pin = deviceConfig.configPin.RELAY_PINS[slot.slot - 1];
        if (pin < 0)
        {
            return; // this board has no physical pin at this slot - a misconfigured server assignment, not a real relay
        }
        bool previousOn = lastAppliedPercent[slotIndex] > 0;
        // timeProportioningPeriodSeconds>0 turns a plain on/off relay into a proportional "average power" output (e.g. a resistive heating element with no PWM/SSR input) instead of the raw >0 on/off decision.
        bool wantsOn = slot.timeProportioningPeriodSeconds > 0
            ? computeTimeProportioningState(rawTargetPercent, slot.timeProportioningPeriodSeconds, epochSeconds)
            : rawTargetPercent > 0;
        // Min-on/off protects a compressor/pump from short-cycling - cooldownActive already exists (WaterPump's own dedicated min-off check), minOnTimeBlocksOff is its mirror for the min-on direction.
        bool blockedOn = wantsOn && !previousOn && cooldownActive(epochSeconds, slotOffSinceEpoch[slotIndex], slot.minOffSeconds);
        bool blockedOff = !wantsOn && previousOn && minOnTimeBlocksOff(epochSeconds, slotOnSinceEpoch[slotIndex], slot.minOnSeconds);
        bool finalOn = blockedOn ? false : (blockedOff ? true : wantsOn);
        // Skip the I2C round trip once the shadow already matches - only the very first dispatch (lastEpoch==0) or a genuine state change writes the bus.
        if (lastEpoch == 0 || finalOn != previousOn)
        {
            relayPinMode(pin, i2cAddr, i2cSda, i2cScl);
            relayWrite(pin, finalOn, i2cAddr, i2cSda, i2cScl, activeLow);
        }
        appliedPercent = finalOn ? 100 : 0;
        break;
    }
    case OUTPUT_KIND_RELAY_PAIR:
    {
        int openPin = deviceConfig.configPin.RELAY_PINS[slot.slot - 1];
        int closePin = (slot.pairSlot >= 1 && slot.pairSlot <= MAX_RELAY_SLOTS) ? deviceConfig.configPin.RELAY_PINS[slot.pairSlot - 1] : -1;
        if (openPin < 0 || closePin < 0)
        {
            return;
        }
        RelayPairDecision decision = computeRelayPairStep(relayPairStates[slotIndex], relayPairPositionPercent[slotIndex],
                                                           rawTargetPercent, slot.travelSeconds, slot.deadTimeSeconds, elapsedSeconds);
        relayPairPositionPercent[slotIndex] = decision.newPositionPercent;
        relayPinMode(openPin, i2cAddr, i2cSda, i2cScl);
        relayPinMode(closePin, i2cAddr, i2cSda, i2cScl);
        relayWrite(openPin, decision.openRelayOn, i2cAddr, i2cSda, i2cScl, activeLow);
        relayWrite(closePin, decision.closeRelayOn, i2cAddr, i2cSda, i2cScl, activeLow);
        appliedPercent = decision.newPositionPercent;
        break;
    }
    case OUTPUT_KIND_PWM:
    {
        int pin = deviceConfig.configPin.PWM_PINS[slot.slot - 1];
        if (pin < 0)
        {
            return;
        }
        int rateLimited = applyRateLimit(lastAppliedPercent[slotIndex], rawTargetPercent, slot.rateLimitPercentPerSecond, elapsedSeconds);
        pwmPinMode(pin, (uint32_t)(slot.pwmFrequencyHz > 0 ? slot.pwmFrequencyHz : 1000));
        pwmWrite(pin, rateLimited);
        appliedPercent = rateLimited;
        break;
    }
    case OUTPUT_KIND_ANALOG_0_10V:
    {
        int pin = deviceConfig.configPin.ANALOG_PINS[slot.slot - 1];
        if (pin < 0)
        {
            return;
        }
        int rateLimited = applyRateLimit(lastAppliedPercent[slotIndex], rawTargetPercent, slot.rateLimitPercentPerSecond, elapsedSeconds);
        analogPinMode(pin);
        analogWrite8Bit(pin, computeAnalogDacValue(rateLimited));
        appliedPercent = rateLimited;
        break;
    }
    case OUTPUT_KIND_SERVO:
    {
        int pin = deviceConfig.configPin.SERVO_PINS[slot.slot - 1];
        if (pin < 0)
        {
            return;
        }
        int rateLimited = applyRateLimit(lastAppliedPercent[slotIndex], rawTargetPercent, slot.rateLimitPercentPerSecond, elapsedSeconds);
        servoPinMode(pin);
        servoWrite(pin, computeServoPulseUs(rateLimited, slot.servoMinPulseUs, slot.servoMaxPulseUs));
        appliedPercent = rateLimited;
        break;
    }
    case OUTPUT_KIND_LATCHING_PULSE:
    {
        int openPin = deviceConfig.configPin.RELAY_PINS[slot.slot - 1];
        int closePin = (slot.pairSlot >= 1 && slot.pairSlot <= MAX_RELAY_SLOTS) ? deviceConfig.configPin.RELAY_PINS[slot.pairSlot - 1] : -1;
        if (openPin < 0 || closePin < 0)
        {
            return;
        }
        int action = computeLatchingPulseAction(lastAppliedPercent[slotIndex], rawTargetPercent);
        if (action != 0)
        {
            int pulsePin = action > 0 ? openPin : closePin;
            relayPinMode(pulsePin, i2cAddr, i2cSda, i2cScl);
            relayWrite(pulsePin, true, i2cAddr, i2cSda, i2cScl, activeLow);
            // Brief H-bridge pulse, zero standing current after - a blocking wait is safe here, this task's own 2s tick (RELAY_TASK_TICK_MS) has ample budget and nothing else shares this FreeRTOS task.
            delay(slot.latchingPulseMs > 0 ? slot.latchingPulseMs : 250);
            relayWrite(pulsePin, false, i2cAddr, i2cSda, i2cScl, activeLow);
        }
        appliedPercent = rawTargetPercent;
        break;
    }
    default:
        return;
    }

    bool nowOn = appliedPercent > 0;
    bool wasOn = lastAppliedPercent[slotIndex] > 0;
    if (nowOn && !wasOn)
    {
        slotOnSinceEpoch[slotIndex] = epochSeconds;
        slotOffSinceEpoch[slotIndex] = 0;
    }
    if (!nowOn && wasOn)
    {
        slotOffSinceEpoch[slotIndex] = epochSeconds;
        slotOnSinceEpoch[slotIndex] = 0;
    }
    lastAppliedPercent[slotIndex] = appliedPercent;
}

// Interval/Schedule are ignored below this point when nested deep in a tree by anything other than these two leaf types themselves - a boundary can come from ANY node inside ANY rule, regardless of its position in that rule's AND/OR tree, so this walks every node recursively rather than just top-level ones (made nesting possible). 30s floor avoids excessive wake-cycle thrashing right next to a boundary, especially for battery devices.
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
    // Epoch plausibility (before the first successful NTP sync) only matters to Interval/Schedule nodes - evaluateNode itself gates those, a Comparison-only tree is unaffected by clock state, same distinction as before.
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

    lastAppliedPercent[slotIndex] = finalState ? 100 : 0; // keeps isRelayOn()/rate-limit tracking in sync with this override, which bypasses dispatchSlot() entirely

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

// Forces every currently-assigned relay slot (and PWM slot) off with no sensor reading or rule evaluation - shared by initController()'s EmergencyStop/relayEnabled branch and the public forceAllRelaysOff() below.
void ActuatorController::driveEveryAssignedRelayOff() const
{
    int i2cAddr = deviceConfig.configPin.RELAY_I2C_ADDRESS;
    int i2cSda = deviceConfig.configPin.RELAY_I2C_SDA;
    int i2cScl = deviceConfig.configPin.RELAY_I2C_SCL;
    bool activeLow = deviceConfig.configPin.RELAY_ACTIVE_LOW;

    // Per-outputKind emergency-stop table: Relay->off, Pwm/Analog->0, RelayPair->STOP (both
    // relays off, does NOT attempt to close - a mid-travel motor just holds wherever it physically is), Servo->
    // its own configured safe position, LatchingPulse->a close impulse (a defined safe state, not "leave it
    // however it last was" - zero standing current either way once the pulse ends).
    for (int i = 0; i < deviceConfig.configController.relayCount; i++)
    {
        const RelaySlot &relaySlot = deviceConfig.configController.relays[i];
        if (relaySlot.slot < 1 || relaySlot.slot > MAX_RELAY_SLOTS)
        {
            continue;
        }
        switch (relaySlot.outputKind)
        {
        case OUTPUT_KIND_RELAY:
        {
            int pin = deviceConfig.configPin.RELAY_PINS[relaySlot.slot - 1];
            if (pin >= 0)
            {
                relayPinMode(pin, i2cAddr, i2cSda, i2cScl);
                relayWrite(pin, false, i2cAddr, i2cSda, i2cScl, activeLow);
            }
            break;
        }
        case OUTPUT_KIND_RELAY_PAIR:
        {
            int openPin = deviceConfig.configPin.RELAY_PINS[relaySlot.slot - 1];
            int closePin = (relaySlot.pairSlot >= 1 && relaySlot.pairSlot <= MAX_RELAY_SLOTS) ? deviceConfig.configPin.RELAY_PINS[relaySlot.pairSlot - 1] : -1;
            if (openPin >= 0)
            {
                relayPinMode(openPin, i2cAddr, i2cSda, i2cScl);
                relayWrite(openPin, false, i2cAddr, i2cSda, i2cScl, activeLow);
            }
            if (closePin >= 0)
            {
                relayPinMode(closePin, i2cAddr, i2cSda, i2cScl);
                relayWrite(closePin, false, i2cAddr, i2cSda, i2cScl, activeLow);
            }
            break;
        }
        case OUTPUT_KIND_PWM:
        {
            int pin = deviceConfig.configPin.PWM_PINS[relaySlot.slot - 1];
            if (pin >= 0)
            {
                pwmPinMode(pin, (uint32_t)(relaySlot.pwmFrequencyHz > 0 ? relaySlot.pwmFrequencyHz : 1000));
                pwmWrite(pin, 0);
            }
            break;
        }
        case OUTPUT_KIND_ANALOG_0_10V:
        {
            int pin = deviceConfig.configPin.ANALOG_PINS[relaySlot.slot - 1];
            if (pin >= 0)
            {
                analogPinMode(pin);
                analogWrite8Bit(pin, 0);
            }
            break;
        }
        case OUTPUT_KIND_SERVO:
        {
            int pin = deviceConfig.configPin.SERVO_PINS[relaySlot.slot - 1];
            if (pin >= 0)
            {
                servoPinMode(pin);
                servoWrite(pin, computeServoPulseUs(relaySlot.servoSafePositionPercent, relaySlot.servoMinPulseUs, relaySlot.servoMaxPulseUs));
            }
            break;
        }
        case OUTPUT_KIND_LATCHING_PULSE:
        {
            int closePin = (relaySlot.pairSlot >= 1 && relaySlot.pairSlot <= MAX_RELAY_SLOTS) ? deviceConfig.configPin.RELAY_PINS[relaySlot.pairSlot - 1] : -1;
            if (closePin >= 0)
            {
                relayPinMode(closePin, i2cAddr, i2cSda, i2cScl);
                relayWrite(closePin, true, i2cAddr, i2cSda, i2cScl, activeLow);
                delay(relaySlot.latchingPulseMs > 0 ? relaySlot.latchingPulseMs : 250);
                relayWrite(closePin, false, i2cAddr, i2cSda, i2cScl, activeLow);
            }
            break;
        }
        default:
            break;
        }
        // Every dispatched slot is now genuinely off/at-rest - reset its tracking so the next real tick's rate-limit/min-on-off/travel math starts fresh instead of ramping from a stale pre-stop value.
        int slotIndex = relaySlot.slot - 1;
        lastAppliedPercent[slotIndex] = 0;
        relayPairPositionPercent[slotIndex] = 0;
        slotOnSinceEpoch[slotIndex] = 0;
        slotOffSinceEpoch[slotIndex] = 0;
        lastDispatchEpoch[slotIndex] = 0;
    }

    // See initController()'s matching check for why this is checked right after every relayWrite pass.
    if (relayI2CFaulted())
    {
        reportHardwareFault("I2C write to relay expander failed while forcing relays off - physical relay state may not match commanded state");
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
    // Software-tracked (lastAppliedPercent), not a hardware readback - works uniformly across every outputKind
    // (a Pwm/Servo/Analog output has no readback path at all), and applyWaterPumpSafetyLimits keeps this in sync
    // with its own post-safety-limit override, so it is never stale relative to what was actually just dispatched.
    for (int i = 0; i < deviceConfig.configController.relayCount; i++)
    {
        const RelaySlot &relaySlot = deviceConfig.configController.relays[i];
        if (relaySlot.relayFunction == (int)relayFunction && relaySlot.slot >= 1 && relaySlot.slot <= MAX_RELAY_SLOTS)
        {
            return lastAppliedPercent[relaySlot.slot - 1] > 0;
        }
    }
    return false;
}

void ActuatorController::recordControllerState(RelayFunctionType function, bool isOn, int percent) const
{
    int idx = (int)function - 1;
    if (idx < 0 || idx >= MAX_REPORTED_FUNCTIONS || pendingControllerDataChangeCount >= MAX_REPORTED_FUNCTIONS)
    {
        return;
    }
    if (isOn == lastReportedOn[idx] && percent == lastReportedPercent[idx])
    {
        return; // no change since the last report - the wire contract only sends a CHANGE, not a periodic state dump
    }
    lastReportedOn[idx] = isOn;
    lastReportedPercent[idx] = percent;

    ControllerDataChange &entry = pendingControllerDataChanges[pendingControllerDataChangeCount++];
    entry.relayFunction = (int)function;
    entry.isOn = isOn;
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
    int configuredOutputKind[MAX_RELAY_SLOTS] = {0};
    for (int i = 0; i < deviceConfig.configController.relayCount; i++)
    {
        const RelaySlot &relaySlot = deviceConfig.configController.relays[i];
        if (relaySlot.slot >= 1 && relaySlot.slot <= MAX_RELAY_SLOTS)
        {
            configuredType[relaySlot.slot - 1] = relaySlot.relayFunction;
            configuredOutputKind[relaySlot.slot - 1] = relaySlot.outputKind;
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

    // Master safety switches - either one lets the server force every relay off regardless of what the rules below would otherwise decide. emergencyStop is tenant-wide and fail-closed; relayEnabled is this device's own per-controller toggle.
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

    // ONE pass per relay function: every rule targeting it MAX-folds to a target percent - same engine for
    // every function now, not just Screen/Vent, and the single result is written to every pin/PWM output
    // assigned to it.
    const RelayFunctionType functions[6] = {
        RelayFunctionType::Ventilation, RelayFunctionType::Light,
        RelayFunctionType::Heating, RelayFunctionType::WaterPump,
        RelayFunctionType::Screen, RelayFunctionType::Vent,
    };
    // WaterPump's Relay-kind slots need their reconciled demand carried from the function loop below into the
    // dedicated safety-limit pass that follows it - see that pass's own remarks.
    int waterPumpTargetPercentThisTick = 0;
    for (RelayFunctionType function : functions)
    {
        int idx = (int)function - 1;
        bool anySlot = false;
        for (int i = 0; i < deviceConfig.configController.relayCount; i++)
        {
            if (deviceConfig.configController.relays[i].relayFunction == (int)function)
            {
                anySlot = true;
                break;
            }
        }
        if (!anySlot)
        {
            continue; // no output assigned to this function at all
        }

        // Threshold rules (and evaluateManualOverride's Target mode) need the function's CURRENT state for
        // hysteresis math - the software-tracked last-reported value, not a hardware readback (works uniformly
        // across every outputKind, and a function's FIRST tick correctly starts from "was off").
        bool isCurrentlyOn = lastReportedOn[idx];

        int targetPercent;
        const FunctionControlConfig &control = deviceConfig.configController.functionControl[idx];
        if (control.controlMode == CONTROL_MODE_PID)
        {
            // PID bypasses the rule fold entirely for this function - see FunctionControlConfig's own remarks.
            double reading = readingForTargetMetric(control.pidSetpointMetric, sensorData);
            // Reverse-acting: this function wants to DECREASE the reading (e.g. Ventilation used for cooling) - without
            // it, error = setpoint - reading is permanently negative once reading exceeds setpoint and clamps to 0.
            bool pidReverseActing = (function == RelayFunctionType::Ventilation);
            // Real elapsed seconds since this function's last PID tick, not the configured sample interval - falls back to it on the very first compute (lastPidComputeEpoch==0), when no real elapsed time exists yet.
            time_t lastPidEpoch = lastPidComputeEpoch[idx];
            double pidDt = lastPidEpoch == 0 ? (double)control.pidSampleIntervalSeconds : (double)(epochSeconds - lastPidEpoch);
            lastPidComputeEpoch[idx] = epochSeconds;
            targetPercent = isnan(reading) ? 0 // no reading this cycle - fail closed, same convention evaluateCondition already uses for a missing/stale sensor
                                            : pidCompute(pidStates[idx], control.pidSetpoint, reading, control.pidKp, control.pidKi, control.pidKd, pidDt, 0, 100, pidReverseActing);
        }
        else
        {
            // Every function's rules MAX-fold to a target percent (foldTargetPercent) - the highest-demanding
            // currently-true rule wins, same engine for a plain on/off function as a positional one.
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
        }
        bool shouldBeOn = targetPercent > 0;

        // Final AND-NOT gate applied AFTER the fold above - a Weather condition can't be a Rule like Threshold/Interval/Schedule, since folding rules together means it could only ever ADD a reason to turn WaterPump on, never suppress one.
        if (function == RelayFunctionType::WaterPump && deviceConfig.configController.skipWaterPumpForRain)
        {
            shouldBeOn = false;
        }

        // A manual command WINS over both the automated rules' fold above and the rain veto - an admin explicitly asking for this function to run right now is a deliberate bypass of automation, not another vote in it.
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

        // The rain veto and manual override above can flip shouldBeOn independently of the fold/PID that produced
        // targetPercent - reconcile so the two can never contradict each other on the wire (e.g. isOn=true,
        // percent=0 from a manual-on override the automated rules never asked for). A real automated-fold/PID
        // percent (shouldBeOn already agrees with it) passes through untouched.
        if (!shouldBeOn)
        {
            targetPercent = 0;
        }
        else if (targetPercent == 0)
        {
            targetPercent = 100;
        }

        // Dispatch to every physical slot assigned to this function - each slot's own outputKind/rate-limit/
        // min-on-off/etc. is applied independently, not a single shared pins[]/pwmPins[] write.
        for (int i = 0; i < deviceConfig.configController.relayCount; i++)
        {
            const RelaySlot &relaySlot = deviceConfig.configController.relays[i];
            if (relaySlot.relayFunction != (int)function || relaySlot.slot < 1 || relaySlot.slot > MAX_RELAY_SLOTS)
            {
                continue;
            }
            // WaterPump's Relay-kind slots are dispatched by the dedicated safety-limit pass below instead (it
            // needs its own on/off decision AFTER threshold/interval/schedule to still be able to force it off) -
            // every other outputKind/function goes straight through dispatchSlot().
            if (function == RelayFunctionType::WaterPump && relaySlot.outputKind == OUTPUT_KIND_RELAY)
            {
                continue;
            }
            dispatchSlot(relaySlot.slot - 1, relaySlot, targetPercent, epochSeconds);
        }

        // WaterPump is recorded separately below, AFTER its own safety-limit pass - that pass can still force it off this same tick, so recording shouldBeOn here would misreport a pump the safety limit is about to override. No such later override exists for any other function.
        if (function != RelayFunctionType::WaterPump)
        {
            recordControllerState(function, shouldBeOn, targetPercent);
        }
        else
        {
            // A non-Relay WaterPump slot (Pwm/Analog/etc, already dispatched above) still needs its safety-limit
            // math even though this codebase doesn't yet enforce the run-time-ceiling/cooldown/low-tank guard on
            // a non-Relay output physically - see applyWaterPumpSafetyLimits' own Relay-only scope below. Stash
            // this tick's pre-safety-limit demand for the Relay-kind pass to start from.
            waterPumpTargetPercentThisTick = targetPercent;
        }
    }

    // Safety limits are applied per PHYSICAL SLOT (not once for the function, unlike the loop above) - each relay slot sharing the WaterPump function keeps its own independent on/off-since history. Reuses configuredType/relayPin declared at the top of this function. Relay-kind only - a WaterPump slot assigned a different outputKind (Pwm-driven VFD pump, say) was already dispatched by the loop above with no run-time-ceiling/cooldown/low-tank guard applied; generalizing that guard to every outputKind is still open work.
    for (int i = 0; i < MAX_RELAY_SLOTS; i++)
    {
        if ((RelayFunctionType)configuredType[i] == RelayFunctionType::WaterPump && configuredOutputKind[i] == OUTPUT_KIND_RELAY && relayPin[i] >= 0)
        {
            // desiredState comes from a real pin read inside applyWaterPumpSafetyLimits, so this slot's Relay
            // write must happen first - mirror the same reconciled targetPercent every other function's slots
            // already got, before the safety-limit pass reads it back.
            relayPinMode(relayPin[i], i2cAddr, i2cSda, i2cScl);
            relayWrite(relayPin[i], waterPumpTargetPercentThisTick > 0, i2cAddr, i2cSda, i2cScl, activeLow);
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
