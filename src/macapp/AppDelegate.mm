// Menu-bar app: owns the tracking engine on a background thread, the status item, global hotkeys and windows.
#import "MacApp.h"

#import <Carbon/Carbon.h>
#import <ServiceManagement/ServiceManagement.h>

#include "app/resources.hpp"

#include <deque>
#include <memory>
#include <mutex>
#include <thread>

#include <signal.h>
#include <sys/stat.h>

using namespace tir;

int functionKeyCode(NSString* name)
{
    static const int codes[19] = {kVK_F1,  kVK_F2,  kVK_F3,  kVK_F4,  kVK_F5,  kVK_F6,  kVK_F7,
                                  kVK_F8,  kVK_F9,  kVK_F10, kVK_F11, kVK_F12, kVK_F13, kVK_F14,
                                  kVK_F15, kVK_F16, kVK_F17, kVK_F18, kVK_F19};
    if (name.length < 2 || ![name hasPrefix:@"F"])
        return -1;
    int n = [name substringFromIndex:1].intValue;
    return n >= 1 && n <= 19 ? codes[n - 1] : -1;
}

NSArray<NSString*>* functionKeyNames()
{
    NSMutableArray* names = [NSMutableArray arrayWithObject:@"none"];
    for (int i = 1; i <= 19; i++)
        [names addObject:[NSString stringWithFormat:@"F%d", i]];
    return names;
}

static NSString* ns(const std::string& s)
{
    return [NSString stringWithUTF8String:s.c_str()] ?: @"";
}

// Runs one Engine at a time on its own thread.
class EngineHost {
public:
    ~EngineHost() { stop(); }

    void start(const EngineOptions& options, const Profile& profile)
    {
        stop();
        engine_ = std::make_unique<Engine>(options);
        engine_->setProfile(profile);
        engine_->log = [this](const std::string& line) {
            std::lock_guard<std::mutex> lock(logMutex_);
            log_.push_back(line);
            while (log_.size() > 200)
                log_.pop_front();
        };
        Engine* e = engine_.get();
        thread_ = std::thread([e] { e->run(); });
    }

    void stop()
    {
        if (!engine_)
            return;
        engine_->stop();
        if (thread_.joinable())
            thread_.join();
        engine_.reset();
    }

    Engine* engine() { return engine_.get(); }

    std::vector<std::string> log()
    {
        std::lock_guard<std::mutex> lock(logMutex_);
        return {log_.begin(), log_.end()};
    }

private:
    std::unique_ptr<Engine> engine_;
    std::thread thread_;
    std::mutex logMutex_;
    std::deque<std::string> log_;
};

@interface AppDelegate : NSObject <NSApplicationDelegate, NSMenuDelegate, TrackerController>
@end

@implementation AppDelegate {
    EngineHost _host;
    Settings _settings;
    Profile _profile;
    NSString* _profilePath;
    BOOL _profileModified;
    BOOL _trackingOn;
    BOOL _pausedWhileOff;

    NSStatusItem* _statusItem;
    NSMenuItem* _statusLine;
    NSMenuItem* _trackingItem;
    NSMenuItem* _pauseItem;
    NSMenuItem* _recenterItem;
    NSMenuItem* _profileItem;
    NSMenuItem* _loginItem;
    NSTimer* _timer;
    NSString* _lastSymbol;
    struct timespec _settingsMtime;

    EventHotKeyRef _hotRecenter;
    EventHotKeyRef _hotPause;

    LiveWindowController* _live;
    SettingsWindowController* _settingsWindow;
}

static OSStatus onHotKey(EventHandlerCallRef, EventRef event, void* context)
{
    EventHotKeyID key{};
    GetEventParameter(event, kEventParamDirectObject, typeEventHotKeyID, nullptr, sizeof(key), nullptr, &key);
    AppDelegate* app = (__bridge AppDelegate*)context;
    dispatch_async(dispatch_get_main_queue(), ^{
        if (key.id == 1)
            [app recenter];
        else if (key.id == 2)
            [app togglePause];
    });
    return noErr;
}

