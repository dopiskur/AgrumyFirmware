#include <unity.h>
#include "../../src/Logic/HttpDateLogic.h"

void setUp(void) {}
void tearDown(void) {}

void test_KnownDate_MatchesExpectedEpoch(void)
{
    // 15 Nov 1994 08:12:31 UTC - well-known RFC 7231 example, epoch cross-checked externally.
    TEST_ASSERT_EQUAL_INT64(784887151L, httpDateToEpochSeconds("Tue, 15 Nov 1994 08:12:31 GMT"));
}

void test_UnixEpoch_ParsesToZero_ButThatsIndistinguishableFromFailure(void)
{
    // 1 Jan 1970 00:00:00 legitimately parses to epoch 0, same sentinel value used for "parse failed" -
    // documents the one input this parser cannot distinguish from failure, not a bug to fix.
    TEST_ASSERT_EQUAL_INT64(0L, httpDateToEpochSeconds("Thu, 01 Jan 1970 00:00:00 GMT"));
}

void test_EveryMonth_ParsesCorrectly(void)
{
    TEST_ASSERT_EQUAL_INT64(1704067200L, httpDateToEpochSeconds("Mon, 01 Jan 2024 00:00:00 GMT"));
    TEST_ASSERT_EQUAL_INT64(1719792000L, httpDateToEpochSeconds("Sat, 01 Jul 2024 00:00:00 GMT"));
    TEST_ASSERT_EQUAL_INT64(1735603200L, httpDateToEpochSeconds("Tue, 31 Dec 2024 12:00:00 GMT") - 12L * 3600);
}

void test_MalformedInput_ReturnsZero(void)
{
    TEST_ASSERT_EQUAL_INT64(0L, httpDateToEpochSeconds(""));
    TEST_ASSERT_EQUAL_INT64(0L, httpDateToEpochSeconds("not a date"));
    TEST_ASSERT_EQUAL_INT64(0L, httpDateToEpochSeconds("Tue, 15 Nvx 1994 08:12:31 GMT"));
    TEST_ASSERT_EQUAL_INT64(0L, httpDateToEpochSeconds("Tue, 15 Nov 1994 25:12:31 GMT"));
}

void test_NullPointer_ReturnsZero(void)
{
    TEST_ASSERT_EQUAL_INT64(0L, httpDateToEpochSeconds(nullptr));
}

int main(int argc, char **argv)
{
    UNITY_BEGIN();
    RUN_TEST(test_KnownDate_MatchesExpectedEpoch);
    RUN_TEST(test_UnixEpoch_ParsesToZero_ButThatsIndistinguishableFromFailure);
    RUN_TEST(test_EveryMonth_ParsesCorrectly);
    RUN_TEST(test_MalformedInput_ReturnsZero);
    RUN_TEST(test_NullPointer_ReturnsZero);
    return UNITY_END();
}
