#include "HttpDateLogic.h"
#include <cstdio>
#include <cstring>

namespace
{
    const char *MONTH_ABBREVIATIONS[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

    int monthIndex(const char *abbrev)
    {
        for (int i = 0; i < 12; i++)
        {
            if (strncmp(abbrev, MONTH_ABBREVIATIONS[i], 3) == 0)
            {
                return i + 1;
            }
        }
        return -1;
    }

    // Howard Hinnant's days-from-civil (proleptic Gregorian, UTC) - avoids mktime/timegm, whose result depends on the process's local timezone/DST rules.
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

long httpDateToEpochSeconds(const char *httpDate)
{
    if (httpDate == nullptr)
    {
        return 0;
    }
    char monthAbbrev[4] = {0};
    int day, year, hour, minute, second;
    if (sscanf(httpDate, "%*3s, %d %3s %d %d:%d:%d", &day, monthAbbrev, &year, &hour, &minute, &second) != 6)
    {
        return 0;
    }
    int month = monthIndex(monthAbbrev);
    if (month < 0 || day < 1 || day > 31 || hour > 23 || minute > 59 || second > 60)
    {
        return 0;
    }
    return daysFromCivil(year, month, day) * 86400L + hour * 3600L + minute * 60L + second;
}
