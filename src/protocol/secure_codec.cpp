#include "secure_codec.hpp"

#include <cstring>

namespace tir {

namespace {

uint32_t loadLE32(const uint8_t* p)
{
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

void storeLE32(uint8_t* p, uint32_t v)
{
    p[0] = uint8_t(v);
    p[1] = uint8_t(v >> 8);
    p[2] = uint8_t(v >> 16);
    p[3] = uint8_t(v >> 24);
}

}  // namespace

void xteaEncipher(uint32_t v[2], const uint32_t key[4])
{
    uint32_t v0 = v[0], v1 = v[1], sum = 0;
    for (int i = 0; i < 32; i++) {
        v0 += (((v1 << 4) ^ (v1 >> 5)) + v1) ^ (sum + key[sum & 3]);
        sum += 0x9E3779B9;
        v1 += (((v0 << 4) ^ (v0 >> 5)) + v0) ^ (sum + key[(sum >> 11) & 3]);
    }
    v[0] = v0;
    v[1] = v1;
}

SecureCodec::SecureCodec(Random random) : random_(std::move(random)) {}

uint32_t SecureCodec::range(uint32_t lo, uint32_t hi)
{
    return lo + random_() % (hi - lo + 1);
}

void SecureCodec::fillRandom(SecurePacket& packet)
{
    for (size_t i = 0; i < packet.size(); i += 4)
        storeLE32(&packet[i], random_());
}

// The payload index is spread over one bit each of bytes 1..4 (Rev35_BuildFieldPacket, 005bca20).
void SecureCodec::hideIndex(SecurePacket& p, int index)
{
    p[1] = uint8_t((p[1] & ~0x08) | (index & 0x08));
    p[2] = uint8_t((p[2] & ~0x04) | (index & 0x04));
    p[3] = uint8_t((p[3] & ~0x02) | (index & 0x02));
    p[4] = uint8_t((p[4] & ~0x01) | (index & 0x01));
}

int SecureCodec::hiddenIndex(const SecurePacket& p)
{
    return (p[1] & 0x08) | (p[2] & 0x04) | (p[3] & 0x02) | (p[4] & 0x01);
}

// Rev35_ObfuscateAndSendPacket (005bd7a0).
void SecureCodec::obfuscate(SecurePacket& p)
{
    uint32_t hi = range(1, 15);
    uint32_t lo = range(1, 15);
    p[0x11] = uint8_t(hi << 4 | lo);
    p[0] ^= p[lo];
    p[0] = uint8_t(p[hi] ^ p[0] ^ 0x69);
}

void SecureCodec::deobfuscate(SecurePacket& p)
{
    int hi = p[0x11] >> 4;
    int lo = p[0x11] & 0x0F;
    p[0] = uint8_t(p[0] ^ p[hi] ^ 0x69 ^ p[lo]);
}

SecurePacket SecureCodec::simple(uint8_t command)
{
    SecurePacket p;
    fillRandom(p);
    p[0] = command;
    obfuscate(p);
    return p;
}

SecurePacket SecureCodec::field(uint8_t command, uint8_t a, uint8_t b, uint8_t c)
{
    SecurePacket p;
    fillRandom(p);
    int index = int(range(6, 14));
    p[0] = command;
    hideIndex(p, index);
    p[index] = p[0x10] ^ a;
    p[index - 1] = p[0x13] ^ b;
    p[index + 1] = p[0x12] ^ c;
    obfuscate(p);
    return p;
}

SecureCodec::DecodedField SecureCodec::decodeField(const SecurePacket& p)
{
    int index = hiddenIndex(p);
    return {p[0], uint8_t(p[index] ^ p[0x10]), uint8_t(p[index - 1] ^ p[0x13]), uint8_t(p[index + 1] ^ p[0x12])};
}

// Rev35_BuildHandshakePacket (005bcaa0).
SecurePacket SecureCodec::handshake(int step)
{
    SecurePacket p;
    fillRandom(p);
    int index = int(range(2, 14));
    int extra = int(range(0, 3));
    hideIndex(p, index);
    p[0] = 0x1A;
    p[index] = uint8_t(((p[index] & 0x0F) | step << 4) ^ (p[0x10] & 0xF0));
    p[index + 1] = uint8_t((p[index + 1] & 0x3F) | extra << 6);

    if (step == 7) {
        expect_.replyLength = size_t(extra) + 14;
        expect_.checkByte = uint8_t(p[index + 2] ^ p[0x12] ^ index);
        expect_.stateXor = uint8_t(p[0x13] ^ p[0x0D]);
        std::memcpy(expect_.nonce.data(), &p[index >> 1], 8);
        haveExpectation_ = true;
    } else if (step == 0 && haveKeyTable_) {
        const auto& key = keyTable_[p[0x15] & 7];
        std::memcpy(sessionKey_.data(), key.data(), sizeof(sessionKey_));
    }
    obfuscate(p);
    return p;
}

int SecureCodec::decodeHandshakeStep(const SecurePacket& p)
{
    int index = hiddenIndex(p);
    return ((p[index] ^ p[0x10]) >> 4) & 0x0F;
}

// Rev35_VerifyHandshakeResponse (005bd6c0) plus the state check of Rev35 +0x4a8 (005bc930).
std::optional<int> SecureCodec::checkStatusReply(const uint8_t* r, size_t length)
{
    lastAuthenticated_ = false;
    if (!haveExpectation_ || length < 14 || r[0] != 0x20 || r[1] != 0x01)
        return std::nullopt;
    if (length != expect_.replyLength || r[4] != expect_.checkByte)
        return std::nullopt;

    if (haveKeyTable_) {
        uint32_t v[2] = {loadLE32(&expect_.nonce[0]), loadLE32(&expect_.nonce[4])};
        uint32_t in0 = v[0], in1 = v[1];
        xteaEncipher(v, sessionKey_.data());
        uint8_t out[8];
        storeLE32(&out[0], in0 ^ v[0]);
        storeLE32(&out[4], in1 ^ v[1]);
        // Like XteaHashChainBlocks (005a4d50), the key advances before the comparison, matched or not.
        sessionKey_ = {loadLE32(&out[0]), loadLE32(&out[4]), 0, 0};
        if (std::memcmp(out, r + 6, 8) != 0)
            return std::nullopt;
        lastAuthenticated_ = true;
    }
    return int(uint8_t(r[5] ^ expect_.stateXor));
}

}  // namespace tir
