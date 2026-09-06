#include <unity.h>
#include <string>
#include "../../src/Logic/AgrumySerialFrameLogic.h"

void setUp(void) {}
void tearDown(void) {}

void test_EncodeUplink_HasMarkerAndValidChecksum(void)
{
    std::string frame = encodeAgrumySerialUplink(42, -80, "{\"t\":\"sensor\"}");

    TEST_ASSERT_EQUAL_UINT8(0xA5, (uint8_t)frame[0]);
    TEST_ASSERT_EQUAL_UINT8(0x01, (uint8_t)frame[1]); // Uplink

    uint8_t checksum = 0;
    for (size_t i = 0; i < frame.size() - 1; i++)
    {
        checksum ^= (uint8_t)frame[i];
    }
    TEST_ASSERT_EQUAL_UINT8(checksum, (uint8_t)frame.back());
}

// Downlink and uplink share the same wire layout except the type byte - flip it to Downlink to
// exercise the decoder without needing a separate C++-side encoder for that direction (only the
// C# Gateway side ever needs to encode a real downlink).
static std::string asDownlinkFrame(uint16_t address, const std::string &payload)
{
    std::string frame = encodeAgrumySerialUplink(address, 0, payload);
    frame[1] = 0x02; // Downlink
    uint8_t checksum = 0;
    for (size_t i = 0; i < frame.size() - 1; i++)
    {
        checksum ^= (uint8_t)frame[i];
    }
    frame[frame.size() - 1] = (char)checksum;
    return frame;
}

void test_TryDecodeDownlink_ValidFrame_ReturnsFullLengthAndFields(void)
{
    std::string frame = asDownlinkFrame(7, "{\"ok\":true}");

    AgrumySerialDownlink decoded;
    bool hasFrame = false;
    size_t consumed = tryDecodeAgrumySerialDownlink((const uint8_t *)frame.data(), frame.size(), decoded, hasFrame);

    TEST_ASSERT_EQUAL_UINT32(frame.size(), consumed);
    TEST_ASSERT_TRUE(hasFrame);
    TEST_ASSERT_EQUAL_UINT16(7, decoded.destAddress);
    TEST_ASSERT_EQUAL_STRING("{\"ok\":true}", decoded.payload.c_str());
}

void test_TryDecodeDownlink_IncompleteFrame_WaitsForMoreBytes(void)
{
    std::string frame = asDownlinkFrame(1, "{\"ok\":false}");

    AgrumySerialDownlink decoded;
    bool hasFrame = false;
    size_t consumed = tryDecodeAgrumySerialDownlink((const uint8_t *)frame.data(), frame.size() - 3, decoded, hasFrame);

    TEST_ASSERT_EQUAL_UINT32(0, consumed);
    TEST_ASSERT_FALSE(hasFrame);
}

void test_TryDecodeDownlink_BadMarker_ResyncsOneByte(void)
{
    uint8_t garbage[4] = {0x00, 0x11, 0x22, 0x33};

    AgrumySerialDownlink decoded;
    bool hasFrame = false;
    size_t consumed = tryDecodeAgrumySerialDownlink(garbage, sizeof(garbage), decoded, hasFrame);

    TEST_ASSERT_EQUAL_UINT32(1, consumed);
    TEST_ASSERT_FALSE(hasFrame);
}

void test_TryDecodeDownlink_CorruptChecksum_ResyncsOneByte(void)
{
    std::string frame = asDownlinkFrame(3, "{}");
    frame[frame.size() - 1] ^= 0xFF;

    AgrumySerialDownlink decoded;
    bool hasFrame = false;
    size_t consumed = tryDecodeAgrumySerialDownlink((const uint8_t *)frame.data(), frame.size(), decoded, hasFrame);

    TEST_ASSERT_EQUAL_UINT32(1, consumed);
    TEST_ASSERT_FALSE(hasFrame);
}

int main(int argc, char **argv)
{
    UNITY_BEGIN();
    RUN_TEST(test_EncodeUplink_HasMarkerAndValidChecksum);
    RUN_TEST(test_TryDecodeDownlink_ValidFrame_ReturnsFullLengthAndFields);
    RUN_TEST(test_TryDecodeDownlink_IncompleteFrame_WaitsForMoreBytes);
    RUN_TEST(test_TryDecodeDownlink_BadMarker_ResyncsOneByte);
    RUN_TEST(test_TryDecodeDownlink_CorruptChecksum_ResyncsOneByte);
    return UNITY_END();
}
