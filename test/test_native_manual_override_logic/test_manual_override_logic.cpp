#include <unity.h>
#include "../../src/Logic/RelayLogic.h"

void setUp(void) {}
void tearDown(void) {}

// mode constants mirror DeviceModel.h's ManualOverrideMode (1=Duration, 2=Target) - duplicated here rather than #included, same reasoning as RelayLogic.h's own header comment about LogicalOperator.
const int MODE_DURATION = 1;
const int MODE_TARGET = 2;

// Mirrors RelayLogic.cpp's own MIN_PLAUSIBLE_EPOCH - every epochSeconds/expiresAtEpoch pair below uses
// plausible values at or above this, since anything below it is a separate, dedicated test group further down.
const time_t PLAUSIBLE_EPOCH = 1700000000;

void test_PastExpiry_Duration_NeverForcesOn(void)
{
    // epochSeconds >= expiresAtEpoch is the hard safety cap - inclusive of the boundary itself, same convention as runTimeCeilingHit.
    TEST_ASSERT_FALSE(evaluateManualOverride(MODE_DURATION, PLAUSIBLE_EPOCH + 60, PLAUSIBLE_EPOCH + 60, false, 0, 0, 0, false));
}

void test_PastExpiry_Target_NeverForcesOn(void)
{
    // Even a reading well past the "should turn on" side of threshold must not force on once expired.
    TEST_ASSERT_FALSE(evaluateManualOverride(MODE_TARGET, PLAUSIBLE_EPOCH + 60, PLAUSIBLE_EPOCH + 60, false, -100, 20, 2, false));
}

void test_Duration_WithinWindow_AlwaysForcesOn(void)
{
    // Duration mode is unconditional while inside the window - reading/threshold/hysteresis are irrelevant.
    TEST_ASSERT_TRUE(evaluateManualOverride(MODE_DURATION, PLAUSIBLE_EPOCH, PLAUSIBLE_EPOCH + 60, false, 999, -999, 0, false));
}

void test_Duration_JustBeforeExpiry_StillForcesOn(void)
{
    TEST_ASSERT_TRUE(evaluateManualOverride(MODE_DURATION, PLAUSIBLE_EPOCH + 59, PLAUSIBLE_EPOCH + 60, true, 0, 0, 0, false));
}

void test_Target_BelowThreshold_TurnsOn(void)
{
    // Heating/WaterPump direction: turnsOnAboveThreshold=false, turns on below threshold.
    TEST_ASSERT_TRUE(evaluateManualOverride(MODE_TARGET, PLAUSIBLE_EPOCH, PLAUSIBLE_EPOCH + 1000000, false, 15.0, 20.0, 2.0, false));
}

void test_Target_InDeadZone_LatchesCurrentState(void)
{
    // threshold=20, hysteresis=2 (non-inverted) -> turns on below 20, turns off at/above 22; 21 is the dead zone in between.
    TEST_ASSERT_TRUE(evaluateManualOverride(MODE_TARGET, PLAUSIBLE_EPOCH, PLAUSIBLE_EPOCH + 1000000, true, 21.0, 20.0, 2.0, false));
    TEST_ASSERT_FALSE(evaluateManualOverride(MODE_TARGET, PLAUSIBLE_EPOCH, PLAUSIBLE_EPOCH + 1000000, false, 21.0, 20.0, 2.0, false));
}

void test_Target_AboveThreshold_Ventilation_TurnsOn(void)
{
    // Ventilation direction: turnsOnAboveThreshold=true, turns on above threshold (exhausting until it drops back to target).
    TEST_ASSERT_TRUE(evaluateManualOverride(MODE_TARGET, PLAUSIBLE_EPOCH, PLAUSIBLE_EPOCH + 1000000, false, 25.0, 20.0, 2.0, true));
}

void test_UnrecognizedMode_NeverForcesOn(void)
{
    TEST_ASSERT_FALSE(evaluateManualOverride(0, PLAUSIBLE_EPOCH, PLAUSIBLE_EPOCH + 1000000, false, 15.0, 20.0, 2.0, false));
    TEST_ASSERT_FALSE(evaluateManualOverride(99, PLAUSIBLE_EPOCH, PLAUSIBLE_EPOCH + 1000000, false, 15.0, 20.0, 2.0, false));
}

void test_ImplausibleEpoch_Duration_NeverForcesOn(void)
{
    // epochSeconds=0 (pre-NTP/server-epoch-sync) with a real future expiresAtEpoch - without the MIN_PLAUSIBLE_EPOCH
    // gate, 0 >= expiresAtEpoch is always false, so a Duration override would otherwise run forever.
    TEST_ASSERT_FALSE(evaluateManualOverride(MODE_DURATION, 0, 2000000000, false, 0, 0, 0, false));
}

void test_ImplausibleEpoch_Target_NeverForcesOn(void)
{
    TEST_ASSERT_FALSE(evaluateManualOverride(MODE_TARGET, 0, 2000000000, false, 15.0, 20.0, 2.0, false));
}

void test_EpochJustBelowMinPlausible_Blocked(void)
{
    TEST_ASSERT_FALSE(evaluateManualOverride(MODE_DURATION, PLAUSIBLE_EPOCH - 1, PLAUSIBLE_EPOCH + 60, false, 0, 0, 0, false));
}

void test_EpochExactlyAtMinPlausible_PassesGate(void)
{
    // Boundary is inclusive - PLAUSIBLE_EPOCH itself is already considered real time.
    TEST_ASSERT_TRUE(evaluateManualOverride(MODE_DURATION, PLAUSIBLE_EPOCH, PLAUSIBLE_EPOCH + 60, false, 0, 0, 0, false));
}

int main(int argc, char **argv)
{
    UNITY_BEGIN();
    RUN_TEST(test_PastExpiry_Duration_NeverForcesOn);
    RUN_TEST(test_PastExpiry_Target_NeverForcesOn);
    RUN_TEST(test_Duration_WithinWindow_AlwaysForcesOn);
    RUN_TEST(test_Duration_JustBeforeExpiry_StillForcesOn);
    RUN_TEST(test_Target_BelowThreshold_TurnsOn);
    RUN_TEST(test_Target_InDeadZone_LatchesCurrentState);
    RUN_TEST(test_Target_AboveThreshold_Ventilation_TurnsOn);
    RUN_TEST(test_UnrecognizedMode_NeverForcesOn);
    RUN_TEST(test_ImplausibleEpoch_Duration_NeverForcesOn);
    RUN_TEST(test_ImplausibleEpoch_Target_NeverForcesOn);
    RUN_TEST(test_EpochJustBelowMinPlausible_Blocked);
    RUN_TEST(test_EpochExactlyAtMinPlausible_PassesGate);
    return UNITY_END();
}
