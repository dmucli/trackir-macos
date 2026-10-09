// A stand-in for X-Plane's XPLM.framework: just enough of the SDK for the plugin, plus fake_* hooks the test drives.
// Built with the real XPLM install name so dyld hands it to the plugin (via DYLD_FRAMEWORK_PATH).
#include "XPLMDataAccess.h"
#include "XPLMMenus.h"
#include "XPLMProcessing.h"
#include "XPLMUtilities.h"

#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace {

struct DataRef {
    std::string name;
    float f = 0;
    int i = 0;
};

struct Command {
    std::string name;
    std::vector<std::pair<XPLMCommandCallback_f, void*>> handlers;
};

struct MenuItem {
    std::string name;
    XPLMCommandRef command = nullptr;
    int checked = xplm_Menu_NoCheck;
    bool enabled = true;
};

struct Menu {
    std::vector<MenuItem> items;
};

std::map<std::string, DataRef*> gRefs;
std::map<std::string, Command*> gCommands;
std::vector<Menu*> gMenus;
Menu gPluginsMenu;
XPLMCreateFlightLoop_t gLoop{};
bool gLoopCreated = false;
float gLoopInterval = 0;
std::string gLog;

}  // namespace

// --- test hooks ---
extern "C" __attribute__((visibility("default"))) void fake_set_float(const char* name, float v)
{
    auto it = gRefs.find(name);
    if (it != gRefs.end())
        it->second->f = v;
}
extern "C" __attribute__((visibility("default"))) float fake_get_float(const char* name)
{
    auto it = gRefs.find(name);
    return it != gRefs.end() ? it->second->f : -9999.0f;
}
extern "C" __attribute__((visibility("default"))) void fake_set_int(const char* name, int v)
{
    auto it = gRefs.find(name);
    if (it != gRefs.end())
        it->second->i = v;
}
extern "C" __attribute__((visibility("default"))) void fake_define(const char* name)
{
    if (!gRefs.count(name))
        gRefs[name] = new DataRef{name};
}
// Runs the scheduled flight loop once; returns 0 if it is not scheduled.
extern "C" __attribute__((visibility("default"))) int fake_run_flight_loop()
{
    if (!gLoopCreated || gLoopInterval == 0)
        return 0;
    gLoopInterval = gLoop.callbackFunc(0.016f, 0.016f, 1, gLoop.refcon);
    return 1;
}
extern "C" __attribute__((visibility("default"))) int fake_command(const char* name)
{
    auto it = gCommands.find(name);
    if (it == gCommands.end())
        return 0;
    for (auto& [cb, ref] : it->second->handlers) {
        cb(it->second, xplm_CommandBegin, ref);
        cb(it->second, xplm_CommandEnd, ref);
    }
    return 1;
}
// Item `index` of the plugin's submenu: returns its name and fills checked state.
extern "C" __attribute__((visibility("default"))) const char* fake_menu_item(int index, int* checked)
{
    if (gMenus.empty() || index < 0 || index >= int(gMenus.back()->items.size()))
        return nullptr;
    const MenuItem& item = gMenus.back()->items[size_t(index)];
    if (checked)
        *checked = item.checked;
    return item.name.c_str();
}
extern "C" __attribute__((visibility("default"))) const char* fake_log()
{
    return gLog.c_str();
}

// --- XPLM ---
XPLMDataRef XPLMFindDataRef(const char* name)
{
    auto it = gRefs.find(name);
    return it != gRefs.end() ? it->second : nullptr;
}
int XPLMGetDatai(XPLMDataRef r) { return static_cast<DataRef*>(r)->i; }
float XPLMGetDataf(XPLMDataRef r) { return static_cast<DataRef*>(r)->f; }
void XPLMSetDataf(XPLMDataRef r, float v) { static_cast<DataRef*>(r)->f = v; }
void XPLMDebugString(const char* s) { gLog += s; }

XPLMFlightLoopID XPLMCreateFlightLoop(XPLMCreateFlightLoop_t* params)
{
    gLoop = *params;
    gLoopCreated = true;
    return reinterpret_cast<XPLMFlightLoopID>(&gLoop);
}
void XPLMScheduleFlightLoop(XPLMFlightLoopID, float interval, int) { gLoopInterval = interval; }
void XPLMDestroyFlightLoop(XPLMFlightLoopID) { gLoopCreated = false; }

XPLMCommandRef XPLMCreateCommand(const char* name, const char*)
{
    auto*& c = gCommands[name];
    if (!c)
        c = new Command{name, {}};
    return c;
}
void XPLMRegisterCommandHandler(XPLMCommandRef cmd, XPLMCommandCallback_f cb, int, void* ref)
{
    static_cast<Command*>(cmd)->handlers.emplace_back(cb, ref);
}
void XPLMUnregisterCommandHandler(XPLMCommandRef cmd, XPLMCommandCallback_f cb, int, void* ref)
{
    auto& h = static_cast<Command*>(cmd)->handlers;
    for (auto it = h.begin(); it != h.end(); ++it)
        if (it->first == cb && it->second == ref) {
            h.erase(it);
            break;
        }
}

XPLMMenuID XPLMFindPluginsMenu(void) { return &gPluginsMenu; }
XPLMMenuID XPLMCreateMenu(const char*, XPLMMenuID, int, XPLMMenuHandler_f, void*)
{
    gMenus.push_back(new Menu);
    return gMenus.back();
}
void XPLMDestroyMenu(XPLMMenuID) {}
int XPLMAppendMenuItem(XPLMMenuID menu, const char* name, void*, int)
{
    auto* m = static_cast<Menu*>(menu);
    m->items.push_back({name});
    return int(m->items.size()) - 1;
}
int XPLMAppendMenuItemWithCommand(XPLMMenuID menu, const char* name, XPLMCommandRef cmd)
{
    auto* m = static_cast<Menu*>(menu);
    m->items.push_back({name, cmd});
    return int(m->items.size()) - 1;
}
void XPLMAppendMenuSeparator(XPLMMenuID menu) { static_cast<Menu*>(menu)->items.push_back({"-"}); }
void XPLMSetMenuItemName(XPLMMenuID menu, int index, const char* name, int)
{
    static_cast<Menu*>(menu)->items.at(size_t(index)).name = name;
}
void XPLMCheckMenuItem(XPLMMenuID menu, int index, XPLMMenuCheck check)
{
    static_cast<Menu*>(menu)->items.at(size_t(index)).checked = check;
}
void XPLMEnableMenuItem(XPLMMenuID menu, int index, int enabled)
{
    static_cast<Menu*>(menu)->items.at(size_t(index)).enabled = enabled != 0;
}
