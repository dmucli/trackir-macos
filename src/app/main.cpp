// trackir-mac: macOS replacement for TrackIR5.exe's tracking path.
//
//   trackir-mac list
//   trackir-mac extract-fpga <TrackIR5.exe> [out-dir]
//   trackir-mac run [options]
//   trackir-mac check [options]
//   trackir-mac recenter
#include "app/engine.hpp"
#include "app/resources.hpp"
#include "app/settings.hpp"
#include "common/tir_bridge.h"
#include "protocol/frame.hpp"
#include "usb/usb_link.hpp"
#include "vision/blobs.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

using namespace tir;

namespace {

Engine* gEngine = nullptr;

void onSignal(int sig)
{
    if (!gEngine)
        return;
    if (sig == SIGUSR1)
        gEngine->recenter();
    else
        gEngine->stop();
}

void usage()
{
    std::puts(
        "usage:\n"
        "  trackir-mac list                         list NaturalPoint USB devices\n"
        "  trackir-mac extract-fpga <TrackIR5.exe> [dir]\n"
        "                                           copy FPGA images/keys (and TrackIR's profiles) from your own\n"
        "                                           TrackIR 5.5.3 install\n"
        "  trackir-mac run [options]                track and publish the head pose\n"
        "  trackir-mac check [options]              guided test of axis directions, distance and jitter\n"
        "  trackir-mac recenter                     recentre a running tracker\n"
        "  trackir-mac parse-dump <file>            decode packets saved with --dump (one hex packet per line)\n"
        "\n"
        "Defaults come from ~/Library/Application Support/TrackIR-macOS/settings.ini (written by the menu-bar\n"
        "app and by `check`); options override them for one run.\n"
        "\n"
        "run options:\n"
        "  --profile <file.xml>       TrackIR profile (Profiles/*.xml); default: 1:1 curves\n"
        "  --udp <host[:port]>        also send opentrack UDP (default port 4242)\n"
        "  --no-bridge                do not publish to Wine NPClient games / the X-Plane plugin\n"
        "  --game-keys <csv>          profile_id,16-hex-digit key lines for TrackIR Enhanced titles\n"
        "  --smoothing <0..0.95>      exponential smoothing factor (default 0.3)\n"
        "  --threshold <0..255>       blob threshold (default 150)\n"
        "  --exposure <n>             exposure (Rev35 default 120, classic default 479)\n"
        "  --ir <n>                   IR illumination intensity (0 = off, for TrackClip PRO)\n"
        "  --clip-type <clip|pro>     TrackClip (reflective, default) or TrackClip PRO (LEDs)\n"
        "  --clip <leg,base>          marker triangle in mm (default 116.052,69.621)\n"
        "  --pivot <x,y,z>            head pivot relative to the clip centre, mm\n"
        "  --pid <hex>                use this product id when several cameras are connected\n"
        "  --wait                     wait for a camera and reconnect after unplugging\n"
        "  --dump <n>                 print the first n raw packets as hex\n"
        "  --verbose                  print tracking statistics every second");
}

bool parseList(const std::string& s, double out[], int count)
{
    size_t pos = 0;
    for (int i = 0; i < count; i++) {
        size_t next = s.find(',', pos);
        try {
            out[i] = std::stod(s.substr(pos, next - pos));
        } catch (...) {
            return false;
        }
        if (next == std::string::npos)
            return i == count - 1;
        pos = next + 1;
    }
    return true;
}

struct RunOptions {
    EngineOptions engine;
    bool verbose = false;
};

RunOptions parseRunOptions(int argc, char** argv)
{
    RunOptions o;
    o.engine.settings = Settings::load();
    Settings& s = o.engine.settings;
    for (int i = 2; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc)
                throw std::runtime_error("missing value for " + a);
            return argv[++i];
        };
        if (a == "--profile") s.profile = next();
        else if (a == "--udp") {
            std::string v = next();
            size_t colon = v.rfind(':');
            s.udp = true;
            if (colon != std::string::npos && v.find(']') == std::string::npos) {
                s.udpHost = v.substr(0, colon);
                s.udpPort = std::stoi(v.substr(colon + 1));
            } else {
                s.udpHost = v;
            }
        }
        else if (a == "--no-bridge") o.engine.bridge = false;
        else if (a == "--game-keys") s.gameKeys = next();
        else if (a == "--smoothing") s.smoothing = std::clamp(std::stod(next()), 0.0, 0.95);
        else if (a == "--threshold") s.camera.threshold = std::stoi(next());
        else if (a == "--exposure") s.camera.exposure = std::stoi(next());
        else if (a == "--ir") s.camera.irIntensity = std::stoi(next());
        else if (a == "--pid") o.engine.productId = uint16_t(std::stoul(next(), nullptr, 16));
        else if (a == "--dump") o.engine.dumpPackets = std::stoi(next());
        else if (a == "--wait") o.engine.waitForCamera = true;
        else if (a == "--verbose") o.verbose = true;
        else if (a == "--clip-type") {
            std::string v = next();
            if (v == "pro")
                s.clipType = ClipType::TrackClipPro;
            else if (v == "clip")
                s.clipType = ClipType::TrackClip;
            else
                throw std::runtime_error("--clip-type expects clip or pro");
        }
        else if (a == "--clip") {
            double v[2];
            if (!parseList(next(), v, 2))
                throw std::runtime_error("--clip expects leg,base");
            s.clipLeg = v[0];
            s.clipBase = v[1];
        } else if (a == "--pivot") {
            double v[3];
            if (!parseList(next(), v, 3))
                throw std::runtime_error("--pivot expects x,y,z");
            s.pivot = {v[0], v[1], v[2]};
        } else {
            throw std::runtime_error("unknown option " + a);
        }
    }
    if (!s.profile.empty()) {
        std::string error;
        Engine::loadProfile(s.profile, &error);
        if (!error.empty())
            throw std::runtime_error(error);
    }
    return o;
}

