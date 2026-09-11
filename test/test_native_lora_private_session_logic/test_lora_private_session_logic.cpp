#include <unity.h>
#include <cstring>
#include <cstdlib>
#include <string>
#include "../../src/Logic/LoRaPrivateSessionLogic.h"

// Vector 1 from contracts/lora-private-v2.vectors.json - counter=1, ordinary case.
const uint8_t V1_BOOT_NONCE[8] = {0x47, 0x61, 0xa8, 0xa7, 0x8e, 0xc4, 0x07, 0xa7};
const uint32_t V1_COUNTER = 1;
const char *V1_CIPHERTEXT = "\x60\x5c\x67\x64\x90\x02\x05\xe4\x73\x55\xe7\xbd\xb9\xf6";
const size_t V1_CIPHERTEXT_LEN = 14;
const uint8_t V1_TAG[16] = {0x9b, 0x1a, 0x32, 0xe2, 0xca, 0x88, 0x59, 0x90, 0x20, 0xd9, 0x05, 0xa2, 0x0e, 0x67, 0x4d, 0x61};
const char *V1_FRAME_HEX = "024761a8a78ec407a700000001605c6764900205e47355e7bdb9f69b1a32e2ca88599020d905a20e674d61";

// Vector 2 - counter = UINT32_MAX, the wraparound edge case.
const uint8_t V2_BOOT_NONCE[8] = {0x44, 0x93, 0x8b, 0xb8, 0xed, 0x33, 0x2e, 0xb6};
const uint32_t V2_COUNTER = 4294967295u;

// Vector 3 - counter = 0.
const uint8_t V3_BOOT_NONCE[8] = {0x10, 0x8d, 0x00, 0x17, 0x44, 0x07, 0xfd, 0xdb};
const uint32_t V3_COUNTER = 0;
const char *V3_CIPHERTEXT = "\xfa\x4e";
const uint8_t V3_TAG[16] = {0xe4, 0x87, 0xf4, 0x99, 0x83, 0xa4, 0xe7, 0x95, 0x80, 0x15, 0x70, 0xb4, 0x25, 0x93, 0xbf, 0xfb};
const char *V3_FRAME_HEX = "02108d00174407fddb00000000fa4ee487f49983a4e795801570b42593bffb";

void setUp(void) {}
void tearDown(void) {}

std::string hexToBytes(const char *hex)
{
    std::string out;
    size_t len = strlen(hex);
    for (size_t i = 0; i + 1 < len; i += 2)
    {
        out.push_back((char)strtoul(std::string(hex + i, 2).c_str(), nullptr, 16));
    }
    return out;
}

void test_NonceV2_IsBootNonceThenCounterBigEndian(void)
{
    uint8_t nonce[12];
    buildLoRaPrivateNonceV2(V1_BOOT_NONCE, V1_COUNTER, nonce);
    for (int i = 0; i < 8; i++)
    {
        TEST_ASSERT_EQUAL_UINT8(V1_BOOT_NONCE[i], nonce[i]);
    }
    TEST_ASSERT_EQUAL_UINT8(0x00, nonce[8]);
    TEST_ASSERT_EQUAL_UINT8(0x00, nonce[9]);
    TEST_ASSERT_EQUAL_UINT8(0x00, nonce[10]);
    TEST_ASSERT_EQUAL_UINT8(0x01, nonce[11]);
}

void test_NonceV2_MaxCounter_AllFF(void)
{
    uint8_t nonce[12];
    buildLoRaPrivateNonceV2(V2_BOOT_NONCE, V2_COUNTER, nonce);
    TEST_ASSERT_EQUAL_UINT8(0xFF, nonce[8]);
    TEST_ASSERT_EQUAL_UINT8(0xFF, nonce[9]);
    TEST_ASSERT_EQUAL_UINT8(0xFF, nonce[10]);
    TEST_ASSERT_EQUAL_UINT8(0xFF, nonce[11]);
}

void test_FrameV2_MatchesSharedVector_Counter1(void)
{
    std::string ciphertext(V1_CIPHERTEXT, V1_CIPHERTEXT_LEN);
    std::string frame = encodeLoRaPrivateCipherFrameV2(V1_BOOT_NONCE, V1_COUNTER, ciphertext, V1_TAG);
    std::string expected = hexToBytes(V1_FRAME_HEX);
    TEST_ASSERT_EQUAL_UINT32(expected.size(), frame.size());
    TEST_ASSERT_EQUAL_UINT8(0x02, (uint8_t)frame[0]); // leading version byte, rejected by the decoder on any other value
    TEST_ASSERT_EQUAL_MEMORY(expected.data(), frame.data(), expected.size());
}

void test_FrameV2_MatchesSharedVector_CounterZero(void)
{
    std::string ciphertext(V3_CIPHERTEXT, 2);
    std::string frame = encodeLoRaPrivateCipherFrameV2(V3_BOOT_NONCE, V3_COUNTER, ciphertext, V3_TAG);
    std::string expected = hexToBytes(V3_FRAME_HEX);
    TEST_ASSERT_EQUAL_UINT32(expected.size(), frame.size());
    TEST_ASSERT_EQUAL_MEMORY(expected.data(), frame.data(), expected.size());
}

void test_IsCounterAckV2_MatchesOwnBootNonceAndCounter(void)
{
    std::string ack = hexToBytes("024761a8a78ec407a700000001");
    TEST_ASSERT_TRUE(isCounterAckV2(ack, V1_BOOT_NONCE, V1_COUNTER));
}

void test_IsCounterAckV2_RejectsWrongCounter(void)
{
    std::string ack = hexToBytes("024761a8a78ec407a700000001");
    TEST_ASSERT_FALSE(isCounterAckV2(ack, V1_BOOT_NONCE, V1_COUNTER + 1));
}

void test_IsCounterAckV2_RejectsWrongBootNonce(void)
{
    std::string ack = hexToBytes("024761a8a78ec407a700000001");
    TEST_ASSERT_FALSE(isCounterAckV2(ack, V2_BOOT_NONCE, V1_COUNTER));
}

void test_IsCounterAckV2_RejectsShorterPayload(void)
{
    // 8 raw bytes with no leading version tag must never be mistaken for a valid ack.
    std::string shortAck = hexToBytes("0000000000000001");
    TEST_ASSERT_FALSE(isCounterAckV2(shortAck, V1_BOOT_NONCE, 1));
}

int main(int argc, char **argv)
{
    UNITY_BEGIN();
    RUN_TEST(test_NonceV2_IsBootNonceThenCounterBigEndian);
    RUN_TEST(test_NonceV2_MaxCounter_AllFF);
    RUN_TEST(test_FrameV2_MatchesSharedVector_Counter1);
    RUN_TEST(test_FrameV2_MatchesSharedVector_CounterZero);
    RUN_TEST(test_IsCounterAckV2_MatchesOwnBootNonceAndCounter);
    RUN_TEST(test_IsCounterAckV2_RejectsWrongCounter);
    RUN_TEST(test_IsCounterAckV2_RejectsWrongBootNonce);
    RUN_TEST(test_IsCounterAckV2_RejectsShorterPayload);
    return UNITY_END();
}
