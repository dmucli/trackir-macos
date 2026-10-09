// Menu-bar app: shared interface between the windows and the object that owns the tracking engine.
#pragma once

#import <Cocoa/Cocoa.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include "app/engine.hpp"
#include "app/settings.hpp"
#include "output/profile.hpp"

@protocol TrackerController <NSObject>
- (tir::EngineStatus)currentStatus;
- (tir::Settings)settings;
// Saves the settings; camera-level changes (clip, IR, threshold, UDP) restart the camera.
- (void)applySettings:(const tir::Settings&)settings;
- (tir::Profile)profile;
- (NSString*)profilePath;  // nil for the built-in 1:1 profile
- (BOOL)profileModified;
// Live curve edits (not saved until saveProfileAs:).
- (void)editProfile:(const tir::Profile&)profile;
- (BOOL)saveProfileAs:(NSString*)path;
- (void)revertProfile;  // back to the saved file
- (void)recenter;
- (void)togglePause;
- (NSArray<NSString*>*)recentLog;
@end

@interface LiveWindowController : NSWindowController
- (instancetype)initWithController:(id<TrackerController>)controller;
- (void)refresh;
@end

@interface SettingsWindowController : NSWindowController
- (instancetype)initWithController:(id<TrackerController>)controller;
- (void)reload;   // settings or profile changed elsewhere
- (void)refresh;  // live values
@end

// "F1".."F19" -> Carbon virtual key code, or -1 ("none").
int functionKeyCode(NSString* name);
NSArray<NSString*>* functionKeyNames();  // "none", "F1".."F19"