- (void)applicationDidFinishLaunching:(NSNotification*)note
{
    signal(SIGUSR1, SIG_IGN);  // older `trackir-mac recenter` builds signal the bridge owner
    _settings = Settings::load();
    _settingsMtime = [self settingsModificationTime];
    [self loadProfileFromSettings];

    _statusItem = [[NSStatusBar systemStatusBar] statusItemWithLength:NSVariableStatusItemLength];
    _statusItem.button.toolTip = @"TrackIR for macOS";
    [self buildMenu];
    [self setSymbol:@"video.slash"];

    EventTypeSpec spec{kEventClassKeyboard, kEventHotKeyPressed};
    InstallApplicationEventHandler(&onHotKey, 1, &spec, (__bridge void*)self, nullptr);
    [self registerHotKeys];

    _trackingOn = YES;
    [self startEngine];
    _timer = [NSTimer scheduledTimerWithTimeInterval:1.0 / 15 target:self selector:@selector(tick)
                                            userInfo:nil repeats:YES];
    [[NSRunLoop currentRunLoop] addTimer:_timer forMode:NSRunLoopCommonModes];

    NSArray* args = [NSProcessInfo processInfo].arguments;
    if ([args containsObject:@"--live"])
        [self showLive:nil];
    if ([args containsObject:@"--settings"])
        [self showSettings:nil];
}

- (void)applicationWillTerminate:(NSNotification*)note
{
    _host.stop();  // switches the camera off cleanly
}

// --- engine -----------------------------------------------------------------------------------------------------

- (void)startEngine
{
    EngineOptions options;
    options.settings = _settings;
    options.waitForCamera = YES;
    _host.start(options, _profile);
}

- (void)loadProfileFromSettings
{
    std::string error;
    _profile = Engine::loadProfile(_settings.profile, &error);
    _profilePath = _settings.profile.empty() || !error.empty() ? nil : ns(_settings.profile);
    _profileModified = NO;
}

- (EngineStatus)currentStatus
{
    if (Engine* e = _host.engine())
        return e->status();
    EngineStatus s;
    s.state = EngineStatus::State::Stopped;
    s.message = "tracking is off";
    return s;
}

- (Settings)settings
{
    return _settings;
}

- (void)applySettings:(const Settings&)settings
{
    const Settings& old = _settings;
    bool restart = old.clipType != settings.clipType || old.clipLeg != settings.clipLeg ||
                   old.clipBase != settings.clipBase || old.camera.threshold != settings.camera.threshold ||
                   old.camera.exposure != settings.camera.exposure ||
                   old.camera.irIntensity != settings.camera.irIntensity || old.udp != settings.udp ||
                   old.udpHost != settings.udpHost || old.udpPort != settings.udpPort ||
                   old.pivot.x != settings.pivot.x || old.pivot.y != settings.pivot.y ||
                   old.pivot.z != settings.pivot.z || old.focalScale != settings.focalScale ||
                   old.gameKeys != settings.gameKeys;
    bool keys = old.hotkeyRecenter != settings.hotkeyRecenter || old.hotkeyPause != settings.hotkeyPause;
    _settings = settings;
    _settings.save();
    _settingsMtime = [self settingsModificationTime];
    if (keys)
        [self registerHotKeys];
    if (Engine* e = _host.engine()) {
        e->setSmoothing(_settings.smoothing);
        e->setAxisSigns(_settings.axisSign);
    }
    if (restart && _trackingOn) {
        bool paused = _host.engine() && _host.engine()->paused();
        [self startEngine];
        _host.engine()->setPaused(paused);
    }
    [self updateMenuState];
}

- (Profile)profile
{
    return _profile;
}

- (NSString*)profilePath
{
    return _profilePath;
}

- (BOOL)profileModified
{
    return _profileModified;
}

- (void)editProfile:(const Profile&)profile
{
    _profile = profile;
    _profileModified = YES;
    if (Engine* e = _host.engine())
        e->setProfile(_profile);
    [self rebuildProfileMenu];
}

