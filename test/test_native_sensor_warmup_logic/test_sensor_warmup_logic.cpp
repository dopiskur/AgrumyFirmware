#include <unity.h>
#include "../../src/Logic/SensorWarmupLogic.h"
#include "../../src/Model/SensorTypeIds.h"

void setUp(void) {}
void tearDown(void) {}

void test_NothingSelected_NoWait(void)
{
    const int selected[] = { 0, 0, 0 };
    TEST_ASSERT_EQUAL_UINT32(0, sensorWarmupMs(selected, 3));
}

void test_InstantDrivers_NoWait(void)
{
    const int selected[] = { SensorTypeIds::Bme280, SensorTypeIds::Ds18B20, SensorTypeIds::AnalogMoisture, SensorTypeIds::Max17048 };
    TEST_ASSERT_EQUAL_UINT32(0, sensorWarmupMs(selected, 4));
}

void test_Dht22_TwoSeconds(void)
{
    const int selected[] = { SensorTypeIds::Dht22, SensorTypeIds::Dht22 };
    TEST_ASSERT_EQUAL_UINT32(2000, sensorWarmupMs(selected, 2));
}

void test_MixedDrivers_SlowestWins(void)
{
    const int selected[] = { SensorTypeIds::Bh1750, SensorTypeIds::Scd4x, SensorTypeIds::Dht11 };
    TEST_ASSERT_EQUAL_UINT32(5000, sensorWarmupMs(selected, 3));
}

void test_EmptyList_NoWait(void)
{
    TEST_ASSERT_EQUAL_UINT32(0, sensorWarmupMs(nullptr, 0));
}

int main(int argc, char** argv)
{
    UNITY_BEGIN();
    RUN_TEST(test_NothingSelected_NoWait);
    RUN_TEST(test_InstantDrivers_NoWait);
    RUN_TEST(test_Dht22_TwoSeconds);
    RUN_TEST(test_MixedDrivers_SlowestWins);
    RUN_TEST(test_EmptyList_NoWait);
    return UNITY_END();
}
