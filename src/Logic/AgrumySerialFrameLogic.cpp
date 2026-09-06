#include "Logic/AgrumySerialFrameLogic.h"

namespace
{
    const uint8_t MARKER = 0xA5;
    const uint8_t TYPE_UPLINK = 0x01;
    const uint8_t TYPE_DOWNLINK = 0x02;
    const size_t HEADER_LENGTH = 7; // marker + type + address(2) + rssi + length(2)

    uint8_t checksum(const uint8_t *data, size_t length)
    {
        uint8_t x = 0;
        for (size_t i = 0; i < length; i++)
        {
            x ^= data[i];
        }
        return x;
    }
}

std::string encodeAgrumySerialUplink(uint16_t sourceAddress, int8_t rssi, const std::string &payload)
{
    std::string frame;
    frame.reserve(HEADER_LENGTH + payload.size() + 1);
    frame.push_back((char)MARKER);
    frame.push_back((char)TYPE_UPLINK);
    frame.push_back((char)(sourceAddress >> 8));
    frame.push_back((char)(sourceAddress & 0xFF));
    frame.push_back((char)rssi);
    frame.push_back((char)(payload.size() >> 8));
    frame.push_back((char)(payload.size() & 0xFF));
    frame += payload;
    frame.push_back((char)checksum((const uint8_t *)frame.data(), frame.size()));
    return frame;
}

size_t tryDecodeAgrumySerialDownlink(const uint8_t *buffer, size_t length, AgrumySerialDownlink &out, bool &hasFrame)
{
    hasFrame = false;
    if (length == 0)
    {
        return 0;
    }
    if (buffer[0] != MARKER)
    {
        return 1;
    }
    if (length < HEADER_LENGTH)
    {
        return 0;
    }
    if (buffer[1] != TYPE_DOWNLINK)
    {
        return 1;
    }
    size_t payloadLength = ((size_t)buffer[5] << 8) | buffer[6];
    size_t frameLength = HEADER_LENGTH + payloadLength + 1;
    if (length < frameLength)
    {
        return 0;
    }
    if (checksum(buffer, frameLength - 1) != buffer[frameLength - 1])
    {
        return 1;
    }

    out.destAddress = ((uint16_t)buffer[2] << 8) | buffer[3];
    out.payload.assign((const char *)buffer + HEADER_LENGTH, payloadLength);
    hasFrame = true;
    return frameLength;
}
