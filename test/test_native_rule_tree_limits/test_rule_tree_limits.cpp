#include <unity.h>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include "../../src/Logic/ConditionTree.h"

// Reads rule_tree_limits.txt, shared with AgrumyService's RuleTreeLimitTests, so
// DeviceFarmUnitApiController's hardcoded caps can't silently drift from these without a build
// failing somewhere.

// Resolved via __FILE__ rather than cwd, since `pio test` may run from different working directories.
static std::unordered_map<std::string, int> loadLimits()
{
    std::string path = __FILE__;
    path = path.substr(0, path.find_last_of("/\\") + 1) + "rule_tree_limits.txt";
    std::ifstream file(path);
    std::unordered_map<std::string, int> limits;
    std::string line;
    while (std::getline(file, line))
    {
        if (line.empty() || line[0] == '#')
        {
            continue;
        }
        std::size_t eq = line.find('=');
        if (eq == std::string::npos)
        {
            continue;
        }
        limits[line.substr(0, eq)] = std::stoi(line.substr(eq + 1));
    }
    return limits;
}

void setUp(void) {}
void tearDown(void) {}

void test_MaxNodesPerRule_MatchesSharedVector(void)
{
    std::unordered_map<std::string, int> limits = loadLimits();
    TEST_ASSERT_TRUE_MESSAGE(!limits.empty(), "rule_tree_limits.txt failed to load or is empty");
    TEST_ASSERT_EQUAL_INT(limits.at("maxNodesPerRule"), MAX_NODES_PER_RULE);
}

void test_MaxChildrenPerGroup_MatchesSharedVector(void)
{
    std::unordered_map<std::string, int> limits = loadLimits();
    TEST_ASSERT_TRUE_MESSAGE(!limits.empty(), "rule_tree_limits.txt failed to load or is empty");
    TEST_ASSERT_EQUAL_INT(limits.at("maxChildrenPerGroup"), MAX_CHILDREN_PER_GROUP);
}

int main(int argc, char **argv)
{
    UNITY_BEGIN();
    RUN_TEST(test_MaxNodesPerRule_MatchesSharedVector);
    RUN_TEST(test_MaxChildrenPerGroup_MatchesSharedVector);
    return UNITY_END();
}
