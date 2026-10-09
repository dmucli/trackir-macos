// Unit and integration tests. The camera emulators implement the device side of the reverse-engineered protocols,
// so they check the drivers against the specification in docs/PROTOCOL.md, not against real hardware.
#include "app/settings.hpp"
#include "common/np_shared.h"
#include "common/tir_bridge.h"
#include "output/filter.hpp"
#include "output/np_bridge.hpp"
#include "output/profile.hpp"
#include "protocol/camera.hpp"
#include "protocol/frame.hpp"
#include "protocol/secure_codec.hpp"
#include "vision/blobs.hpp"
#include "vision/pose.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <functional>
#include <map>
#include <random>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

using namespace tir;

namespace tir {
uint16_t fpgaChecksum(const std::vector<uint8_t>& image);
}

static int gFailures = 0;
static int gChecks = 0;

#define CHECK(cond)                                                                 \
    do {                                                                            \
        gChecks++;                                                                  \
        if (!(cond)) {                                                              \
            gFailures++;                                                            \
            std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
        }                                                                           \
    } while (0)

#define CHECK_NEAR(a, b, tol) CHECK(std::fabs(double(a) - double(b)) <= (tol))

static std::mt19937 gRng(12345);
static uint32_t rng32() { return gRng(); }

// ---------------------------------------------------------------------------------------------------------------

static void testLayouts()
{
    CHECK(sizeof(np_trackir_data) == 0x44);
    CHECK(sizeof(np_shared_block) == 0x1E6);
    CHECK(offsetof(np_shared_block, last_result) == 0x02);
    CHECK(offsetof(np_shared_block, param) == 0x06);
    CHECK(offsetof(np_shared_block, sig_dll) == 0x12);
    CHECK(offsetof(np_shared_block, sig_app) == 0xDA);
    CHECK(offsetof(np_shared_block, data) == 0x1A2);
    CHECK(offsetof(tir_bridge, block) == 24);
    CHECK(offsetof(tir_bridge, cmd_head) == 512);
    CHECK(offsetof(tir_bridge, pose_seq) == 900);
    CHECK(offsetof(tir_bridge, pose) == 912);
    CHECK(sizeof(tir_bridge) <= TIR_BRIDGE_FILE_SIZE);
}

static void testNpEncryption()
{
    np_trackir_data d{};
    d.status = 0;
    d.frame_signature = 1234;
    d.roll = 100.5f;
    d.pitch = -2000.25f;
    d.yaw = 16383.0f;
    d.x = -50;
    d.y = 7;
    d.z = 0.125f;
    np_trackir_data original = d;
    const uint8_t key[8] = {0x31, 0x9f, 0x02, 0xaa, 0x55, 0x10, 0xfe, 0x7b};
    np_data_encrypt(&d, key, rng32);
    CHECK(std::memcmp(&d, &original, 32) != 0);
    CHECK(np_data_decrypt(&d, key) == NP_OK);
    CHECK(d.frame_signature == 1234);
    CHECK(d.yaw == 16383.0f && d.pitch == -2000.25f && d.roll == 100.5f);
    CHECK(d.x == -50 && d.y == 7 && d.z == 0.125f);
    CHECK(d.raw_x == 0 && d.smooth_z == 0);

    np_data_encrypt(&d, key, rng32);
    const uint8_t wrong[8] = {0x30, 0x9f, 0x02, 0xaa, 0x55, 0x10, 0xfe, 0x7b};
    CHECK(np_data_decrypt(&d, wrong) == NP_ERR_CHECKSUM);
}

// Standard XTEA known-answer test (32 cycles).
static void testXtea()
{
    uint32_t key[4] = {0x00010203, 0x04050607, 0x08090A0B, 0x0C0D0E0F};
    uint32_t v[2] = {0x41424344, 0x45464748};
    xteaEncipher(v, key);
    CHECK(v[0] == 0x497DF3D0 && v[1] == 0x72612CB5);
}

static void testSecureCodecRoundTrip()
{
    SecureCodec codec(rng32);
    for (int i = 0; i < 2000; i++) {
        uint8_t a = uint8_t(rng32()), b = uint8_t(rng32()), c = uint8_t(rng32());
        SecurePacket p = codec.field(0x19, a, b, c);
        SecureCodec::deobfuscate(p);
        auto f = SecureCodec::decodeField(p);
        CHECK(f.command == 0x19 && f.a == a && f.b == b && f.c == c);
        int idx = SecureCodec::hiddenIndex(p);
        CHECK(idx >= 6 && idx <= 14);
    }
    for (int step = 0; step < 8; step++)
        for (int i = 0; i < 200; i++) {
            SecurePacket p = codec.handshake(step);
            SecureCodec::deobfuscate(p);
            CHECK(p[0] == 0x1A);
            CHECK(SecureCodec::decodeHandshakeStep(p) == step);
        }
    SecurePacket s = codec.simple(0x13);
    SecureCodec::deobfuscate(s);
    CHECK(s[0] == 0x13);
}

