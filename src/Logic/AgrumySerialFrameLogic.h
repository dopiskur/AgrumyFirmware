#ifndef AgrumySerialFrameLogic_H
#define AgrumySerialFrameLogic_H

#include <string>
#include <cstdint>

// Pure, native-testable mirror of AgrumyService's api.Gateway.LoRaPrivate.AgrumySerialFrame (C#) -
// the wire protocol between the LoRa gateway radio-frontend board (Controller/LoRaGatewayBridgeController)
// and Agrumy.Gateway over USB serial. Wire layout, big-endian: [0]=0xA5 marker, [1]=type
// (1=Uplink/2=Downlink), [2-3]=nodeAddress, [4]=RSSI as signed byte (Uplink only), [5-6]=payload
// length, [7..]=payload, last byte=XOR checksum of every byte before it. This file only implements
// the bridge's half: encoding an Uplink (bridge->Gateway) and decoding a Downlink (Gateway->bridge) -
// the C# side implements the mirror-image half.

std::string encodeAgrumySerialUplink(uint16_t sourceAddress, int8_t rssi, const std::string &payload);

struct AgrumySerialDownlink
{
    uint16_t destAddress = 0;
    std::string payload;
};

// Returns bytes consumed from the front of `buffer` (0 = wait for more data, 1 = drop one byte and
// resync past a bad marker/checksum) - `out`/`hasFrame` are only meaningful when the return value
// covers a complete, checksum-valid frame.
size_t tryDecodeAgrumySerialDownlink(const uint8_t *buffer, size_t length, AgrumySerialDownlink &out, bool &hasFrame);

#endif
