#include <unity.h>
#include <string>
#include "../../src/Logic/LoRaPrivateFrameLogic.h"

void setUp(void) {}
void tearDown(void) {}

void test_EncodeThenDecode_RoundTripsAddressesAndPayload(void)
{
    std::string frame = encodeLoRaPrivateFrame(1, 42, "{\"t\":\"sensor\"}");

    LoRaPrivateFrame decoded;
    bool ok = decodeLoRaPrivateFrame((const uint8_t *)frame.data(), frame.size(), decoded);

    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_UINT16(1, decoded.destAddress);
    TEST_ASSERT_EQUAL_UINT16(42, decoded.srcAddress);
    TEST_ASSERT_EQUAL_STRING("{\"t\":\"sensor\"}", decoded.payload.c_str());
}

void test_Encode_AddressesAreBigEndian(void)
{
    std::string frame = encodeLoRaPrivateFrame(0x0102, 0x0304, "x");

    TEST_ASSERT_EQUAL_UINT8(0x01, (uint8_t)frame[0]);
    TEST_ASSERT_EQUAL_UINT8(0x02, (uint8_t)frame[1]);
    TEST_ASSERT_EQUAL_UINT8(0x03, (uint8_t)frame[2]);
    TEST_ASSERT_EQUAL_UINT8(0x04, (uint8_t)frame[3]);
}

void test_Decode_EmptyPayload_Succeeds(void)
{
    std::string frame = encodeLoRaPrivateFrame(5, 6, "");

    LoRaPrivateFrame decoded;
    bool ok = decodeLoRaPrivateFrame((const uint8_t *)frame.data(), frame.size(), decoded);

    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_STRING("", decoded.payload.c_str());
}

void test_Decode_TooShort_Fails(void)
{
    uint8_t threeBytes[3] = {0, 1, 2};
    LoRaPrivateFrame decoded;

    bool ok = decodeLoRaPrivateFrame(threeBytes, sizeof(threeBytes), decoded);

    TEST_ASSERT_FALSE(ok);
}

int main(int argc, char **argv)
{
    UNITY_BEGIN();
    RUN_TEST(test_EncodeThenDecode_RoundTripsAddressesAndPayload);
    RUN_TEST(test_Encode_AddressesAreBigEndian);
    RUN_TEST(test_Decode_EmptyPayload_Succeeds);
    RUN_TEST(test_Decode_TooShort_Fails);
    return UNITY_END();
}
