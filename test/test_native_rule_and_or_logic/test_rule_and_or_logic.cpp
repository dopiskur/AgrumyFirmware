#include <unity.h>
#include "../../src/Logic/RelayLogic.h"

// ops[0] is never read by foldConditions (nothing precedes the first result) - filled with 0 in
// these tests to make that explicit rather than leaving it looking meaningful.
const int UNUSED_FIRST_OP = 0;
const int AND = 1;
const int OR = 2;

void setUp(void) {}
void tearDown(void) {}

void test_SingleCondition_ReturnsItUnchanged_True(void)
{
    bool results[] = {true};
    int ops[] = {UNUSED_FIRST_OP};
    TEST_ASSERT_TRUE(foldConditions(results, ops, 1));
}

void test_SingleCondition_ReturnsItUnchanged_False(void)
{
    bool results[] = {false};
    int ops[] = {UNUSED_FIRST_OP};
    TEST_ASSERT_FALSE(foldConditions(results, ops, 1));
}

void test_ZeroConditions_ReturnsFalse(void)
{
    bool results[] = {true};
    int ops[] = {UNUSED_FIRST_OP};
    TEST_ASSERT_FALSE(foldConditions(results, ops, 0));
}

void test_TwoConditions_And_BothTrue_IsTrue(void)
{
    bool results[] = {true, true};
    int ops[] = {UNUSED_FIRST_OP, AND};
    TEST_ASSERT_TRUE(foldConditions(results, ops, 2));
}

void test_TwoConditions_And_OneFalse_IsFalse(void)
{
    bool results[] = {true, false};
    int ops[] = {UNUSED_FIRST_OP, AND};
    TEST_ASSERT_FALSE(foldConditions(results, ops, 2));
}

void test_TwoConditions_Or_OneTrue_IsTrue(void)
{
    bool results[] = {false, true};
    int ops[] = {UNUSED_FIRST_OP, OR};
    TEST_ASSERT_TRUE(foldConditions(results, ops, 2));
}

void test_TwoConditions_Or_BothFalse_IsFalse(void)
{
    bool results[] = {false, false};
    int ops[] = {UNUSED_FIRST_OP, OR};
    TEST_ASSERT_FALSE(foldConditions(results, ops, 2));
}

// The whole point of "strict left-to-right, no precedence": (false AND true) OR true = true OR true = true -> TRUE.
// A precedence-aware evaluator (AND binding tighter than OR) would instead compute false AND (true OR true) = false AND true = FALSE.
// This is the regression lock distinguishing the two.
void test_ThreeConditions_LeftToRight_NotOperatorPrecedence(void)
{
    bool results[] = {false, true, true};
    int ops[] = {UNUSED_FIRST_OP, AND, OR};
    TEST_ASSERT_TRUE(foldConditions(results, ops, 3));
}

// (true AND false) OR false = false OR false = false - confirms the same left-to-right fold the
// previous test exercises also correctly propagates a false through to the end when nothing
// downstream rescues it.
void test_ThreeConditions_LeftToRight_AndThenOr_AllFalseChain(void)
{
    bool results[] = {true, false, false};
    int ops[] = {UNUSED_FIRST_OP, AND, OR};
    TEST_ASSERT_FALSE(foldConditions(results, ops, 3));
}

// (true OR false) AND false = true AND false = false - OR first, then AND, still strictly left-to-right.
void test_ThreeConditions_LeftToRight_OrThenAnd(void)
{
    bool results[] = {true, false, false};
    int ops[] = {UNUSED_FIRST_OP, OR, AND};
    TEST_ASSERT_FALSE(foldConditions(results, ops, 3));
}

int main(int argc, char **argv)
{
    UNITY_BEGIN();
    RUN_TEST(test_SingleCondition_ReturnsItUnchanged_True);
    RUN_TEST(test_SingleCondition_ReturnsItUnchanged_False);
    RUN_TEST(test_ZeroConditions_ReturnsFalse);
    RUN_TEST(test_TwoConditions_And_BothTrue_IsTrue);
    RUN_TEST(test_TwoConditions_And_OneFalse_IsFalse);
    RUN_TEST(test_TwoConditions_Or_OneTrue_IsTrue);
    RUN_TEST(test_TwoConditions_Or_BothFalse_IsFalse);
    RUN_TEST(test_ThreeConditions_LeftToRight_NotOperatorPrecedence);
    RUN_TEST(test_ThreeConditions_LeftToRight_AndThenOr_AllFalseChain);
    RUN_TEST(test_ThreeConditions_LeftToRight_OrThenAnd);
    return UNITY_END();
}