// ---------------------------------------------------------------------------------------------------------------
// Device-side emulation of a CameraRev35 (secure protocol).

struct SecureCameraEmulator : CommandSink {
    SecureKeyTable keys{};
    std::array<uint32_t, 4> session{};
    int lastStep = -1;
    std::deque<std::vector<uint8_t>> replies;
    std::map<int, int> registers;
    std::vector<std::array<uint8_t, 3>> imagerWrites;
    int simpleCommands = 0;
    bool streaming = false;
    bool corruptReplies = false;

    bool send(const uint8_t* data, size_t n) override
    {
        if (n != 24)
            return false;
        SecurePacket p;
        std::memcpy(p.data(), data, 24);
        SecureCodec::deobfuscate(p);
        switch (p[0]) {
        case 0x1A: handshake(p); break;
        case 0x19: { auto f = SecureCodec::decodeField(p); registers[f.a] = f.b << 8 | f.c; break; }
        case 0x23: { auto f = SecureCodec::decodeField(p); imagerWrites.push_back({f.a, f.b, f.c}); break; }
        default: simpleCommands++; break;
        }
        return true;
    }

    void handshake(const SecurePacket& p)
    {
        int step = SecureCodec::decodeHandshakeStep(p);
        int idx = SecureCodec::hiddenIndex(p);
        if (step == 0)
            session = keys[p[0x15] & 7];
        if (step == 4) streaming = true;
        if (step == 5) streaming = false;
        if (step != 7) {
            lastStep = step;
            return;
        }
        size_t len = 14 + (p[idx + 1] >> 6);
        std::vector<uint8_t> r(len + 1);
        for (auto& b : r)
            b = uint8_t(rng32());
        r[1] = 0x20;
        r[2] = 0x01;
        r[1 + 4] = uint8_t(p[idx + 2] ^ p[0x12] ^ idx);
        r[1 + 5] = uint8_t(lastStep ^ p[0x13] ^ p[0x0D]);
        uint8_t nonce[8];
        std::memcpy(nonce, &p[idx >> 1], 8);
        uint32_t v[2], in[2];
        std::memcpy(in, nonce, 8);
        std::memcpy(v, nonce, 8);
        xteaEncipher(v, session.data());
        uint32_t out[2] = {in[0] ^ v[0], in[1] ^ v[1]};
        std::memcpy(&r[1 + 6], out, 8);
        session = {out[0], out[1], 0, 0};
        if (corruptReplies)
            r[1 + 7] ^= 0x40;
        replies.push_back(r);
    }
};

static SecureKeyTable testKeys()
{
    SecureKeyTable k{};
    for (int i = 0; i < 8; i++)
        for (int w = 0; w < 4; w++)
            k[i][w] = 0x01020304u * uint32_t(i + 1) + uint32_t(w) * 0x11111111u;
    return k;
}

static bool pump(CameraDriver& driver, SecureCameraEmulator& cam, const std::function<bool()>& done)
{
    for (int i = 0; i < 400 && !done(); i++) {
        while (!cam.replies.empty()) {
            auto r = cam.replies.front();
            cam.replies.pop_front();
            driver.handleReply(r.data(), r.size());
        }
        driver.tick(Clock::now() + std::chrono::milliseconds(i * 300));
    }
    return done();
}

static void testSecureDriverHandshake()
{
    const CameraModel* model = findCameraModel(0x0159);
    CHECK(model && model->protocol == ProtocolKind::Secure);

    SecureCameraEmulator cam;
    cam.keys = testKeys();
    DriverResources res;
    res.haveKeyTable = true;
    for (int k = 0; k < 8; k++)
        std::memcpy(res.keyTable + k * 16, cam.keys[size_t(k)].data(), 16);

    auto driver = makeCameraDriver(*model, cam, res);
    std::vector<std::string> log;
    driver->log = [&](const std::string& s) { log.push_back(s); };
    CameraSettings settings;
    driver->applySettings(settings);
    driver->connect();
    driver->start();
    CHECK(pump(*driver, cam, [&] { return driver->streaming(); }));
    CHECK(cam.streaming);
    bool verified = false;
    for (const auto& l : log)
        verified |= l.find("XTEA verified") != std::string::npos;
    CHECK(verified);

    CHECK(cam.registers[5] == 300);  // threshold 150 -> 2t
    CHECK(cam.registers[3] == 5);    // video type
    CHECK(cam.registers[9] == 1);    // IR on
    CHECK(cam.imagerWrites.size() == 5);
    if (cam.imagerWrites.size() == 5) {  // exposure 120
        CHECK(cam.imagerWrites[0][0] == 0x35 && cam.imagerWrites[0][1] == 2 && cam.imagerWrites[0][2] == uint8_t(120 << 4));
        CHECK(cam.imagerWrites[3][0] == 0x3B && cam.imagerWrites[3][1] == 0x8F && cam.imagerWrites[3][2] == 120);
    }

    driver->stop();
    CHECK(pump(*driver, cam, [&] { return !cam.streaming && cam.replies.empty(); }));
    CHECK(!driver->streaming());
    CHECK(cam.registers[9] == 0);
}

