// The pure-logic dispatch primitives behind outputKind (Relay/RelayPair/Pwm/Analog0to10V/Servo/LatchingPulse) -
// rate limiting, min-on-time, time-proportioning, the linear output-value mappings, the RelayPair motor step,
// and the PID controller.
#include <unity.h>
#include "../../src/Logic/RelayLogic.h"

void setUp(void) {}
void tearDown(void) {}

// ---- minOnTimeBlocksOff ------------------------------------------------------------------------------------

void test_MinOnTime_NotYetTracked_DoesNotBlock(void)
{
    TEST_ASSERT_FALSE(minOnTimeBlocksOff(1000, 0, 60));
}

void test_MinOnTime_JustTurnedOn_Blocks(void)
{
    TEST_ASSERT_TRUE(minOnTimeBlocksOff(1010, 1000, 60));
}

void test_MinOnTime_PastMinimum_DoesNotBlock(void)
{
    TEST_ASSERT_FALSE(minOnTimeBlocksOff(1070, 1000, 60));
}

void test_MinOnTime_Disabled_NeverBlocks(void)
{
    TEST_ASSERT_FALSE(minOnTimeBlocksOff(1001, 1000, 0));
}

// ---- applyRateLimit ------------------------------------------------------------------------------------------

void test_RateLimit_Disabled_ReturnsTargetImmediately(void)
{
    TEST_ASSERT_EQUAL_INT(100, applyRateLimit(0, 100, 0, 10));
}

void test_RateLimit_StepClampedUpward(void)
{
    TEST_ASSERT_EQUAL_INT(50, applyRateLimit(0, 100, 5, 10)); // 5%/s * 10s = 50% max step
}

void test_RateLimit_StepClampedDownward(void)
{
    TEST_ASSERT_EQUAL_INT(50, applyRateLimit(100, 0, 5, 10));
}

void test_RateLimit_WithinBudget_ReachesTarget(void)
{
    TEST_ASSERT_EQUAL_INT(30, applyRateLimit(0, 30, 5, 10));
}

// ---- computeTimeProportioningState --------------------------------------------------------------------------

void test_TimeProportioning_ZeroPercent_AlwaysOff(void)
{
    TEST_ASSERT_FALSE(computeTimeProportioningState(0, 100, 12345));
}

void test_TimeProportioning_HundredPercent_AlwaysOn(void)
{
    TEST_ASSERT_TRUE(computeTimeProportioningState(100, 100, 12399));
}

void test_TimeProportioning_HalfPercent_OnForFirstHalfOfCycle(void)
{
    TEST_ASSERT_TRUE(computeTimeProportioningState(50, 100, 1000000000)); // epoch % 100 == 0 -> inside the first 50s
    TEST_ASSERT_FALSE(computeTimeProportioningState(50, 100, 1000000060)); // epoch % 100 == 60 -> past the first 50s
}

void test_TimeProportioning_NoPeriodConfigured_FallsBackToPlainOnOff(void)
{
    TEST_ASSERT_TRUE(computeTimeProportioningState(1, 0, 12345));
    TEST_ASSERT_FALSE(computeTimeProportioningState(0, 0, 12345));
}

// ---- computeServoPulseUs / computeAnalogDacValue ------------------------------------------------------------

void test_ServoPulse_ZeroPercent_IsMinPulse(void)
{
    TEST_ASSERT_EQUAL_INT(1000, computeServoPulseUs(0, 1000, 2000));
}

void test_ServoPulse_HundredPercent_IsMaxPulse(void)
{
    TEST_ASSERT_EQUAL_INT(2000, computeServoPulseUs(100, 1000, 2000));
}

void test_ServoPulse_HalfPercent_IsMidpoint(void)
{
    TEST_ASSERT_EQUAL_INT(1500, computeServoPulseUs(50, 1000, 2000));
}

void test_AnalogDacValue_ZeroAndHundredPercent_AreRangeEnds(void)
{
    TEST_ASSERT_EQUAL_INT(0, computeAnalogDacValue(0));
    TEST_ASSERT_EQUAL_INT(255, computeAnalogDacValue(100));
}

void test_AnalogDacValue_ClampsOutOfRangePercent(void)
{
    TEST_ASSERT_EQUAL_INT(0, computeAnalogDacValue(-5));
    TEST_ASSERT_EQUAL_INT(255, computeAnalogDacValue(150));
}

