// Loads the built X-Plane plugin into the fake XPLM and drives it with a real NpBridge (the daemon side).
//   DYLD_FRAMEWORK_PATH=build/xplane-test build/xplane-test/plugin-test build/xplane/TrackIR-macOS/mac_x64/TrackIR-macOS.xpl
#include "common/tir_bridge.h"
#include "output/np_bridge.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>

#include <dlfcn.h>
#include <unistd.h>

using namespace tir;

extern "C" {
void fake_define(const char* name);
void fake_set_float(const char* name, float v);
float fake_get_float(const char* name);
void fake_set_int(const char* name, int v);
int fake_run_flight_loop();
int fake_command(const char* name);
const char* fake_menu_item(int index, int* checked);
const char* fake_log();
}

static int gChecks = 0, gFailures = 0;
#define CHECK(c)                                                                  \
    do {                                                                          \
        gChecks++;                                                                \
        if (!(c)) {                                                               \
            gFailures++;                                                          \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);              \
        }                                                                         \
    } while (0)
#define CHECK_NEAR(a, b, tol) CHECK(std::fabs(double(a) - double(b)) <= (tol))

static const char* kRefs[6] = {"sim/graphics/view/pilots_head_x",   "sim/graphics/view/pilots_head_y",
                               "sim/graphics/view/pilots_head_z",   "sim/graphics/view/pilots_head_psi",
                               "sim/graphics/view/pilots_head_the", "sim/graphics/view/pilots_head_phi"};

static void setHead(float x, float y, float z, float psi, float the, float phi)
{
    const float v[6] = {x, y, z, psi, the, phi};
    for (int i = 0; i < 6; i++)
        fake_set_float(kRefs[i], v[i]);
}

static void expectHead(int line, double x, double y, double z, double psi, double the, double phi)
{
    const double want[6] = {x, y, z, psi, the, phi};
    for (int i = 0; i < 6; i++) {
        double got = fake_get_float(kRefs[i]);
        gChecks++;
        if (std::fabs(got - want[i]) > 1e-4) {
            gFailures++;
            std::printf("FAIL line %d: %s = %.5f, want %.5f\n", line, kRefs[i], got, want[i]);
        }
    }
}
#define EXPECT_HEAD(...) expectHead(__LINE__, __VA_ARGS__)

static std::string status()
{
    const char* s = fake_menu_item(4, nullptr);
    return s ? s : "";
}

