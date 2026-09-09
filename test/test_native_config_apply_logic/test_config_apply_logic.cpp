#include <unity.h>
#include "../../src/Logic/ConfigApplyLogic.h"

void setUp(void) {}
void tearDown(void) {}

void test_RejectedConfig_HookNotInvoked(void)
{
    // configOk=false is DeviceController::loadConfig() returning false, e.g. ConfigParser::parse's error 21 (apiId missing).
    int calls = 0;
    bool applied = applyEpochFallbackIfCommitted(false, 1700000001L, [&](long) { calls++; });
    TEST_ASSERT_FALSE(applied);
    TEST_ASSERT_EQUAL_INT(0, calls);
}

void test_CommittedConfig_HookInvokedExactlyOnceWithEpoch(void)
{
    int calls = 0;
    long seen = 0;
    bool applied = applyEpochFallbackIfCommitted(true, 1700000001L, [&](long epoch) { calls++; seen = epoch; });
    TEST_ASSERT_TRUE(applied);
    TEST_ASSERT_EQUAL_INT(1, calls);
    TEST_ASSERT_EQUAL_INT64(1700000001L, seen);
}

void test_CommittedConfig_NoServerEpoch_HookNotInvoked(void)
{
    int calls = 0;
    bool applied = applyEpochFallbackIfCommitted(true, 0L, [&](long) { calls++; });
    TEST_ASSERT_FALSE(applied);
    TEST_ASSERT_EQUAL_INT(0, calls);
}

int main(int argc, char **argv)
{
    UNITY_BEGIN();
    RUN_TEST(test_RejectedConfig_HookNotInvoked);
    RUN_TEST(test_CommittedConfig_HookInvokedExactlyOnceWithEpoch);
    RUN_TEST(test_CommittedConfig_NoServerEpoch_HookNotInvoked);
    return UNITY_END();
}