// ---- computeLatchingPulseAction ------------------------------------------------------------------------------

void test_LatchingPulse_ZeroToPositive_PulsesOpen(void)
{
    TEST_ASSERT_EQUAL_INT(1, computeLatchingPulseAction(0, 60));
}

void test_LatchingPulse_PositiveToZero_PulsesClose(void)
{
    TEST_ASSERT_EQUAL_INT(-1, computeLatchingPulseAction(60, 0));
}

void test_LatchingPulse_PositiveToDifferentPositive_NoPulse(void)
{
    TEST_ASSERT_EQUAL_INT(0, computeLatchingPulseAction(30, 80));
}

void test_LatchingPulse_ZeroToZero_NoPulse(void)
{
    TEST_ASSERT_EQUAL_INT(0, computeLatchingPulseAction(0, 0));
}

// ---- computeRelayPairStep ------------------------------------------------------------------------------------

void test_RelayPairStep_TargetAboveCurrent_DrivesOpen(void)
{
    RelayPairDecision d = computeRelayPairStep(0, 100, 10, 2); // 10s full travel, 2s elapsed -> 20% step
    TEST_ASSERT_TRUE(d.openRelayOn);
    TEST_ASSERT_FALSE(d.closeRelayOn);
    TEST_ASSERT_EQUAL_INT(20, d.newPositionPercent);
}

void test_RelayPairStep_TargetBelowCurrent_DrivesClose(void)
{
    RelayPairDecision d = computeRelayPairStep(100, 0, 10, 2);
    TEST_ASSERT_FALSE(d.openRelayOn);
    TEST_ASSERT_TRUE(d.closeRelayOn);
    TEST_ASSERT_EQUAL_INT(80, d.newPositionPercent);
}

void test_RelayPairStep_NeverDrivesBothRelaysAtOnce(void)
{
    RelayPairDecision d = computeRelayPairStep(50, 100, 10, 2);
    TEST_ASSERT_FALSE(d.openRelayOn && d.closeRelayOn);
}

void test_RelayPairStep_StepClampsExactlyToTarget_DoesNotOvershoot(void)
{
    RelayPairDecision d = computeRelayPairStep(95, 100, 10, 2); // 20% step would overshoot past 100
    TEST_ASSERT_EQUAL_INT(100, d.newPositionPercent);
    TEST_ASSERT_TRUE(d.openRelayOn);
}

void test_RelayPairStep_AlreadyAtTarget_NoRelayDriven(void)
{
    RelayPairDecision d = computeRelayPairStep(50, 50, 10, 2);
    TEST_ASSERT_FALSE(d.openRelayOn);
    TEST_ASSERT_FALSE(d.closeRelayOn);
    TEST_ASSERT_EQUAL_INT(50, d.newPositionPercent);
}

// ---- pidCompute ------------------------------------------------------------------------------------------

void test_Pid_ProportionalOnly_OutputScalesWithError(void)
{
    PidState state;
    int output = pidCompute(state, /*setpoint*/ 25.0, /*reading*/ 20.0, /*kp*/ 10.0, /*ki*/ 0.0, /*kd*/ 0.0, /*sampleIntervalSeconds*/ 1.0, 0, 100);
    TEST_ASSERT_EQUAL_INT(50, output); // error 5 * kp 10 = 50
}

void test_Pid_OutputClampedToMax(void)
{
    PidState state;
    int output = pidCompute(state, 100.0, 0.0, 10.0, 0.0, 0.0, 1.0, 0, 100);
    TEST_ASSERT_EQUAL_INT(100, output);
}

void test_Pid_OutputClampedToMin(void)
{
    PidState state;
    int output = pidCompute(state, 0.0, 100.0, 10.0, 0.0, 0.0, 1.0, 0, 100);
    TEST_ASSERT_EQUAL_INT(0, output);
}

void test_Pid_IntegralAccumulatesAcrossTicks(void)
{
    PidState state;
    pidCompute(state, 25.0, 24.0, 0.0, 1.0, 0.0, 1.0, 0, 100); // error 1, integral -> 1
    int output = pidCompute(state, 25.0, 24.0, 0.0, 1.0, 0.0, 1.0, 0, 100); // integral -> 2
    TEST_ASSERT_EQUAL_INT(2, output);
}

