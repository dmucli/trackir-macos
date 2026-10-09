// Records what the port's SecureCodec does for a deterministic random stream, for comparison with TrackIR5.exe.
#include "protocol/secure_codec.hpp"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>
using namespace tir;

static uint32_t state = 7;
static std::vector<uint32_t> used;
static uint32_t next() { state = state * 1664525u + 1013904223u; uint32_t v = state ^ (state >> 13); used.push_back(v); return v; }
static uint32_t param() { static uint32_t s = 99; s = s * 69069u + 1; return s >> 16; }

static void emit(const char* kind, int a, int b, int c, const SecurePacket& p)
{
    std::printf("%s %d %d %d ", kind, a, b, c);
    for (size_t i = 0; i < used.size(); i++) std::printf("%s%08x", i ? "," : "", used[i]);
    std::printf(" ");
    for (uint8_t x : p) std::printf("%02x", x);
    used.clear();
}

int main(int argc, char** argv)
{
    SecureCodec codec(next);
    SecureKeyTable keys{};
    std::ifstream f(argv[1], std::ios::binary);
    uint8_t raw[128];
    f.read(reinterpret_cast<char*>(raw), 128);
    std::memcpy(keys.data(), raw, 128);
    codec.setKeyTable(keys);
    std::array<uint32_t, 4> session{};
    for (int round = 0; round < 60; round++) {
        uint8_t a = uint8_t(param()), b = uint8_t(param()), c = uint8_t(param());
        emit("field", a, b, c, codec.field(0x19, a, b, c)); std::printf("\n");
        emit("simple", 0x13, 0, 0, codec.simple(0x13)); std::printf("\n");
        for (int step = 0; step < 8; step++) {
            SecurePacket p = codec.handshake(step);
            if (step == 0) {
                SecurePacket q = p;
                SecureCodec::deobfuscate(q);
                session = keys[q[0x15] & 7];
            }
            emit("handshake", step, 0, 0, p);
            if (step == 7) {
                const auto& e = codec.expectation();
                std::printf(" %zu %u %u ", e.replyLength, e.checkByte, e.stateXor);
                for (uint8_t x : e.nonce) std::printf("%02x", x);
                // The reply a genuine camera would send, built from the expectation and the session key.
                std::vector<uint8_t> r(e.replyLength, 0x5A);
                r[0] = 0x20; r[1] = 0x01; r[4] = e.checkByte; r[5] = uint8_t(3 ^ e.stateXor);
                uint32_t v[2], in[2];
                std::memcpy(in, e.nonce.data(), 8);
                std::memcpy(v, in, 8);
                xteaEncipher(v, session.data());
                uint32_t out[2] = {in[0] ^ v[0], in[1] ^ v[1]};
                std::memcpy(&r[6], out, 8);
                auto ack = codec.checkStatusReply(r.data(), r.size());
                if (!ack || *ack != 3 || !codec.lastReplyAuthenticated()) { std::fprintf(stderr, "port rejected its own reply\n"); return 1; }
                session = {out[0], out[1], 0, 0};
                std::printf(" ");
                for (uint8_t x : r) std::printf("%02x", x);
            }
            std::printf("\n");
        }
    }
    return 0;
}
