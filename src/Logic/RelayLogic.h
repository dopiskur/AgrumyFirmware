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

// Recursively evaluates one ConditionNode - a GroupNode folds its children
// left-to-right by groupOperator ("(A op B) op C", never re-associated), same fold AgrumyFirmware
// used before, just now over an arbitrarily nested tree instead of one flat list. wasRuleTrue is
// the WHOLE rule's last-known folded result, used as every Comparison node's GT/LT dead-zone latch
// input (no per-node state is kept, same approximation the server-side evaluator documents). A NaN
// reading makes a Comparison node evaluate false, same fail-safe default as before - the one
// exception (Heating's bounded hold-through-NaN) is applied by the caller (ActuatorController::
// evaluateRule), not here, since it needs stateful Serial/event-reporting this pure function can't do.
bool evaluateNode(const ConditionNode nodes[], int nodeIndex, bool wasRuleTrue, const MetricReadings &readings,
                   time_t epochSeconds, int localWeekday, int localSecondsOfDay);

// Two independent hard ceilings applied to WaterPump AFTER threshold/interval/schedule already decided its state - the safety net, regardless of which mode produced that decision. Losing the RAM-only timestamps on reboot is deliberate: a reboot physically de-energizes every relay, so there is no ON stretch or cooldown left to remember.

// True once a continuous ON stretch (onSinceEpoch, 0 = not currently tracked) has run for at least maxRunSeconds. maxRunSeconds <= 0 disables the ceiling (never hit) rather than treating 0 as "hit immediately".
bool runTimeCeilingHit(time_t epochSeconds, time_t onSinceEpoch, int maxRunSeconds);

// True while less than cooldownSeconds have passed since the pump's last real OFF transition (offSinceEpoch, 0 = never been off since boot). cooldownSeconds <= 0 disables the cooldown (never active).
bool cooldownActive(time_t epochSeconds, time_t offSinceEpoch, int cooldownSeconds);

// Whether a manual override should force its target relay function ON this tick. Past expiresAtEpoch (the hard per-command safety cap, computed server-side from the zone's own MaxRunSeconds) always returns false regardless of mode - the caller falls back to its normal automated-rule result for that tick. Also false while epochSeconds is implausible (before the first NTP/server-epoch sync - same MIN_PLAUSIBLE_EPOCH gate CONDITION_INTERVAL/CONDITION_SCHEDULE already use in ActuatorController.cpp, duplicated here rather than #included to keep this header Arduino-independent), since expiresAtEpoch could otherwise never be reached and the override would run forever. mode==1 (Duration) is unconditional while inside the window; mode==2 (Target) defers to the SAME dead-zone math as an automated Threshold condition (computeThresholdState) - reading/threshold/hysteresis/turnsOnAboveThreshold are ignored for Duration mode.
bool evaluateManualOverride(int mode, time_t epochSeconds, time_t expiresAtEpoch, bool isCurrentlyOn, double reading, double threshold, double hysteresis, bool turnsOnAboveThreshold);

// PWM output is a proportional decorator on the SAME on/off decision a relay slot already computed for this function, not an independent output: full intensityPercent while on, 0 while off. Clamped to [0,100] since a bad server value must not exceed the physical duty-cycle range.
int computePwmDutyPercent(bool shouldBeOn, int intensityPercent);

// Positional actuators (Screen/Vent) fold several simultaneously-true rules to ONE target percent by
// taking the MAX among them ("how much airflow/shade is needed" naturally wants "at least this much"),
// 0 when none are currently true (closed/rest position) - same "OR across rules" spirit the binary
// functions already use in initController's own loop, just MAX instead of boolean-OR.
int foldTargetPercent(const int targetPercents[], const bool ruleIsTrue[], int count);

// Dry-run protection: true if WaterPump must be forced off regardless of what Threshold/Interval/Schedule/Manual
// decided, because the tank is below minLevelPercent - covers Interval/Schedule/Manual too, which never consult
// waterLevel on their own. minLevelPercent<=0 or rawEmpty==rawFull (uncalibrated - a Water Valve zone with no tank
// sensor to protect) always returns false, same "no protection without a real tank sensor" decision as the
// server's own TankCalculator. A NaN reading on an otherwise-calibrated zone fails closed (blocked), matching
// WaterPump's own Threshold NaN handling - a stale/missing reading must not be read as "tank is fine".
bool waterPumpBlockedByLowTank(double waterLevel, int rawEmpty, int rawFull, double minLevelPercent);

// --- outputKind dispatch layer (unified demand model, every kind driven by the same 0-100 fold result) -----------

// Mirror of cooldownActive (a min-OFF-time guard blocking a same-tick ON decision) for the min-ON-time direction -
// blocks a same-tick OFF decision until minOnSeconds have passed since the last real ON transition, protecting a
// compressor/pump from short-cycling. onSinceEpoch==0 (never tracked) or minOnSeconds<=0 never blocks.
bool minOnTimeBlocksOff(time_t epochSeconds, time_t onSinceEpoch, int minOnSeconds);

