#include <unity.h>
#include <cmath>
#include "../../src/Logic/RelayLogic.h"

void setUp(void) {}
void tearDown(void) {}

void test_Ceiling_NotTracked_NeverHits(void)
{
    // onSinceEpoch == 0 means "not currently on" - no stretch to measure.
    TEST_ASSERT_FALSE(runTimeCeilingHit(1000000, 0, 60));
}

void test_Ceiling_JustBelowMax_NotHit(void)
{
    TEST_ASSERT_FALSE(runTimeCeilingHit(1000059, 1000000, 60));
}

void test_Ceiling_AtMax_Hit(void)
{
    // elapsed >= maxRunSeconds is the hit condition - inclusive of the boundary itself.
    TEST_ASSERT_TRUE(runTimeCeilingHit(1000060, 1000000, 60));
}

void test_Ceiling_WellPastMax_Hit(void)
{
    TEST_ASSERT_TRUE(runTimeCeilingHit(1003600, 1000000, 60));
}

void test_Ceiling_MaxRunSecondsZero_Disabled(void)
{
    // 0 (or negative) means "no ceiling configured", not "hits immediately".
    TEST_ASSERT_FALSE(runTimeCeilingHit(1003600, 1000000, 0));
}

void test_Ceiling_MaxRunSecondsNegative_Disabled(void)
{
    TEST_ASSERT_FALSE(runTimeCeilingHit(1003600, 1000000, -1));
}

void test_Cooldown_NeverBeenOff_NotActive(void)
{
    // offSinceEpoch == 0 means "never been off since boot" - a fresh reboot must not be blocked by a cooldown it never actually observed.
    TEST_ASSERT_FALSE(cooldownActive(1000000, 0, 300));
}

void test_Cooldown_JustAfterOff_Active(void)
{
    TEST_ASSERT_TRUE(cooldownActive(1000001, 1000000, 300));
}

void test_Cooldown_JustBeforeElapsed_Active(void)
{
    TEST_ASSERT_TRUE(cooldownActive(1000299, 1000000, 300));
}

void test_Cooldown_AtElapsed_NotActive(void)
{
    // elapsed >= cooldownSeconds clears it - inclusive of the boundary itself.
    TEST_ASSERT_FALSE(cooldownActive(1000300, 1000000, 300));
}

void test_Cooldown_WellPastElapsed_NotActive(void)
{
    TEST_ASSERT_FALSE(cooldownActive(1010000, 1000000, 300));
}

void test_Cooldown_SecondsZero_Disabled(void)
{
    TEST_ASSERT_FALSE(cooldownActive(1000001, 1000000, 0));
}

void test_Cooldown_SecondsNegative_Disabled(void)
{
    TEST_ASSERT_FALSE(cooldownActive(1000001, 1000000, -1));
}

void test_DryRun_Uncalibrated_RawEmptyEqualsRawFull_NeverBlocks(void)
{
    // Water Valve case - no tank sensor, no protection, regardless of minLevelPercent or reading.
    TEST_ASSERT_FALSE(waterPumpBlockedByLowTank(0.0, 100, 100, 50.0));
}

void test_DryRun_MinLevelZeroOrNegative_Disabled(void)
{
    TEST_ASSERT_FALSE(waterPumpBlockedByLowTank(50.0, 0, 100, 0.0));
    TEST_ASSERT_FALSE(waterPumpBlockedByLowTank(50.0, 0, 100, -5.0));
}

void test_DryRun_Calibrated_BelowMinLevel_Blocks(void)
{
    // rawEmpty=0, rawFull=100 -> waterLevel IS the fill percent, keeps the test trivial.
    TEST_ASSERT_TRUE(waterPumpBlockedByLowTank(10.0, 0, 100, 20.0));
}

void test_DryRun_Calibrated_AtOrAboveMinLevel_NotBlocked(void)
{
    TEST_ASSERT_FALSE(waterPumpBlockedByLowTank(20.0, 0, 100, 20.0));
    TEST_ASSERT_FALSE(waterPumpBlockedByLowTank(80.0, 0, 100, 20.0));
}

void test_DryRun_ReadingBeyondCalibration_Clamped(void)
{
    // Below rawEmpty/above rawFull clamps to 0%/100% rather than an out-of-range fraction.
    TEST_ASSERT_TRUE(waterPumpBlockedByLowTank(-50.0, 0, 100, 20.0));
    TEST_ASSERT_FALSE(waterPumpBlockedByLowTank(500.0, 0, 100, 20.0));
}

void test_DryRun_NaNReading_Calibrated_FailsClosed(void)
{
    // A calibrated zone with no reading this cycle can't verify the tank isn't dry - block it, same fail-closed
    // convention as WaterPump's own Threshold NaN handling.
    TEST_ASSERT_TRUE(waterPumpBlockedByLowTank(NAN, 0, 100, 20.0));
}

void test_DryRun_NaNReading_Uncalibrated_StillNotBlocked(void)
{
    TEST_ASSERT_FALSE(waterPumpBlockedByLowTank(NAN, 100, 100, 20.0));
}

int main(int argc, char **argv)
{
    UNITY_BEGIN();
    RUN_TEST(test_Ceiling_NotTracked_NeverHits);
    RUN_TEST(test_Ceiling_JustBelowMax_NotHit);
    RUN_TEST(test_Ceiling_AtMax_Hit);
    RUN_TEST(test_Ceiling_WellPastMax_Hit);
    RUN_TEST(test_Ceiling_MaxRunSecondsZero_Disabled);
    RUN_TEST(test_Ceiling_MaxRunSecondsNegative_Disabled);
    RUN_TEST(test_Cooldown_NeverBeenOff_NotActive);
    RUN_TEST(test_Cooldown_JustAfterOff_Active);
    RUN_TEST(test_Cooldown_JustBeforeElapsed_Active);
    RUN_TEST(test_Cooldown_AtElapsed_NotActive);
    RUN_TEST(test_Cooldown_WellPastElapsed_NotActive);
    RUN_TEST(test_Cooldown_SecondsZero_Disabled);
    RUN_TEST(test_Cooldown_SecondsNegative_Disabled);
    RUN_TEST(test_DryRun_Uncalibrated_RawEmptyEqualsRawFull_NeverBlocks);
    RUN_TEST(test_DryRun_MinLevelZeroOrNegative_Disabled);
    RUN_TEST(test_DryRun_Calibrated_BelowMinLevel_Blocks);
    RUN_TEST(test_DryRun_Calibrated_AtOrAboveMinLevel_NotBlocked);
    RUN_TEST(test_DryRun_ReadingBeyondCalibration_Clamped);
    RUN_TEST(test_DryRun_NaNReading_Calibrated_FailsClosed);
    RUN_TEST(test_DryRun_NaNReading_Uncalibrated_StillNotBlocked);
    return UNITY_END();
}
