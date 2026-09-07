#include <unity.h>
#include "../../src/Logic/RelayLogic.h"

void setUp(void) {}
void tearDown(void) {}

void test_ShouldBeOff_AlwaysZero(void)
{
    TEST_ASSERT_EQUAL_INT(0, computePwmDutyPercent(false, 100));
    TEST_ASSERT_EQUAL_INT(0, computePwmDutyPercent(false, 50));
    TEST_ASSERT_EQUAL_INT(0, computePwmDutyPercent(false, 0));
}

void test_ShouldBeOn_ReturnsConfiguredIntensity(void)
{
    TEST_ASSERT_EQUAL_INT(100, computePwmDutyPercent(true, 100));
    TEST_ASSERT_EQUAL_INT(42, computePwmDutyPercent(true, 42));
    TEST_ASSERT_EQUAL_INT(0, computePwmDutyPercent(true, 0));
}

void test_ShouldBeOn_IntensityAboveHundred_Clamped(void)
{
    TEST_ASSERT_EQUAL_INT(100, computePwmDutyPercent(true, 150));
}

void test_ShouldBeOn_IntensityNegative_ClampedToZero(void)
{
    TEST_ASSERT_EQUAL_INT(0, computePwmDutyPercent(true, -10));
}

int main(int argc, char **argv)
{
    UNITY_BEGIN();
    RUN_TEST(test_ShouldBeOff_AlwaysZero);
    RUN_TEST(test_ShouldBeOn_ReturnsConfiguredIntensity);
    RUN_TEST(test_ShouldBeOn_IntensityAboveHundred_Clamped);
    RUN_TEST(test_ShouldBeOn_IntensityNegative_ClampedToZero);
    return UNITY_END();
}