std::string hex(const uint8_t* p, size_t n)
{
    std::string s;
    char b[4];
    for (size_t i = 0; i < n; i++) {
        snprintf(b, sizeof(b), "%02x", p[i]);
        s += b;
    }
    return s;
}

int cmdList()
{
    auto devices = UsbLink::enumerate();
    if (devices.empty()) {
        std::puts("no NaturalPoint (131d) devices found");
        return 1;
    }
    for (const auto& d : devices) {
        const CameraModel* m = findCameraModel(d.productId);
        std::printf("131d:%04x  %-22s %-12s serial=%s location=0x%08x%s\n", d.productId,
                    m ? m->name : "unknown NaturalPoint device", m ? m->libraryClass : "-", d.serial.c_str(),
                    d.locationId, m && !m->supported ? "  (not supported yet)" : "");
    }
    return 0;
}

int cmdExtract(int argc, char** argv)
{
    if (argc < 3) {
        usage();
        return 2;
    }
    std::string out = argc > 3 ? argv[3] : resourceDirectory();
    std::vector<std::string> messages;
    bool ok = extractVendorBlobs(argv[2], out, messages);
    for (const auto& m : messages)
        std::puts(m.c_str());

    // TrackIR's stock profiles sit next to the executable; the menu-bar app lists them from profileDirectory().
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path stock = fs::path(argv[2]).parent_path() / "Profiles";
    if (argc <= 3 && fs::is_directory(stock, ec)) {
        fs::create_directories(profileDirectory(), ec);
        for (const auto& entry : fs::directory_iterator(stock, ec)) {
            if (entry.path().extension() != ".xml")
                continue;
            fs::path target = fs::path(profileDirectory()) / entry.path().filename();
            if (fs::exists(target, ec))
                continue;
            if (fs::copy_file(entry.path(), target, ec))
                std::printf("copied profile %s\n", target.c_str());
        }
    }
    return ok ? 0 : 1;
}

