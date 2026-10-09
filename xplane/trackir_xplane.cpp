// X-Plane plugin: moves the pilot's head in the 3-D cockpit from the pose trackir-mac (CLI or menu-bar app)
// publishes in /tmp/TrackIR-macOS.bridge. No opentrack or network needed.
//
// Plugins menu > TrackIR (macOS): enable, pause, recenter. The same actions are X-Plane commands
// (trackir_macos/toggle, trackir_macos/pause, trackir_macos/recenter) for keyboard or joystick bindings.
#include "XPLMDataAccess.h"
#include "XPLMMenus.h"
#include "XPLMPlugin.h"
#include "XPLMProcessing.h"
#include "XPLMUtilities.h"

#include "common/np_shared.h"
#include "common/tir_bridge.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

using Clock = std::chrono::steady_clock;

constexpr int kView3DCockpit = 1026;  // sim/graphics/view/view_type in the 3-D cockpit
constexpr double kStale = 1.0;        // seconds without a heartbeat before the tracker counts as gone

enum HeadAxis { HeadX, HeadY, HeadZ, HeadPsi, HeadThe, HeadPhi, HeadAxes };
const char* const kHeadRefs[HeadAxes] = {
    "sim/graphics/view/pilots_head_x",   "sim/graphics/view/pilots_head_y",   "sim/graphics/view/pilots_head_z",
    "sim/graphics/view/pilots_head_psi", "sim/graphics/view/pilots_head_the", "sim/graphics/view/pilots_head_phi",
};

// The bridge pose (yaw + left, pitch + up, roll + left, x + right, y + up, z + towards the screen; deg / cm)
// in X-Plane's head axes (metres, x right, y up, z aft; psi + right, the + up, phi + right).
void toHeadOffsets(const double pose[6], double out[HeadAxes])
{
    out[HeadX] = pose[3] / 100.0;
    out[HeadY] = pose[4] / 100.0;
    out[HeadZ] = -pose[5] / 100.0;
    out[HeadPsi] = -pose[0];
    out[HeadThe] = pose[1];
    out[HeadPhi] = -pose[2];
}

void debug(const char* fmt, const char* arg = "")
{
    char line[256];
    std::snprintf(line, sizeof(line), fmt, arg);
    std::string s = std::string("TrackIR-macOS: ") + line + "\n";
    XPLMDebugString(s.c_str());
}

// --- connection to the tracker ---------------------------------------------------------------------------------

struct Bridge {
    tir_bridge* map = nullptr;
    ino_t inode = 0;
    uint32_t lastBeat = 0;
    Clock::time_point lastBeatChange{}, lastOpenAttempt{}, lastInodeCheck{};

    static std::string path()
    {
        const char* env = std::getenv(TIR_BRIDGE_ENV);
        return env && *env ? env : TIR_BRIDGE_UNIX_PATH;
    }

    void close()
    {
        if (map)
            munmap(map, TIR_BRIDGE_FILE_SIZE);
        map = nullptr;
    }

    bool open()
    {
        std::string p = path();
        int fd = ::open(p.c_str(), O_RDWR);
        if (fd < 0)
            return false;
        struct stat st {};
        void* mem = fstat(fd, &st) == 0 && st.st_size >= TIR_BRIDGE_FILE_SIZE
                        ? mmap(nullptr, TIR_BRIDGE_FILE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0)
                        : MAP_FAILED;
        ::close(fd);
        if (mem == MAP_FAILED)
            return false;
        map = static_cast<tir_bridge*>(mem);
        inode = st.st_ino;
        lastBeat = map->heartbeat;
        lastBeatChange = Clock::now();
        return true;
    }

    // True while a tracker is publishing. Reopens the file if the tracker recreated it.
    bool alive()
    {
        auto now = Clock::now();
        if (map && now - lastInodeCheck > std::chrono::seconds(2)) {
            lastInodeCheck = now;
            struct stat st {};
            if (stat(path().c_str(), &st) != 0 || st.st_ino != inode)
                close();
        }
        if (!map) {
            if (now - lastOpenAttempt < std::chrono::milliseconds(500))
                return false;
            lastOpenAttempt = now;
            if (!open())
                return false;
        }
        if (__atomic_load_n(&map->magic, __ATOMIC_ACQUIRE) != TIR_BRIDGE_MAGIC || map->daemon_pid <= 0)
            return false;
        uint32_t beat = map->heartbeat;
        if (beat != lastBeat) {
            lastBeat = beat;
            lastBeatChange = now;
        }
        return std::chrono::duration<double>(now - lastBeatChange).count() < kStale;
    }
};

