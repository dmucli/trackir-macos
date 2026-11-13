// AirPods head tracking through CoreMotion's CMHeadphoneMotionManager (macOS 14 or later). The model follows
// AirTracker's HeadphoneMotionSource (https://github.com/crippler95/airtracker, MIT licence).
//
// Works with headphones that support spatial-audio head tracking: AirPods (3rd generation), AirPods 4, AirPods Pro,
// AirPods Max, Beats Fit Pro. They only report motion while they are the Mac's active audio output, and the app
// needs the Motion & Fitness permission (asked for on the first start).
#pragma once

#include "motion/headphone_pose.hpp"

#include <chrono>
#include <memory>
#include <string>
#include <vector>

namespace tir {

struct HeadphoneSample {
    Quat attitude;
    std::chrono::steady_clock::time_point time;
};

struct HeadphoneMotionState;

class HeadphoneMotion {
public:
    enum class Permission { NotDetermined, Authorized, Denied };

    struct Status {
        bool available = true;   // false: this Mac cannot do headphone motion at all
        bool connected = false;  // headphones with head tracking are connected
        int connects = 0;        // connect events since the last status() call
        int disconnects = 0;
    };

    HeadphoneMotion();
    ~HeadphoneMotion();
    HeadphoneMotion(const HeadphoneMotion&) = delete;
    HeadphoneMotion& operator=(const HeadphoneMotion&) = delete;

    static bool supported();  // macOS 14 or later
    static Permission permission();

    // Starts the motion updates. The manager lives on the main thread (where CoreMotion delivers its connect and
    // disconnect events), so this needs a running main run loop, as in the menu-bar app.
    void start();
    void stop();
    void restart();  // stop and start again, for when samples stall

    // Moves the queued samples into `out`, waiting up to `timeout` if none are queued yet.
    void wait(std::vector<HeadphoneSample>& out, std::chrono::milliseconds timeout);
    Status status();

private:
    std::shared_ptr<HeadphoneMotionState> state_;
};

}  // namespace tir
