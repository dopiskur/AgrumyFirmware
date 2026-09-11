#ifndef ActuatorController_H
#define ActuatorController_H
#include "Arduino.h"
#include <ArduinoJson.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "../Model/DeviceModel.h"
#include "../Logic/RelayLogic.h"
#include "../Logic/SleepScheduleLogic.h"
#include "RelayIO.h"
#include "PwmIO.h"
#include "ServoIO.h"
#include "AnalogIO.h"

// Forward declarations instead of includes
class DeviceController;
class SensorController;

// Recursive because ActuatorController's own public methods call each other (initController calls
// isRelayOn) - a plain mutex would deadlock the same task on the second take. Guards every public
// ActuatorController method's shared state (relay/PWM decisions, pending-event fields) and the
// deviceConfig fields they read, against the relay task (see beginRelayTask below) running
// concurrently with loopTask - and guards SensorController's relay-facing sensor snapshot the same
// way. Created once by beginRelayTask(); every guarded method takes it as its first line via
// ActuatorStateLock.
extern SemaphoreHandle_t deviceStateMutex;

// RAII guard over deviceStateMutex.
struct ActuatorStateLock
{
    ActuatorStateLock();
    ~ActuatorStateLock();
};

// Must match deviceTypeRelay's DB seed order (1=Ventilation, 2=Light, 3=Heating, 4=Water pump, 5=Screen, 6=Vent) - the Web admin dropdown stores this ID directly into one of ConfigController.relays[].relayFunction. Every function folds through the same TargetPercent/MAX engine - Screen/Vent no longer get a separate fold, isPositionalRelayFunction is gone from initController's decision entirely.
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

// Highest RelayFunctionType value in use - sizes the per-function last-reported-state arrays below.
static const int MAX_REPORTED_FUNCTIONS = 6;

// One entry ActuatorController::consumeControllerDataChanges hands back - mirrors api.Models.ControllerDataPush's wire shape (relayFunction/isOn/percent). percent is always the fold's target percent now, for every function.
struct ControllerDataChange
{
    int relayFunction = 0;
    bool isOn = false;
    int percent = 0;
};

class ActuatorController
{
public:
    void setupController();

    // Creates deviceStateMutex and the persistent relay task, which ticks initController() on its
    // own cadence (RELAY_TASK_TICK_MS) independent of the network/config cycle - so a slow apiConfig()
    // round trip no longer delays relay decisions. Call once from setup(), only for a controller-
    // capable device: a sensor-only node has no relays to evaluate and may deep-sleep, which a
    // persistent task cannot survive (main.cpp's loop() already never deep-sleeps a controller-
    // enabled device for the same reason).
    static void beginRelayTask();
    static TaskHandle_t relayTaskHandle();

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

    // One-shot poll of every function whose driven state (on/off, and for a positional function its target percent) changed since the last call - same polling contract as consumeSafetyLimitEvent. changes must hold at least MAX_REPORTED_FUNCTIONS entries; returns how many were written.
    int consumeControllerDataChanges(ControllerDataChange changes[]) const;

private:
    // Dispatches ONE physical slot (slotIndex, 0..MAX_RELAY_SLOTS-1, indexes every per-slot state
    // array below) according to its own outputKind, given this function's raw fold/PID targetPercent for this
    // tick. elapsedSeconds is time since THIS slot's own last dispatch (rate-limit/RelayPair travel math)-
    // computed once per slot, not shared across slots, so a newly (re)assigned slot's first tick doesn't see a
    // huge/garbage elapsed value.
    void dispatchSlot(int slotIndex, const RelaySlot &slot, int rawTargetPercent, time_t epochSeconds) const;

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

    // Compares (isOn, percent) against this function's own last REPORTED values (not its last DRIVEN values - a tick that decides the same state again is not a change); queues a ControllerDataChange only on a genuine difference. const: called from driveEveryAssignedRelayOff(), which is const - the arrays below are mutable accordingly.
    void recordControllerState(RelayFunctionType function, bool isOn, int percent) const;

    // Indexed by (int)function - 1. Both default to "off"/0 so a function that starts off and never turns on is correctly never reported - matches the wire contract's "sent every time state actually CHANGES" convention, not a full state dump every tick.
    mutable bool lastReportedOn[MAX_REPORTED_FUNCTIONS] = {false, false, false, false, false, false};
    mutable int lastReportedPercent[MAX_REPORTED_FUNCTIONS] = {0, 0, 0, 0, 0, 0};
    mutable ControllerDataChange pendingControllerDataChanges[MAX_REPORTED_FUNCTIONS];
    mutable int pendingControllerDataChangeCount = 0;

    time_t waterPumpOnSinceEpoch[MAX_RELAY_SLOTS] = {0};
    time_t waterPumpOffSinceEpoch[MAX_RELAY_SLOTS] = {0};
    // Last tick's function assignment per physical slot index, so a remap (e.g. WaterPump->Light->WaterPump) can be detected and the stale slot's on/off-since history cleared instead of reused.
    int lastConfiguredType[MAX_RELAY_SLOTS] = {0};

    // outputKind dispatch state, all indexed by physical slot (0..MAX_RELAY_SLOTS-1), cleared on a
    // relayFunction remap same as waterPumpOnSinceEpoch/waterPumpOffSinceEpoch above (see initController's
    // configuredType-vs-lastConfiguredType pass). lastAppliedPercent is the software-tracked "last thing this slot
    // was actually told to do" - min-on/off and rate-limit both measure against it rather than a hardware
    // readback, since a Pwm/Servo/Analog output has no readback path at all.
    // mutable: dispatchSlot() and driveEveryAssignedRelayOff() (both called from const forceAllRelaysOff() as
    // well as non-const initController()) need to update these.
    mutable int lastAppliedPercent[MAX_RELAY_SLOTS] = {0};
    mutable time_t slotOnSinceEpoch[MAX_RELAY_SLOTS] = {0};
    mutable time_t slotOffSinceEpoch[MAX_RELAY_SLOTS] = {0};
    mutable time_t lastDispatchEpoch[MAX_RELAY_SLOTS] = {0};
    // RelayPair only - current tracked open/closed position (0-100), re-derived from 0 at boot (a real reboot
    // physically de-energizes the motor, so there is no position left to remember - same reasoning as every other
    // RAM-only safety/state array in this class).
    mutable int relayPairPositionPercent[MAX_RELAY_SLOTS] = {0};
    // RelayPair only - direction-reversal dead-time tracking, see RelayLogic's RelayPairState/computeRelayPairStep.
    mutable RelayPairState relayPairStates[MAX_RELAY_SLOTS];
    // PID controller state, one per RelayFunctionType (indexed function-1) - only meaningful while that
    // function's FunctionControlConfig.controlMode is CONTROL_MODE_PID.
    PidState pidStates[MAX_REPORTED_FUNCTIONS];
    // Epoch of this function's last PID compute (0 = never) - lets pidCompute use the REAL elapsed dt instead of the configured pidSampleIntervalSeconds.
    time_t lastPidComputeEpoch[MAX_REPORTED_FUNCTIONS] = {0};
    String pendingSafetyEventMessage = "";
    mutable String pendingHardwareFaultMessage = "";
    mutable String pendingSensorStaleMessage = "";
    // 0 = temperature reading currently valid (or never gone stale yet); set to the epoch of the first NaN reading in a stale streak, cleared back to 0 the moment a real reading returns. const: touched from evaluateCondition(), which is const.
    mutable time_t heatingSensorStaleSinceEpoch = 0;
};

// The one ActuatorController instance, defined in main.cpp.
extern ActuatorController controller;

#endif