// Queues NP_ReCenter through the bridge, as a game would.
int cmdRecenter()
{
    int fd = open(TIR_BRIDGE_UNIX_PATH, O_RDWR);
    if (fd < 0) {
        std::puts("no tracker is running");
        return 1;
    }
    void* mem = mmap(nullptr, TIR_BRIDGE_FILE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (mem == MAP_FAILED) {
        std::puts("no tracker is running");
        return 1;
    }
    auto* b = static_cast<tir_bridge*>(mem);
    bool running = b->magic == TIR_BRIDGE_MAGIC && b->daemon_pid > 0 && kill(b->daemon_pid, 0) == 0;
    if (running)
        tir_bridge_push_command(b, NP_CMD_RECENTER, 0);
    munmap(mem, TIR_BRIDGE_FILE_SIZE);
    if (!running) {
        std::puts("no tracker is running");
        return 1;
    }
    return 0;
}

int cmdParseDump(int argc, char** argv)
{
    if (argc < 3) {
        usage();
        return 2;
    }
    std::ifstream f(argv[2]);
    std::string line;
    while (std::getline(f, line)) {
        std::vector<uint8_t> p;
        for (size_t i = 0; i + 1 < line.size(); i += 2)
            p.push_back(uint8_t(std::stoul(line.substr(i, 2), nullptr, 16)));
        PacketKind kind = classifyPacket(p.data(), p.size());
        if (kind == PacketKind::Frame) {
            Frame frame;
            bool ok = decodeFrame(p.data(), p.size(), 640, 480, frame);
            auto blobs = extractBlobs(frame.segments);
            std::printf("frame #%u type %u %s: %zu runs, %zu blobs", frame.counter, frame.type, ok ? "ok" : "BAD",
                        frame.segments.size(), blobs.size());
            for (const auto& b : blobs)
                std::printf("  (%.1f,%.1f w=%.0f)", b.x, b.y, b.weight);
            std::puts("");
        } else {
            std::printf("reply type 0x%02x, %zu bytes\n", p.size() > 1 ? p[1] : 0, p.size());
        }
    }
    return 0;
}

int cmdRun(RunOptions opt)
{
    Engine engine(opt.engine);
    engine.log = [](const std::string& s) { std::printf("%s\n", s.c_str()); };
    engine.onPacket = [](const uint8_t* p, size_t n) { std::printf("%s\n", hex(p, n).c_str()); };
    if (opt.verbose)
        engine.onSecond = [](const EngineStatus& s) {
            const HeadPose& o = s.output;
            std::printf("[%s] %u frames/s, %u poses/s, %zu blobs | yaw %6.1f pitch %6.1f roll %6.1f  x %5.1f y %5.1f "
                        "z %5.1f%s\n",
                        s.cameraState.c_str(), s.framesPerSecond, s.posesPerSecond, s.blobs.size(), o.yaw, o.pitch,
                        o.roll, o.x, o.y, o.z, s.paused ? "  (paused)" : "");
        };
    gEngine = &engine;
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    std::signal(SIGUSR1, onSignal);
    int rc = engine.run();
    gEngine = nullptr;
    if (rc != 0)
        std::printf("%s\n", engine.status().message.c_str());
    return rc;
}

// ---------------------------------------------------------------------------------------------------------------
// `check`: walks the user through one movement per axis and verifies the direction the port reports.

struct Sample {
    double v[6];
};

double axisValue(const HeadPose& p, int a)
{
    const double v[6] = {p.yaw, p.pitch, p.roll, p.x, p.y, p.z};
    return v[a];
}

bool waitEnter()
{
    std::string line;
    return bool(std::getline(std::cin, line));
}

// Averages the measured pose over `ms` milliseconds; fails if the clip is not visible.
std::optional<Sample> capture(Engine& engine, int ms, double* noise = nullptr)
{
    std::vector<Sample> samples;
    uint64_t lastCounter = engine.status().poseCounter;
    auto end = Clock::now() + std::chrono::milliseconds(ms);
    while (Clock::now() < end) {
        EngineStatus s = engine.status();
        if (s.poseCounter != lastCounter && s.clipVisible) {
            lastCounter = s.poseCounter;
            Sample x;
            for (int a = 0; a < 6; a++)
                x.v[a] = axisValue(s.measured, a);
            samples.push_back(x);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (samples.size() < 10)
        return std::nullopt;
    Sample mean{};
    for (const auto& x : samples)
        for (int a = 0; a < 6; a++)
            mean.v[a] += x.v[a] / double(samples.size());
    if (noise) {
        for (int a = 0; a < 6; a++) {
            double var = 0;
            for (const auto& x : samples)
                var += (x.v[a] - mean.v[a]) * (x.v[a] - mean.v[a]);
            noise[a] = std::sqrt(var / double(samples.size()));
        }
    }
    return mean;
}

bool waitForClip(Engine& engine, int seconds)
{
    auto end = Clock::now() + std::chrono::seconds(seconds);
    int stableFor = 0;
    while (Clock::now() < end) {
        EngineStatus s = engine.status();
        if (s.state == EngineStatus::State::Failed || s.state == EngineStatus::State::Stopped)
            return false;
        std::printf("\r  camera: %-28s markers seen: %-3zu %s   ", s.cameraState.empty() ? s.message.c_str() : s.cameraState.c_str(),
                    s.blobs.size(), s.clipVisible ? "clip found" : "");
        std::fflush(stdout);
        stableFor = s.clipVisible && s.blobs.size() == 3 ? stableFor + 1 : 0;
        if (stableFor >= 10) {
            std::puts("");
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    std::puts("");
    return false;
}

int cmdCheck(RunOptions opt)
{
    static const char* kAxis[6] = {"yaw", "pitch", "roll", "x", "y", "z"};
    static const char* kUnit[6] = {"deg", "deg", "deg", "cm", "cm", "cm"};
    struct Move {
        int axis;
        const char* instruction;
    };
    static const Move kMoves[6] = {
        {0, "TURN your head to the LEFT (about 30 degrees), keep looking that way"},
        {1, "Look UP (about 20 degrees) by tilting your head back"},
        {2, "TILT your head to the LEFT, left ear towards your left shoulder"},
        {3, "SLIDE your head to the RIGHT (about 10 cm) without turning it"},
        {4, "Move your head UP (sit up taller, about 5-10 cm)"},
        {5, "LEAN towards the screen (about 10 cm)"},
    };

    Settings saved = opt.engine.settings;
    opt.engine.settings.axisSign = {{1, 1, 1, 1, 1, 1}};  // measure the uncorrected directions
    opt.engine.settings.focalScale = saved.focalScale;
    opt.engine.waitForCamera = true;
    Engine engine(opt.engine);
    std::vector<std::string> logLines;
    std::mutex logMutex;
    engine.log = [&](const std::string& s) {
        std::lock_guard<std::mutex> lock(logMutex);
        logLines.push_back(s);
    };
    gEngine = &engine;
    // No SA_RESTART: Ctrl-C must interrupt the blocking prompt so the camera is shut down cleanly.
    struct sigaction sa {};
    sa.sa_handler = onSignal;
    sigaction(SIGINT, &sa, nullptr);
    std::thread worker([&] { engine.run(); });
    auto finish = [&](int rc) {
        engine.stop();
        worker.join();
        gEngine = nullptr;
        return rc;
    };

    std::puts("TrackIR axis check. Put the clip on as you normally wear it, sit where you normally sit,\n"
              "and answer each prompt by pressing Enter. Ctrl-C quits without saving.\n");
    std::puts("1. Waiting for the camera and all three markers...");
    if (!waitForClip(engine, 60)) {
        EngineStatus s = engine.status();
        std::printf("No stable clip after 60 s (%s, %zu markers). Check the camera faces you and the clip is on.\n",
                    s.message.c_str(), s.blobs.size());
        std::lock_guard<std::mutex> lock(logMutex);
        for (const auto& l : logLines)
            std::printf("  log: %s\n", l.c_str());
        return finish(1);
    }

    std::puts("\n2. Sit normally and look at the centre of your screen. Press Enter, then hold still for 3 seconds.");
    if (!waitEnter())
        return finish(1);
    engine.recenter();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    double noise[6];
    auto rest = capture(engine, 3000, noise);
    if (!rest) {
        std::puts("Lost the clip while holding still. Run `trackir-mac check` again.");
        return finish(1);
    }
    EngineStatus st = engine.status();
    std::printf("   %u frames/s. Jitter while still (standard deviation): yaw %.2f, pitch %.2f, roll %.2f deg; "
                "x %.2f, y %.2f, z %.2f mm\n",
                st.framesPerSecond, noise[0], noise[1], noise[2], noise[3] * 10, noise[4] * 10, noise[5] * 10);
    bool jittery = noise[0] > 0.3 || noise[1] > 0.3 || noise[2] > 0.3;
    std::printf("   %s\n", jittery ? "That is noisy: check for reflections or other IR sources in view." : "Steady.");

    double distance = st.distanceCm;
    std::printf("\n3. The port estimates the clip is %.0f cm from the camera.\n"
                "   Measure the distance from the camera's front to the clip with a tape measure and type it in cm\n"
                "   (or just press Enter to skip): ",
                distance);
    std::fflush(stdout);
    std::string line;
    if (!std::getline(std::cin, line))
        return finish(1);
    double focalScale = saved.focalScale;
    try {
        double measured = std::stod(line);
        if (measured > 20 && measured < 300 && distance > 1) {
            double ratio = measured / distance;
            focalScale = std::clamp(saved.focalScale * ratio, 0.5, 2.0);
            std::printf("   Ratio %.3f -> lens focal length correction %.3f%s\n", ratio, focalScale,
                        std::fabs(ratio - 1) < 0.03 ? " (within 3%, fine either way)" : "");
        }
    } catch (...) {
        std::puts("   skipped");
    }

    std::puts("\n4. Six movements. For each: return to centre, press Enter, do the movement, hold it, press Enter.");
    std::array<int, 6> signs = saved.axisSign;
    int problems = 0;
    for (const Move& m : kMoves) {
        std::printf("\n   Back to centre, then press Enter. ");
        std::fflush(stdout);
        if (!waitEnter())
            return finish(1);
        auto base = capture(engine, 300);
        std::printf("   %s - hold it and press Enter. ", m.instruction);
        std::fflush(stdout);
        if (!waitEnter())
            return finish(1);
        auto held = capture(engine, 500);
        if (!base || !held) {
            std::puts("   Lost the clip during this movement; skipped (try a smaller movement).");
            problems++;
            continue;
        }
        double delta[6];
        for (int a = 0; a < 6; a++)
            delta[a] = held->v[a] - base->v[a];
        // Compare like with like: rotations against rotations, translations against translations.
        int first = m.axis < 3 ? 0 : 3, dominant = first;
        for (int a = first; a < first + 3; a++)
            if (std::fabs(delta[a]) > std::fabs(delta[dominant]))
                dominant = a;
        double v = delta[m.axis];
        double minimum = m.axis < 3 ? 5.0 : 2.0;
        std::printf("   measured: yaw %+.1f pitch %+.1f roll %+.1f deg | x %+.1f y %+.1f z %+.1f cm\n", delta[0],
                    delta[1], delta[2], delta[3], delta[4], delta[5]);
        if (std::fabs(v) < minimum) {
            std::printf("   -> %s barely moved (%.1f %s). Repeat with a bigger movement.\n", kAxis[m.axis], v,
                        kUnit[m.axis]);
            problems++;
        } else if (dominant != m.axis) {
            std::printf("   -> the biggest change was on %s, not %s. That is a bug in the port; please report it.\n",
                        kAxis[dominant], kAxis[m.axis]);
            problems++;
        } else {
            signs[size_t(m.axis)] = v > 0 ? 1 : -1;
            std::printf("   -> %s %s (%+.1f %s)\n", kAxis[m.axis], v > 0 ? "correct" : "INVERTED, will be corrected", v,
                        kUnit[m.axis]);
        }
    }

    std::printf("\nResult: axis signs yaw %+d pitch %+d roll %+d x %+d y %+d z %+d, focal correction %.3f",
                signs[0], signs[1], signs[2], signs[3], signs[4], signs[5], focalScale);
    if (problems)
        std::printf(" (%d step%s not measured)", problems, problems == 1 ? "" : "s");
    std::printf("\nSave to %s? [Y/n] ", Settings::defaultPath().c_str());
    std::fflush(stdout);
    if (!std::getline(std::cin, line))
        return finish(1);
    if (line.empty() || line[0] == 'y' || line[0] == 'Y') {
        Settings updated = Settings::load();
        updated.axisSign = signs;
        updated.focalScale = focalScale;
        std::puts(updated.save() ? "Saved. `trackir-mac run` and the menu-bar app use it from now on." : "Could not save.");
    }
    return finish(problems ? 1 : 0);
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        usage();
        return 2;
    }
    std::string cmd = argv[1];
    try {
        if (cmd == "list")
            return cmdList();
        if (cmd == "extract-fpga")
            return cmdExtract(argc, argv);
        if (cmd == "recenter")
            return cmdRecenter();
        if (cmd == "parse-dump")
            return cmdParseDump(argc, argv);
        if (cmd == "run")
            return cmdRun(parseRunOptions(argc, argv));
        if (cmd == "check")
            return cmdCheck(parseRunOptions(argc, argv));
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    usage();
    return 2;
}
