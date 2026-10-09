// Camera models and the driver interface shared by the classic and secure protocols.
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace tir {

using Clock = std::chrono::steady_clock;

enum class ProtocolKind { Classic, Secure };

// Radial lens model (+0xC8 of the camera classes, e.g. Rev35_GetLensModel 005bcc00).
struct LensModel {
    double focalPx = 594.0;
    double cx = 320.0;
    double cy = 240.0;
    double k1 = 0, k2 = 0, k3 = 0;
};

struct CameraModel {
    uint16_t productId;
    const char* name;
    const char* libraryClass;
    ProtocolKind protocol;
    bool supported;
    int width;
    int height;
    int fpgaResource;  // -1 when the camera boots its own firmware
};

const CameraModel* findCameraModel(uint16_t productId);
const std::vector<CameraModel>& knownCameraModels();
LensModel lensFor(const CameraModel& model, uint32_t serial);

// Anything that can put a command packet on the OUT pipe.
class CommandSink {
public:
    virtual ~CommandSink() = default;
    virtual bool send(const uint8_t* data, size_t length) = 0;
};

struct CameraSettings {
    int threshold = 150;       // 0..255
    int exposure = -1;         // -1 = the model's default
    int irIntensity = -1;      // -1 = the model's default
    int videoType = 5;         // 5 = segments with intensity sums
    bool ledsOn = true;
    int statusIntensity = 255; // green status LED brightness
};

class CameraDriver {
public:
    virtual ~CameraDriver() = default;

    // Begins the initialisation sequence; progress continues as replies arrive.
    virtual void connect() = 0;
    virtual void start() = 0;
    virtual void stop() = 0;
    virtual void shutdown() = 0;

    // Non-frame packets from the IN pipe (status/config/version replies).
    virtual void handleReply(const uint8_t* packet, size_t length) = 0;
    // Timers and retries; called from the run loop.
    virtual void tick(Clock::time_point now) = 0;

    virtual bool ready() const = 0;
    virtual bool streaming() const = 0;
    virtual bool failed() const = 0;
    virtual std::string describeState() const = 0;

    virtual void applySettings(const CameraSettings& settings) = 0;

    std::function<void(const std::string&)> log = [](const std::string&) {};
};

struct DriverResources {
    std::vector<uint8_t> fpgaImage;   // classic cameras
    bool haveKeyTable = false;        // secure cameras: authenticate replies when present
    uint8_t keyTable[128] = {};
};

std::unique_ptr<CameraDriver> makeCameraDriver(const CameraModel& model, CommandSink& sink,
                                               const DriverResources& resources);

}  // namespace tir
