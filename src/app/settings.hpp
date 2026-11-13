// User settings shared by the CLI, the menu-bar app and `trackir-mac check`:
// ~/Library/Application Support/TrackIR-macOS/settings.ini, one `key = value` per line.
#pragma once

#include "protocol/camera.hpp"
#include "vision/pose.hpp"

#include <array>
#include <string>

namespace tir {

// Which head tracker drives the outputs.
enum class HeadSource { TrackIR = 0, AirPods = 1 };

struct Settings {
    HeadSource source = HeadSource::TrackIR;
    std::string profile;                  // TrackIR profile XML; empty = 1:1 curves
    double smoothing = 0.3;               // 0..0.95
    ClipType clipType = ClipType::TrackClip;
    double clipLeg = 116.052, clipBase = 69.621;  // mm
    Vec3 pivot{};                         // head pivot relative to the clip centre, mm
    CameraSettings camera;                // threshold, exposure, IR intensity
    bool udp = false;                     // opentrack "UDP over network" output
    std::string udpHost = "127.0.0.1";
    int udpPort = 4242;
    std::string gameKeys;                 // CSV for TrackIR Enhanced titles
    // Hardware corrections written by `trackir-mac check`, applied before the profile.
    std::array<int, 6> axisSign{{1, 1, 1, 1, 1, 1}};  // yaw, pitch, roll, x, y, z
    double focalScale = 1.0;              // multiplies the lens focal length
    std::string hotkeyRecenter = "F12";   // menu-bar app; "none" disables
    std::string hotkeyPause = "F9";
    std::string hotkeySource = "none";    // switches between TrackIR and AirPods
    // AirPods: direction fixes for yaw, pitch, roll, and the yaw drift correction in degrees per second (0 = off).
    std::array<int, 3> airpodsAxisSign{{1, 1, 1}};
    double airpodsDrift = 0;

    static std::string defaultPath();     // resourceDirectory() + "/settings.ini"
    // Missing file or keys keep the defaults; unknown keys are ignored.
    static Settings load(const std::string& path = defaultPath());
    bool save(const std::string& path = defaultPath()) const;
};

// User profiles live here (TrackIR's own are copied in by `extract-fpga`).
std::string profileDirectory();

}  // namespace tir
