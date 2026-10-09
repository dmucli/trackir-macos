// CameraRev35 driver (131d:0159). Mirrors Rev35_HandshakeStateMachine (005bd300) and the Rev35 setters.
#include "camera.hpp"
#include "secure_codec.hpp"

#include <algorithm>
#include <cstring>
#include <random>
#include <thread>

#include <stdlib.h>

namespace tir {

namespace {

enum State : int { Reset0 = 0, Auth1 = 1, Auth2 = 2, Ready = 3, Running = 4, Initial = 0xE, Error = 0xF };

// Events of the original state machine (first argument of Rev35 +0x49c).
enum Event : int { EvReset = 0, EvStart = 4, EvStop = 5, EvIdle = 6, EvSoftReset = 0xE, EvReply = 0xF };

constexpr auto kReplyTimeout = std::chrono::milliseconds(250);
constexpr int kMaxRetries = 8;

class SecureCameraDriver final : public CameraDriver {
public:
    SecureCameraDriver(const CameraModel& model, CommandSink& sink, const DriverResources& resources)
        : sink_(sink), codec_([] { return arc4random(); })
    {
        if (resources.haveKeyTable) {
            SecureKeyTable table;
            for (int k = 0; k < 8; k++)
                for (int w = 0; w < 4; w++)
                    std::memcpy(&table[k][w], resources.keyTable + k * 16 + w * 4, 4);
            codec_.setKeyTable(table);
        }
    }

    void connect() override
    {
        state_ = Initial;
        retries_ = 0;
        event(EvSoftReset);
    }

    // Rev35 Start (005bd240): handshake step 4, then restore the IR intensity.
    void start() override
    {
        wantStreaming_ = true;
        if (state_ == Ready) {
            event(EvStart);
            if (state_ == Running)
                writeRegister(9, irIntensity_ != 0);
        }
    }

    // Rev35 Stop (005bd280).
    void stop() override
    {
        wantStreaming_ = false;
        if (state_ == Running)
            event(EvStop);
        writeRegister(9, 0);
        event(EvIdle);
        sendSimple(0x13);
    }

    // Rev35 shutdown (005bd1d0).
    void shutdown() override
    {
        stop();
        setLeds(0);
        event(EvReset);
    }

    void handleReply(const uint8_t* p, size_t n) override
    {
        if (n < 2)
            return;
        const uint8_t* r = p + 1;  // routing byte stripped, as DispatchCameraPipePacket does
        size_t len = n - 1;
        if (r[0] == 0x20 && len >= 2 && r[1] == 0x01) {
            event(EvReply, r, len);
            if (state_ == Ready)
                sendSimple(0x17);  // cCommand_ReadConfigData, Rev35 +0x32c
            pumpPoll();
        } else if (r[0] == 0x40) {
            log("config data received (" + std::to_string(len) + " bytes)");
        }
    }

    void tick(Clock::time_point now) override
    {
        if (awaitingReply_ && now - lastPoll_ >= kReplyTimeout) {
            if (++retries_ > kMaxRetries) {
                log("camera stopped answering the handshake; restarting it");
                retries_ = 0;
                awaitingReply_ = false;
                pollPending_ = false;
                state_ = Initial;
                event(EvSoftReset);
                return;
            }
            sendPoll();
        }
        pumpPoll();
    }

    bool ready() const override { return state_ == Ready || state_ == Running; }
    bool streaming() const override { return state_ == Running; }
    bool failed() const override { return state_ == Error; }

    std::string describeState() const override
    {
        switch (state_) {
        case Initial: return "reset";
        case Reset0: case Auth1: case Auth2: return "handshake step " + std::to_string(state_);
        case Ready: return "ready";
        case Running: return "streaming";
        default: return "error";
        }
    }

    // Rev35_InitializeCamera (005bcd70) defaults: threshold 150, exposure 120, intensity 1, video type 5.
    void applySettings(const CameraSettings& s) override
    {
        settings_ = s;
        if (!ready())
            return;
        setThreshold(s.threshold);
        setExposure(s.exposure < 0 ? 120 : s.exposure);
        irIntensity_ = s.irIntensity < 0 ? 1 : s.irIntensity;
        writeRegister(9, irIntensity_ != 0);
        writeRegister(3, uint16_t(s.videoType));
        setStatusIntensity(s.statusIntensity);
        setLeds(s.ledsOn ? 0x33 : 0);
    }

private:
    void sendPacket(const SecurePacket& p) { sink_.send(p.data(), p.size()); }
    void sendSimple(uint8_t command) { sendPacket(codec_.simple(command)); }
    void sendHandshake(int step) { sendPacket(codec_.handshake(step)); }

    // The original queues a cCommand_ReadCameraStatus after each transition; the command queue runs them one at a
    // time, so only one status poll (handshake step 7) is ever outstanding. Each poll replaces the expected reply.
    void requestPoll()
    {
        pollPending_ = true;
        pumpPoll();
    }

