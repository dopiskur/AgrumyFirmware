#include <unity.h>
#include "../../src/Logic/RelayLogic.h"

void setUp(void) {}
void tearDown(void) {}

// Three independently-controllable leaves (temperature/humidity/moisture, each a simple >=0 comparison)
// so a test can set any true/false combination via MetricReadings without the leaves interfering.
// desiredTrue documents intent at each call site only - actual truth is controlled by the paired readingsFor() call (>=0 or <0 for this metric).
ConditionNode makeLeaf(int metric, bool /*desiredTrue*/)
{
    ConditionNode node;
    node.type = NODE_COMPARISON;
    node.metric = metric;
    node.op = COMPARE_GTE;
    node.value1 = 0;
    return node;
}

MetricReadings readingsFor(bool a, bool b, bool c)
{
    MetricReadings r;
    r.temperature = a ? 1 : -1;
    r.humidity = b ? 1 : -1;
    r.moisture = c ? 1 : -1;
    return r;
}

bool evalGroup(ConditionNode nodes[], int rootIndex, const MetricReadings &readings)
{
    return evaluateNode(nodes, rootIndex, /*wasRuleTrue=*/false, readings, /*epochSeconds=*/1800000000, /*localWeekday=*/0, /*localSecondsOfDay=*/0);
}

void test_SingleComparison_ReturnsItUnchanged_True(void)
{
    ConditionNode nodes[1] = {makeLeaf(METRIC_TEMPERATURE, true)};
    TEST_ASSERT_TRUE(evalGroup(nodes, 0, readingsFor(true, false, false)));
}

void test_SingleComparison_ReturnsItUnchanged_False(void)
{
    ConditionNode nodes[1] = {makeLeaf(METRIC_TEMPERATURE, false)};
    TEST_ASSERT_FALSE(evalGroup(nodes, 0, readingsFor(false, false, false)));
}

void test_EmptyGroup_ReturnsFalse(void)
{
    ConditionNode nodes[1];
    nodes[0].type = NODE_GROUP;
    nodes[0].groupOperator = LOGICAL_AND;
    nodes[0].childCount = 0;
    TEST_ASSERT_FALSE(evalGroup(nodes, 0, readingsFor(true, true, true)));
}

// nodes[0]=A(temperature), nodes[1]=B(humidity), nodes[2]=group(A,B)
void test_TwoConditions_And_BothTrue_IsTrue(void)
{
    ConditionNode nodes[3];
    nodes[0] = makeLeaf(METRIC_TEMPERATURE, true);
    nodes[1] = makeLeaf(METRIC_HUMIDITY, true);
    nodes[2].type = NODE_GROUP;
    nodes[2].groupOperator = LOGICAL_AND;
    nodes[2].childIndices[0] = 0;
    nodes[2].childIndices[1] = 1;
    nodes[2].childCount = 2;
    TEST_ASSERT_TRUE(evalGroup(nodes, 2, readingsFor(true, true, false)));
}

void test_TwoConditions_And_OneFalse_IsFalse(void)
{
    ConditionNode nodes[3];
    nodes[0] = makeLeaf(METRIC_TEMPERATURE, true);
    nodes[1] = makeLeaf(METRIC_HUMIDITY, true);
    nodes[2].type = NODE_GROUP;
    nodes[2].groupOperator = LOGICAL_AND;
    nodes[2].childIndices[0] = 0;
    nodes[2].childIndices[1] = 1;
    nodes[2].childCount = 2;
    TEST_ASSERT_FALSE(evalGroup(nodes, 2, readingsFor(true, false, false)));
}

void test_TwoConditions_Or_OneTrue_IsTrue(void)
{
    ConditionNode nodes[3];
    nodes[0] = makeLeaf(METRIC_TEMPERATURE, false);
    nodes[1] = makeLeaf(METRIC_HUMIDITY, true);
    nodes[2].type = NODE_GROUP;
    nodes[2].groupOperator = LOGICAL_OR;
    nodes[2].childIndices[0] = 0;
    nodes[2].childIndices[1] = 1;
    nodes[2].childCount = 2;
    TEST_ASSERT_TRUE(evalGroup(nodes, 2, readingsFor(false, true, false)));
}

void test_TwoConditions_Or_BothFalse_IsFalse(void)
{
    ConditionNode nodes[3];
    nodes[0] = makeLeaf(METRIC_TEMPERATURE, false);
    nodes[1] = makeLeaf(METRIC_HUMIDITY, false);
    nodes[2].type = NODE_GROUP;
    nodes[2].groupOperator = LOGICAL_OR;
    nodes[2].childIndices[0] = 0;
    nodes[2].childIndices[1] = 1;
    nodes[2].childCount = 2;
    TEST_ASSERT_FALSE(evalGroup(nodes, 2, readingsFor(false, false, false)));
}

