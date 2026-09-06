#include <unity.h>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include "../../src/Logic/RelayLogic.h"

// Reads threshold_vectors.csv, shared with AgrumyService's RuleConditionEvaluatorTests, so the two independently-implemented formulas can't silently drift apart.

struct ThresholdVector
{
    std::string name;
    bool currentlyOn;
    double reading;
    double threshold;
    double hysteresis;
    bool turnsOnAboveThreshold;
    bool expected;
};

static bool parseBool(const std::string &field)
{
    return field == "true";
}

// Resolved via __FILE__ rather than cwd, since `pio test` may run from different working directories.
static std::vector<ThresholdVector> loadVectors()
{
    std::string path = __FILE__;
    path = path.substr(0, path.find_last_of("/\\") + 1) + "threshold_vectors.csv";
    std::ifstream file(path);
    std::vector<ThresholdVector> vectors;
    std::string line;
    bool headerSkipped = false;
    while (std::getline(file, line))
    {
        if (line.empty() || line[0] == '#')
        {
            continue;
        }
        if (!headerSkipped)
        {
            headerSkipped = true;
            continue;
        }
        std::stringstream ss(line);
        std::string field;
        ThresholdVector v;
        std::getline(ss, v.name, ',');
        std::getline(ss, field, ','); v.currentlyOn = parseBool(field);
        std::getline(ss, field, ','); v.reading = std::stod(field);
        std::getline(ss, field, ','); v.threshold = std::stod(field);
        std::getline(ss, field, ','); v.hysteresis = std::stod(field);
        std::getline(ss, field, ','); v.turnsOnAboveThreshold = parseBool(field);
        std::getline(ss, field, ','); v.expected = parseBool(field);
        vectors.push_back(v);
    }
    return vectors;
}

void setUp(void) {}
void tearDown(void) {}

void test_ThresholdVectors_MatchComputeThresholdState(void)
{
    std::vector<ThresholdVector> vectors = loadVectors();
    TEST_ASSERT_TRUE_MESSAGE(!vectors.empty(), "threshold_vectors.csv failed to load or is empty");
    for (const ThresholdVector &v : vectors)
    {
        bool actual = computeThresholdState(v.currentlyOn, v.reading, v.threshold, v.hysteresis, v.turnsOnAboveThreshold);
        TEST_ASSERT_EQUAL_MESSAGE(v.expected, actual, v.name.c_str());
    }
}

int main(int argc, char **argv)
{
    UNITY_BEGIN();
    RUN_TEST(test_ThresholdVectors_MatchComputeThresholdState);
    return UNITY_END();
}
