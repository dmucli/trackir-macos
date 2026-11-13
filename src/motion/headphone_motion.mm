#include "headphone_motion.hpp"

#import <CoreMotion/CoreMotion.h>
#import <Foundation/Foundation.h>

#include <condition_variable>
#include <deque>
#include <mutex>

namespace tir {

struct HeadphoneMotionState {
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<HeadphoneSample> samples;
    HeadphoneMotion::Status status;

    // Main thread only.
    id manager = nil;   // CMHeadphoneMotionManager
    id delegate = nil;  // TIRHeadphoneDelegate
    NSOperationQueue* queue = nil;
    bool wanted = false;
};

}  // namespace tir

using tir::HeadphoneMotionState;

API_AVAILABLE(macos(14.0))
@interface TIRHeadphoneDelegate : NSObject <CMHeadphoneMotionManagerDelegate>
- (instancetype)initWithState:(const std::shared_ptr<HeadphoneMotionState>&)state;
@end

@implementation TIRHeadphoneDelegate {
    std::weak_ptr<HeadphoneMotionState> _state;
}

- (instancetype)initWithState:(const std::shared_ptr<HeadphoneMotionState>&)state
{
    if ((self = [super init]))
        _state = state;
    return self;
}

- (void)headphoneMotionManagerDidConnect:(CMHeadphoneMotionManager*)manager
{
    if (auto s = _state.lock()) {
        std::lock_guard<std::mutex> lock(s->mutex);
        s->status.connected = true;
        s->status.connects++;
    }
}

- (void)headphoneMotionManagerDidDisconnect:(CMHeadphoneMotionManager*)manager
{
    if (auto s = _state.lock()) {
        std::lock_guard<std::mutex> lock(s->mutex);
        s->status.connected = false;
        s->status.disconnects++;
    }
}

@end

namespace tir {

namespace {

// Main thread.
void beginUpdates(const std::shared_ptr<HeadphoneMotionState>& s)
{
    if (@available(macOS 14.0, *)) {
        if (!s->wanted)
            return;
        if (!s->manager) {
            CMHeadphoneMotionManager* m = [[CMHeadphoneMotionManager alloc] init];
            TIRHeadphoneDelegate* d = [[TIRHeadphoneDelegate alloc] initWithState:s];
            m.delegate = d;
            s->manager = m;
            s->delegate = d;
            s->queue = [[NSOperationQueue alloc] init];
            s->queue.name = @"TrackIR-macOS headphone motion";
            s->queue.maxConcurrentOperationCount = 1;
        }
        CMHeadphoneMotionManager* m = s->manager;
        bool available = m.deviceMotionAvailable;
        {
            std::lock_guard<std::mutex> lock(s->mutex);
            s->status.available = available;
        }
        if (!available || m.deviceMotionActive)
            return;
        std::weak_ptr<HeadphoneMotionState> weak = s;
        [m startDeviceMotionUpdatesToQueue:s->queue
                               withHandler:^(CMDeviceMotion* motion, NSError* error) {
                                   auto state = weak.lock();
                                   if (!state || !motion)
                                       return;
                                   CMQuaternion q = motion.attitude.quaternion;
                                   {
                                       std::lock_guard<std::mutex> lock(state->mutex);
                                       state->samples.push_back(
                                           {Quat{q.w, q.x, q.y, q.z}, std::chrono::steady_clock::now()});
                                       while (state->samples.size() > 256)
                                           state->samples.pop_front();
                                   }
                                   state->wake.notify_one();
                               }];
    }
}

// Main thread.
void endUpdates(const std::shared_ptr<HeadphoneMotionState>& s)
{
    if (@available(macOS 14.0, *)) {
        if (s->manager)
            [(CMHeadphoneMotionManager*)s->manager stopDeviceMotionUpdates];
    }
}

}  // namespace

HeadphoneMotion::HeadphoneMotion() : state_(std::make_shared<HeadphoneMotionState>()) {}

HeadphoneMotion::~HeadphoneMotion()
{
    stop();
}

bool HeadphoneMotion::supported()
{
    if (@available(macOS 14.0, *))
        return true;
    return false;
}

HeadphoneMotion::Permission HeadphoneMotion::permission()
{
    if (@available(macOS 14.0, *)) {
        switch ([CMHeadphoneMotionManager authorizationStatus]) {
        case CMAuthorizationStatusAuthorized:
            return Permission::Authorized;
        case CMAuthorizationStatusDenied:
        case CMAuthorizationStatusRestricted:
            return Permission::Denied;
        default:
            return Permission::NotDetermined;
        }
    }
    return Permission::Denied;
}

void HeadphoneMotion::start()
{
    auto s = state_;
    dispatch_async(dispatch_get_main_queue(), ^{
        s->wanted = true;
        beginUpdates(s);
    });
}

void HeadphoneMotion::stop()
{
    // The blocks keep the state alive until the main thread has torn the manager down.
    auto s = state_;
    dispatch_async(dispatch_get_main_queue(), ^{
        s->wanted = false;
        endUpdates(s);
        s->manager = nil;
        s->delegate = nil;
        s->queue = nil;
    });
}

void HeadphoneMotion::restart()
{
    auto s = state_;
    dispatch_async(dispatch_get_main_queue(), ^{
        endUpdates(s);
        beginUpdates(s);
    });
}

void HeadphoneMotion::wait(std::vector<HeadphoneSample>& out, std::chrono::milliseconds timeout)
{
    out.clear();
    std::unique_lock<std::mutex> lock(state_->mutex);
    if (state_->samples.empty())
        state_->wake.wait_for(lock, timeout, [this] { return !state_->samples.empty(); });
    out.assign(state_->samples.begin(), state_->samples.end());
    state_->samples.clear();
}

HeadphoneMotion::Status HeadphoneMotion::status()
{
    std::lock_guard<std::mutex> lock(state_->mutex);
    Status s = state_->status;
    state_->status.connects = state_->status.disconnects = 0;
    return s;
}

}  // namespace tir