- (BOOL)saveProfileAs:(NSString*)path
{
    Profile p = _profile;
    p.name = path.lastPathComponent.stringByDeletingPathExtension.UTF8String;
    [[NSFileManager defaultManager] createDirectoryAtPath:path.stringByDeletingLastPathComponent
                              withIntermediateDirectories:YES attributes:nil error:nil];
    if (!p.save(path.UTF8String))
        return NO;
    _profile = p;
    _profilePath = path;
    _profileModified = NO;
    Settings s = _settings;
    s.profile = path.UTF8String;
    [self applySettings:s];
    [self rebuildProfileMenu];
    return YES;
}

- (void)revertProfile
{
    [self loadProfileFromSettings];
    if (Engine* e = _host.engine())
        e->setProfile(_profile);
    [self rebuildProfileMenu];
}

- (void)recenter
{
    if (Engine* e = _host.engine())
        e->recenter();
}

- (void)togglePause
{
    if (Engine* e = _host.engine())
        e->setPaused(!e->paused());
    [self updateMenuState];
}

- (NSArray<NSString*>*)recentLog
{
    NSMutableArray* lines = [NSMutableArray array];
    for (const auto& l : _host.log())
        [lines addObject:ns(l)];
    return lines;
}

// --- menu -------------------------------------------------------------------------------------------------------

- (NSMenuItem*)item:(NSString*)title action:(SEL)action
{
    NSMenuItem* item = [[NSMenuItem alloc] initWithTitle:title action:action keyEquivalent:@""];
    item.target = self;
    return item;
}

- (void)buildMenu
{
    NSMenu* menu = [[NSMenu alloc] init];
    menu.delegate = self;
    menu.autoenablesItems = NO;
    _statusLine = [self item:@"Starting…" action:nil];
    _statusLine.enabled = NO;
    [menu addItem:_statusLine];
    [menu addItem:[NSMenuItem separatorItem]];
    _trackingItem = [self item:@"Tracking" action:@selector(toggleTracking:)];
    [menu addItem:_trackingItem];
    _pauseItem = [self item:@"Pause" action:@selector(pauseClicked:)];
    [menu addItem:_pauseItem];
    _recenterItem = [self item:@"Recenter" action:@selector(recenterClicked:)];
    [menu addItem:_recenterItem];
    [menu addItem:[NSMenuItem separatorItem]];
    [menu addItem:[self item:@"Live View…" action:@selector(showLive:)]];
    _profileItem = [self item:@"Profile" action:nil];
    _profileItem.submenu = [[NSMenu alloc] init];
    [menu addItem:_profileItem];
    NSMenuItem* settings = [self item:@"Settings…" action:@selector(showSettings:)];
    settings.keyEquivalent = @",";
    [menu addItem:settings];
    [menu addItem:[self item:@"Run Axis Check in Terminal…" action:@selector(runAxisCheck:)]];
    [menu addItem:[NSMenuItem separatorItem]];
    _loginItem = [self item:@"Open at Login" action:@selector(toggleLogin:)];
    [menu addItem:_loginItem];
    NSMenuItem* quit = [[NSMenuItem alloc] initWithTitle:@"Quit TrackIR for macOS" action:@selector(terminate:)
                                           keyEquivalent:@"q"];
    [menu addItem:quit];
    _statusItem.menu = menu;
    [self rebuildProfileMenu];
    [self updateMenuState];
}

- (void)menuWillOpen:(NSMenu*)menu
{
    [self rebuildProfileMenu];
    [self updateMenuState];
}

- (NSString*)hotkeySuffix:(const std::string&)key
{
    return key == "none" || key.empty() ? @"" : [NSString stringWithFormat:@"   (%@)", ns(key)];
}

