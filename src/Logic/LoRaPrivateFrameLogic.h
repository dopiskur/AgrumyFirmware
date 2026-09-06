#ifndef LoRaPrivateFrameLogic_H
#define LoRaPrivateFrameLogic_H

#include <string>
#include <cstdint>

// Pure, native-testable over-the-air frame for the LoRa private-protocol profile (roadmap: LoRa
// private-protocol Gateway, alternative to the LoRaWAN/ChirpStack Profile B) - no Arduino/RadioLib
// dependency here, that lives in Controller/LoRaPrivateController and Controller/LoRaGatewayBridgeController.
// Wire layout, big-endian: [0-1]=destAddress, [2-3]=srcAddress, [4..]=payload (the same {"t":...}
// JSON envelope LoRaPayloadLogic produces). No length prefix or checksum - RadioLib's receive() call
// already reports the exact packet length, and the SX126x's own hardware CRC (setCRC(true)) already
// rejects a corrupt over-the-air packet before it reaches this decoder.

std::string encodeLoRaPrivateFrame(uint16_t destAddress, uint16_t srcAddress, const std::string &payload);

struct LoRaPrivateFrame
{
    uint16_t destAddress = 0;
    uint16_t srcAddress = 0;
    std::string payload;
};

// False if `data` is shorter than the 4-byte address header - out is left unmodified.
bool decodeLoRaPrivateFrame(const uint8_t *data, size_t length, LoRaPrivateFrame &out);

#endif
