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

int computePwmDutyPercent(bool shouldBeOn, int intensityPercent)
{
    if (!shouldBeOn)
    {
        return 0;
    }
    return intensityPercent < 0 ? 0 : (intensityPercent > 100 ? 100 : intensityPercent);
}

namespace
{
    // Magnus formula, same constants api.Utils.DewPointCalculator uses server-side.
    double dewPoint(double temperatureC, double humidityPercent)
    {
        if (std::isnan(temperatureC) || std::isnan(humidityPercent) || humidityPercent <= 0)
        {
            return NAN;
        }
        const double a = 17.62, b = 243.12;
        double gamma = a * temperatureC / (b + temperatureC) + log(humidityPercent / 100.0);
        return b * gamma / (a - gamma);
    }

    // Tetens formula, same constants api.Utils.VpdCalculator uses server-side.
    double vpd(double temperatureC, double humidityPercent)
    {
        if (std::isnan(temperatureC) || std::isnan(humidityPercent))
        {
            return NAN;
        }
        double saturationVaporPressureKPa = 0.6108 * exp(17.27 * temperatureC / (temperatureC + 237.3));
        return saturationVaporPressureKPa * (1 - humidityPercent / 100.0);
    }
}

double readMetric(int metric, const MetricReadings &readings)
{
    switch (metric)
    {
    case METRIC_TEMPERATURE:
        return readings.temperature;
    case METRIC_SOIL_TEMPERATURE:
        return readings.soilTemperature;
    case METRIC_HUMIDITY:
        return readings.humidity;
    case METRIC_VPD:
        return vpd(readings.temperature, readings.humidity);
    case METRIC_DEW_POINT:
        return dewPoint(readings.temperature, readings.humidity);
    case METRIC_DEW_POINT_SPREAD:
    {
        double dp = dewPoint(readings.temperature, readings.humidity);
        return std::isnan(dp) ? NAN : readings.temperature - dp;
    }
    case METRIC_MOISTURE:
        return readings.moisture;
    case METRIC_LIGHT:
        return readings.light;
    case METRIC_CO2:
        return readings.co2;
    case METRIC_TVOC:
        return readings.tvoc;
    case METRIC_BAROMETER:
        return readings.barometer;
    case METRIC_LIQUID_PH:
        return readings.liquidPH;
    case METRIC_RAIN_LEVEL:
        return readings.rainLevel;
    case METRIC_WATER_LEVEL:
        return readings.waterLevel;
    case METRIC_WIND:
        return readings.wind;
    default:
        return NAN;
    }
}

namespace
{
    bool evaluateComparison(const ConditionNode &node, bool wasRuleTrue, const MetricReadings &readings)
    {
        double reading = readMetric(node.metric, readings);
        if (std::isnan(reading))
        {
            return false;
        }
        switch (node.op)
        {
        case COMPARE_GT:
            return computeThresholdState(wasRuleTrue, reading, node.value1, node.hysteresis, /*turnsOnAboveThreshold=*/true);
        case COMPARE_LT:
            return computeThresholdState(wasRuleTrue, reading, node.value1, node.hysteresis, /*turnsOnAboveThreshold=*/false);
        case COMPARE_GTE:
            return reading >= node.value1;
        case COMPARE_LTE:
            return reading <= node.value1;
        case COMPARE_EQ:
            return reading == node.value1;
        case COMPARE_BETWEEN:
        {
            double lo = node.value1 < node.value2 ? node.value1 : node.value2;
            double hi = node.value1 < node.value2 ? node.value2 : node.value1;
            return reading >= lo && reading <= hi;
        }
        default:
            return false;
        }
    }
}

bool evaluateNode(const ConditionNode nodes[], int nodeIndex, bool wasRuleTrue, const MetricReadings &readings,
                   time_t epochSeconds, int localWeekday, int localSecondsOfDay)
{
    const ConditionNode &node = nodes[nodeIndex];
    switch (node.type)
    {
    case NODE_COMPARISON:
        return evaluateComparison(node, wasRuleTrue, readings);
    case NODE_INTERVAL:
        // epoch is 0 (or otherwise implausible) before the first successful NTP sync - evaluating against that computes nonsense (Jan 1 1970) rather than skipping until real time is known.
        return epochSeconds >= MIN_PLAUSIBLE_EPOCH && node.interval > 0 && computeIntervalState(node.interval, node.intervalLength, epochSeconds);
    case NODE_SCHEDULE:
        return epochSeconds >= MIN_PLAUSIBLE_EPOCH && computeScheduleState(node.daysOfWeek, node.start, node.duration, localWeekday, localSecondsOfDay);
    case NODE_GROUP:
    {
        if (node.childCount <= 0)
        {
            return false;
        }
        bool result = evaluateNode(nodes, node.childIndices[0], wasRuleTrue, readings, epochSeconds, localWeekday, localSecondsOfDay);
        for (int i = 1; i < node.childCount; i++)
        {
            bool next = evaluateNode(nodes, node.childIndices[i], wasRuleTrue, readings, epochSeconds, localWeekday, localSecondsOfDay);
            result = (node.groupOperator == LOGICAL_AND) ? (result && next) : (result || next);
        }
        return result;
    }
    default:
        return false;
    }
}
