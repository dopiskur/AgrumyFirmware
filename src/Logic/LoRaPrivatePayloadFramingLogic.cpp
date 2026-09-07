#include "Logic/LoRaPrivatePayloadFramingLogic.h"

std::string encodeLoRaPrivateCipherFrame(uint64_t counter, const std::string &ciphertext, const uint8_t tag[16])
{
    std::string frame;
    frame.reserve(8 + ciphertext.size() + 16);
    for (int shift = 56; shift >= 0; shift -= 8)
    {
        frame.push_back((char)((counter >> shift) & 0xFF));
    }
    frame += ciphertext;
    frame.append((const char *)tag, 16);
    return frame;
}
