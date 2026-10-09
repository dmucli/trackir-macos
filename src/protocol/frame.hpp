// Inbound packet routing and frame decoding. See docs/PROTOCOL.md sections 2.2-2.4.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace tir {

enum class PacketKind { Frame, Status, Config, FirmwareVersion, Other, Invalid };

// Classifies by packet[1] & 0xF0 (DispatchCameraPipePacket, 00583e30).
PacketKind classifyPacket(const uint8_t* packet, size_t length);

// A horizontal run of above-threshold pixels on one sensor row (x0..x1 inclusive).
struct Segment {
    int y = 0;
    float x0 = 0;
    float x1 = 0;
    uint32_t intensitySum = 0;  // only type 5 carries it; 0 otherwise
};

struct Frame {
    uint8_t counter = 0;
    uint8_t type = 0;
    std::vector<Segment> segments;
};

// ValidateFramePacketChecksum (005a4280).
bool frameChecksumValid(const uint8_t* packet, size_t length);

// Decodes frame types 0 and 5 (the segment modes used for tracking). Returns false for invalid packets
// or unsupported types. width/height are the sensor size used to discard out-of-range runs.
bool decodeFrame(const uint8_t* packet, size_t length, int width, int height, Frame& out);

}  // namespace tir