static void testSecureDriverRejectsForgedCamera()
{
    const CameraModel* model = findCameraModel(0x0159);
    SecureCameraEmulator cam;
    cam.keys = testKeys();
    cam.corruptReplies = true;
    DriverResources res;
    res.haveKeyTable = true;
    for (int k = 0; k < 8; k++)
        std::memcpy(res.keyTable + k * 16, cam.keys[size_t(k)].data(), 16);
    auto driver = makeCameraDriver(*model, cam, res);
    driver->connect();
    driver->start();
    pump(*driver, cam, [&] { return driver->ready(); });
    CHECK(!driver->ready());
}

// Without the key table the driver checks only the reply structure and still completes the handshake.
static void testSecureDriverWithoutKeys()
{
    const CameraModel* model = findCameraModel(0x0159);
    SecureCameraEmulator cam;
    cam.keys = testKeys();
    auto driver = makeCameraDriver(*model, cam, DriverResources{});
    driver->connect();
    driver->start();
    CHECK(pump(*driver, cam, [&] { return driver->streaming(); }));
}

// ---------------------------------------------------------------------------------------------------------------
// Device-side emulation of a CameraRev9 (classic protocol).

struct ClassicCameraEmulator : CommandSink {
    std::vector<uint8_t> loaded;
    bool loading = false;
    std::deque<std::vector<uint8_t>> replies;
    std::vector<std::vector<uint8_t>> log;
    bool streaming = false;

    bool send(const uint8_t* d, size_t n) override
    {
        std::vector<uint8_t> v(d, d + n);
        log.push_back(v);
        switch (d[0]) {
        case 0x1B: loading = true; loaded.clear(); break;
        case 0x1C: if (loading) loaded.insert(loaded.end(), d + 2, d + 2 + d[1]); break;
        case 0x1D: {
            uint16_t sum = fpgaChecksum(loaded);
            replies.push_back({0x00, 0x20, 0x00, 0x00, uint8_t(sum >> 8), uint8_t(sum), 0x00});
            break;
        }
        case 0x14: streaming = n > 1 && d[1] == 0x00; break;
        default: break;
        }
        return true;
    }
};

static void testClassicDriverUploadsFpga()
{
    const CameraModel* model = findCameraModel(0x0157);
    CHECK(model && model->protocol == ProtocolKind::Classic);
    ClassicCameraEmulator cam;
    DriverResources res;
    res.fpgaImage.resize(1000);
    for (auto& b : res.fpgaImage)
        b = uint8_t(rng32());
    auto driver = makeCameraDriver(*model, cam, res);
    driver->connect();
    driver->start();
    for (int i = 0; i < 20 && !driver->streaming(); i++) {
        while (!cam.replies.empty()) {
            auto r = cam.replies.front();
            cam.replies.pop_front();
            driver->handleReply(r.data(), r.size());
        }
    }
    CHECK(driver->streaming());
    CHECK(cam.streaming);
    CHECK(cam.loaded == res.fpgaImage);
    size_t chunks = 0;
    for (const auto& c : cam.log)
        chunks += c[0] == 0x1C;
    CHECK(chunks == (1000 + 59) / 60);
}

// ---------------------------------------------------------------------------------------------------------------

// Each run is {x, y, len}; pixels have intensity `level` unless `profiles` gives per-pixel values for that run.
static std::vector<uint8_t> makeType5Packet(const std::vector<std::array<int, 3>>& runs, uint32_t level = 100,
                                            const std::vector<std::vector<uint32_t>>& profiles = {})
{
    std::vector<uint8_t> p = {0x07, 0x10, 0x05, 0x00};
    p[3] = uint8_t(p[0] ^ p[1] ^ p[2] ^ 0xAA);
    for (size_t r = 0; r < runs.size(); r++) {
        int x = runs[r][0], y = runs[r][1], len = runs[r][2];
        uint32_t m0 = 0, m1 = 0;
        for (int i = 0; i < len; i++) {
            uint32_t v = r < profiles.size() && !profiles[r].empty() ? profiles[r][size_t(i)] : level;
            m0 += v;
            m1 += uint32_t(i) * v;
        }
        p.push_back(uint8_t(x >> 2));
        p.push_back(uint8_t((x & 3) << 6 | ((y >> 3) & 0x3F)));
        p.push_back(uint8_t((y & 7) << 5 | ((len >> 5) & 0x1F)));
        p.push_back(uint8_t((len & 0x1F) << 3 | ((m1 >> 17) & 7)));
        p.push_back(uint8_t(m1 >> 9));
        p.push_back(uint8_t(m1 >> 1));
        p.push_back(uint8_t((m1 & 1) << 7 | ((m0 >> 8) & 0x7F)));
        p.push_back(uint8_t(m0));
    }
    uint32_t n = uint32_t(p.size() + 4 - 8);
    p.push_back(uint8_t(n >> 24));
    p.push_back(uint8_t(n >> 16));
    p.push_back(uint8_t(n >> 8));
    p.push_back(uint8_t(n));
    return p;
}

