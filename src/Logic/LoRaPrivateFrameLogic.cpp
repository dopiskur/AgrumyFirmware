#include "Logic/LoRaPrivateFrameLogic.h"

std::string encodeLoRaPrivateFrame(uint16_t destAddress, uint16_t srcAddress, const std::string &payload)
{
    std::string frame;
    frame.reserve(4 + payload.size());
    frame.push_back((char)(destAddress >> 8));
    frame.push_back((char)(destAddress & 0xFF));
    frame.push_back((char)(srcAddress >> 8));
    frame.push_back((char)(srcAddress & 0xFF));
    frame += payload;
    return frame;
}

bool decodeLoRaPrivateFrame(const uint8_t *data, size_t length, LoRaPrivateFrame &out)
{
    if (data == nullptr || length < 4)
    {
        return false;
    }
    out.destAddress = ((uint16_t)data[0] << 8) | data[1];
    out.srcAddress = ((uint16_t)data[2] << 8) | data[3];
    out.payload.assign((const char *)data + 4, length - 4);
    return true;
}
