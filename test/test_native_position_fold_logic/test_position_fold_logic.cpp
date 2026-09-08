#include <unity.h>
#include "../../src/Logic/RelayLogic.h"

void setUp(void) {}
void tearDown(void) {}

void test_NoRules_ReturnsZero(void)
{
    int percents[1] = {0};
    bool trueFlags[1] = {false};
    TEST_ASSERT_EQUAL_INT(0, foldTargetPercent(percents, trueFlags, 0));
}

void test_NoRuleCurrentlyTrue_ReturnsZero(void)
{
    int percents[2] = {30, 70};
    bool trueFlags[2] = {false, false};
    TEST_ASSERT_EQUAL_INT(0, foldTargetPercent(percents, trueFlags, 2));
}

void test_OneRuleTrue_ReturnsItsPercent(void)
{
    int percents[2] = {30, 70};
    bool trueFlags[2] = {true, false};
    TEST_ASSERT_EQUAL_INT(30, foldTargetPercent(percents, trueFlags, 2));
}

// Both "open to 30%" and "open to 70%" are simultaneously true (e.g. two threshold rules both past their point) - the higher-demanding one wins, same "more protection/airflow wins" reasoning as an OR-fold picking "on" over "off".
void test_TwoRulesTrue_HighestPercentWins(void)
{
    int percents[2] = {30, 70};
    bool trueFlags[2] = {true, true};
    TEST_ASSERT_EQUAL_INT(70, foldTargetPercent(percents, trueFlags, 2));
}

// The lower-percent rule being listed AFTER the winning one must not overwrite it.
void test_LowerPercentRuleListedAfterWinner_DoesNotOverwrite(void)
{
    int percents[2] = {70, 30};
    bool trueFlags[2] = {true, true};
    TEST_ASSERT_EQUAL_INT(70, foldTargetPercent(percents, trueFlags, 2));
}

int main(int argc, char **argv)
{
    UNITY_BEGIN();
    RUN_TEST(test_NoRules_ReturnsZero);
    RUN_TEST(test_NoRuleCurrentlyTrue_ReturnsZero);
    RUN_TEST(test_OneRuleTrue_ReturnsItsPercent);
    RUN_TEST(test_TwoRulesTrue_HighestPercentWins);
    RUN_TEST(test_LowerPercentRuleListedAfterWinner_DoesNotOverwrite);
    return UNITY_END();
}
