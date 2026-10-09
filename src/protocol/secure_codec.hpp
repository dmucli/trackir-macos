// Packet codec for the CameraRev35 ("secure") TrackIR 5 protocol. See docs/PROTOCOL.md section 4.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>

namespace tir {

using SecurePacket = std::array<uint8_t, 24>;

// XTEA, 32 cycles, as in TrackIR5.exe XteaEncipherBlock (005a4ce0).
void xteaEncipher(uint32_t v[2], const uint32_t key[4]);

// 8 keys of 128 bits, little-endian words, as stored at VA 0x01850D80 of TrackIR5.exe.
using SecureKeyTable = std::array<std::array<uint32_t, 4>, 8>;

struct HandshakeExpectation {
    size_t replyLength = 0;   // length of the reply after the routing byte is stripped
    uint8_t checkByte = 0;    // reply[4]
    uint8_t stateXor = 0;     // reply[5] ^ stateXor == acknowledged step
    std::array<uint8_t, 8> nonce{};
};

class SecureCodec {
public:
    using Random = std::function<uint32_t()>;

    explicit SecureCodec(Random random);

    // Packet builders. Each returns a packet that is already obfuscated and ready to send.
    SecurePacket simple(uint8_t command);
    SecurePacket field(uint8_t command, uint8_t a, uint8_t b, uint8_t c);
    SecurePacket handshake(int step);

    // Reply to a handshake step 7 poll, with the routing byte stripped (reply[0] == 0x20).
    // Returns the acknowledged step, or nullopt when the reply is malformed or (if a key table is set) not authentic.
    std::optional<int> checkStatusReply(const uint8_t* reply, size_t length);

    void setKeyTable(const SecureKeyTable& table) { keyTable_ = table; haveKeyTable_ = true; }
    bool authenticating() const { return haveKeyTable_; }
    bool lastReplyAuthenticated() const { return lastAuthenticated_; }
    const HandshakeExpectation& expectation() const { return expect_; }

    // Inverse operations, used by tests and by the packet dump tool.
    static void deobfuscate(SecurePacket& packet);
    static int hiddenIndex(const SecurePacket& packet);
    struct DecodedField { uint8_t command, a, b, c; };
    static DecodedField decodeField(const SecurePacket& deobfuscated);
    static int decodeHandshakeStep(const SecurePacket& deobfuscated);

private:
    uint32_t range(uint32_t lo, uint32_t hi);
    void fillRandom(SecurePacket& packet);
    void hideIndex(SecurePacket& packet, int index);
    void obfuscate(SecurePacket& packet);

    Random random_;
    HandshakeExpectation expect_{};
    bool haveExpectation_ = false;
    SecureKeyTable keyTable_{};
    bool haveKeyTable_ = false;
    std::array<uint32_t, 4> sessionKey_{};
    bool lastAuthenticated_ = false;
};

}  // namespace tir
