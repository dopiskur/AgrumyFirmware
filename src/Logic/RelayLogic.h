#ifndef RelayLogic_H
#define RelayLogic_H

// Deliberately plain C++ (no Arduino.h/digitalWrite/pinMode/Serial) so this header and its .cpp compile identically on a dev laptop's native test env and on the device.
// "enabled" flags are deliberately NOT parameters here for interval/schedule: disabled means "don't touch these pins at all" (leave them under whatever other mode last set them), a different outcome than "should be OFF" - the caller checks *Enabled before ever calling these.
#include <ctime>

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

// Roadmap #212. Strict left-to-right fold of already-evaluated condition results, joined by the
// operator immediately preceding each one (1=AND, 2=OR, matching DeviceModel.h's LogicalOperator -
// duplicated as plain ints here rather than #included, to keep this header Arduino-independent for
// native tests) - "((results[0] op ops[1]) op ops[2])...". ops[0] is never read (nothing precedes
// the first condition). count<=0 returns false (nothing to evaluate).
bool foldConditions(const bool results[], const int ops[], int count);

// Two independent hard ceilings applied to WaterPump AFTER threshold/interval/schedule already decided its state - the safety net, regardless of which mode produced that decision. Losing the RAM-only timestamps on reboot is deliberate: a reboot physically de-energizes every relay, so there is no ON stretch or cooldown left to remember.

// True once a continuous ON stretch (onSinceEpoch, 0 = not currently tracked) has run for at least maxRunSeconds. maxRunSeconds <= 0 disables the ceiling (never hit) rather than treating 0 as "hit immediately".
bool runTimeCeilingHit(time_t epochSeconds, time_t onSinceEpoch, int maxRunSeconds);

// True while less than cooldownSeconds have passed since the pump's last real OFF transition (offSinceEpoch, 0 = never been off since boot). cooldownSeconds <= 0 disables the cooldown (never active).
bool cooldownActive(time_t epochSeconds, time_t offSinceEpoch, int cooldownSeconds);

#endif
