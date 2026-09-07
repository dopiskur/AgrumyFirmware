#include "RelayLogic.h"
#include <cmath>

// 2023-11-14 UTC, safely before any real deployment - mirrors ActuatorController.cpp's MIN_PLAUSIBLE_EPOCH, duplicated (not #included) to keep this file Arduino-independent for native tests.
static const time_t MIN_PLAUSIBLE_EPOCH = 1700000000;

bool computeIntervalState(int interval, int intervalLength, time_t epochSeconds)
{
    if (interval <= 0)
    {
        return false;
    }

    unsigned long positionInCycle = (unsigned long)epochSeconds % (unsigned long)interval;
    return positionInCycle < (unsigned long)intervalLength;
}

bool computeScheduleState(int daysOfWeekMask, int startSeconds, int durationSeconds, int localWeekday, int localSecondsOfDay)
{
    bool todayIsScheduled = (daysOfWeekMask & (1 << localWeekday)) != 0;
    return todayIsScheduled &&
           localSecondsOfDay >= startSeconds &&
           localSecondsOfDay < (startSeconds + durationSeconds);
}

bool computeAnyScheduleState(const ScheduleWindow slots[], int count, int localWeekday, int localSecondsOfDay)
{
    for (int i = 0; i < count; i++)
    {
        if (computeScheduleState(slots[i].daysOfWeek, slots[i].start, slots[i].duration, localWeekday, localSecondsOfDay))
        {
            return true;
        }
    }
    return false;
}

bool computeThresholdState(bool currentlyOn, double reading, double threshold, double hysteresis, bool turnsOnAboveThreshold)
{
    bool shouldTurnOn = turnsOnAboveThreshold ? (reading > threshold) : (reading < threshold);
    bool shouldTurnOff = turnsOnAboveThreshold ? (reading <= threshold - hysteresis) : (reading >= threshold + hysteresis);

    if (!currentlyOn && shouldTurnOn)
    {
        return true;
    }
    if (currentlyOn && shouldTurnOff)
    {
        return false;
    }
    return currentlyOn; // dead zone - neither condition met, state latches
}

bool runTimeCeilingHit(time_t epochSeconds, time_t onSinceEpoch, int maxRunSeconds)
{
    return maxRunSeconds > 0 && onSinceEpoch != 0 && (epochSeconds - onSinceEpoch) >= (time_t)maxRunSeconds;
}

bool cooldownActive(time_t epochSeconds, time_t offSinceEpoch, int cooldownSeconds)
{
    return cooldownSeconds > 0 && offSinceEpoch != 0 && (epochSeconds - offSinceEpoch) < (time_t)cooldownSeconds;
}

bool evaluateManualOverride(int mode, time_t epochSeconds, time_t expiresAtEpoch, bool isCurrentlyOn, double reading, double threshold, double hysteresis, bool turnsOnAboveThreshold)
{
    if (epochSeconds < MIN_PLAUSIBLE_EPOCH)
    {
        return false; // real time not known yet - expiresAtEpoch could never be reached, so the override would otherwise never end
    }
    if (epochSeconds >= expiresAtEpoch)
    {
        return false; // past the hard safety cap - override no longer applies this tick
    }
    switch (mode)
    {
    case 1: // MANUAL_OVERRIDE_DURATION, see DeviceModel.h
        return true;
    case 2: // MANUAL_OVERRIDE_TARGET
        return computeThresholdState(isCurrentlyOn, reading, threshold, hysteresis, turnsOnAboveThreshold);
    default:
        return false; // unrecognized mode - ConfigParser already skips these at parse time, belt and suspenders
    }
}

bool waterPumpBlockedByLowTank(double waterLevel, int rawEmpty, int rawFull, double minLevelPercent)
{
    if (minLevelPercent <= 0 || rawEmpty == rawFull)
    {
        return false; // uncalibrated - Water Valve case, no tank sensor to protect
    }
    if (std::isnan(waterLevel))
    {
        return true; // calibrated but no reading this cycle - can't verify the tank isn't dry, fail closed
    }
    double fraction = (waterLevel - rawEmpty) / (double)(rawFull - rawEmpty);
    fraction = fraction < 0.0 ? 0.0 : (fraction > 1.0 ? 1.0 : fraction);
    return (fraction * 100.0) < minLevelPercent;
}

bool foldConditions(const bool results[], const int ops[], int count)
{
    if (count <= 0)
    {
        return false;
    }
    bool result = results[0];
    for (int i = 1; i < count; i++)
    {
        result = (ops[i] == 1) ? (result && results[i]) : (result || results[i]);
    }
    return result;
}
