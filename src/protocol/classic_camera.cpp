// CameraRev9 / CameraRev18 driver (131d:0157, 131d:0158): plain byte commands and a host-side FPGA upload.
#include "camera.hpp"

#include <algorithm>
#include <initializer_list>
#include <thread>

namespace tir {

uint16_t fpgaChecksum(const std::vector<uint8_t>& image)
{
    uint32_t s = 0;  // FUN_005834e0
    for (uint8_t b : image)
        s = (s + b) ^ (uint32_t(b) << 4);
    return uint16_t(s);
}

namespace {

constexpr auto kStatusTimeout = std::chrono::milliseconds(500);

class ClassicCameraDriver final : public CameraDriver {
public:
    ClassicCameraDriver(const CameraModel& model, CommandSink& sink, const DriverResources& resources)
        : sink_(sink), fpga_(resources.fpgaImage)
    {
    }

    void connect() override
    {
        phase_ = Phase::CheckingFpga;
        readStatus();
    }

    // Camera Start (00585410).
    void start() override
    {
        wantStreaming_ = true;
        if (phase_ != Phase::Ready)
            return;
        flush();
        setIntensity(irIntensity_);
        send({0x14, 0x00});
        phase_ = Phase::Streaming;
    }

    // Camera Stop (00585450) with Rev9's stop sequence (005a3370).
    void stop() override
    {
        wantStreaming_ = false;
        if (phase_ != Phase::Streaming && phase_ != Phase::Ready)
            return;
        for (int i = 0; i < 5; i++)
            send({0x14, 0x01});
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        send({0x12, 0x01});
        send({0x13, 0x01});
        for (int i = 0; i < 5; i++)
            send({0x14, 0x01});
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        send({0x10, 0x00, 0x80});
        flush();
        phase_ = Phase::Ready;
    }

    // Rev9 shutdown (0059f3b0).
    void shutdown() override
    {
        stop();
        send({0x10, 0x00, 0x80});
        send({0x1B});
        phase_ = Phase::Idle;
    }

    // Rev9 status handler (005a2e90): reply bytes [3..4] are the checksum of the loaded FPGA image.
    void handleReply(const uint8_t* p, size_t n) override
    {
        if (n < 6 || (p[1] & 0xF0) != 0x20 || phase_ != Phase::CheckingFpga)
            return;
        const uint8_t* r = p + 1;
        uint16_t loaded = uint16_t(r[3] << 8 | r[4]);
        if (fpga_.empty()) {
            log("no FPGA image available; run `trackir-mac extract-fpga` first");
            phase_ = Phase::Failed;
            return;
        }
        uint16_t wanted = fpgaChecksum(fpga_);
        if (loaded != wanted) {
            if (uploads_++ >= 2) {
                log("FPGA checksum still wrong after upload");
                phase_ = Phase::Failed;
                return;
            }
            log("uploading FPGA image (" + std::to_string(fpga_.size()) + " bytes)");
            uploadFpga();
            return;
        }
        phase_ = Phase::Ready;
        log("FPGA ready");
        applySettings(settings_);
        if (wantStreaming_)
            start();
    }

    void tick(Clock::time_point now) override
    {
        if (phase_ == Phase::CheckingFpga && now - lastStatus_ > kStatusTimeout)
            readStatus();
    }

    bool ready() const override { return phase_ == Phase::Ready || phase_ == Phase::Streaming; }
    bool streaming() const override { return phase_ == Phase::Streaming; }
    bool failed() const override { return phase_ == Phase::Failed; }

    std::string describeState() const override
    {
        switch (phase_) {
        case Phase::Idle: return "idle";
        case Phase::CheckingFpga: return "checking FPGA";
        case Phase::Ready: return "ready";
        case Phase::Streaming: return "streaming";
        default: return "error";
        }
    }