static void testFrameDecodeAndBlobs()
{
    // Two 3x3 squares and one 2-row blob.
    std::vector<std::array<int, 3>> runs = {
        {100, 50, 3}, {100, 51, 3}, {100, 52, 3},
        {300, 200, 3}, {300, 201, 3}, {300, 202, 3},
        {500, 400, 4}, {501, 401, 4},
    };
    auto pkt = makeType5Packet(runs);
    CHECK(classifyPacket(pkt.data(), pkt.size()) == PacketKind::Frame);
    CHECK(frameChecksumValid(pkt.data(), pkt.size()));
    Frame f;
    CHECK(decodeFrame(pkt.data(), pkt.size(), 640, 480, f));
    CHECK(f.type == 5 && f.counter == 7);
    CHECK(f.segments.size() == runs.size());
    CHECK(f.segments[0].x0 == 100 && f.segments[0].x1 == 102 && f.segments[0].y == 50);
    CHECK(f.segments[0].moment0 == 300 && f.segments[0].moment1 == 300);  // 3 pixels of 100: sum 300, sum i*I 300

    // Sub-pixel centroid from the intensity moments (TrackIR's FUN_00588600): a run brighter on its right, and a
    // brighter lower row, pull the centre right and down.
    {
        auto sub = makeType5Packet({{200, 100, 3}, {200, 101, 3}}, 100, {{50, 100, 200}, {150, 300, 600}});
        Frame g;
        CHECK(decodeFrame(sub.data(), sub.size(), 640, 480, g));
        auto one = extractBlobs(g.segments);
        CHECK(one.size() == 1);
        if (one.size() == 1) {
            CHECK_NEAR(one[0].x, 200 + (100 + 400 + 300 + 1200) / 1400.0, 1e-9);  // 201.4286
            CHECK_NEAR(one[0].y, (100 * 350 + 101 * 1050) / 1400.0, 1e-9);        // 100.75
            CHECK_NEAR(one[0].weight, 1400, 1e-9);
        }
    }

    auto blobs = extractBlobs(f.segments);
    CHECK(blobs.size() == 3);
    bool foundA = false, foundC = false;
    for (const auto& b : blobs) {
        if (std::fabs(b.x - 101) < 1e-9 && std::fabs(b.y - 51) < 1e-9 && b.area == 9)
            foundA = true;
        if (std::fabs(b.x - 502) < 1e-9 && std::fabs(b.y - 400.5) < 1e-9 && b.area == 8)
            foundC = true;
    }
    CHECK(foundA && foundC);

    pkt[5] ^= 1;  // payload change keeps the header checksum valid
    CHECK(frameChecksumValid(pkt.data(), pkt.size()));
    pkt[3] ^= 1;
    CHECK(!frameChecksumValid(pkt.data(), pkt.size()));
}

static void testType0Decode()
{
    // x0 = 0x4A3/2, x1 = 0x4B1/2, y = 0x1C2 with the encoding from 005a40d0.
    uint8_t rec[4] = {0xC2, 0xA3, 0xB1, 0};
    int x0 = 0x4A3, x1 = 0x4B1;
    uint8_t f = 0;
    f |= (x0 & 0x100) ? 0x80 : 0; f |= (x0 & 0x200) ? 0x10 : 0; f |= (x0 & 0x400) ? 0x02 : 0;
    f |= (x1 & 0x100) ? 0x40 : 0; f |= (x1 & 0x200) ? 0x08 : 0; f |= (x1 & 0x400) ? 0x01 : 0;
    f |= 0x20;  // y bit 8
    rec[3] = f;
    std::vector<uint8_t> p = {1, 0x10, 0x00, uint8_t(1 ^ 0x10 ^ 0 ^ 0xAA)};
    p.insert(p.end(), rec, rec + 4);
    p.insert(p.end(), {0, 0, 0, 4});
    Frame fr;
    CHECK(decodeFrame(p.data(), p.size(), 640, 480, fr));
    CHECK(fr.segments.size() == 1);
    if (!fr.segments.empty()) {
        CHECK(fr.segments[0].y == 0x1C2);
        CHECK_NEAR(fr.segments[0].x0, x0 / 2.0, 1e-6);
        CHECK_NEAR(fr.segments[0].x1, x1 / 2.0, 1e-6);
    }
}

