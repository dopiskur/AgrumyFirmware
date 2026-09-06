#include "RelayLogic.h"

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
