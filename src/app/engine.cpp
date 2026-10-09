#include "engine.hpp"

#include "app/resources.hpp"
#include "common/tir_bridge.h"
#include "output/np_bridge.hpp"
#include "output/udp_sender.hpp"
#include "protocol/frame.hpp"
#include "usb/usb_link.hpp"

#include <algorithm>
#include <cmath>
#include <thread>

namespace tir {

namespace {

struct Smoother {
    double alpha = 0;
    bool primed = false;
    HeadPose state;
    HeadPose step(const HeadPose& in)
    {
        if (!primed) {
            state = in;
            primed = true;
            return state;
        }
        auto mix = [&](double& s, double v) { s = alpha * s + (1 - alpha) * v; };
        mix(state.yaw, in.yaw);
        mix(state.pitch, in.pitch);
        mix(state.roll, in.roll);
        mix(state.x, in.x);
        mix(state.y, in.y);
        mix(state.z, in.z);
        return state;
    }
};

HeadPose withSigns(HeadPose p, const std::array<int, 6>& s)
{
    p.yaw *= s[0];
    p.pitch *= s[1];
    p.roll *= s[2];
    p.x *= s[3];
    p.y *= s[4];
    p.z *= s[5];
    return p;
}

}  // namespace

Engine::Engine(EngineOptions options)
    : options_(std::move(options)),
      profile_(loadProfile(options_.settings.profile)),
      smoothing_(options_.settings.smoothing),
      axisSign_(options_.settings.axisSign)
{
}

Engine::~Engine() = default;

Profile Engine::loadProfile(const std::string& path, std::string* error)
{
    if (path.empty())
        return Profile::linear();
    try {
        return Profile::load(path);
    } catch (const std::exception& e) {
        if (error)
            *error = e.what();
        return Profile::linear();
    }
}

void Engine::setProfile(const Profile& profile)
{
    std::lock_guard<std::mutex> lock(mutex_);
    profile_ = profile;
    outputChanged_ = true;
}

void Engine::setSmoothing(double alpha)
{
    std::lock_guard<std::mutex> lock(mutex_);
    smoothing_ = std::clamp(alpha, 0.0, 0.95);
    outputChanged_ = true;
}

void Engine::setAxisSigns(const std::array<int, 6>& signs)
{
    std::lock_guard<std::mutex> lock(mutex_);
    axisSign_ = signs;
    outputChanged_ = true;
}

EngineStatus Engine::status() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    EngineStatus s = status_;
    s.paused = paused_;
    return s;
}

void Engine::setState(EngineStatus::State state, const std::string& message)
{
    std::lock_guard<std::mutex> lock(mutex_);
    status_.state = state;
    status_.message = message;
    if (state != EngineStatus::State::Streaming) {
        status_.framesPerSecond = status_.posesPerSecond = 0;
        status_.blobs.clear();
        status_.clipVisible = false;
    }
}

