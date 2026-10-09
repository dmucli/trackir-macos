#include "frame.hpp"

namespace tir {

PacketKind classifyPacket(const uint8_t* p, size_t n)
{
    if (n < 2)
        return PacketKind::Invalid;
    switch (p[1] & 0xF0) {
    case 0x10: return PacketKind::Frame;
    case 0x20: return PacketKind::Status;
    case 0x40: return PacketKind::Config;
    case 0x50: return PacketKind::FirmwareVersion;
    case 0x00: return PacketKind::Invalid;
    default: return PacketKind::Other;
    }
}

bool frameChecksumValid(const uint8_t* p, size_t n)
{
    if (n < 8)
        return false;
    if (p[3] != uint8_t(p[0] ^ p[1] ^ p[2] ^ 0xAA))
        return false;
    uint32_t trailer = uint32_t(p[n - 4]) << 24 | uint32_t(p[n - 3]) << 16 | uint32_t(p[n - 2]) << 8 | p[n - 1];
    return trailer == n - 8;
}

namespace {

// ParseFrameType5Segments (005a3720): 8-byte records with the run's intensity moments.
void decodeType5(const uint8_t* p, size_t n, int width, Frame& out)
{
    size_t payload = n - 8;
    for (size_t off = 4; off + 8 <= 4 + payload; off += 8) {
        const uint8_t* b = p + off;
        int x = b[0] << 2 | b[1] >> 6;
        int y = (b[1] & 0x3F) << 3 | b[2] >> 5;
        int len = (b[2] & 0x1F) << 5 | b[3] >> 3;
        if (x >= width || y == 0 || x + len > width || len == 0)
            continue;
        Segment s;
        s.y = y;
        s.x0 = float(x);
        s.x1 = float(x + len - 1);
        s.moment1 = ((uint32_t(b[3] & 7) << 8 | b[4]) << 8 | b[5]) << 1 | b[6] >> 7;  // segment +8
        s.moment0 = (uint32_t(b[6]) << 8 | b[7]) & 0x7FFF;                         // segment +0xC
        out.segments.push_back(s);
    }
}

// ParseFrameType0Segments (005a40d0): 4-byte records, x in half pixels.
void decodeType0(const uint8_t* p, size_t n, int width, int height, Frame& out)
{
    size_t payload = n - 8;
    for (size_t off = 4; off + 4 <= 4 + payload; off += 4) {
        const uint8_t* b = p + off;
        int f = b[3];
        int y = b[0] + ((f & 0x20) + (f & 0x04) * 8) * 8;
        int x0 = b[1] + (f & 0x80) * 2 + (f & 0x10) * 0x20 + (f & 0x02) * 0x200;
        int x1 = b[2] + (f & 0x40) * 4 + (f & 0x08) * 0x40 + (f & 0x01) * 0x400;
        Segment s;
        s.y = y;
        s.x0 = x0 * 0.5f;
        s.x1 = x1 * 0.5f;
        if (s.y >= height || s.x0 >= width || s.x1 < s.x0)
            continue;
        out.segments.push_back(s);
    }
}

}  // namespace

bool decodeFrame(const uint8_t* p, size_t n, int width, int height, Frame& out)
{
    out.segments.clear();
    if (!frameChecksumValid(p, n))
        return false;
    out.counter = p[0];
    out.type = p[2];
    switch (out.type) {
    case 5: decodeType5(p, n, width, out); return true;
    case 0: decodeType0(p, n, width, height, out); return true;
    default: return false;
    }
}

}  // namespace tir
