#include <unity.h>
#include "../../src/Logic/LoRaPrivatePayloadFramingLogic.h"

void setUp(void) {}
void tearDown(void) {}

void test_CounterEncodedBigEndian_8Bytes(void)
{
    uint8_t tag[16] = {0};
    std::string frame = encodeLoRaPrivateCipherFrame(0x0102030405060708ULL, "", tag);

    TEST_ASSERT_EQUAL_UINT32(24, frame.size()); // 8 counter + 0 ciphertext + 16 tag
    TEST_ASSERT_EQUAL_UINT8(0x01, (uint8_t)frame[0]);
    TEST_ASSERT_EQUAL_UINT8(0x02, (uint8_t)frame[1]);
    TEST_ASSERT_EQUAL_UINT8(0x03, (uint8_t)frame[2]);
    TEST_ASSERT_EQUAL_UINT8(0x04, (uint8_t)frame[3]);
    TEST_ASSERT_EQUAL_UINT8(0x05, (uint8_t)frame[4]);
    TEST_ASSERT_EQUAL_UINT8(0x06, (uint8_t)frame[5]);
    TEST_ASSERT_EQUAL_UINT8(0x07, (uint8_t)frame[6]);
    TEST_ASSERT_EQUAL_UINT8(0x08, (uint8_t)frame[7]);
}

void test_ZeroCounter_AllZeroPrefix(void)
{
    uint8_t tag[16] = {0};
    std::string frame = encodeLoRaPrivateCipherFrame(0, "x", tag);

    for (int i = 0; i < 8; i++)
    {
        TEST_ASSERT_EQUAL_UINT8(0, (uint8_t)frame[i]);
    }
}

void test_CiphertextPlacedAfterCounter_TagAfterCiphertext(void)
{
    uint8_t tag[16];
    for (int i = 0; i < 16; i++)
    {
        tag[i] = (uint8_t)(0xA0 + i);
    }
    std::string frame = encodeLoRaPrivateCipherFrame(1, "abc", tag);

    TEST_ASSERT_EQUAL_UINT32(8 + 3 + 16, frame.size());
    TEST_ASSERT_EQUAL_STRING_LEN("abc", frame.data() + 8, 3);
    for (int i = 0; i < 16; i++)
    {
        TEST_ASSERT_EQUAL_UINT8(0xA0 + i, (uint8_t)frame[8 + 3 + i]);
    }
}

void test_EmptyCiphertext_StillProducesCounterPlusTag(void)
{
    uint8_t tag[16] = {0};
    std::string frame = encodeLoRaPrivateCipherFrame(5, "", tag);

    TEST_ASSERT_EQUAL_UINT32(24, frame.size());
}

int main(int argc, char **argv)
{
    UNITY_BEGIN();
    RUN_TEST(test_CounterEncodedBigEndian_8Bytes);
    RUN_TEST(test_ZeroCounter_AllZeroPrefix);
    RUN_TEST(test_CiphertextPlacedAfterCounter_TagAfterCiphertext);
    RUN_TEST(test_EmptyCiphertext_StillProducesCounterPlusTag);
    return UNITY_END();
}