    void pumpPoll()
    {
        if (pollPending_ && !awaitingReply_) {
            pollPending_ = false;
            sendPoll();
        }
    }

    void sendPoll()
    {
        sendHandshake(7);
        awaitingReply_ = true;
        lastPoll_ = Clock::now();
    }

    // Rev35_WriteRegister (005bc950): field packet 0x19 reg hi lo.
    void writeRegister(uint8_t reg, uint16_t value) { sendPacket(codec_.field(0x19, reg, uint8_t(value >> 8), uint8_t(value))); }
    // Rev35_WriteImagerRegister (005bd740).
    void writeImager(uint8_t a, uint8_t b, uint8_t c) { sendPacket(codec_.field(0x23, a, b, c)); }

    void setThreshold(int t) { writeRegister(5, uint16_t(std::clamp(t, 0, 255) * 2)); }

    // Rev35 SetExposure (005bd040).
    void setExposure(int v)
    {
        v = std::clamp(v, 1, 479);
        writeImager(0x35, 2, uint8_t(v << 4));
        writeImager(0x35, 1, uint8_t(v >> 4));
        writeImager(0x35, 0, uint8_t(v >> 12));
        writeImager(0x3B, 0x8F, uint8_t(v));
        writeImager(0x3B, 0x8E, uint8_t(v >> 8));
    }

    // Shared status-intensity mapping (005a32d0).
    void setStatusIntensity(int v)
    {
        static const uint16_t map[] = {3, 2, 1};
        int bucket = v >> 6;
        statusIntensity_ = bucket < 3 ? map[bucket] : 0;
        writeLedWord();
    }

    void setLeds(uint16_t mask)
    {
        ledMask_ = mask;
        writeLedWord();
    }

    // Rev35 LED word (005bc9f0) written to register 4.
    void writeLedWord()
    {
        uint16_t word = uint16_t((statusIntensity_ << 4 | (ledMask_ & 3)) << 4 | ((ledMask_ >> 4) & 3));
        writeRegister(4, word);
    }

    void enter(int s)
    {
        state_ = s;
        retries_ = 0;
    }

    void event(int ev, const uint8_t* reply = nullptr, size_t len = 0)
    {
        int acked = -1;
        if (ev == EvReply) {
            awaitingReply_ = false;
            auto ack = codec_.checkStatusReply(reply, len);
            if (!ack) {
                log("status reply rejected (" + std::to_string(len) + " bytes)");
                awaitingReply_ = true;  // keep polling; tick() restarts after repeated failures
                return;
            }
            acked = *ack;
        }

        switch (state_) {
        case Initial:
            if (ev != EvSoftReset)
                return;
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            enter(Reset0);
            sendHandshake(0);
            sendHandshake(0);
            sendSimple(0x13);
            break;
        case Reset0:
            if (ev != EvReply)
                return;
            if (acked == 0) {
                enter(Auth1);
                sendHandshake(1);
            } else {
                sendHandshake(0);
            }
            break;
        case Auth1:
            if (ev != EvReply)
                return;
            if (acked != 1) {
                awaitingReply_ = true;
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            enter(Auth2);
            sendHandshake(2);
            break;
        case Auth2:
            if (ev != EvReply)
                return;
            if (acked == 2) {
                sendHandshake(3);
            } else if (acked == 3) {
                enter(Ready);
                log(std::string("camera authenticated") +
                    (codec_.lastReplyAuthenticated() ? " (XTEA verified)" : " (structure only)"));
                applySettings(settings_);
                if (wantStreaming_)
                    start();
                return;
            } else {
                awaitingReply_ = true;
                return;
            }
            break;
        case Ready:
            if (ev == EvReset) {
                enter(Reset0);
                sendHandshake(0);
            } else if (ev == EvStart) {
                enter(Running);
                sendHandshake(4);
            } else if (ev == EvIdle) {
                sendHandshake(6);
            } else {
                return;
            }
            break;
        case Running:
            if (ev != EvStop)
                return;
            enter(Ready);
            sendHandshake(5);
            break;
        default:
            return;
        }
        requestPoll();
    }

    CommandSink& sink_;
    SecureCodec codec_;
    CameraSettings settings_;
    int state_ = Initial;
    bool awaitingReply_ = false;
    bool pollPending_ = false;
    bool wantStreaming_ = false;
    int retries_ = 0;
    Clock::time_point lastPoll_{};
    int irIntensity_ = 1;
    uint16_t ledMask_ = 0;
    uint16_t statusIntensity_ = 0;
};

}  // namespace

std::unique_ptr<CameraDriver> makeSecureCameraDriver(const CameraModel& model, CommandSink& sink,
                                                     const DriverResources& resources)
{
    return std::make_unique<SecureCameraDriver>(model, sink, resources);
}

}  // namespace tir