// ---------------------------------------------------------------------------------------------------------------

static Mat3 ypr(double yawDeg, double pitchDeg, double rollDeg)
{
    return rotationY(yawDeg / kDegPerRad) * rotationX(pitchDeg / kDegPerRad) * rotationZ(rollDeg / kDegPerRad);
}

static std::vector<Blob> renderMarkers(const LensUndistorter& lens, const MarkerModel& m, const Pose& pose)
{
    std::vector<Blob> blobs;
    double weights[3] = {900, 1000, 950};
    const Vec3* pts[3] = {&m.apex, &m.left, &m.right};
    for (int i = 0; i < 3; i++) {
        Vec3 c = pose.rotation * *pts[i] + pose.translation;
        Blob b;
        lens.project(c, b.x, b.y);
        b.weight = weights[i];
        b.area = 20;
        blobs.push_back(b);
    }
    std::sort(blobs.begin(), blobs.end(), [](const Blob& a, const Blob& b) { return a.weight > b.weight; });
    return blobs;
}

static void testP3PSolver()
{
    MarkerModel m = MarkerModel::trackClipDefault();
    CHECK_NEAR((m.apex - m.left).norm(), 116.052, 1e-9);
    CHECK_NEAR((m.apex - m.right).norm(), 116.052, 1e-9);
    CHECK_NEAR((m.left - m.right).norm(), 69.621, 1e-9);

    std::uniform_real_distribution<double> ang(-35, 35), off(-120, 120), depth(450, 900);
    int recovered = 0, trials = 300;
    for (int t = 0; t < trials; t++) {
        Pose truth{ypr(ang(gRng), ang(gRng), ang(gRng)), {off(gRng), off(gRng), depth(gRng)}};
        std::array<Vec3, 3> model = {m.apex, m.left, m.right};
        std::array<Vec3, 3> bearings;
        for (int i = 0; i < 3; i++)
            bearings[size_t(i)] = (truth.rotation * model[size_t(i)] + truth.translation).normalized();
        auto sols = solveP3P(model, bearings);
        for (const auto& s : sols)
            if (s.rotation.angleTo(truth.rotation) < 1e-6 && (s.translation - truth.translation).norm() < 1e-5) {
                recovered++;
                break;
            }
    }
    CHECK(recovered == trials);
}

static void testNewtonDepthSolver()
{
    // Poses on the TrackClip branch (apex tilted away from the camera) must be found from TrackIR's start point.
    MarkerModel m = MarkerModel::trackClipDefault();
    std::array<Vec3, 3> model = {m.apex, m.left, m.right};
    const double dist[3] = {(model[0] - model[1]).norm(), (model[0] - model[2]).norm(), (model[1] - model[2]).norm()};
    std::uniform_real_distribution<double> yaw(-30, 30), pitch(-60, -15), roll(-25, 25), off(-100, 100), depth(450, 900);
    int ok = 0, trials = 300;
    for (int t = 0; t < trials; t++) {
        Pose truth{ypr(yaw(gRng), pitch(gRng), roll(gRng)), {off(gRng), off(gRng), depth(gRng)}};
        std::array<Vec3, 3> bearings;
        double trueDepth[3];
        for (int i = 0; i < 3; i++) {
            Vec3 c = truth.rotation * model[size_t(i)] + truth.translation;
            bearings[size_t(i)] = c.normalized();
            trueDepth[i] = c.norm();
        }
        double d[3];
        if (solveDepthsNewton(bearings, dist, m.initialDepth, d) && std::fabs(d[0] - trueDepth[0]) < 1e-6 &&
            std::fabs(d[1] - trueDepth[1]) < 1e-6 && std::fabs(d[2] - trueDepth[2]) < 1e-6)
            ok++;
    }
    std::printf("  Newton recovered %d/%d poses on the TrackClip branch\n", ok, trials);
    CHECK(ok >= trials * 95 / 100);
}