    // Rev9 InitializeCamera (005a2b60) defaults: threshold 150, exposure 479, intensity 10, video type 5.
    void applySettings(const CameraSettings& s) override
    {
        settings_ = s;
        if (!ready())
            return;
        setThreshold(s.threshold);
        setExposure(s.exposure < 0 ? 479 : s.exposure);
        irIntensity_ = s.irIntensity < 0 ? 10 : s.irIntensity;
        setIntensity(irIntensity_);
        ledMask_ = s.ledsOn ? 0x33 : 0;
        setStatusIntensity(s.statusIntensity);
        writeRegister(3, 0x10, uint16_t(s.videoType));
    }

private:
    enum class Phase { Idle, CheckingFpga, Ready, Streaming, Failed };

    void send(std::initializer_list<uint8_t> bytes)
    {
        std::vector<uint8_t> v(bytes);
        sink_.send(v.data(), v.size());
    }

    void flush()
    {
        send({0x12});
        send({0x13});
    }

    void readStatus()
    {
        flush();
        send({0x1D});
        lastStatus_ = Clock::now();
    }

    // Classic_WriteRegister (00583d30).
    void writeRegister(uint8_t reg, uint8_t sub, uint16_t value)
    {
        send({0x19, reg, sub, uint8_t(value >> 8), uint8_t(value)});
    }

    // Classic_WriteImagerRegister (00587d90).
    void writeImager(uint8_t a, uint8_t b, uint8_t c) { send({0x23, a, b, c, 0, 0}); }

    void setThreshold(int t)
    {
        uint8_t v = uint8_t(std::clamp(t, 0, 255));
        send({0x15, v, 0x01, 0x00});
    }

    void setExposure(int v)
    {
        v = std::clamp(v, 0, 479);
        writeImager(0x42, 0x08, uint8_t(v >> 8));
        writeImager(0x42, 0x10, uint8_t(v));
    }

    void setIntensity(int v)
    {
        writeRegister(9, 0x10, v > 0 ? uint16_t((v & ~1) << 7 | 1) : 0);
    }

    void setStatusIntensity(int v)
    {
        static const uint16_t map[] = {3, 2, 1};
        int bucket = v >> 6;
        statusIntensity_ = bucket < 3 ? map[bucket] : 0;
        writeRegister(4, 0x10, uint16_t(statusIntensity_ << 8 | ledMask_));  // Rev9 LED word (005a2630)
    }

    // Rev9_UploadFpgaBitstream (005a2c00).
    void uploadFpga()
    {
        send({0x1B});
        for (size_t off = 0; off < fpga_.size(); off += 60) {
            size_t n = std::min<size_t>(60, fpga_.size() - off);
            std::vector<uint8_t> chunk{0x1C, uint8_t(n)};
            chunk.insert(chunk.end(), fpga_.begin() + long(off), fpga_.begin() + long(off + n));
            sink_.send(chunk.data(), chunk.size());
        }
        writeRegister(5, 0x10, 0x1000);
        readStatus();
        send({0x20});
    }

    CommandSink& sink_;
    std::vector<uint8_t> fpga_;
    CameraSettings settings_;
    Phase phase_ = Phase::Idle;
    bool wantStreaming_ = false;
    int uploads_ = 0;
    int irIntensity_ = 10;
    uint16_t ledMask_ = 0x33;
    uint16_t statusIntensity_ = 0;
    Clock::time_point lastStatus_{};
};

}  // namespace

std::unique_ptr<CameraDriver> makeClassicCameraDriver(const CameraModel& model, CommandSink& sink,
                                                      const DriverResources& resources)
{
    return std::make_unique<ClassicCameraDriver>(model, sink, resources);
}

std::unique_ptr<CameraDriver> makeSecureCameraDriver(const CameraModel& model, CommandSink& sink,
                                                     const DriverResources& resources);

std::unique_ptr<CameraDriver> makeCameraDriver(const CameraModel& model, CommandSink& sink,
                                               const DriverResources& resources)
{
    if (model.protocol == ProtocolKind::Secure)
        return makeSecureCameraDriver(model, sink, resources);
    return makeClassicCameraDriver(model, sink, resources);
}

}  // namespace tir
