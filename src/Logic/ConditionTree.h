#ifndef ConditionTree_H
#define ConditionTree_H

// Deliberately plain C++ (no Arduino.h) so RelayLogic.h/.cpp can #include this and stay
// natively-testable - same reasoning as RelayLogic.h's own header comment.

// Roadmap #396(4). NODE_ASTRONOMICAL/NODE_RULE_TRIGGERED never reach firmware - the server compiles
// every Astronomical node into an effective Schedule node before sending (api.Devices.
// AstronomicalRuleResolver), and RuleTriggered only ever appears inside a Notification-action rule,
// which never leaves the server at all (only Relay-action rules are sent to a device).
enum NodeType
{
    NODE_COMPARISON = 1,
    NODE_INTERVAL = 2,
    NODE_SCHEDULE = 3,
    NODE_GROUP = 4,
};

// Mirrors api.Models.SensorMetric's numeric values exactly - RAW (1-13, a real sensor reading) or
// DERIVED (14+, computed on-the-fly from raw readings by RelayLogic::readMetric, never stored).
enum SensorMetricType
{
    METRIC_TEMPERATURE = 1,
    METRIC_SOIL_TEMPERATURE = 2,
    METRIC_HUMIDITY = 3,
    METRIC_VPD = 4,
    METRIC_MOISTURE = 5,
    METRIC_LIGHT = 6,
    METRIC_CO2 = 7,
    METRIC_TVOC = 8,
    METRIC_BAROMETER = 9,
    METRIC_LIQUID_PH = 10,
    METRIC_RAIN_LEVEL = 11,
    METRIC_WATER_LEVEL = 12,
    METRIC_WIND = 13,
    METRIC_DEW_POINT = 14,
    METRIC_DEW_POINT_SPREAD = 15,
};

// GT/LT carry the old Threshold condition's dead-zone latch (hysteresis); GTE/LTE/EQ/BETWEEN are
// plain stateless comparisons with no latch.
enum ComparisonOperatorType
{
    COMPARE_GT = 1,
    COMPARE_LT = 2,
    COMPARE_GTE = 3,
    COMPARE_LTE = 4,
    COMPARE_EQ = 5,
    COMPARE_BETWEEN = 6,
};

// Roadmap #212/#396. Operator joining a GroupNode's children left-to-right - unused (0) for a non-group node.
enum LogicalOperator
{
    LOGICAL_AND = 1,
    LOGICAL_OR = 2,
};

// Beyond this cap, ConfigParser silently drops the excess children of one group (server enforces a matching cap). Kept small - MAX_RULES(32) x MAX_NODES_PER_RULE both multiply this struct's static size, DRAM is tight (see MAX_NODES_PER_RULE's remarks).
static const int MAX_CHILDREN_PER_GROUP = 4;

// Flat, tagged-union style: only the fields matching `type` are meaningful (not a real C++ union).
// A GroupNode's children are INDICES into the owning Rule's own flat `nodes` array below, not nested
// pointers/structs - same "no dynamic allocation, no recursion at the storage layer" spirit as the
// rest of this embedded-friendly model (recursion happens only in RelayLogic's EVALUATION over this
// flat representation, see evaluateNode).
struct ConditionNode
{
    int type = 0; // NodeType raw value

    // Comparison only. Metric is explicit per node (roadmap #396(4)) - no longer implicit from the owning Rule's targetFunction.
    int metric = 0; // SensorMetricType raw value
    int op = 0;      // ComparisonOperatorType raw value
    double value1 = 0;
    double value2 = 0; // BETWEEN only
    double hysteresis = 0;

    // On for intervalLength seconds out of every interval-second period, grid-aligned to epoch.
    int interval = 0;
    int intervalLength = 0;

    // daysOfWeek: 7-bit mask, bit0=Sunday..bit6=Saturday. start/duration: seconds since local midnight; a window may not cross midnight.
    int daysOfWeek = 0;
    int start = 0;
    int duration = 0;

    // Group only.
    int groupOperator = 0; // LogicalOperator raw value
    int childIndices[MAX_CHILDREN_PER_GROUP];
    int childCount = 0;
};

// Roadmap #396(4). Beyond this cap, ConfigParser silently drops the whole rule (server enforces a
// matching cap) - total node count across the WHOLE tree (leaves+groups), not just top-level
// conditions like the old flat MAX_CONDITIONS_PER_RULE(8) was. Kept equal to that old cap deliberately -
// deviceConfig.configController.rules[MAX_RULES] is a static array (ConditionNode's own size x this x
// MAX_RULES, all multiplied), and DRAM on an ESP32 is tight; a much larger cap overflowed the DRAM
// segment at link time. Still enough for real nesting (e.g. two 3-condition groups, or one group of
// four plus an ungrouped fifth), just not deep/wide trees.
static const int MAX_NODES_PER_RULE = 8;

// One automation rule: targetFunction plus a recursive ConditionNode tree (roadmap #396(4), replaces
// the old flat left-to-right AND/OR fold - "(A AND B) OR (C AND D)" is now expressible via nested
// GroupNodes, evaluated by RelayLogic::evaluateNode). Several Rules for the same targetFunction fold
// together by MAX'ing their targetPercent (foldTargetPercent), for every function.
struct Rule
{
    int targetFunction = 0; // RelayFunctionType raw value: 1=Ventilation,2=Light,3=Heating,4=WaterPump,5=Screen,6=Vent
    ConditionNode nodes[MAX_NODES_PER_RULE];
    int nodeCount = 0;
    int rootIndex = 0; // index into nodes[] of the tree's root
    // The demand (0-100) this rule asserts while its tree evaluates true, for every targetFunction - every function folds through foldTargetPercent now, not just Screen/Vent.
    int targetPercent = 0;
};

#endif
