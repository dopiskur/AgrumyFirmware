#include <unity.h>
#include "../../src/Logic/CommandReplayLogic.h"

void setUp(void) {}
void tearDown(void) {}

void test_KnownTimestamp_MatchesExpectedEpoch(void)
{
    // 2026-09-06T12:00:00Z - same value the server-side CanonicalString test already fixes as its example.
    TEST_ASSERT_EQUAL_INT64(1788696000L, isoUtcToEpochSeconds("2026-09-06T12:00:00Z"));
}

void test_FractionalSeconds_Ignored_SameSecondAsWithout(void)
{
    TEST_ASSERT_EQUAL_INT64(isoUtcToEpochSeconds("2026-09-06T12:00:00Z"), isoUtcToEpochSeconds("2026-09-06T12:00:00.1234567Z"));
}

void test_MalformedInput_ReturnsZero(void)
{
    TEST_ASSERT_EQUAL_INT64(0L, isoUtcToEpochSeconds(""));
    TEST_ASSERT_EQUAL_INT64(0L, isoUtcToEpochSeconds("not a timestamp"));
    TEST_ASSERT_EQUAL_INT64(0L, isoUtcToEpochSeconds("2026-13-06T12:00:00Z")); // month 13
    TEST_ASSERT_EQUAL_INT64(0L, isoUtcToEpochSeconds("2026-09-06T25:00:00Z")); // hour 25
}

void test_NullPointer_ReturnsZero(void)
{
    TEST_ASSERT_EQUAL_INT64(0L, isoUtcToEpochSeconds(nullptr));
}

void test_LowerOrEqualCommandId_IsReplayed(void)
{
    TEST_ASSERT_TRUE(commandIsReplayed(5, 5, 2000000000L, 1800000000L));
    TEST_ASSERT_TRUE(commandIsReplayed(4, 5, 2000000000L, 1800000000L));
}

void test_HigherCommandId_NotReplayed_WhenStillUnexpired(void)
{
    TEST_ASSERT_FALSE(commandIsReplayed(6, 5, 2000000000L, 1800000000L));
}

void test_ExpiredByClock_IsReplayed_EvenWithAHigherCommandId(void)
{
    TEST_ASSERT_TRUE(commandIsReplayed(6, 5, /*expiresAt*/ 1000000000L, /*now*/ 1800000000L));
}

void test_ClockNotYetPlausible_NeverBlocksOnExpiry(void)
{
    // Device hasn't synced NTP/server-epoch yet - "now" is near-zero, must not treat every command as expired.
    TEST_ASSERT_FALSE(commandIsReplayed(6, 5, /*expiresAt*/ 1800000000L, /*now*/ 100L));
}

void test_UnparseableExpiresAt_NeverBlocksOnExpiry(void)
{
    TEST_ASSERT_FALSE(commandIsReplayed(6, 5, /*expiresAt*/ 0L, /*now*/ 1800000000L));
}

int main(int argc, char **argv)
{
    UNITY_BEGIN();
    RUN_TEST(test_KnownTimestamp_MatchesExpectedEpoch);
    RUN_TEST(test_FractionalSeconds_Ignored_SameSecondAsWithout);
    RUN_TEST(test_MalformedInput_ReturnsZero);
    RUN_TEST(test_NullPointer_ReturnsZero);
    RUN_TEST(test_LowerOrEqualCommandId_IsReplayed);
    RUN_TEST(test_HigherCommandId_NotReplayed_WhenStillUnexpired);
    RUN_TEST(test_ExpiredByClock_IsReplayed_EvenWithAHigherCommandId);
    RUN_TEST(test_ClockNotYetPlausible_NeverBlocksOnExpiry);
    RUN_TEST(test_UnparseableExpiresAt_NeverBlocksOnExpiry);
    return UNITY_END();
}