- (void)updateMenuState
{
    _trackingItem.state = _trackingOn ? NSControlStateValueOn : NSControlStateValueOff;
    Engine* e = _host.engine();
    _pauseItem.state = e && e->paused() ? NSControlStateValueOn : NSControlStateValueOff;
    _pauseItem.enabled = e != nullptr;
    _recenterItem.enabled = e != nullptr;
    _pauseItem.title = [@"Pause" stringByAppendingString:[self hotkeySuffix:_settings.hotkeyPause]];
    _recenterItem.title = [@"Recenter" stringByAppendingString:[self hotkeySuffix:_settings.hotkeyRecenter]];
    if (@available(macOS 13.0, *)) {
        _loginItem.state = SMAppService.mainAppService.status == SMAppServiceStatusEnabled ? NSControlStateValueOn
                                                                                            : NSControlStateValueOff;
    } else {
        _loginItem.hidden = YES;
    }
}

- (void)rebuildProfileMenu
{
    NSMenu* sub = _profileItem.submenu;
    [sub removeAllItems];
    NSMenuItem* linear = [self item:@"Linear (1:1)" action:@selector(chooseProfile:)];
    linear.representedObject = @"";
    linear.state = _profilePath == nil ? NSControlStateValueOn : NSControlStateValueOff;
    [sub addItem:linear];
    NSString* dir = ns(profileDirectory());
    NSArray* files = [[[NSFileManager defaultManager] contentsOfDirectoryAtPath:dir error:nil]
        sortedArrayUsingSelector:@selector(localizedCaseInsensitiveCompare:)];
    BOOL listedCurrent = _profilePath == nil;
    for (NSString* f in files) {
        if (![f.pathExtension.lowercaseString isEqualToString:@"xml"])
            continue;
        NSString* path = [dir stringByAppendingPathComponent:f];
        NSMenuItem* it = [self item:f.stringByDeletingPathExtension action:@selector(chooseProfile:)];
        it.representedObject = path;
        if ([path isEqualToString:_profilePath]) {
            it.state = NSControlStateValueOn;
            listedCurrent = YES;
        }
        [sub addItem:it];
    }
    if (!listedCurrent) {
        NSMenuItem* it = [self item:_profilePath.lastPathComponent.stringByDeletingPathExtension
                             action:@selector(chooseProfile:)];
        it.representedObject = _profilePath;
        it.state = NSControlStateValueOn;
        [sub addItem:it];
    }
    if (_profileModified) {
        NSMenuItem* m = [self item:@"(edited, not saved)" action:nil];
        m.enabled = NO;
        [sub addItem:m];
    }
    [sub addItem:[NSMenuItem separatorItem]];
    [sub addItem:[self item:@"Import Profile…" action:@selector(importProfile:)]];
    [sub addItem:[self item:@"Edit Curves…" action:@selector(showSettings:)]];
    [sub addItem:[self item:@"Show Profiles Folder" action:@selector(showProfilesFolder:)]];
    NSString* name = _profilePath ? _profilePath.lastPathComponent.stringByDeletingPathExtension : @"Linear (1:1)";
    _profileItem.title = [NSString stringWithFormat:@"Profile: %@%@", name, _profileModified ? @" (edited)" : @""];
}

- (void)chooseProfile:(NSMenuItem*)sender
{
    NSString* path = sender.representedObject;
    Settings s = _settings;
    s.profile = path.length ? path.UTF8String : "";
    std::string error;
    Profile p = Engine::loadProfile(s.profile, &error);
    if (!error.empty()) {
        [self alert:@"Cannot load profile" info:ns(error)];
        return;
    }
    _profile = p;
    _profilePath = path.length ? path : nil;
    _profileModified = NO;
    if (Engine* e = _host.engine())
        e->setProfile(_profile);
    [self applySettings:s];
    [self rebuildProfileMenu];
    [_settingsWindow reload];
}

