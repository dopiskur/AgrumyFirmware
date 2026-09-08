#ifndef ActuatorController_H
#define ActuatorController_H
#include "Arduino.h"
#include <ArduinoJson.h>

#include "../Model/DeviceModel.h"
#include "../Logic/RelayLogic.h"
#include "../Logic/SleepScheduleLogic.h"
#include "RelayIO.h"
#include "PwmIO.h"

// Forward declarations instead of includes
class DeviceController;
class SensorController;

// Must match deviceTypeRelay's DB seed order (1=Ventilation, 2=Light, 3=Heating, 4=Water pump, 5=Screen, 6=Vent) - the Web admin dropdown stores this ID directly into one of ConfigController.relays[].relayFunction. Screen/Vent are POSITIONAL (a target percent, not on/off) - see initController's separate fold for them.
enum class RelayFunctionType
{
    None = 0,
    Ventilation = 1,
    Light = 2,
    Heating = 3,
    WaterPump = 4,
    Screen = 5,
    Vent = 6,
};

// True for a positional actuator (target percent 0-100, not a plain on/off decision).
bool isPositionalRelayFunction(RelayFunctionType function);

class ActuatorController
{
public:
    void setupController();

    // epochSeconds: NTP wall-clock time (DeviceController::getEpochSeconds()), needed by the grid-aligned interval formula below.
    void initController(SensorData sensorData, time_t epochSeconds);

    // True (and clears the pending message into outMessage) exactly once per trip - a safety limit forcing the pump off THIS tick, not still off from a previous trip. Caller polls once per sensor cycle.
    bool consumeSafetyLimitEvent(String &outMessage);

    // Same one-shot polling contract as consumeSafetyLimitEvent, for an I2C write failure to the PCF8574 relay expander.
    bool consumeHardwareFaultEvent(String &outMessage);

    // Same one-shot polling contract as consumeSafetyLimitEvent, for a NaN threshold reading (sensor absent/disabled/failed) hit during rule evaluation.
    bool consumeSensorStaleEvent(String &outMessage);

    // Minimum of defaultSleepSeconds and every configured Schedule/Interval rule's own next boundary, floor-clamped - so a short window isn't skipped or overrun by a longer default sleep. Returns defaultSleepSeconds unchanged when no Schedule/Interval rule is configured.
    int computeNextWakeSeconds(time_t epochSeconds, int defaultSleepSeconds) const;

    // Forces every assigned relay off without reading sensors or evaluating rules - for a cycle where initController() itself is being skipped entirely (disabled device, server backoff wait).
    void forceAllRelaysOff() const;

    // Read-only, no relay writes - roadmap #133's local display page. False for an unassigned function, same as it being physically off.
    bool isRelayOn(RelayFunctionType relayFunction) const;

private:
    // Walks ConfigController.relays[] and collects the physical pin of every slot assigned to relayFunction into pins[] (caller-provided, must hold MAX_RELAY_SLOTS). Returns how many were found.
    int collectPinsForFunction(RelayFunctionType relayFunction, int pins[MAX_RELAY_SLOTS]) const;

    // Roadmap #231 - same idea as collectPinsForFunction but for ConfigController.pwmSlots[], returning the resolved PWM_PINS[] pin + intensityPercent pairs (caller-provided arrays, must each hold MAX_PWM_SLOTS). Skips a slot whose PWM_PINS[slot-1] is -1 (unassigned on this board). Returns how many were found.
    int collectPwmSlotsForFunction(RelayFunctionType relayFunction, int pins[MAX_PWM_SLOTS], int intensities[MAX_PWM_SLOTS]) const;

    // Shared by initController()'s EmergencyStop/relayEnabled branch and forceAllRelaysOff().
    void driveEveryAssignedRelayOff() const;

    // Roadmap #396(4). Evaluates a Rule's whole ConditionNode tree (RelayLogic::evaluateNode) - the
    // ONE exception is Heating's bounded hold-through-NaN-temperature safety net, applied here (not
    // inside the pure evaluateNode) since it needs Serial/event-reporting and heatingSensorStaleSinceEpoch
    // state. localWeekday (0=Sunday..6=Saturday) and localSecondsOfDay (0..86399) are computed ONCE per
    // initController() tick and passed through rather than re-derived per rule. isCurrentlyOn is the
    // target function's CURRENT physical pin state, needed only by a GT/LT ComparisonNode's hysteresis math.
    bool evaluateRule(const Rule &rule, SensorData sensorData, time_t epochSeconds,
                       int localWeekday, int localSecondsOfDay, bool isCurrentlyOn) const;

    // Collects every RAW metric a ComparisonNode might read from sensorData into RelayLogic's plain (Arduino-independent) MetricReadings shape - DERIVED metrics (VPD/DewPoint/DewPointSpread) are computed by RelayLogic::readMetric itself, not here.
    static MetricReadings collectMetricReadings(const SensorData &sensorData);

    // The LAST word for a WaterPump-assigned physical relay slot, applied right after this function's rules are OR'd and written for this tick. slotIndex (0..MAX_RELAY_SLOTS-1) is the physical relay index, not the discovery order collectPinsForFunction gives - so each slot's history stays independent even if several relays share the WaterPump function. waterLevel is this tick's raw reading, checked against the zone's tank calibration/minLevel regardless of which mode (Threshold/Interval/Schedule/Manual) turned the pump on.
    void applyWaterPumpSafetyLimits(int slotIndex, int pin, time_t epochSeconds, double waterLevel);
    void reportSafetyLimitTripped(const String &message);

    // const: called from driveEveryAssignedRelayOff()/forceAllRelaysOff(), both const - pendingHardwareFaultMessage is mutable accordingly.
    void reportHardwareFault(const String &message) const;

    // const: called from evaluateCondition(), which is const - pendingSensorStaleMessage is mutable accordingly.
    void reportSensorStale(const String &message) const;

    // Roadmap #219. nullptr if no manual command targets this function (or it never arrived - the server only sends what's still active).
    const ManualOverride *findManualOverride(RelayFunctionType relayFunction) const;

    // NAN for an unrecognized metric - same "no reading this cycle" convention evaluateCondition already uses, so the caller's existing isnan() guard covers it too.
    double readingForTargetMetric(int targetMetric, const SensorData &sensorData) const;

    time_t waterPumpOnSinceEpoch[MAX_RELAY_SLOTS] = {0};
    time_t waterPumpOffSinceEpoch[MAX_RELAY_SLOTS] = {0};
    // Last tick's function assignment per physical slot index, so a remap (e.g. WaterPump->Light->WaterPump) can be detected and the stale slot's on/off-since history cleared instead of reused.
    int lastConfiguredType[MAX_RELAY_SLOTS] = {0};
    String pendingSafetyEventMessage = "";
    mutable String pendingHardwareFaultMessage = "";
    mutable String pendingSensorStaleMessage = "";
    // 0 = temperature reading currently valid (or never gone stale yet); set to the epoch of the first NaN reading in a stale streak, cleared back to 0 the moment a real reading returns. const: touched from evaluateCondition(), which is const.
    mutable time_t heatingSensorStaleSinceEpoch = 0;
};

// The one ActuatorController instance, defined in main.cpp.
extern ActuatorController controller;

#endif
