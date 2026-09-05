#ifndef ActuatorController_H
#define ActuatorController_H
#include "Arduino.h"
#include <ArduinoJson.h>

#include "../Model/DeviceModel.h"
#include "../Logic/RelayLogic.h"
#include "../Logic/SleepScheduleLogic.h"
#include "RelayIO.h"

// Forward declarations instead of includes
class DeviceController;
class SensorController;

// Must match deviceTypeRelay's DB seed order (1=Ventilation, 2=Light, 3=Heating, 4=Water pump) - the Web admin dropdown stores this ID directly into one of ConfigController.relays[].relayFunction.
enum class RelayFunctionType
{
    None = 0,
    Ventilation = 1,
    Light = 2,
    Heating = 3,
    WaterPump = 4,
};

class ActuatorController
{
public:
    void setupController();

    // epochSeconds: NTP wall-clock time (DeviceController::getEpochSeconds()), needed by the grid-aligned interval formula below.
    void initController(SensorData sensorData, time_t epochSeconds);

    // True (and clears the pending message into outMessage) exactly once per trip - a safety limit forcing the pump off THIS tick, not still off from a previous trip. Caller polls once per sensor cycle.
    bool consumeSafetyLimitEvent(String &outMessage);

    // Minimum of defaultSleepSeconds and every configured Schedule/Interval rule's own next boundary, floor-clamped - so a short window isn't skipped or overrun by a longer default sleep (roadmap #325). Returns defaultSleepSeconds unchanged when no Schedule/Interval rule is configured.
    int computeNextWakeSeconds(time_t epochSeconds, int defaultSleepSeconds) const;

private:
    // Walks ConfigController.relays[] and collects the physical pin of every slot assigned to relayFunction into pins[] (caller-provided, must hold MAX_RELAY_SLOTS). Returns how many were found.
    int collectPinsForFunction(RelayFunctionType relayFunction, int pins[MAX_RELAY_SLOTS]) const;

    // Roadmap #212. Evaluates ONE condition - the per-conditionType dispatch (used to be evaluateRule's whole body, back when a rule was exactly one condition). targetFunction is the owning Rule's, passed separately since Condition itself no longer carries it.
    bool evaluateCondition(const Condition &condition, int targetFunction, SensorData sensorData, time_t epochSeconds,
                            int localWeekday, int localSecondsOfDay, bool isCurrentlyOn) const;

    // Folds a Rule's conditions[] strictly left-to-right by their operatorBefore - "(A op B) op C", never nested/parenthesized (roadmap #212). localWeekday (0=Sunday..6=Saturday) and localSecondsOfDay (0..86399) are computed ONCE per initController() tick and passed through rather than re-derived per rule. isCurrentlyOn is the target function's CURRENT physical pin state, needed only by Threshold's hysteresis math.
    bool evaluateRule(const Rule &rule, SensorData sensorData, time_t epochSeconds,
                       int localWeekday, int localSecondsOfDay, bool isCurrentlyOn) const;

    // The LAST word for a WaterPump-assigned physical relay slot, applied right after this function's rules are OR'd and written for this tick. slotIndex (0..MAX_RELAY_SLOTS-1) is the physical relay index, not the discovery order collectPinsForFunction gives - so each slot's history stays independent even if several relays share the WaterPump function.
    void applyWaterPumpSafetyLimits(int slotIndex, int pin, time_t epochSeconds);
    void reportSafetyLimitTripped(const String &message);

    time_t waterPumpOnSinceEpoch[MAX_RELAY_SLOTS] = {0};
    time_t waterPumpOffSinceEpoch[MAX_RELAY_SLOTS] = {0};
    // Last tick's function assignment per physical slot index, so a remap (e.g. WaterPump->Light->WaterPump) can be detected and the stale slot's on/off-since history cleared instead of reused.
    int lastConfiguredType[MAX_RELAY_SLOTS] = {0};
    String pendingSafetyEventMessage = "";
};

// The one ActuatorController instance, defined in main.cpp.
extern ActuatorController controller;

#endif
