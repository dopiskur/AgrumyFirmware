#ifndef RelayLogic_H
#define RelayLogic_H

// Deliberately plain C++ (no Arduino.h/digitalWrite/pinMode/Serial) so this header and its .cpp compile identically on a dev laptop's native test env and on the device.
// "enabled" flags are deliberately NOT parameters here for interval/schedule: disabled means "don't touch these pins at all" (leave them under whatever other mode last set them), a different outcome than "should be OFF" - the caller checks *Enabled before ever calling these.
#include <ctime>
#include <cmath>
#include "ConditionTree.h"

// Grid-aligned duty cycle: true for the first intervalLength seconds of every `interval`-second cycle, keyed off epochSeconds directly so the state is a pure function of wall-clock time. interval <= 0 returns false rather than dividing by it.
bool computeIntervalState(int interval, int intervalLength, time_t epochSeconds);

// True whenever today's bit is set in daysOfWeekMask (bit0=Sunday..bit6=Saturday) AND localSecondsOfDay falls in [startSeconds, startSeconds+durationSeconds). Caller must already localize weekday/seconds-of-day. A window crossing local midnight is not supported (rejected server-side).
bool computeScheduleState(int daysOfWeekMask, int startSeconds, int durationSeconds, int localWeekday, int localSecondsOfDay);

// One relay function can have several windows a day. Fixed-capacity slot array (see MAX_SCHEDULE_SLOTS_PER_FUNCTION), not a std::vector - this runs on an embedded target.
struct ScheduleWindow
{
    int daysOfWeek = 0;
    int start = 0;
    int duration = 0;
};

const int MAX_SCHEDULE_SLOTS_PER_FUNCTION = 4;

// True if ANY of the first `count` windows in slots[] is currently active (OR'd together). count == 0 always returns false - "no windows configured", same as the disabled case: leave the pins alone rather than actively writing them off.
bool computeAnyScheduleState(const ScheduleWindow slots[], int count, int localWeekday, int localSecondsOfDay);

// Dead-zone latch: turns on once `reading` crosses the "on" side of `threshold`, stays on until it crosses back past threshold +/- hysteresis, and otherwise holds `currentlyOn` (this function has no state of its own). turnsOnAboveThreshold=true is ventilation's inverted case; every other relay function turns on BELOW its threshold.
bool computeThresholdState(bool currentlyOn, double reading, double threshold, double hysteresis, bool turnsOnAboveThreshold);

// Every RAW/DERIVED metric a ComparisonNode might read, already resolved for THIS tick - NAN means
// "no reading" (sensor absent/disabled/failed), same convention SensorData itself uses. A plain
// struct (not SensorData, which pulls in Arduino's String) so this stays natively-testable.
struct MetricReadings
{
    double temperature = NAN;
    double soilTemperature = NAN;
    double humidity = NAN;
    double moisture = NAN;
    double light = NAN;
    double co2 = NAN;
    double tvoc = NAN;
    double barometer = NAN;
    double liquidPH = NAN;
    double rainLevel = NAN;
    double waterLevel = NAN;
    double wind = NAN;
};

// DERIVED metrics (VPD/DewPoint/DewPointSpread) computed on-the-fly from temperature+humidity - never
// stored, same "no new columns" rule the server side follows (api.Utils.VpdCalculator/
// DewPointCalculator). NAN in, NAN out - a missing ingredient must not silently produce a fake derived
// value. Returns NAN for an unrecognized metric too.
double readMetric(int metric, const MetricReadings &readings);

// Roadmap #396(4). Recursively evaluates one ConditionNode - a GroupNode folds its children
// left-to-right by groupOperator ("(A op B) op C", never re-associated), same fold AgrumyFirmware
// used before #396 just now over an arbitrarily nested tree instead of one flat list. wasRuleTrue is
// the WHOLE rule's last-known folded result, used as every Comparison node's GT/LT dead-zone latch
// input (no per-node state is kept, same approximation the server-side evaluator documents). A NaN
// reading makes a Comparison node evaluate false, same fail-safe default as before #396 - the one
// exception (Heating's bounded hold-through-NaN) is applied by the caller (ActuatorController::
// evaluateRule), not here, since it needs stateful Serial/event-reporting this pure function can't do.
bool evaluateNode(const ConditionNode nodes[], int nodeIndex, bool wasRuleTrue, const MetricReadings &readings,
                   time_t epochSeconds, int localWeekday, int localSecondsOfDay);

// Two independent hard ceilings applied to WaterPump AFTER threshold/interval/schedule already decided its state - the safety net, regardless of which mode produced that decision. Losing the RAM-only timestamps on reboot is deliberate: a reboot physically de-energizes every relay, so there is no ON stretch or cooldown left to remember.

// True once a continuous ON stretch (onSinceEpoch, 0 = not currently tracked) has run for at least maxRunSeconds. maxRunSeconds <= 0 disables the ceiling (never hit) rather than treating 0 as "hit immediately".
bool runTimeCeilingHit(time_t epochSeconds, time_t onSinceEpoch, int maxRunSeconds);

// True while less than cooldownSeconds have passed since the pump's last real OFF transition (offSinceEpoch, 0 = never been off since boot). cooldownSeconds <= 0 disables the cooldown (never active).
bool cooldownActive(time_t epochSeconds, time_t offSinceEpoch, int cooldownSeconds);

// Roadmap #219: whether a manual override should force its target relay function ON this tick. Past expiresAtEpoch (the hard per-command safety cap, computed server-side from the zone's own MaxRunSeconds) always returns false regardless of mode - the caller falls back to its normal automated-rule result for that tick. Also false while epochSeconds is implausible (before the first NTP/server-epoch sync - same MIN_PLAUSIBLE_EPOCH gate CONDITION_INTERVAL/CONDITION_SCHEDULE already use in ActuatorController.cpp, duplicated here rather than #included to keep this header Arduino-independent), since expiresAtEpoch could otherwise never be reached and the override would run forever. mode==1 (Duration) is unconditional while inside the window; mode==2 (Target) defers to the SAME dead-zone math as an automated Threshold condition (computeThresholdState) - reading/threshold/hysteresis/turnsOnAboveThreshold are ignored for Duration mode.
bool evaluateManualOverride(int mode, time_t epochSeconds, time_t expiresAtEpoch, bool isCurrentlyOn, double reading, double threshold, double hysteresis, bool turnsOnAboveThreshold);

// Dry-run protection: true if WaterPump must be forced off regardless of what Threshold/Interval/Schedule/Manual
// decided, because the tank is below minLevelPercent - covers Interval/Schedule/Manual too, which never consult
// waterLevel on their own. minLevelPercent<=0 or rawEmpty==rawFull (uncalibrated - a Water Valve zone with no tank
// sensor to protect) always returns false, same "no protection without a real tank sensor" decision as the
// server's own TankCalculator. A NaN reading on an otherwise-calibrated zone fails closed (blocked), matching
// WaterPump's own Threshold NaN handling - a stale/missing reading must not be read as "tank is fine".
bool waterPumpBlockedByLowTank(double waterLevel, int rawEmpty, int rawFull, double minLevelPercent);

#endif
