#include "CommandReplayLogic.h"
#include <cstdio>

namespace
{
    // Mirrors ActuatorController.cpp's MIN_PLAUSIBLE_EPOCH, duplicated (not #included) to keep this file Arduino-independent for native tests.
    const long MIN_PLAUSIBLE_EPOCH = 1700000000;

    // Howard Hinnant's days-from-civil (proleptic Gregorian, UTC) - mirrors HttpDateLogic.cpp's own copy, duplicated for the same "no mktime/timegm timezone dependency" reason.
    long daysFromCivil(int y, int m, int d)
    {
        y -= m <= 2;
        long era = (y >= 0 ? y : y - 399) / 400;
        long yoe = y - era * 400;
        long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
        long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
        return era * 146097 + doe - 719468;
    }
}

long isoUtcToEpochSeconds(const char *iso)
{
    if (iso == nullptr)
    {
        return 0;
    }
    int year, month, day, hour, minute, second;
    // %d for seconds stops at the first non-digit, so an optional ".fffffffZ" tail is silently ignored rather than needing its own format specifier.
    if (sscanf(iso, "%d-%d-%dT%d:%d:%d", &year, &month, &day, &hour, &minute, &second) != 6)
    {
        return 0;
    }
    if (month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || minute > 59 || second > 60)
    {
        return 0;
    }
    return daysFromCivil(year, month, day) * 86400L + hour * 3600L + minute * 60L + second;
}

bool commandIsReplayed(int idDeviceCommand, int lastProcessedCommandId, long expiresAtEpochSeconds, long nowEpochSeconds)
{
    if (idDeviceCommand <= lastProcessedCommandId)
    {
        return true;
    }
    if (nowEpochSeconds < MIN_PLAUSIBLE_EPOCH || expiresAtEpochSeconds <= 0)
    {
        return false;
    }
    return nowEpochSeconds > expiresAtEpochSeconds;
}
