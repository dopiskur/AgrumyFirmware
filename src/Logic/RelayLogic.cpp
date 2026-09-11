#include "RelayLogic.h"
#include "EpochPlausibility.h"
#include <cmath>

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

bool minOnTimeBlocksOff(time_t epochSeconds, time_t onSinceEpoch, int minOnSeconds)
{
    return minOnSeconds > 0 && onSinceEpoch != 0 && (epochSeconds - onSinceEpoch) < (time_t)minOnSeconds;
}

int applyRateLimit(int currentPercent, int targetPercent, int maxPercentPerSecond, int elapsedSeconds)
{
    if (maxPercentPerSecond <= 0 || elapsedSeconds <= 0)
    {
        return targetPercent;
    }
    int maxStep = maxPercentPerSecond * elapsedSeconds;
    int delta = targetPercent - currentPercent;
    if (delta > maxStep)
    {
        return currentPercent + maxStep;
    }
    if (delta < -maxStep)
    {
        return currentPercent - maxStep;
    }
    return targetPercent;
}

bool computeTimeProportioningState(int percent, int periodSeconds, time_t epochSeconds)
{
    int clamped = percent < 0 ? 0 : (percent > 100 ? 100 : percent);
    if (periodSeconds <= 0)
    {
        return clamped > 0;
    }
    unsigned long positionInCycle = (unsigned long)epochSeconds % (unsigned long)periodSeconds;
    unsigned long onSeconds = (unsigned long)periodSeconds * (unsigned long)clamped / 100UL;
    return positionInCycle < onSeconds;
}

int computeServoPulseUs(int percent, int minPulseUs, int maxPulseUs)
{
    int clamped = percent < 0 ? 0 : (percent > 100 ? 100 : percent);
    return minPulseUs + (maxPulseUs - minPulseUs) * clamped / 100;
}

int computeAnalogDacValue(int percent)
{
    int clamped = percent < 0 ? 0 : (percent > 100 ? 100 : percent);
    return clamped * 255 / 100;
}

int computeLatchingPulseAction(int previousPercent, int newPercent)
{
    bool wasOpen = previousPercent > 0;
    bool isOpen = newPercent > 0;
    if (!wasOpen && isOpen)
    {
        return 1;
    }
    if (wasOpen && !isOpen)
    {
        return -1;
    }
    return 0;
}

RelayPairDecision computeRelayPairStep(RelayPairState &state, int currentPositionPercent, int targetPercent,
                                        int travelSeconds, int deadTimeSeconds, int elapsedSeconds)
{
    RelayPairDecision result;
    int clampedTarget = targetPercent < 0 ? 0 : (targetPercent > 100 ? 100 : targetPercent);
    int clampedCurrent = currentPositionPercent < 0 ? 0 : (currentPositionPercent > 100 ? 100 : currentPositionPercent);
    result.newPositionPercent = clampedCurrent;

    if (elapsedSeconds > 0 && state.deadTimeRemainingSeconds > 0)
    {
        state.deadTimeRemainingSeconds -= elapsedSeconds;
        if (state.deadTimeRemainingSeconds < 0)
        {
            state.deadTimeRemainingSeconds = 0;
        }
    }
    if (travelSeconds <= 0 || elapsedSeconds <= 0 || clampedCurrent == clampedTarget || state.deadTimeRemainingSeconds > 0)
    {
        return result; // holding: no travel configured, first tick for this slot, already at target, or still serving a reversal's dead time
    }

    int desiredDirection = clampedTarget > clampedCurrent ? RELAY_PAIR_DIRECTION_OPEN : RELAY_PAIR_DIRECTION_CLOSE;
    if (deadTimeSeconds > 0 && state.lastDirection != RELAY_PAIR_DIRECTION_NONE && desiredDirection != state.lastDirection)
    {
        // Direction reversal - stop this tick and start the mandatory pause instead of flipping the motor straight
        // through. lastDirection is updated to the new intended direction right away (not only once driving
        // resumes) so this branch fires exactly once per reversal, not on every tick still waiting out the pause.
        state.lastDirection = desiredDirection;
        state.deadTimeRemainingSeconds = deadTimeSeconds;
        return result;
    }

    int stepPercent = elapsedSeconds * 100 / travelSeconds;
    if (stepPercent < 1)
    {
        stepPercent = 1; // guarantee forward progress even when a long travelSeconds/short tick would otherwise round to a stall
    }
    if (desiredDirection == RELAY_PAIR_DIRECTION_OPEN)
    {
        result.openRelayOn = true;
        int next = clampedCurrent + stepPercent;
        result.newPositionPercent = next > clampedTarget ? clampedTarget : next;
    }
    else
    {
        result.closeRelayOn = true;
        int next = clampedCurrent - stepPercent;
        result.newPositionPercent = next < clampedTarget ? clampedTarget : next;
    }
    state.lastDirection = desiredDirection;
    return result;
}

int pidCompute(PidState &state, double setpoint, double reading, double kp, double ki, double kd, double sampleIntervalSeconds, int outputMin, int outputMax, bool reverseActing)
{
    double error = reverseActing ? (reading - setpoint) : (setpoint - reading);
    double integralCandidate = state.integral + error * (sampleIntervalSeconds > 0 ? sampleIntervalSeconds : 0.0);
    double iTermCandidate = ki * integralCandidate;
    // Clamp the CANDIDATE integral pre-emptively (anti-windup) - an already-saturated output must not keep
    // accumulating integral it can never use once the reading finally catches up to setpoint.
    if (ki != 0.0 && iTermCandidate > outputMax)
    {
        integralCandidate = outputMax / ki;
    }
    if (ki != 0.0 && iTermCandidate < outputMin)
    {
        integralCandidate = outputMin / ki;
    }
    state.integral = integralCandidate;

    double measurementDelta = (state.hasLastReading && sampleIntervalSeconds > 0) ? (reading - state.lastReading) / sampleIntervalSeconds : 0.0;
    double derivative = reverseActing ? measurementDelta : -measurementDelta;
    state.lastReading = reading;
    state.hasLastReading = true;

    double output = kp * error + ki * state.integral + kd * derivative;
    output = output > outputMax ? outputMax : (output < outputMin ? outputMin : output);
    return (int)(output + (output >= 0 ? 0.5 : -0.5)); // round to nearest int
}

int foldTargetPercent(const int targetPercents[], const bool ruleIsTrue[], int count)
{
    int best = 0;
    for (int i = 0; i < count; i++)
    {
        if (ruleIsTrue[i] && targetPercents[i] > best)
        {
            best = targetPercents[i];
        }
    }
    return best;
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