- (void)importProfile:(id)sender
{
    NSOpenPanel* panel = [NSOpenPanel openPanel];
    panel.allowedContentTypes = @[ UTTypeXML ];
    panel.message = @"Choose a TrackIR profile (.xml)";
    [NSApp activateIgnoringOtherApps:YES];
    if ([panel runModal] != NSModalResponseOK)
        return;
    NSString* src = panel.URL.path;
    try {
        Profile::load(src.UTF8String);
    } catch (const std::exception& e) {
        [self alert:@"Not a TrackIR profile" info:ns(e.what())];
        return;
    }
    NSString* dir = ns(profileDirectory());
    [[NSFileManager defaultManager] createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
    NSString* dst = [dir stringByAppendingPathComponent:src.lastPathComponent];
    [[NSFileManager defaultManager] removeItemAtPath:dst error:nil];
    NSError* err = nil;
    if (![[NSFileManager defaultManager] copyItemAtPath:src toPath:dst error:&err]) {
        [self alert:@"Cannot import profile" info:err.localizedDescription];
        return;
    }
    NSMenuItem* fake = [[NSMenuItem alloc] init];
    fake.representedObject = dst;
    [self chooseProfile:fake];
}

- (void)showProfilesFolder:(id)sender
{
    NSString* dir = ns(profileDirectory());
    [[NSFileManager defaultManager] createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
    [[NSWorkspace sharedWorkspace] openURL:[NSURL fileURLWithPath:dir]];
}

- (void)toggleTracking:(id)sender
{
    _trackingOn = !_trackingOn;
    if (_trackingOn)
        [self startEngine];
    else
        _host.stop();
    [self updateMenuState];
}

- (void)pauseClicked:(id)sender
{
    [self togglePause];
}

- (void)recenterClicked:(id)sender
{
    [self recenter];
}

- (void)showLive:(id)sender
{
    if (!_live)
        _live = [[LiveWindowController alloc] initWithController:self];
    [NSApp activateIgnoringOtherApps:YES];
    [_live showWindow:nil];
    [_live.window makeKeyAndOrderFront:nil];
}

- (void)showSettings:(id)sender
{
    if (!_settingsWindow)
        _settingsWindow = [[SettingsWindowController alloc] initWithController:self];
    [_settingsWindow reload];
    [NSApp activateIgnoringOtherApps:YES];
    [_settingsWindow showWindow:nil];
    [_settingsWindow.window makeKeyAndOrderFront:nil];
}

// The check is interactive (it asks the user to move and press Enter), so it runs in Terminal with the CLI bundled
// in the app. Tracking stops meanwhile because only one program can drive the camera.
- (void)runAxisCheck:(id)sender
{
    NSString* cli = [[NSBundle mainBundle].executablePath.stringByDeletingLastPathComponent
        stringByAppendingPathComponent:@"trackir-mac"];
    if (![[NSFileManager defaultManager] isExecutableFileAtPath:cli]) {
        [self alert:@"trackir-mac not found" info:@"Rebuild the app with `make app`."];
        return;
    }
    _trackingOn = NO;
    _host.stop();
    [self updateMenuState];
    NSString* script = [NSTemporaryDirectory() stringByAppendingPathComponent:@"TrackIR-axis-check.command"];
    NSString* body = [NSString stringWithFormat:@"#!/bin/sh\nclear\n'%@' check\necho\necho 'Done. Switch "
                                                @"Tracking back on in the TrackIR menu. You can close this window.'\n",
                                                [cli stringByReplacingOccurrencesOfString:@"'" withString:@"'\\''"]];
    [body writeToFile:script atomically:YES encoding:NSUTF8StringEncoding error:nil];
    chmod(script.fileSystemRepresentation, 0755);
    [[NSWorkspace sharedWorkspace] openURL:[NSURL fileURLWithPath:script]];
}

- (void)toggleLogin:(id)sender
{
    if (@available(macOS 13.0, *)) {
        NSError* err = nil;
        SMAppService* service = SMAppService.mainAppService;
        BOOL ok = service.status == SMAppServiceStatusEnabled ? [service unregisterAndReturnError:&err]
                                                              : [service registerAndReturnError:&err];
        if (!ok)
            [self alert:@"Cannot change Open at Login" info:err.localizedDescription];
    }
    [self updateMenuState];
}

- (void)alert:(NSString*)title info:(NSString*)info
{
    NSAlert* a = [[NSAlert alloc] init];
    a.messageText = title;
    a.informativeText = info ?: @"";
    [NSApp activateIgnoringOtherApps:YES];
    [a runModal];
}

// --- hotkeys ----------------------------------------------------------------------------------------------------

- (void)registerHotKeys
{
    if (_hotRecenter)
        UnregisterEventHotKey(_hotRecenter);
    if (_hotPause)
        UnregisterEventHotKey(_hotPause);
    _hotRecenter = _hotPause = nullptr;
    int recenter = functionKeyCode(ns(_settings.hotkeyRecenter));
    int pause = functionKeyCode(ns(_settings.hotkeyPause));
    if (recenter >= 0)
        RegisterEventHotKey(UInt32(recenter), 0, EventHotKeyID{'TIRM', 1}, GetApplicationEventTarget(), 0, &_hotRecenter);
    if (pause >= 0 && pause != recenter)
        RegisterEventHotKey(UInt32(pause), 0, EventHotKeyID{'TIRM', 2}, GetApplicationEventTarget(), 0, &_hotPause);
}

// --- periodic UI update -----------------------------------------------------------------------------------------

- (struct timespec)settingsModificationTime
{
    struct stat st {};
    if (stat(Settings::defaultPath().c_str(), &st) != 0)
        return {};
    return st.st_mtimespec;
}

- (void)setSymbol:(NSString*)name
{
    if ([name isEqualToString:_lastSymbol])
        return;
    _lastSymbol = name;
    NSImage* image = [NSImage imageWithSystemSymbolName:name accessibilityDescription:@"TrackIR"];
    [image setTemplate:YES];
    _statusItem.button.image = image;
}

- (void)tick
{
    // `trackir-mac check` (or a text editor) changed settings.ini: pick it up.
    struct timespec m = [self settingsModificationTime];
    if (m.tv_sec != _settingsMtime.tv_sec || m.tv_nsec != _settingsMtime.tv_nsec) {
        _settingsMtime = m;
        Settings fresh = Settings::load();
        [self applySettings:fresh];
        [_settingsWindow reload];
    }

    EngineStatus s = [self currentStatus];
    NSString* line;
    NSString* symbol;
    using State = EngineStatus::State;
    if (!_trackingOn) {
        line = @"Tracking is off";
        symbol = @"pause.circle";
    } else if (s.state == State::NoCamera) {
        line = @"No TrackIR camera connected";
        symbol = @"video.slash";
    } else if (s.state == State::Failed) {
        line = [NSString stringWithFormat:@"Camera error: %@", ns(s.message)];
        symbol = @"exclamationmark.triangle";
    } else if (s.state != State::Streaming) {
        line = @"Connecting to the camera…";
        symbol = @"video";
    } else if (s.paused) {
        line = @"Paused";
        symbol = @"pause.circle";
    } else if (!s.clipVisible) {
        line = [NSString stringWithFormat:@"Clip not visible (%zu markers seen)", s.blobs.size()];
        symbol = @"eye.slash";
    } else {
        line = [NSString stringWithFormat:@"Tracking, %u fps, clip %.0f cm away", s.framesPerSecond, s.distanceCm];
        symbol = @"scope";
    }
    _statusLine.title = line;
    _statusItem.button.toolTip = line;
    [self setSymbol:symbol];
    if (_live.window.visible)
        [_live refresh];
    if (_settingsWindow.window.visible)
        [_settingsWindow refresh];
}

@end

int main(int argc, const char* argv[])
{
    @autoreleasepool {
        NSApplication* app = [NSApplication sharedApplication];
        app.activationPolicy = NSApplicationActivationPolicyAccessory;
        AppDelegate* delegate = [[AppDelegate alloc] init];
        app.delegate = delegate;
        [app run];
    }
    return 0;
}