// Clamps a target percent's per-tick change to maxPercentPerSecond*elapsedSeconds (ramp/soft-start, protects a
// motor/VFD from a step change) - maxPercentPerSecond<=0 or elapsedSeconds<=0 disables the ramp entirely (returns
// targetPercent unchanged, the previous no-rate-limit behavior).
int applyRateLimit(int currentPercent, int targetPercent, int maxPercentPerSecond, int elapsedSeconds);

// A Relay slot's time-proportioning decorator: ON for the first percent% of every periodSeconds-second cycle,
// grid-aligned off epochSeconds (same style as computeIntervalState) - turns a plain on/off relay into a
// proportional "average power" output (e.g. a resistive heating element with no PWM/SSR input). periodSeconds<=0
// falls back to a plain on/off decision (percent>0) - a slot with no period configured isn't using this decorator.
bool computeTimeProportioningState(int percent, int periodSeconds, time_t epochSeconds);

// Linear-maps a 0-100 target percent to a servo pulse width in microseconds between minPulseUs/maxPulseUs.
int computeServoPulseUs(int percent, int minPulseUs, int maxPulseUs);

// Linear-maps a 0-100 target percent to the ESP32's native 8-bit DAC range (0-255) - dacWrite() takes a raw
// 0-255 value, not a percent.
int computeAnalogDacValue(int percent);

// Whether this tick's target crossed the 0/not-0 boundary since last tick, and which way to pulse - a
// LatchingPulse valve holds position with zero standing current, so it is only ever driven by a brief H-bridge
// pulse ON A TRANSITION, never continuously. +1 = pulse open (0->positive), -1 = pulse close (positive->0), 0 =
// no pulse (including a percent change that stays positive - a latching valve is only ever fully open or fully
// closed, not partially, so that needs no new pulse).
int computeLatchingPulseAction(int previousPercent, int newPercent);

// One tick's RelayPair motor decision (Screen/Vent style positional actuator on two relays, open direction and
// close direction): given the currently tracked position (0-100, re-derived from 0 at boot - see AgrumyDevice's
// ActuatorController) and this tick's target, decide which relay should drive and the resulting position after
// elapsedSeconds of travel at travelSeconds-per-full-traverse. Interlocked by construction - exactly one of
// openRelayOn/closeRelayOn is ever true, never both. A step that would reach or pass the target this tick clamps
// to the target exactly, rather than overshooting.
struct RelayPairDecision
{
    bool openRelayOn = false;
    bool closeRelayOn = false;
    int newPositionPercent = 0;
};

enum RelayPairDirection
{
    RELAY_PAIR_DIRECTION_NONE = 0,
    RELAY_PAIR_DIRECTION_OPEN = 1,
    RELAY_PAIR_DIRECTION_CLOSE = -1,
};

// Persists across ticks (one instance per RelayPair slot). lastDirection is the last direction actually DRIVEN -
// it is never reset back to NONE just because the pair reached its target and stopped, so a reversal is still
// detected even when the new target arrives ticks after motion already stopped.
struct RelayPairState
{
    int lastDirection = RELAY_PAIR_DIRECTION_NONE;
    // >0 while a just-detected reversal's mandatory pause is still running - neither relay drives during this window.
    int deadTimeRemainingSeconds = 0;
};

// Same decision as above, but a direction reversal (open->close or close->open) first stops the pair for
// deadTimeSeconds instead of flipping it straight through - protects the motor/mechanism from a direct load
// reversal. deadTimeSeconds<=0 disables this entirely (immediate reversal, matching the original behavior).
RelayPairDecision computeRelayPairStep(RelayPairState &state, int currentPositionPercent, int targetPercent,
                                        int travelSeconds, int deadTimeSeconds, int elapsedSeconds);

// PID controller state - integral/lastReading persist across ticks (one instance per PID-controlled function/slot).
struct PidState
{
    double integral = 0.0;
    double lastReading = 0.0;
    bool hasLastReading = false;
};

// Standard Kp/Ki/Kd PID, clamped to [outputMin,outputMax] and returned as a 0-100-ish percent (whatever range the
// caller passes). reverseActing false: error = setpoint - reading (output rises as reading falls below setpoint,
// e.g. Heating); true: error = reading - setpoint (output rises as reading climbs above setpoint, e.g. Ventilation
// used for cooling - without this a cooling function's error is permanently negative and clamps to outputMin,
// so it never turns on). Derivative is on the MEASUREMENT, not the error, so a setpoint change alone can never
// spike it (no "derivative kick"). Anti-windup: the CANDIDATE integral is clamped before being folded into
// state.integral, so an already-saturated output can't accumulate an integral term it can never use once the
// reading finally catches up. hasLastReading false (first call) skips the derivative term - no prior reading to
// diff against yet, rather than spiking off an assumed-zero delta.
int pidCompute(PidState &state, double setpoint, double reading, double kp, double ki, double kd, double sampleIntervalSeconds, int outputMin, int outputMax, bool reverseActing = false);

#endif