static void testPoseTrackerAndRecentering()
{
    LensModel lens = lensFor(*findCameraModel(0x0159), 500000);
    LensUndistorter und(lens);
    MarkerModel m = MarkerModel::trackClipDefault();
    PoseTracker tracker(lens, m);
    Recentering centre;

    Pose base{ypr(0, -30, 0), {0, 0, 600}};
    tracker.setReference(base);
    auto p0 = tracker.update(renderMarkers(und, m, base));
    CHECK(p0.has_value());
    if (!p0)
        return;
    CHECK(p0->rotation.angleTo(base.rotation) < 1e-6);
    centre.recenter(*p0);

    // Smoothly turn the head and make sure the tracker follows the true solution frame by frame.
    double maxErr = 0;
    for (int i = 1; i <= 60; i++) {
        double yaw = 0.5 * i, pitch = -30 + 0.2 * i, roll = 0.3 * i;
        Pose truth{ypr(yaw, pitch, roll), {1.5 * i, -0.5 * i, 600.0 + i}};
        auto p = tracker.update(renderMarkers(und, m, truth));
        CHECK(p.has_value());
        if (p)
            maxErr = std::max(maxErr, p->rotation.angleTo(truth.rotation) * kDegPerRad);
    }
    std::printf("  max rotation error while tracking: %.2e deg\n", maxErr);
    CHECK(maxErr < 1e-4);

    // Pure 20 degree turn about the camera's vertical axis relative to the centre pose, after losing the clip.
    Pose turned{rotationY(20 / kDegPerRad) * base.rotation, base.translation};
    tracker.reset();
    auto p = tracker.update(renderMarkers(und, m, turned));
    CHECK(p.has_value());
    if (p) {
        HeadPose h = centre.relative(*p);
        CHECK_NEAR(std::fabs(h.yaw), 20, 1e-6);
        CHECK_NEAR(h.pitch, 0, 1e-6);
        CHECK_NEAR(h.roll, 0, 1e-6);
    }

    // 5 cm sideways move of the whole head.
    Pose moved{base.rotation, base.translation + Vec3{50, 0, 0}};
    p = tracker.update(renderMarkers(und, m, moved));
    CHECK(p.has_value());
    if (p) {
        HeadPose h = centre.relative(*p);
        CHECK_NEAR(std::fabs(h.x), 5.0, 1e-6);
        CHECK_NEAR(h.yaw, 0, 1e-6);
    }
}