// --- plugin state ----------------------------------------------------------------------------------------------

struct State {
    Bridge bridge;
    XPLMDataRef head[HeadAxes] = {};
    XPLMDataRef viewType = nullptr, vrEnabled = nullptr;
    XPLMFlightLoopID loop = nullptr;
    XPLMCommandRef cmdToggle = nullptr, cmdPause = nullptr, cmdRecenter = nullptr;
    XPLMMenuID menu = nullptr;
    int menuToggle = -1, menuPause = -1, menuStatus = -1;

    bool enabled = true;
    bool paused = false;
    double frozen[HeadAxes] = {};
    // While applied: X-Plane's head position when tracking took over (`base`); every frame writes base + pose.
    // The values are never read back: X-Plane nudges them between frames (head-motion effects), and folding those
    // nudges into the base made the view drift.
    bool applied = false;
    int appliedView = 0;
    double base[HeadAxes] = {};
    std::string status;
} g;

void setStatus(const char* text)
{
    if (g.status == text)
        return;
    g.status = text;
    debug("%s", text);
    if (g.menu && g.menuStatus >= 0)
        XPLMSetMenuItemName(g.menu, g.menuStatus, (std::string("Status: ") + text).c_str(), 0);
}

void refreshMenu()
{
    if (!g.menu)
        return;
    XPLMCheckMenuItem(g.menu, g.menuToggle, g.enabled ? xplm_Menu_Checked : xplm_Menu_Unchecked);
    XPLMCheckMenuItem(g.menu, g.menuPause, g.paused ? xplm_Menu_Checked : xplm_Menu_Unchecked);
}

// Puts the head back where it was when tracking took over.
void release(bool write)
{
    if (g.applied && write)
        for (int i = 0; i < HeadAxes; i++)
            XPLMSetDataf(g.head[i], float(g.base[i]));
    g.applied = false;
}

void apply(const double target[HeadAxes], int view)
{
    for (int i = 0; i < HeadAxes; i++) {
        if (!g.applied)
            g.base[i] = XPLMGetDataf(g.head[i]);
        XPLMSetDataf(g.head[i], float(g.base[i] + target[i]));
    }
    g.applied = true;
    g.appliedView = view;
}

float onFlightLoop(float, float, int, void*)
{
    int view = XPLMGetDatai(g.viewType);
    bool vr = g.vrEnabled && XPLMGetDatai(g.vrEnabled) != 0;
    if (g.applied && view != g.appliedView)
        release(false);  // X-Plane resets the head on a view change; nothing of ours to undo

    double pose[6] = {};
    uint32_t flags = 0;
    bool alive = g.bridge.alive();
    bool posed = alive && tir_bridge_read_pose(g.bridge.map, pose, &flags) && (flags & TIR_POSE_TRACKING);

    const char* status;
    if (!g.enabled)
        status = "disabled";
    else if (!alive)
        status = "tracker not running (start trackir-mac)";
    else if (!posed)
        status = "clip not visible";
    else if (vr)
        status = "inactive in VR";
    else if (view != kView3DCockpit)
        status = "waiting for the 3-D cockpit view";
    else
        status = g.paused || (flags & TIR_POSE_PAUSED) ? "paused" : "tracking";
    setStatus(status);

    if (g.enabled && posed && !vr && view == kView3DCockpit) {
        double target[HeadAxes];
        toHeadOffsets(pose, target);
        if (g.paused)
            std::memcpy(target, g.frozen, sizeof(target));
        else
            std::memcpy(g.frozen, target, sizeof(target));
        apply(target, view);
    } else {
        release(view == g.appliedView);
    }
    return -1.0f;  // every frame
}