int Engine::run()
{
    std::string error;
    NpBridge bridge;
    if (options_.bridge) {
        if (!bridge.open(error)) {
            log("NPClient bridge disabled: " + error);
        } else {
            log(std::string("bridge: ") + TIR_BRIDGE_UNIX_PATH);
            if (!options_.settings.gameKeys.empty() && !bridge.loadGameKeys(options_.settings.gameKeys, error))
                log("game keys: " + error);
            bridge.setEvents({[this] { recenter_ = true; },
                              [this](uint32_t id) { log("game registered profile id " + std::to_string(id)); },
                              [this](bool on) { log(on ? "game started data transmission" : "game stopped data transmission"); }});
        }
    }
    UdpSender udp;
    const Settings& s = options_.settings;
    if (s.udp) {
        if (udp.open(s.udpHost, s.udpPort, error))
            log("opentrack UDP: " + s.udpHost + ":" + std::to_string(s.udpPort));
        else
            log("UDP disabled: " + error);
    }

    int result = 0;
    auto lastAttempt = Clock::now() - std::chrono::seconds(10);
    while (!stop_) {
        if (Clock::now() - lastAttempt >= std::chrono::seconds(1)) {
            lastAttempt = Clock::now();
            result = runCamera(bridge, udp);
            if (!options_.waitForCamera || stop_)
                break;
        }
        // Waiting for a camera: keep the bridge alive so games see a running (idle) TrackIR.
        bridge.pollCommands();
        bridge.heartbeat();
        bridge.publishNative(HeadPose{}, 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    setState(EngineStatus::State::Stopped, "stopped");
    return stop_ ? 0 : result;
}

int Engine::runCamera(NpBridge& bridge, UdpSender& udp)
{
    const Settings& s = options_.settings;
    auto devices = UsbLink::enumerate();
    const UsbDeviceInfo* chosen = nullptr;
    for (const auto& d : devices) {
        const CameraModel* m = findCameraModel(d.productId);
        if (m && m->supported && (!options_.productId || options_.productId == d.productId)) {
            chosen = &d;
            break;
        }
    }
    if (!chosen) {
        setState(EngineStatus::State::NoCamera, "no supported TrackIR camera found");
        return 1;
    }
    const CameraModel& model = *findCameraModel(chosen->productId);
    setState(EngineStatus::State::Connecting, model.name);

    std::string error;
    auto link = UsbLink::open(*chosen, error);
    if (!link) {
        setState(EngineStatus::State::Failed, std::string("cannot open ") + model.name + ": " + error);
        log(status().message);
        return 1;
    }
    log(std::string(model.name) + " (" + model.libraryClass + ") serial " + chosen->serial + ", " +
        link->describeEndpoints());

    std::vector<std::string> messages;
    DriverResources resources = loadDriverResources(model, messages);
    for (const auto& m : messages)
        log(m);

    uint32_t serial = uint32_t(std::strtoul(chosen->serial.c_str(), nullptr, 10));
    LensModel lens = lensFor(model, serial);
    lens.focalPx *= s.focalScale;
    PoseTracker tracker(lens, MarkerModel::triangle(s.clipLeg, s.clipBase, s.clipType));
    Recentering centre(s.pivot);

    Profile profile;
    Smoother smoother;
    std::array<int, 6> signs{};
    auto reloadOutput = [&] {
        std::lock_guard<std::mutex> lock(mutex_);
        profile = profile_;
        smoother.alpha = smoothing_;
        signs = axisSign_;
        outputChanged_ = false;
    };
    reloadOutput();
    log("profile: " + profile.name);

    auto driver = makeCameraDriver(model, *link, resources);
    driver->log = [this](const std::string& m) { log("camera: " + m); };
    driver->applySettings(s.camera);
    driver->connect();
    driver->start();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        status_.sensorWidth = model.width;
        status_.sensorHeight = model.height;
    }

    int dumped = 0;
    unsigned frames = 0, poses = 0;
    HeadPose out, frozen;
    bool wasPaused = false;
    uint32_t lastFlags = 0;
    auto lastStats = Clock::now(), lastBeat = Clock::now(), lastPose = Clock::now();
    Frame frame;
    std::vector<uint8_t> packet;
    int result = 0;
    while (!stop_) {
        if (link->waitPacket(packet, std::chrono::milliseconds(10))) {
            if (onPacket && dumped < options_.dumpPackets) {
                onPacket(packet.data(), packet.size());
                dumped++;
            }
            if (classifyPacket(packet.data(), packet.size()) == PacketKind::Frame) {
                if (decodeFrame(packet.data(), packet.size(), model.width, model.height, frame)) {
                    frames++;
                    auto blobs = extractBlobs(frame.segments);
                    std::sort(blobs.begin(), blobs.end(), [](const Blob& a, const Blob& b) { return a.weight > b.weight; });
                    if (blobs.size() > 16)
                        blobs.resize(16);
                    auto pose = tracker.update(blobs);
                    bool paused = paused_;
                    if (outputChanged_)
                        reloadOutput();
                    HeadPose measured;
                    if (pose) {
                        poses++;
                        lastPose = Clock::now();
                        if (!centre.centred() || recenter_.exchange(false)) {
                            centre.recenter(*pose);
                            tracker.setReference(*pose);
                        }
                        measured = centre.relative(*pose);
                        out = profile.apply(smoother.step(withSigns(measured, signs)));
                    }
                    if (paused && !wasPaused)
                        frozen = out;
                    wasPaused = paused;
                    const HeadPose& published = paused ? frozen : out;
                    if (pose || paused) {
                        bridge.publish(published, true);
                        udp.send(published);
                    }
                    uint32_t flags = (pose ? TIR_POSE_TRACKING : 0) | (paused ? TIR_POSE_PAUSED | TIR_POSE_TRACKING : 0);
                    // Brief dropouts (a marker blinking out) keep the last pose rather than snapping the view back.
                    if (!pose && !paused && Clock::now() - lastPose < std::chrono::milliseconds(500))
                        flags = lastFlags;
                    bridge.publishNative(published, flags);
                    lastFlags = flags;

                    std::lock_guard<std::mutex> lock(mutex_);
                    status_.blobs = std::move(blobs);
                    status_.clipVisible = bool(pose);
                    if (pose) {
                        status_.measured = measured;
                        status_.distanceCm = pose->translation.norm() / 10.0;
                        status_.poseCounter++;
                    }
                    status_.output = published;
                }
            } else {
                driver->handleReply(packet.data(), packet.size());
            }
        }
        auto now = Clock::now();
        driver->tick(now);
        bridge.pollCommands();
        if (now - lastBeat > std::chrono::milliseconds(100)) {
            bridge.heartbeat();
            lastBeat = now;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            status_.cameraState = driver->describeState();
            if (driver->streaming() && status_.state != EngineStatus::State::Streaming) {
                status_.state = EngineStatus::State::Streaming;
                status_.message = model.name;
            }
        }
        if (link->disconnected()) {
            log("camera disconnected");
            setState(EngineStatus::State::NoCamera, "camera disconnected");
            result = 1;
            break;
        }
        if (driver->failed()) {
            log("camera initialisation failed");
            setState(EngineStatus::State::Failed, "camera initialisation failed");
            result = 1;
            break;
        }
        if (now - lastStats >= std::chrono::seconds(1)) {
            EngineStatus snapshot;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                status_.framesPerSecond = frames;
                status_.posesPerSecond = poses;
                snapshot = status_;
            }
            snapshot.paused = paused_;
            if (onSecond)
                onSecond(snapshot);
            frames = poses = 0;
            lastStats = now;
        }
    }
    if (!link->disconnected())
        driver->shutdown();
    bridge.publishNative(HeadPose{}, 0);
    return result;
}

}  // namespace tir
