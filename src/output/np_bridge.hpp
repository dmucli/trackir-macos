// macOS side of the NPClient bridge: plays the role TrackIR5.exe plays for NPClient.dll.
#pragma once

#include "common/tir_bridge.h"
#include "vision/pose.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <string>

namespace tir {

class NpBridge {
public:
    struct Events {
        std::function<void()> recenter;
        std::function<void(uint32_t profileId)> profileRegistered;
        std::function<void(bool)> transmissionChanged;
    };

    explicit NpBridge(std::string path = TIR_BRIDGE_UNIX_PATH);
    ~NpBridge();
    NpBridge(const NpBridge&) = delete;
    NpBridge& operator=(const NpBridge&) = delete;

    bool open(std::string& error);
    void setEvents(Events events) { events_ = std::move(events); }
    // Keys for "TrackIR Enhanced" titles, by profile id (CSV: id,16 hex digits).
    bool loadGameKeys(const std::string& csvPath, std::string& error);

    // Drains client commands; call often.
    void pollCommands();
    // Publishes a pose already transformed by the profile (degrees / centimetres).
    void publish(const HeadPose& pose, bool tracking);
    void heartbeat();
    // Unscaled pose for native clients (tir_bridge.pose); `flags` is a TIR_POSE_* mask.
    void publishNative(const HeadPose& pose, uint32_t flags);

    uint32_t profileId() const { return profileId_; }

private:
    std::string path_;
    int fd_ = -1;
    tir_bridge* bridge_ = nullptr;
    uint32_t cmdTail_ = 0;
    uint32_t profileId_ = 0;
    bool transmitting_ = false;
    uint16_t frameSignature_ = 0;
    std::map<uint32_t, std::array<uint8_t, 8>> keys_;
    Events events_;
};

// TrackIR's final scaling: degrees/180 and centimetres/50 to +-16383, pitch and x negated (00497120).
np_trackir_data toTrackIRData(const HeadPose& pose);

}  // namespace tir