// (A AND B) OR C - the whole point of nesting: mixed AND/OR now needs an explicit inner group,
// unlike the old flat left-to-right fold where this was implicit. Regression lock against a
// precedence-aware evaluator (which would instead compute something else for A=false,B=true,C=true).
// nodes: 0=A,1=B,2=innerAND(A,B),3=C,4=outerOR(inner,C)
void test_ThreeConditions_AndThenOr_NestedGroup(void)
{
    ConditionNode nodes[5];
    nodes[0] = makeLeaf(METRIC_TEMPERATURE, false); // A
    nodes[1] = makeLeaf(METRIC_HUMIDITY, true);      // B
    nodes[2].type = NODE_GROUP;
    nodes[2].groupOperator = LOGICAL_AND;
    nodes[2].childIndices[0] = 0;
    nodes[2].childIndices[1] = 1;
    nodes[2].childCount = 2; // A AND B = false
    nodes[3] = makeLeaf(METRIC_MOISTURE, true); // C = true
    nodes[4].type = NODE_GROUP;
    nodes[4].groupOperator = LOGICAL_OR;
    nodes[4].childIndices[0] = 2;
    nodes[4].childIndices[1] = 3;
    nodes[4].childCount = 2; // (A AND B) OR C = false OR true = true
    TEST_ASSERT_TRUE(evalGroup(nodes, 4, readingsFor(false, true, true)));
}

// (A AND B) OR C with C also false - confirms the nested fold propagates false through when nothing rescues it.
void test_ThreeConditions_AndThenOr_AllFalseChain(void)
{
    ConditionNode nodes[5];
    nodes[0] = makeLeaf(METRIC_TEMPERATURE, true);
    nodes[1] = makeLeaf(METRIC_HUMIDITY, false);
    nodes[2].type = NODE_GROUP;
    nodes[2].groupOperator = LOGICAL_AND;
    nodes[2].childIndices[0] = 0;
    nodes[2].childIndices[1] = 1;
    nodes[2].childCount = 2; // A AND B = false
    nodes[3] = makeLeaf(METRIC_MOISTURE, false); // C = false
    nodes[4].type = NODE_GROUP;
    nodes[4].groupOperator = LOGICAL_OR;
    nodes[4].childIndices[0] = 2;
    nodes[4].childIndices[1] = 3;
    nodes[4].childCount = 2;
    TEST_ASSERT_FALSE(evalGroup(nodes, 4, readingsFor(true, false, false)));
}

// (A OR B) AND C - OR nested inside AND, the other mixing order.
void test_ThreeConditions_OrThenAnd_NestedGroup(void)
{
    ConditionNode nodes[5];
    nodes[0] = makeLeaf(METRIC_TEMPERATURE, true);  // A
    nodes[1] = makeLeaf(METRIC_HUMIDITY, false);     // B
    nodes[2].type = NODE_GROUP;
    nodes[2].groupOperator = LOGICAL_OR;
    nodes[2].childIndices[0] = 0;
    nodes[2].childIndices[1] = 1;
    nodes[2].childCount = 2; // A OR B = true
    nodes[3] = makeLeaf(METRIC_MOISTURE, false); // C = false
    nodes[4].type = NODE_GROUP;
    nodes[4].groupOperator = LOGICAL_AND;
    nodes[4].childIndices[0] = 2;
    nodes[4].childIndices[1] = 3;
    nodes[4].childCount = 2; // (A OR B) AND C = true AND false = false
    TEST_ASSERT_FALSE(evalGroup(nodes, 4, readingsFor(true, false, false)));
}

int main(int argc, char **argv)
{
    UNITY_BEGIN();
    RUN_TEST(test_SingleComparison_ReturnsItUnchanged_True);
    RUN_TEST(test_SingleComparison_ReturnsItUnchanged_False);
    RUN_TEST(test_EmptyGroup_ReturnsFalse);
    RUN_TEST(test_TwoConditions_And_BothTrue_IsTrue);
    RUN_TEST(test_TwoConditions_And_OneFalse_IsFalse);
    RUN_TEST(test_TwoConditions_Or_OneTrue_IsTrue);
    RUN_TEST(test_TwoConditions_Or_BothFalse_IsFalse);
    RUN_TEST(test_ThreeConditions_AndThenOr_NestedGroup);
    RUN_TEST(test_ThreeConditions_AndThenOr_AllFalseChain);
    RUN_TEST(test_ThreeConditions_OrThenAnd_NestedGroup);
    return UNITY_END();
}
