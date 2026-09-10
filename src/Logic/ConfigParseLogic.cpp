#include "ConfigParseLogic.h"

int parseConditionNode(JsonObject nodeJson, Rule &rule)
{
    if (rule.nodeCount >= MAX_NODES_PER_RULE)
    {
        return -1;
    }
    int index = rule.nodeCount;
    rule.nodeCount++;
    ConditionNode &node = rule.nodes[index];
    node.type = nodeJson["type"];
    switch (node.type)
    {
    case NODE_COMPARISON:
        node.metric = nodeJson["metric"];
        node.op = nodeJson["operator"];
        node.value1 = nodeJson["value1"];
        node.value2 = nodeJson["value2"] | 0.0;
        node.hysteresis = nodeJson["hysteresis"] | 0.0;
        return index;
    case NODE_INTERVAL:
        node.interval = nodeJson["interval"];
        node.intervalLength = nodeJson["intervalLength"];
        return index;
    case NODE_SCHEDULE:
        node.daysOfWeek = nodeJson["daysOfWeek"];
        node.start = nodeJson["start"];
        node.duration = nodeJson["duration"];
        return index;
    case NODE_GROUP:
    {
        node.groupOperator = nodeJson["groupOperator"];
        JsonArray children = nodeJson["children"];
        if (children.size() == 0 || (int)children.size() > MAX_CHILDREN_PER_GROUP)
        {
            return -1;
        }
        int childCount = 0;
        for (JsonObject childJson : children)
        {
            int childIndex = parseConditionNode(childJson, rule);
            if (childIndex < 0)
            {
                return -1;
            }
            node.childIndices[childCount] = childIndex;
            childCount++;
        }
        node.childCount = childCount;
        return index;
    }
    default:
        return -1; // unrecognized node type (e.g. Astronomical/RuleTriggered, which should never reach firmware at all - see DeviceModel.h's NodeType remarks)
    }
}

bool parseRule(JsonObject r, Rule &outRule)
{
    Rule candidate;
    candidate.targetFunction = r["relayFunction"];
    candidate.targetPercent = r["targetPercent"] | 0; // every Relay rule's fold input now, not just Screen/Vent
    candidate.nodeCount = 0;
    JsonObject rootJson = r["root"];
    int rootIndex = rootJson.isNull() ? -1 : parseConditionNode(rootJson, candidate);
    if (rootIndex < 0)
    {
        // Unrecognized node type, too many total nodes, or a group with too many/zero children -
        // reject the WHOLE rule rather than store a partial/broken tree (same spirit as the old
        // flat model's "a bad condition rejects the whole rule").
        return false;
    }
    candidate.rootIndex = rootIndex;
    outRule = candidate;
    return true;
}

bool isConfigSchemaNewerThanFirmware(int serverSchemaVersion, int localSchemaVersion)
{
    return serverSchemaVersion > localSchemaVersion;
}
