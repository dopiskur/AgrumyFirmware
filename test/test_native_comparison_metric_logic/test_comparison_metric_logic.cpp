#include <unity.h>
#include <cmath>
#include "../../src/Logic/RelayLogic.h"

// Roadmap #396(4) - the new ComparisonOperator variants (GTE/LTE/EQ/BETWEEN, no dead-zone latch unlike
// GT/LT) and the two new DERIVED metrics (DewPoint/DewPointSpread), reachable through readMetric.

void setUp(void) {}
void tearDown(void) {}

ConditionNode comparison(int metric, int op, double value1, double value2 = 0)
{
    ConditionNode node;
    node.type = NODE_COMPARISON;
    node.metric = metric;
    node.op = op;
    node.value1 = value1;
    node.value2 = value2;
    return node;
}

bool eval(ConditionNode &node, const MetricReadings &readings)
{
    ConditionNode nodes[1] = {node};
    return evaluateNode(nodes, 0, /*wasRuleTrue=*/false, readings, /*epochSeconds=*/1800000000, 0, 0);
}

void test_GreaterThanOrEqual_AtBoundary_IsTrue(void)
{
    MetricReadings r;
    r.temperature = 30.0;
    ConditionNode node = comparison(METRIC_TEMPERATURE, COMPARE_GTE, 30.0);
    TEST_ASSERT_TRUE(eval(node, r));
}

void test_LessThanOrEqual_JustAbove_IsFalse(void)
{
    MetricReadings r;
    r.humidity = 40.01;
    ConditionNode node = comparison(METRIC_HUMIDITY, COMPARE_LTE, 40.0);
    TEST_ASSERT_FALSE(eval(node, r));
}

void test_Equal_ExactMatch_IsTrue(void)
{
    MetricReadings r;
    r.co2 = 400.0;
    ConditionNode node = comparison(METRIC_CO2, COMPARE_EQ, 400.0);
    TEST_ASSERT_TRUE(eval(node, r));
}

void test_Between_Inclusive_BothBoundsMatch(void)
{
    MetricReadings r;
    r.moisture = 20.0;
    ConditionNode lowNode = comparison(METRIC_MOISTURE, COMPARE_BETWEEN, 20.0, 60.0);
    TEST_ASSERT_TRUE(eval(lowNode, r));
    r.moisture = 60.0;
    ConditionNode highNode = comparison(METRIC_MOISTURE, COMPARE_BETWEEN, 20.0, 60.0);
    TEST_ASSERT_TRUE(eval(highNode, r));
    r.moisture = 60.01;
    ConditionNode outNode = comparison(METRIC_MOISTURE, COMPARE_BETWEEN, 20.0, 60.0);
    TEST_ASSERT_FALSE(eval(outNode, r));
}

void test_Between_ValuesReversed_StillWorks(void)
{
    // value1/value2 in the "wrong" order (value1 > value2) - readMetric normalizes via min/max.
    MetricReadings r;
    r.moisture = 40.0;
    ConditionNode node = comparison(METRIC_MOISTURE, COMPARE_BETWEEN, 60.0, 20.0);
    TEST_ASSERT_TRUE(eval(node, r));
}

void test_NaNReading_AnyOperator_IsFalse(void)
{
    MetricReadings r; // temperature left as NAN default
    ConditionNode node = comparison(METRIC_TEMPERATURE, COMPARE_GTE, -1000.0); // would trivially be true if NaN weren't guarded
    TEST_ASSERT_FALSE(eval(node, r));
}

// Reference values cross-checked against api.Utils.VpdCalculator/DewPointCalculator (same Tetens/Magnus formulas) at T=25C, RH=60%.
void test_Vpd_MatchesReferenceValue(void)
{
    MetricReadings r;
    r.temperature = 25.0;
    r.humidity = 60.0;
    TEST_ASSERT_DOUBLE_WITHIN(0.001, 1.267111, readMetric(METRIC_VPD, r));
}

void test_DewPoint_MatchesReferenceValue(void)
{
    MetricReadings r;
    r.temperature = 25.0;
    r.humidity = 60.0;
    TEST_ASSERT_DOUBLE_WITHIN(0.001, 16.693149, readMetric(METRIC_DEW_POINT, r));
}

void test_DewPointSpread_MatchesReferenceValue(void)
{
    MetricReadings r;
    r.temperature = 25.0;
    r.humidity = 60.0;
    TEST_ASSERT_DOUBLE_WITHIN(0.001, 8.306851, readMetric(METRIC_DEW_POINT_SPREAD, r));
}

void test_DewPoint_MissingHumidity_IsNaN(void)
{
    MetricReadings r;
    r.temperature = 25.0; // humidity left NAN
    TEST_ASSERT_TRUE(std::isnan(readMetric(METRIC_DEW_POINT, r)));
}

int main(int argc, char **argv)
{
    UNITY_BEGIN();
    RUN_TEST(test_GreaterThanOrEqual_AtBoundary_IsTrue);
    RUN_TEST(test_LessThanOrEqual_JustAbove_IsFalse);
    RUN_TEST(test_Equal_ExactMatch_IsTrue);
    RUN_TEST(test_Between_Inclusive_BothBoundsMatch);
    RUN_TEST(test_Between_ValuesReversed_StillWorks);
    RUN_TEST(test_NaNReading_AnyOperator_IsFalse);
    RUN_TEST(test_Vpd_MatchesReferenceValue);
    RUN_TEST(test_DewPoint_MatchesReferenceValue);
    RUN_TEST(test_DewPointSpread_MatchesReferenceValue);
    RUN_TEST(test_DewPoint_MissingHumidity_IsNaN);
    return UNITY_END();
}
