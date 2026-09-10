#include "Logic/LoRaPrivateSessionLogic.h"
#include <cstring>

void buildLoRaPrivateNonceV2(const uint8_t bootNonce[8], uint32_t counter, uint8_t nonceOut[12])
{
    memcpy(nonceOut, bootNonce, 8);
    nonceOut[8] = (uint8_t)((counter >> 24) & 0xFF);
    nonceOut[9] = (uint8_t)((counter >> 16) & 0xFF);
    nonceOut[10] = (uint8_t)((counter >> 8) & 0xFF);
    nonceOut[11] = (uint8_t)(counter & 0xFF);
}

std::string encodeLoRaPrivateCipherFrameV2(const uint8_t bootNonce[8], uint32_t counter, const std::string &ciphertext, const uint8_t tag[16])
{
    std::string frame;
    frame.reserve(1 + 8 + 4 + ciphertext.size() + 16);
    frame.push_back((char)0x02);
    frame.append((const char *)bootNonce, 8);
    for (int shift = 24; shift >= 0; shift -= 8)
    {
        frame.push_back((char)((counter >> shift) & 0xFF));
    }
    frame += ciphertext;
    frame.append((const char *)tag, 16);
    return frame;
}

bool isCounterAckV2(const std::string &payload, const uint8_t bootNonce[8], uint32_t counter)
{
    if (payload.size() != 13 || (uint8_t)payload[0] != 0x02)
    {
        return false;
    }
    if (memcmp(payload.data() + 1, bootNonce, 8) != 0)
    {
        return false;
    }
    uint32_t echoed = ((uint8_t)payload[9] << 24) | ((uint8_t)payload[10] << 16) | ((uint8_t)payload[11] << 8) | (uint8_t)payload[12];
    return echoed == counter;
}
