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

void test_ClockNotYetPlausible_FailsClosed_DeferredNotAccepted(void)
{
    // Device hasn't synced NTP/server-epoch yet - an expiry that can't be checked must not run; the next poll re-delivers it once the clock is trusted.
    TEST_ASSERT_TRUE(commandIsReplayed(6, 5, /*expiresAt*/ 1800000000L, /*now*/ 100L));
    TEST_ASSERT_EQUAL_INT(INBOX_REJECT_CLOCK_UNVERIFIABLE, commandInboxDecision(6, 5, 1800000000L, 100L));
}

void test_UnparseableExpiresAt_FailsClosed(void)
{
    TEST_ASSERT_TRUE(commandIsReplayed(6, 5, /*expiresAt*/ 0L, /*now*/ 1800000000L));
    TEST_ASSERT_EQUAL_INT(INBOX_REJECT_NO_EXPIRY, commandInboxDecision(6, 5, 0L, 1800000000L));
}

void test_Decision_ReportsWhyARejectedCommandWasDropped(void)
{
    TEST_ASSERT_EQUAL_INT(INBOX_ACCEPT, commandInboxDecision(6, 5, 2000000000L, 1800000000L));
    TEST_ASSERT_EQUAL_INT(INBOX_REJECT_ALREADY_PROCESSED, commandInboxDecision(5, 5, 2000000000L, 1800000000L));
    TEST_ASSERT_EQUAL_INT(INBOX_REJECT_EXPIRED, commandInboxDecision(6, 5, 1000000000L, 1800000000L));
}

void test_AlreadyProcessed_WinsOverEveryOtherReason(void)
{
    // A stale id is dropped for good even when the clock is untrusted - it must never be "deferred" into running later.
    TEST_ASSERT_EQUAL_INT(INBOX_REJECT_ALREADY_PROCESSED, commandInboxDecision(3, 5, 0L, 100L));
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
    RUN_TEST(test_ClockNotYetPlausible_FailsClosed_DeferredNotAccepted);
    RUN_TEST(test_UnparseableExpiresAt_FailsClosed);
    RUN_TEST(test_Decision_ReportsWhyARejectedCommandWasDropped);
    RUN_TEST(test_AlreadyProcessed_WinsOverEveryOtherReason);
    return UNITY_END();
}