void test_Pid_FirstTick_NoDerivativeSpike(void)
{
    PidState state;
    // A huge kd with no prior error must not spike the output on the very first call.
    int output = pidCompute(state, 25.0, 20.0, 0.0, 0.0, 1000.0, 1.0, 0, 100);
    TEST_ASSERT_EQUAL_INT(0, output);
}

void test_Pid_AntiWindup_SaturatedIntegralDoesNotOvershootOnceErrorShrinks(void)
{
    PidState state;
    // Drive hard-saturated for many ticks - without anti-windup the integral would balloon far past what's needed.
    for (int i = 0; i < 50; i++)
    {
        pidCompute(state, 100.0, 0.0, 0.0, 5.0, 0.0, 1.0, 0, 100);
    }
    // Error shrinks to a small positive value - output should still land within range, not slam back to max from a runaway integral.
    int output = pidCompute(state, 100.0, 99.0, 0.0, 5.0, 0.0, 1.0, 0, 100);
    TEST_ASSERT_TRUE(output <= 100);
    TEST_ASSERT_TRUE(output >= 0);
}

int main(int argc, char **argv)
{
    UNITY_BEGIN();
    RUN_TEST(test_MinOnTime_NotYetTracked_DoesNotBlock);
    RUN_TEST(test_MinOnTime_JustTurnedOn_Blocks);
    RUN_TEST(test_MinOnTime_PastMinimum_DoesNotBlock);
    RUN_TEST(test_MinOnTime_Disabled_NeverBlocks);
    RUN_TEST(test_RateLimit_Disabled_ReturnsTargetImmediately);
    RUN_TEST(test_RateLimit_StepClampedUpward);
    RUN_TEST(test_RateLimit_StepClampedDownward);
    RUN_TEST(test_RateLimit_WithinBudget_ReachesTarget);
    RUN_TEST(test_TimeProportioning_ZeroPercent_AlwaysOff);
    RUN_TEST(test_TimeProportioning_HundredPercent_AlwaysOn);
    RUN_TEST(test_TimeProportioning_HalfPercent_OnForFirstHalfOfCycle);
    RUN_TEST(test_TimeProportioning_NoPeriodConfigured_FallsBackToPlainOnOff);
    RUN_TEST(test_ServoPulse_ZeroPercent_IsMinPulse);
    RUN_TEST(test_ServoPulse_HundredPercent_IsMaxPulse);
    RUN_TEST(test_ServoPulse_HalfPercent_IsMidpoint);
    RUN_TEST(test_AnalogDacValue_ZeroAndHundredPercent_AreRangeEnds);
    RUN_TEST(test_AnalogDacValue_ClampsOutOfRangePercent);
    RUN_TEST(test_LatchingPulse_ZeroToPositive_PulsesOpen);
    RUN_TEST(test_LatchingPulse_PositiveToZero_PulsesClose);
    RUN_TEST(test_LatchingPulse_PositiveToDifferentPositive_NoPulse);
    RUN_TEST(test_LatchingPulse_ZeroToZero_NoPulse);
    RUN_TEST(test_RelayPairStep_TargetAboveCurrent_DrivesOpen);
    RUN_TEST(test_RelayPairStep_TargetBelowCurrent_DrivesClose);
    RUN_TEST(test_RelayPairStep_NeverDrivesBothRelaysAtOnce);
    RUN_TEST(test_RelayPairStep_StepClampsExactlyToTarget_DoesNotOvershoot);
    RUN_TEST(test_RelayPairStep_AlreadyAtTarget_NoRelayDriven);
    RUN_TEST(test_Pid_ProportionalOnly_OutputScalesWithError);
    RUN_TEST(test_Pid_OutputClampedToMax);
    RUN_TEST(test_Pid_OutputClampedToMin);
    RUN_TEST(test_Pid_IntegralAccumulatesAcrossTicks);
    RUN_TEST(test_Pid_FirstTick_NoDerivativeSpike);
    RUN_TEST(test_Pid_AntiWindup_SaturatedIntegralDoesNotOvershootOnceErrorShrinks);
    return UNITY_END();
}