int onCommand(XPLMCommandRef cmd, XPLMCommandPhase phase, void*)
{
    if (phase != xplm_CommandBegin)
        return 0;
    if (cmd == g.cmdToggle) {
        g.enabled = !g.enabled;
        if (!g.enabled)
            release(true);
    } else if (cmd == g.cmdPause) {
        g.paused = !g.paused;
    } else if (cmd == g.cmdRecenter) {
        if (g.bridge.alive())
            tir_bridge_push_command(g.bridge.map, NP_CMD_RECENTER, 0);
    }
    refreshMenu();
    return 0;
}

void onMenu(void*, void*) {}

}  // namespace

PLUGIN_API int XPluginStart(char* name, char* signature, char* description)
{
    std::strcpy(name, "TrackIR for macOS");
    std::strcpy(signature, "trackir_macos.head_tracking");
    std::strcpy(description, "Head tracking from trackir-mac (TrackIR 5 on macOS).");

    for (int i = 0; i < HeadAxes; i++) {
        g.head[i] = XPLMFindDataRef(kHeadRefs[i]);
        if (!g.head[i]) {
            debug("missing dataref %s", kHeadRefs[i]);
            return 0;
        }
    }
    g.viewType = XPLMFindDataRef("sim/graphics/view/view_type");
    g.vrEnabled = XPLMFindDataRef("sim/graphics/VR/enabled");
    if (!g.viewType)
        return 0;

    g.cmdToggle = XPLMCreateCommand("trackir_macos/toggle", "TrackIR: toggle head tracking");
    g.cmdPause = XPLMCreateCommand("trackir_macos/pause", "TrackIR: pause/resume head tracking");
    g.cmdRecenter = XPLMCreateCommand("trackir_macos/recenter", "TrackIR: recenter");
    for (XPLMCommandRef c : {g.cmdToggle, g.cmdPause, g.cmdRecenter})
        XPLMRegisterCommandHandler(c, onCommand, 1, nullptr);

    XPLMMenuID plugins = XPLMFindPluginsMenu();
    int item = XPLMAppendMenuItem(plugins, "TrackIR (macOS)", nullptr, 0);
    g.menu = XPLMCreateMenu("TrackIR (macOS)", plugins, item, onMenu, nullptr);
    g.menuToggle = XPLMAppendMenuItemWithCommand(g.menu, "Head tracking", g.cmdToggle);
    g.menuPause = XPLMAppendMenuItemWithCommand(g.menu, "Pause", g.cmdPause);
    XPLMAppendMenuItemWithCommand(g.menu, "Recenter", g.cmdRecenter);
    XPLMAppendMenuSeparator(g.menu);
    g.menuStatus = XPLMAppendMenuItem(g.menu, "Status: starting", nullptr, 0);
    XPLMEnableMenuItem(g.menu, g.menuStatus, 0);
    refreshMenu();

    XPLMCreateFlightLoop_t params{int(sizeof(params)), xplm_FlightLoop_Phase_AfterFlightModel, onFlightLoop, nullptr};
    g.loop = XPLMCreateFlightLoop(&params);
    return 1;
}

PLUGIN_API void XPluginStop(void)
{
    if (g.loop)
        XPLMDestroyFlightLoop(g.loop);
    g.loop = nullptr;
    for (XPLMCommandRef c : {g.cmdToggle, g.cmdPause, g.cmdRecenter})
        if (c)
            XPLMUnregisterCommandHandler(c, onCommand, 1, nullptr);
    if (g.menu)
        XPLMDestroyMenu(g.menu);
    g.menu = nullptr;
    g.bridge.close();
}

PLUGIN_API int XPluginEnable(void)
{
    g.status.clear();
    XPLMScheduleFlightLoop(g.loop, -1.0f, 1);
    return 1;
}

PLUGIN_API void XPluginDisable(void)
{
    XPLMScheduleFlightLoop(g.loop, 0, 1);
    release(true);
    g.bridge.close();
}

PLUGIN_API void XPluginReceiveMessage(XPLMPluginID, int message, void* param)
{
    // A newly loaded user aircraft gets its own default head position.
    if (message == XPLM_MSG_PLANE_LOADED && param == nullptr)
        release(false);
}