static void testLensRoundTrip()
{
    for (uint16_t pid : {uint16_t(0x0157), uint16_t(0x0159)}) {
        LensUndistorter l(lensFor(*findCameraModel(pid), 0));
        for (int i = 0; i < 100; i++) {
            Vec3 p{double(int(rng32() % 600) - 300), double(int(rng32() % 400) - 200), 700};
            double u, v, xn, yn;
            l.project(p, u, v);
            l.undistort(u, v, xn, yn);
            CHECK_NEAR(xn, p.x / p.z, 1e-9);
            CHECK_NEAR(yn, p.y / p.z, 1e-9);
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------

static void testProfileCurves()
{
    Profile lin = Profile::linear();
    HeadPose in{12, -7, 3, 1.5, -2, 4};
    HeadPose out = lin.apply(in);
    CHECK_NEAR(out.yaw, 12, 1e-9);
    CHECK_NEAR(out.pitch, -7, 1e-9);
    CHECK_NEAR(out.z, 4, 1e-9);

    std::vector<std::pair<double, double>> pts = {{-50, 2}, {0, 0}, {50, 2}};
    // slope grows linearly: f(t) = t/25 -> integral 0..x = x^2/50
    CHECK_NEAR(integrateSlopeCurve(pts, 10), 2.0, 1e-9);
    CHECK_NEAR(integrateSlopeCurve(pts, -10), -2.0, 1e-9);
    CHECK_NEAR(integrateSlopeCurve(pts, 80), 50.0 + 30 * 2, 1e-9);  // constant slope past the last point

    const char* stock = "../extracted/msi/Program Files/TrackIR5/Profiles/default.xml";
    std::ifstream probe(stock);
    if (probe) {
        Profile p = Profile::load(stock);
        CHECK(p.name == "Default");
        HeadPose o = p.apply(HeadPose{10, 0, 0, 0, 0, 0});
        // 0 slope up to 1 degree, ramp to 4.10256 at 7.39645 and towards 6 at 15 (hand-integrated: 24.648).
        CHECK_NEAR(o.yaw, 24.648, 0.01);
        CHECK_NEAR(p.apply(HeadPose{-10, 0, 0, 0, 0, 0}).yaw, -o.yaw, 1e-9);
        // save() writes TrackIR's layout and load() reads it back unchanged.
        char path[] = "/tmp/trackir-profile-XXXXXX";
        close(mkstemp(path));
        p.name = "Edited <copy>";
        p.axes[AxisRoll].inverted = true;
        p.axes[AxisZ].enabled = false;
        CHECK(p.save(path));
        Profile q = Profile::load(path);
        CHECK(q.name == "Edited <copy>");
        CHECK(q.axes[AxisRoll].inverted && !q.axes[AxisZ].enabled);
        CHECK(q.axes[AxisYaw].points == p.axes[AxisYaw].points);
        CHECK_NEAR(q.apply(HeadPose{10, 0, 0, 0, 0, 0}).yaw, 24.648, 0.01);
        unlink(path);
    } else {
        std::printf("  (stock profiles not found, skipping default.xml check)\n");
    }
}

// The pose filter must steady a still head (sensor noise) without lagging behind real movements.
static void testPoseFilter()
{
    const double dt = 1.0 / 120;
    std::mt19937 rng(7);
    std::normal_distribution<double> noise(0.0, 0.1);  // 0.1 deg of jitter, as seen on a real camera

    for (double smoothing : {0.3, 0.5}) {
        PoseFilter f(smoothing);
        double sum2 = 0;
        int n = 0;
        for (int i = 0; i < 1200; i++) {
            HeadPose in;
            in.yaw = 5 + noise(rng);
            HeadPose out = f.step(in, dt);
            if (i > 240) {
                sum2 += (out.yaw - 5) * (out.yaw - 5);
                n++;
            }
        }
        double residual = std::sqrt(sum2 / n);
        std::printf("  smoothing %.1f: rest cutoff %.1f Hz, jitter 0.100 -> %.3f deg\n", smoothing,
                    PoseFilter::restCutoffHz(smoothing), residual);
        CHECK(residual < (smoothing < 0.4 ? 0.035 : 0.022));

        // A quick 60-degree glance at 200 deg/s: the output must keep up (lag well under a frame of X-Plane at 30 fps
        // worth of angle, i.e. a few degrees), and settle on the target.
        PoseFilter g(smoothing);
        double worstLag = 0, yaw = 0;
        for (int i = 0; i < 240; i++) {
            yaw = std::min(60.0, i * dt * 200);
            HeadPose in;
            in.yaw = yaw;
            HeadPose out = g.step(in, dt);
            if (i > 10 && yaw < 60)
                worstLag = std::max(worstLag, yaw - out.yaw);
            if (i == 239)
                CHECK_NEAR(out.yaw, 60, 0.5);
        }
        std::printf("  smoothing %.1f: lag during a 200 deg/s turn %.1f deg\n", smoothing, worstLag);
        CHECK(worstLag < 4.0);
    }
}

static void testSettingsFile()
{
    char path[] = "/tmp/trackir-settings-XXXXXX";
    close(mkstemp(path));
    Settings s;
    s.profile = "/some where/flying.xml";
    s.smoothing = 0.5;
    s.clipType = ClipType::TrackClipPro;
    s.pivot = {0, 80, 90};
    s.camera.irIntensity = 0;
    s.udp = true;
    s.udpPort = 5555;
    s.axisSign = {{1, -1, 1, -1, 1, 1}};
    s.focalScale = 1.0625;
    s.hotkeyPause = "none";
    CHECK(s.save(path));
    Settings t = Settings::load(path);
    CHECK(t.profile == s.profile && t.smoothing == 0.5 && t.clipType == ClipType::TrackClipPro);
    CHECK(t.pivot.y == 80 && t.pivot.z == 90 && t.camera.irIntensity == 0 && t.camera.threshold == 150);
    CHECK(t.udp && t.udpPort == 5555 && t.udpHost == "127.0.0.1");
    CHECK(t.axisSign == s.axisSign && std::fabs(t.focalScale - 1.0625) < 1e-9);
    CHECK(t.hotkeyPause == "none" && t.hotkeyRecenter == "F12");
    Settings missing = Settings::load("/nonexistent/settings.ini");
    CHECK(missing.smoothing == 0.3 && missing.axisSign[0] == 1);
    unlink(path);
}

static void testTrackIRScaling()
{
    HeadPose h{90, 45, -180, 25, -50, 100};
    np_trackir_data d = toTrackIRData(h);
    CHECK_NEAR(d.yaw, 8191.5, 0.01);
    CHECK_NEAR(d.pitch, -4095.75, 0.01);  // TrackIR negates pitch
    CHECK_NEAR(d.roll, -16383, 0.01);
    CHECK_NEAR(d.x, -8191.5, 0.01);       // and x
    CHECK_NEAR(d.y, -16383, 0.01);
    CHECK_NEAR(d.z, 16383, 0.01);         // clamped
}

// Plays the game side of the bridge the way wine/npclient.c does.
static void testNpBridge()
{
    char path[] = "/tmp/trackir-bridge-test-XXXXXX";
    int tmp = mkstemp(path);
    close(tmp);
    char keyPath[] = "/tmp/trackir-keys-test-XXXXXX";
    int kfd = mkstemp(keyPath);
    const char csv[] = "# id,key\n4242,0123456789abcdef\n";
    CHECK(write(kfd, csv, sizeof(csv) - 1) == ssize_t(sizeof(csv) - 1));
    close(kfd);

    {
        NpBridge bridge(path);
        std::string error;
        CHECK(bridge.open(error));
        CHECK(bridge.loadGameKeys(keyPath, error));
        int recentres = 0;
        uint32_t profile = 0;
        bool transmitting = false;
        bridge.setEvents({[&] { recentres++; }, [&](uint32_t id) { profile = id; }, [&](bool on) { transmitting = on; }});

        int fd = open(path, O_RDWR);
        auto* client = static_cast<tir_bridge*>(mmap(nullptr, TIR_BRIDGE_FILE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
        CHECK(client->magic == TIR_BRIDGE_MAGIC && client->daemon_pid == getpid());
        CHECK(client->block.version == NP_VERSION_TRACKIR_5_5);
        CHECK(std::strcmp(client->block.sig_dll, NP_SIGNATURE_DLL) == 0);

        auto sendCommand = [&](uint32_t code, uint32_t arg) {
            uint32_t index = __atomic_fetch_add(&client->cmd_head, 1, __ATOMIC_ACQ_REL);
            tir_bridge_command& slot = client->cmds[index % TIR_BRIDGE_RING];
            slot.code = code;
            slot.arg = arg;
            __atomic_store_n(&slot.sequence, index + 1, __ATOMIC_RELEASE);
        };
        auto readData = [&](np_trackir_data& out) {
            for (;;) {
                uint32_t before = __atomic_load_n(&client->data_seq, __ATOMIC_ACQUIRE);
                if (before & 1)
                    continue;
                std::memcpy(&out, &client->block.data, sizeof(out));
                if (__atomic_load_n(&client->data_seq, __ATOMIC_ACQUIRE) == before)
                    return;
            }
        };

        sendCommand(NP_CMD_START_TRANSMISSION, 0);
        sendCommand(NP_CMD_RECENTER, 0);
        bridge.pollCommands();
        CHECK(transmitting && recentres == 1);

        HeadPose pose{30, -10, 5, 2, 3, -4};
        bridge.publish(pose, true);
        np_trackir_data d{};
        readData(d);
        np_trackir_data want = toTrackIRData(pose);
        CHECK(d.yaw == want.yaw && d.pitch == want.pitch && d.z == want.z);
        CHECK(d.frame_signature == 1);

        // A keyed "TrackIR Enhanced" title gets encrypted data, which NP_GetDataEX's algorithm decrypts.
        sendCommand(NP_CMD_REGISTER_PROFILE_ID, 4242);
        bridge.pollCommands();
        CHECK(profile == 4242);
        bridge.publish(pose, true);
        readData(d);
        CHECK(d.yaw != want.yaw);
        const uint8_t key[8] = {0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef};
        CHECK(np_data_decrypt(&d, key) == NP_OK);
        CHECK(d.yaw == want.yaw && d.frame_signature == 2);

        // Native clients (X-Plane plugin) read the unscaled pose and queue commands with the C helpers.
        bridge.publishNative(pose, TIR_POSE_TRACKING);
        double native[6];
        uint32_t flags = 0;
        CHECK(tir_bridge_read_pose(client, native, &flags) == 1);
        CHECK(flags == TIR_POSE_TRACKING && native[0] == 30 && native[1] == -10 && native[5] == -4);
        tir_bridge_push_command(client, NP_CMD_RECENTER, 0);
        bridge.pollCommands();
        CHECK(recentres == 2);

        munmap(client, TIR_BRIDGE_FILE_SIZE);
        close(fd);
    }
    unlink(path);
    unlink(keyPath);
}

// ---------------------------------------------------------------------------------------------------------------

int main()
{
    struct { const char* name; void (*fn)(); } tests[] = {
        {"shared layouts", testLayouts},
        {"NP encryption", testNpEncryption},
        {"XTEA", testXtea},
        {"secure codec round trip", testSecureCodecRoundTrip},
        {"secure driver handshake", testSecureDriverHandshake},
        {"secure driver rejects forged camera", testSecureDriverRejectsForgedCamera},
        {"secure driver without key table", testSecureDriverWithoutKeys},
        {"classic driver FPGA upload", testClassicDriverUploadsFpga},
        {"frame decode and blobs", testFrameDecodeAndBlobs},
        {"type 0 decode", testType0Decode},
        {"lens round trip", testLensRoundTrip},
        {"P3P solver", testP3PSolver},
        {"Newton depth solver", testNewtonDepthSolver},
        {"pose tracker and recentering", testPoseTrackerAndRecentering},
        {"profile curves", testProfileCurves},
        {"settings file", testSettingsFile},
        {"pose filter", testPoseFilter},
        {"TrackIR scaling", testTrackIRScaling},
        {"NPClient bridge", testNpBridge},
    };
    for (const auto& t : tests) {
        int before = gFailures;
        std::printf("%s\n", t.name);
        t.fn();
        if (gFailures == before)
            std::printf("  ok\n");
    }
    std::printf("\n%d checks, %d failures\n", gChecks, gFailures);
    return gFailures ? 1 : 0;
}
