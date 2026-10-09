// The tracking loop of RunTrackingFrameLoop (00497120), shared by the CLI, `check` and the menu-bar app:
// camera -> blobs -> pose -> recentre -> axis corrections -> smoothing -> profile -> outputs.
#pragma once

#include "app/settings.hpp"
#include "output/profile.hpp"
#include "vision/blobs.hpp"
#include "vision/pose.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace tir {

struct EngineOptions {
    Settings settings;
    bool bridge = true;           // publish to /tmp/TrackIR-macOS.bridge (Wine NPClient + X-Plane plugin)
    bool waitForCamera = false;   // keep looking for / reconnecting to a camera instead of returning
    uint16_t productId = 0;       // 0 = first supported camera
    int dumpPackets = 0;          // pass the first n raw packets to `onPacket`
};

struct EngineStatus {
    enum class State { Starting, NoCamera, Connecting, Streaming, Failed, Stopped };
    State state = State::Starting;
    std::string message;          // camera name / error text
    std::string cameraState;      // driver state machine, e.g. "streaming"
    unsigned framesPerSecond = 0, posesPerSecond = 0;
    std::vector<Blob> blobs;      // last frame, heaviest first (at most 16)
    int sensorWidth = 640, sensorHeight = 480;
    bool clipVisible = false;
    bool paused = false;
    double distanceCm = 0;        // clip centre to camera
    HeadPose measured;            // centred, before axis corrections, smoothing and profile
    HeadPose output;              // what the outputs receive
    uint64_t poseCounter = 0;     // increments with every solved pose
};

class Engine {
public:
    explicit Engine(EngineOptions options);
    ~Engine();

    // Blocking. Returns 0 after stop(), 1 when the camera cannot be used and waitForCamera is false.
    int run();
    void stop() { stop_ = true; }

    void recenter() { recenter_ = true; }
    void setPaused(bool paused) { paused_ = paused; }
    bool paused() const { return paused_; }
    // Live changes that do not need the camera restarted.
    void setProfile(const Profile& profile);
    void setSmoothing(double alpha);
    void setAxisSigns(const std::array<int, 6>& signs);

    EngineStatus status() const;

    // Called from the engine thread.
    std::function<void(const std::string&)> log = [](const std::string&) {};
    std::function<void(const uint8_t*, size_t)> onPacket;
    std::function<void(const EngineStatus&)> onSecond;

    // Loads a profile path from settings (empty = linear); errors are logged and fall back to linear.
    static Profile loadProfile(const std::string& path, std::string* error = nullptr);

private:
    int runCamera(class NpBridge& bridge, class UdpSender& udp);
    void setState(EngineStatus::State state, const std::string& message);

    EngineOptions options_;
    std::atomic<bool> stop_{false}, recenter_{false}, paused_{false};
    mutable std::mutex mutex_;
    EngineStatus status_;
    Profile profile_;
    double smoothing_;
    std::array<int, 6> axisSign_;
    bool outputChanged_ = false;
};

}  // namespace tir
