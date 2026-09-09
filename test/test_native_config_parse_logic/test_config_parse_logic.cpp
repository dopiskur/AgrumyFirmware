#include <unity.h>
#include <ArduinoJson.h>
#include "../../src/Logic/ConfigParseLogic.h"

void setUp(void) {}
void tearDown(void) {}

static JsonObject parseObject(JsonDocument &doc, const char *json)
{
    TEST_ASSERT_TRUE(deserializeJson(doc, json) == DeserializationError::Ok);
    return doc.as<JsonObject>();
}

// --- parseConditionNode -----------------------------------------------

void test_ParseConditionNode_SimpleComparison_Accepted(void)
{
    JsonDocument doc;
    JsonObject node = parseObject(doc, R"({"type":1,"metric":1,"operator":1,"value1":24.5,"hysteresis":0.5})");
    Rule rule;
    rule.nodeCount = 0;

    int index = parseConditionNode(node, rule);

    TEST_ASSERT_EQUAL_INT(0, index);
    TEST_ASSERT_EQUAL_INT(1, rule.nodeCount);
    TEST_ASSERT_EQUAL_INT(NODE_COMPARISON, rule.nodes[0].type);
}

void test_ParseConditionNode_UnknownType_Rejected(void)
{
    JsonDocument doc;
    JsonObject node = parseObject(doc, R"({"type":99})"); // not one of the 4 recognized NodeType values
    Rule rule;
    rule.nodeCount = 0;

    TEST_ASSERT_EQUAL_INT(-1, parseConditionNode(node, rule));
}

void test_ParseConditionNode_GroupWithZeroChildren_Rejected(void)
{
    JsonDocument doc;
    JsonObject node = parseObject(doc, R"({"type":4,"groupOperator":1,"children":[]})");
    Rule rule;
    rule.nodeCount = 0;

    TEST_ASSERT_EQUAL_INT(-1, parseConditionNode(node, rule));
}

void test_ParseConditionNode_GroupOverMaxChildren_Rejected(void)
{
    // MAX_CHILDREN_PER_GROUP is 4 (ConditionTree.h) - 5 children must reject the whole node.
    JsonDocument doc;
    JsonObject node = parseObject(doc,
        R"({"type":4,"groupOperator":1,"children":[
            {"type":1,"metric":1,"operator":1,"value1":1},
            {"type":1,"metric":1,"operator":1,"value1":2},
            {"type":1,"metric":1,"operator":1,"value1":3},
            {"type":1,"metric":1,"operator":1,"value1":4},
            {"type":1,"metric":1,"operator":1,"value1":5}
        ]})");
    Rule rule;
    rule.nodeCount = 0;

    TEST_ASSERT_EQUAL_INT(-1, parseConditionNode(node, rule));
}

void test_ParseConditionNode_NestedGroupWithinCap_Accepted(void)
{
    JsonDocument doc;
    JsonObject node = parseObject(doc,
        R"({"type":4,"groupOperator":1,"children":[
            {"type":1,"metric":1,"operator":1,"value1":24},
            {"type":2,"interval":300,"intervalLength":60}
        ]})");
    Rule rule;
    rule.nodeCount = 0;

    int rootIndex = parseConditionNode(node, rule);

    TEST_ASSERT_TRUE(rootIndex >= 0);
    TEST_ASSERT_EQUAL_INT(3, rule.nodeCount); // group + 2 children
    TEST_ASSERT_EQUAL_INT(2, rule.nodes[rootIndex].childCount);
}

void test_ParseConditionNode_TotalNodesOverCap_Rejected(void)
{
    // MAX_NODES_PER_RULE is 8 (ConditionTree.h) - pre-fill so only room for one more, then a group
    // needing 2 more slots (itself + 1 child) must reject rather than store a partial tree.
    JsonDocument doc;
    JsonObject node = parseObject(doc,
        R"({"type":4,"groupOperator":1,"children":[
            {"type":1,"metric":1,"operator":1,"value1":1}
        ]})");
    Rule rule;
    rule.nodeCount = MAX_NODES_PER_RULE - 1; // only 1 slot left before this call

    TEST_ASSERT_EQUAL_INT(-1, parseConditionNode(node, rule));
}

// --- parseRule -----------------------------------------------------------

void test_ParseRule_ValidRoot_Accepted(void)
{
    JsonDocument doc;
    JsonObject r = parseObject(doc,
        R"({"relayFunction":3,"root":{"type":1,"metric":1,"operator":1,"value1":24}})");
    Rule outRule;

    TEST_ASSERT_TRUE(parseRule(r, outRule));
    TEST_ASSERT_EQUAL_INT(3, outRule.targetFunction);
    TEST_ASSERT_TRUE(outRule.rootIndex >= 0);
}

void test_ParseRule_MissingRoot_Rejected(void)
{
    JsonDocument doc;
    JsonObject r = parseObject(doc, R"({"relayFunction":3})"); // no "root" key at all
    Rule outRule;

    TEST_ASSERT_FALSE(parseRule(r, outRule));
}

void test_ParseRule_UnrecognizedRootNodeType_RejectsWholeRule(void)
{
    JsonDocument doc;
    JsonObject r = parseObject(doc, R"({"relayFunction":3,"root":{"type":99}})");
    Rule outRule;

    TEST_ASSERT_FALSE(parseRule(r, outRule));
}

// --- isConfigSchemaNewerThanFirmware --------------------------------------

void test_IsConfigSchemaNewerThanFirmware_ServerAhead_True(void)
{
    TEST_ASSERT_TRUE(isConfigSchemaNewerThanFirmware(3, 2));
}

void test_IsConfigSchemaNewerThanFirmware_ServerBehindOrEqual_False(void)
{
    TEST_ASSERT_FALSE(isConfigSchemaNewerThanFirmware(2, 2));
    TEST_ASSERT_FALSE(isConfigSchemaNewerThanFirmware(1, 2));
    TEST_ASSERT_FALSE(isConfigSchemaNewerThanFirmware(0, 2)); // server build predating the field entirely
}

int main(int argc, char **argv)
{
    UNITY_BEGIN();
    RUN_TEST(test_ParseConditionNode_SimpleComparison_Accepted);
    RUN_TEST(test_ParseConditionNode_UnknownType_Rejected);
    RUN_TEST(test_ParseConditionNode_GroupWithZeroChildren_Rejected);
    RUN_TEST(test_ParseConditionNode_GroupOverMaxChildren_Rejected);
    RUN_TEST(test_ParseConditionNode_NestedGroupWithinCap_Accepted);
    RUN_TEST(test_ParseConditionNode_TotalNodesOverCap_Rejected);
    RUN_TEST(test_ParseRule_ValidRoot_Accepted);
    RUN_TEST(test_ParseRule_MissingRoot_Rejected);
    RUN_TEST(test_ParseRule_UnrecognizedRootNodeType_RejectsWholeRule);
    RUN_TEST(test_IsConfigSchemaNewerThanFirmware_ServerAhead_True);
    RUN_TEST(test_IsConfigSchemaNewerThanFirmware_ServerBehindOrEqual_False);
    return UNITY_END();
}