static void frames(int n = 3)
{
    for (int i = 0; i < n; i++)
        fake_run_flight_loop();
}

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::puts("usage: plugin-test <plugin.xpl>");
        return 2;
    }
    char path[] = "/tmp/trackir-xplane-test-XXXXXX";
    close(mkstemp(path));
    unlink(path);
    setenv(TIR_BRIDGE_ENV, path, 1);

    for (const char* r : kRefs)
        fake_define(r);
    fake_define("sim/graphics/view/view_type");
    fake_define("sim/graphics/VR/enabled");
    fake_set_int("sim/graphics/view/view_type", 1026);
    setHead(0.1f, 0.9f, -0.3f, 0, 0, 0);

    void* plugin = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!plugin) {
        std::printf("dlopen: %s\n", dlerror());
        return 1;
    }
    auto start = reinterpret_cast<int (*)(char*, char*, char*)>(dlsym(plugin, "XPluginStart"));
    auto stop = reinterpret_cast<void (*)()>(dlsym(plugin, "XPluginStop"));
    auto enable = reinterpret_cast<int (*)()>(dlsym(plugin, "XPluginEnable"));
    auto disable = reinterpret_cast<void (*)()>(dlsym(plugin, "XPluginDisable"));
    auto message = reinterpret_cast<void (*)(int, int, void*)>(dlsym(plugin, "XPluginReceiveMessage"));
    CHECK(start && stop && enable && disable && message);
    if (!start || !stop || !enable || !disable || !message)
        return 1;

    char name[256] = {}, sig[256] = {}, desc[256] = {};
    CHECK(start(name, sig, desc) == 1);
    CHECK(std::strcmp(sig, "trackir_macos.head_tracking") == 0);
    CHECK(enable() == 1);
    int checked = 0;
    CHECK(fake_menu_item(0, &checked) && checked == 2);  // "Head tracking", on by default (xplm_Menu_Checked)

    // No tracker yet: the head is left alone.
    frames();
    EXPECT_HEAD(0.1, 0.9, -0.3, 0, 0, 0);
    CHECK(status().find("not running") != std::string::npos);

    // The daemon starts and publishes a pose.
    int recentres = 0;
    auto bridge = std::make_unique<NpBridge>(path);
    std::string error;
    CHECK(bridge->open(error));
    bridge->setEvents({[&] { recentres++; }, nullptr, nullptr});
    std::this_thread::sleep_for(std::chrono::milliseconds(550));  // the plugin retries the file every 500 ms
    bridge->publishNative(HeadPose{30, 10, 5, 2, 3, 4}, TIR_POSE_TRACKING);
    bridge->heartbeat();
    frames();
    // yaw left 30 -> psi -30; pitch up 10 -> the +10; roll left 5 -> phi -5; x right 2 cm; y up 3 cm;
    // 4 cm towards the screen -> z 4 cm forward (-z).
    EXPECT_HEAD(0.12, 0.93, -0.34, -30, 10, -5);
    CHECK(status() == "Status: tracking");

    // Moving the head follows the pose, always relative to X-Plane's own head position.
    bridge->publishNative(HeadPose{-10, 0, 0, 0, 0, 0}, TIR_POSE_TRACKING);
    bridge->heartbeat();
    frames();
    EXPECT_HEAD(0.1, 0.9, -0.3, 10, 0, 0);

    // The user nudges the seat forward with the keyboard: the nudge is kept underneath the tracking offset.
    fake_set_float(kRefs[2], fake_get_float(kRefs[2]) - 0.05f);
    bridge->publishNative(HeadPose{-10, 0, 0, 0, 0, 1}, TIR_POSE_TRACKING);
    bridge->heartbeat();
    frames();
    EXPECT_HEAD(0.1, 0.9, -0.36, 10, 0, 0);

    // Pause freezes the view.
    CHECK(fake_command("trackir_macos/pause"));
    CHECK(fake_menu_item(1, &checked) && checked == 2);
    bridge->publishNative(HeadPose{40, 20, 0, 0, 0, 0}, TIR_POSE_TRACKING);
    bridge->heartbeat();
    frames();
    EXPECT_HEAD(0.1, 0.9, -0.36, 10, 0, 0);
    CHECK(status() == "Status: paused");
    CHECK(fake_command("trackir_macos/pause"));
    frames();
    EXPECT_HEAD(0.1, 0.9, -0.35, -40, 20, 0);

    // Recenter is forwarded to the tracker as NP_ReCenter.
    CHECK(fake_command("trackir_macos/recenter"));
    bridge->pollCommands();
    CHECK(recentres == 1);

    // Clip lost: the head goes back to X-Plane's position (including the user's nudge).
    bridge->publishNative(HeadPose{}, 0);
    bridge->heartbeat();
    frames();
    EXPECT_HEAD(0.1, 0.9, -0.35, 0, 0, 0);
    CHECK(status() == "Status: clip not visible");

    // Outside the 3-D cockpit nothing is written.
    fake_set_int("sim/graphics/view/view_type", 1017);
    setHead(1, 2, 3, 4, 5, 6);
    bridge->publishNative(HeadPose{30, 0, 0, 0, 0, 0}, TIR_POSE_TRACKING);
    bridge->heartbeat();
    frames();
    EXPECT_HEAD(1, 2, 3, 4, 5, 6);
    CHECK(status().find("3-D cockpit") != std::string::npos);
    fake_set_int("sim/graphics/view/view_type", 1026);
    setHead(0.1f, 0.9f, -0.3f, 0, 0, 0);
    frames();
    EXPECT_HEAD(0.1, 0.9, -0.3, -30, 0, 0);

    // VR: left alone.
    fake_set_int("sim/graphics/VR/enabled", 1);
    frames();
    EXPECT_HEAD(0.1, 0.9, -0.3, 0, 0, 0);
    fake_set_int("sim/graphics/VR/enabled", 0);
    frames();
    EXPECT_HEAD(0.1, 0.9, -0.3, -30, 0, 0);

    // The toggle command switches tracking off and restores the head.
    CHECK(fake_command("trackir_macos/toggle"));
    frames();
    EXPECT_HEAD(0.1, 0.9, -0.3, 0, 0, 0);
    CHECK(fake_menu_item(0, &checked) && checked == 1);  // xplm_Menu_Unchecked
    CHECK(fake_command("trackir_macos/toggle"));
    frames();
    EXPECT_HEAD(0.1, 0.9, -0.3, -30, 0, 0);

    // The tracker quits (no more heartbeats): the head is restored once the bridge goes stale.
    bridge.reset();
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    frames();
    EXPECT_HEAD(0.1, 0.9, -0.3, 0, 0, 0);
    CHECK(status().find("not running") != std::string::npos);

    // A new tracker on the same path is picked up again.
    bridge = std::make_unique<NpBridge>(path);
    CHECK(bridge->open(error));
    bridge->publishNative(HeadPose{0, 0, 0, 0, 0, 10}, TIR_POSE_TRACKING);
    bridge->heartbeat();
    frames();
    EXPECT_HEAD(0.1, 0.9, -0.4, 0, 0, 0);

    // Loading another aircraft gives a fresh base; disabling the plugin restores the head.
    message(0, 102 /* XPLM_MSG_PLANE_LOADED */, nullptr);
    setHead(0, 1, 0, 0, 0, 0);
    frames();
    EXPECT_HEAD(0, 1, -0.1, 0, 0, 0);
    disable();
    EXPECT_HEAD(0, 1, 0, 0, 0, 0);
    stop();

    bridge.reset();
    unlink(path);
    std::printf("%d checks, %d failures\n", gChecks, gFailures);
    return gFailures ? 1 : 0;
}
