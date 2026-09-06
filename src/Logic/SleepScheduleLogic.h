#ifndef SleepScheduleLogic_H
#define SleepScheduleLogic_H

// Pure, native-testable "sleep until the next relevant boundary" math - a fixed sleepSeconds can skip a short Schedule window entirely, or overrun it, if sleepSeconds is longer than the window itself. ActuatorController loops its own rules[] and calls these per-rule, same split as RelayLogic.h (pure math here, array iteration stays in the .cpp).
#include <ctime>

// Seconds until the next ON<->OFF transition of one Schedule window (same semantics as
// computeScheduleState), scanning up to 7 days ahead so a window later in the week is found once
// today's occurrence (if any) has already passed. -1 if daysOfWeekMask has no bit set (can never fire).
int secondsUntilScheduleBoundary(int daysOfWeekMask, int startSeconds, int durationSeconds, int nowLocalWeekday, int nowLocalSecondsOfDay);

// Seconds until the next ON<->OFF transition of one grid-aligned Interval condition (same semantics
// as computeIntervalState). -1 if interval <= 0 (can never fire).
int secondsUntilIntervalBoundary(int interval, int intervalLength, time_t epochSeconds);

// Raises a candidate sleep duration to floorSeconds if it's shorter - a boundary landing seconds
// away must not thrash the wake cycle, especially for battery devices.
int clampToSleepFloor(int candidateSeconds, int floorSeconds);

#endif
