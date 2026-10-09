// Settings: camera/clip, per-axis curves (TrackIR profile format), outputs and hotkeys.
#import "MacApp.h"

#include <algorithm>
#include <cmath>

using namespace tir;

static NSString* ns(const std::string& s)
{
    return [NSString stringWithUTF8String:s.c_str()] ?: @"";
}

// --- curve editor -----------------------------------------------------------------------------------------------
// A TrackIR curve is a list of (input, slope) points; the output is the integral of the slope (profile.cpp), so the
// slope is the "speed" at that head angle. The editor shows the positive half and mirrors it.

@interface CurveView : NSView
@property(nonatomic) AxisCurve curve;
@property(nonatomic) double liveInput;   // current |measured| value, drawn as a marker
@property(nonatomic) BOOL rotation;      // degrees vs centimetres
@property(nonatomic, copy) void (^onChange)(const AxisCurve&);
@end

@implementation CurveView {
    int _dragging;
    double _xMax, _yMax;
}

- (instancetype)initWithFrame:(NSRect)frame
{
    if ((self = [super initWithFrame:frame]))
        _dragging = -1;
    return self;
}

- (BOOL)isFlipped
{
    return NO;
}

- (std::vector<std::pair<double, double>>)positiveHalf
{
    std::vector<std::pair<double, double>> pts;
    for (const auto& p : _curve.points)
        if (p.first >= 0)
            pts.push_back(p);
    if (pts.empty())
        pts = {{0, 1}, {_rotation ? 90.0 : 30.0, 1}};
    return pts;
}

- (void)setPositiveHalf:(const std::vector<std::pair<double, double>>&)pts
{
    std::vector<std::pair<double, double>> all;
    for (const auto& p : pts)
        if (p.first > 0)
            all.emplace_back(-p.first, p.second);
    all.insert(all.end(), pts.begin(), pts.end());
    std::sort(all.begin(), all.end());
    _curve.points = all;
}

- (void)rescale
{
    auto pts = [self positiveHalf];
    double maxIn = pts.back().first, maxSlope = 0;
    for (const auto& p : pts)
        maxSlope = std::max(maxSlope, p.second);
    _xMax = std::max(maxIn * 1.15, _rotation ? 20.0 : 10.0);
    _yMax = std::max(2.0, std::ceil(maxSlope * 1.25 + 0.5));
}

- (void)setCurve:(AxisCurve)curve
{
    _curve = curve;
    [self rescale];
    self.needsDisplay = YES;
}

- (NSRect)plot
{
    return NSInsetRect(self.bounds, 36, 24);
}

- (NSPoint)pointFor:(double)x slope:(double)y
{
    NSRect r = [self plot];
    return NSMakePoint(r.origin.x + x / _xMax * r.size.width, r.origin.y + y / _yMax * r.size.height);
}

- (void)drawRect:(NSRect)dirty
{
    if (_xMax <= 0)
        [self rescale];
    NSRect r = [self plot];
    [[NSColor textBackgroundColor] setFill];
    NSRectFill(self.bounds);
    NSDictionary* small = @{
        NSFontAttributeName : [NSFont systemFontOfSize:10],
        NSForegroundColorAttributeName : NSColor.secondaryLabelColor
    };
    NSBezierPath* grid = [NSBezierPath bezierPath];
    for (int i = 0; i <= 4; i++) {
        CGFloat x = r.origin.x + r.size.width * i / 4, y = r.origin.y + r.size.height * i / 4;
        [grid moveToPoint:NSMakePoint(x, r.origin.y)];
        [grid lineToPoint:NSMakePoint(x, NSMaxY(r))];
        [grid moveToPoint:NSMakePoint(r.origin.x, y)];
        [grid lineToPoint:NSMakePoint(NSMaxX(r), y)];
        [[NSString stringWithFormat:@"%.0f%@", _xMax * i / 4, _rotation ? @"°" : @""]
               drawAtPoint:NSMakePoint(x - 8, r.origin.y - 18)
            withAttributes:small];
        [[NSString stringWithFormat:@"%.1f×", _yMax * i / 4] drawAtPoint:NSMakePoint(2, y - 6) withAttributes:small];
    }
    [[NSColor separatorColor] setStroke];
    grid.lineWidth = 0.5;
    [grid stroke];

    auto pts = [self positiveHalf];
    NSBezierPath* line = [NSBezierPath bezierPath];
    [line moveToPoint:[self pointFor:0 slope:pts.front().second]];
    for (const auto& p : pts)
        [line lineToPoint:[self pointFor:p.first slope:p.second]];
    [line lineToPoint:[self pointFor:_xMax slope:pts.back().second]];
    NSBezierPath* area = [line copy];
    [area lineToPoint:[self pointFor:_xMax slope:0]];
    [area lineToPoint:[self pointFor:0 slope:0]];
    [area closePath];
    [[NSColor.controlAccentColor colorWithAlphaComponent:0.15] setFill];
    [area fill];
    [NSColor.controlAccentColor setStroke];
    line.lineWidth = 2;
    [line stroke];
    for (size_t i = 0; i < pts.size(); i++) {
        NSPoint c = [self pointFor:pts[i].first slope:pts[i].second];
        [(int(i) == _dragging ? NSColor.systemOrangeColor : NSColor.controlAccentColor) setFill];
        [[NSBezierPath bezierPathWithOvalInRect:NSMakeRect(c.x - 5, c.y - 5, 10, 10)] fill];
    }

    // Live marker: where the head is now, and what the curve makes of it.
    double in = std::min(std::fabs(_liveInput), _xMax);
    NSPoint top = [self pointFor:in slope:_yMax];
    [[NSColor.systemRedColor colorWithAlphaComponent:0.7] setFill];
    NSRectFill(NSMakeRect(top.x - 0.75, r.origin.y, 1.5, r.size.height));
    double out = integrateSlopeCurve(_curve.points, std::fabs(_liveInput));
    NSString* caption = [NSString stringWithFormat:@"head %.1f%@ → view %.1f%@   (drag points: up = faster)",
                                                   std::fabs(_liveInput), _rotation ? @"°" : @" cm", out,
                                                   _rotation ? @"°" : @" cm"];
    [caption drawAtPoint:NSMakePoint(r.origin.x, NSMaxY(r) + 6) withAttributes:small];
}

- (void)mouseDown:(NSEvent*)event
{
    NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
    auto pts = [self positiveHalf];
    _dragging = -1;
    double best = 12;
    for (size_t i = 0; i < pts.size(); i++) {
        NSPoint c = [self pointFor:pts[i].first slope:pts[i].second];
        double d = std::hypot(c.x - p.x, c.y - p.y);
        if (d < best) {
            best = d;
            _dragging = int(i);
        }
    }
    self.needsDisplay = YES;
}

- (void)mouseDragged:(NSEvent*)event
{
    if (_dragging < 0)
        return;
    NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
    NSRect r = [self plot];
    auto pts = [self positiveHalf];
    size_t i = size_t(_dragging);
    double slope = std::clamp((p.y - r.origin.y) / r.size.height * _yMax, 0.0, _yMax);
    pts[i].second = std::round(slope * 100) / 100;
    if (pts[i].first > 0) {  // the centre point stays at 0
        double lo = i > 0 ? pts[i - 1].first + 0.5 : 0.5;
        double hi = i + 1 < pts.size() ? pts[i + 1].first - 0.5 : _xMax;
        double x = std::clamp((p.x - r.origin.x) / r.size.width * _xMax, lo, std::max(lo, hi));
        pts[i].first = std::round(x * 10) / 10;
    }
    [self setPositiveHalf:pts];
    self.needsDisplay = YES;
    if (_onChange)
        _onChange(_curve);
}

- (void)mouseUp:(NSEvent*)event
{
    _dragging = -1;
    [self rescale];
    self.needsDisplay = YES;
}

@end

// --- window -----------------------------------------------------------------------------------------------------

@implementation SettingsWindowController {
    __weak id<TrackerController> _controller;
    int _axis;
    // General
    NSPopUpButton* _clipType;
    NSButton* _irLights;
    NSSlider* _smoothing;
    NSTextField* _smoothingValue;
    NSSlider* _threshold;
    NSTextField* _thresholdValue;
    NSTextField* _signs;
    // Curves
    NSSegmentedControl* _axisPicker;
    CurveView* _curveView;
    NSButton* _axisEnabled;
    NSButton* _axisInverted;
    NSTextField* _profileLabel;
    // Output & keys
    NSButton* _udp;
    NSTextField* _udpHost;
    NSTextField* _udpPort;
    NSPopUpButton* _keyRecenter;
    NSPopUpButton* _keyPause;
}

static NSTextField* label(NSString* text)
{
    NSTextField* l = [NSTextField labelWithString:text];
    l.alignment = NSTextAlignmentRight;
    return l;
}

static NSTextField* note(NSString* text)
{
    NSTextField* l = [NSTextField wrappingLabelWithString:text];
    l.font = [NSFont systemFontOfSize:11];
    l.textColor = NSColor.secondaryLabelColor;
    l.preferredMaxLayoutWidth = 440;
    return l;
}

static NSView* padded(NSView* content)
{
    NSView* box = [[NSView alloc] init];
    content.translatesAutoresizingMaskIntoConstraints = NO;
    [box addSubview:content];
    [NSLayoutConstraint activateConstraints:@[
        [content.leadingAnchor constraintEqualToAnchor:box.leadingAnchor constant:20],
        [content.trailingAnchor constraintLessThanOrEqualToAnchor:box.trailingAnchor constant:-20],
        [content.topAnchor constraintEqualToAnchor:box.topAnchor constant:20],
        [content.bottomAnchor constraintLessThanOrEqualToAnchor:box.bottomAnchor constant:-20],
    ]];
    return box;
}

- (instancetype)initWithController:(id<TrackerController>)controller
{
    NSWindow* w = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 640, 520)
                                              styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                                                backing:NSBackingStoreBuffered
                                                  defer:NO];
    w.title = @"TrackIR Settings";
    w.releasedWhenClosed = NO;
    if (!(self = [super initWithWindow:w]))
        return nil;
    _controller = controller;

    NSTabView* tabs = [[NSTabView alloc] initWithFrame:NSMakeRect(0, 0, 640, 520)];
    NSTabViewItem* general = [[NSTabViewItem alloc] initWithIdentifier:@"general"];
    general.label = @"Camera";
    general.view = [self buildGeneral];
    NSTabViewItem* curves = [[NSTabViewItem alloc] initWithIdentifier:@"curves"];
    curves.label = @"Curves";
    curves.view = [self buildCurves];
    NSTabViewItem* output = [[NSTabViewItem alloc] initWithIdentifier:@"output"];
    output.label = @"Output & Keys";
    output.view = [self buildOutput];
    [tabs addTabViewItem:general];
    [tabs addTabViewItem:curves];
    [tabs addTabViewItem:output];
    w.contentView = tabs;
    [w center];
    [self reload];
    return self;
}

- (NSView*)buildGeneral
{
    _clipType = [[NSPopUpButton alloc] init];
    [_clipType addItemsWithTitles:@[ @"TrackClip (reflective)", @"TrackClip PRO (LEDs)" ]];
    _clipType.target = self;
    _clipType.action = @selector(changed:);
    _irLights = [NSButton checkboxWithTitle:@"Camera IR lights on (turn off for TrackClip PRO)" target:self
                                     action:@selector(changed:)];
    _smoothing = [NSSlider sliderWithValue:0.3 minValue:0 maxValue:0.95 target:self action:@selector(changed:)];
    _smoothingValue = [NSTextField labelWithString:@""];
    _threshold = [NSSlider sliderWithValue:150 minValue:40 maxValue:250 target:self action:@selector(changed:)];
    _threshold.continuous = NO;
    _thresholdValue = [NSTextField labelWithString:@""];
    _signs = [NSTextField labelWithString:@""];
    _signs.font = [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];
    for (NSSlider* s in @[ _smoothing, _threshold ])
        [s.widthAnchor constraintEqualToConstant:260].active = YES;

    NSGridView* grid = [NSGridView gridViewWithViews:@[
        @[ label(@"Clip:"), _clipType ],
        @[ [NSGridCell emptyContentView], _irLights ],
        @[ label(@"Smoothing:"), [NSStackView stackViewWithViews:@[ _smoothing, _smoothingValue ]] ],
        @[ [NSGridCell emptyContentView], note(@"Higher is steadier but adds lag. TrackIR's default is about 0.3.") ],
        @[ label(@"Marker threshold:"), [NSStackView stackViewWithViews:@[ _threshold, _thresholdValue ]] ],
        @[ [NSGridCell emptyContentView],
           note(@"Raise it if reflections show up as extra markers in the Live View; lower it if the clip "
                @"drops out at the edges.") ],
        @[ label(@"Axis corrections:"), _signs ],
        @[ [NSGridCell emptyContentView],
           note(@"Measured by “Run Axis Check in Terminal…” in the menu. Changing the clip, IR or threshold "
                @"restarts the camera for a second.") ],
    ]];
    grid.rowSpacing = 10;
    grid.columnSpacing = 10;
    return padded(grid);
}

- (NSView*)buildCurves
{
    _axisPicker = [NSSegmentedControl segmentedControlWithLabels:@[ @"Yaw", @"Pitch", @"Roll", @"X", @"Y", @"Z" ]
                                                    trackingMode:NSSegmentSwitchTrackingSelectOne
                                                          target:self
                                                          action:@selector(axisPicked:)];
    _axisPicker.selectedSegment = 0;
    _curveView = [[CurveView alloc] init];
    [_curveView.widthAnchor constraintEqualToConstant:580].active = YES;
    [_curveView.heightAnchor constraintEqualToConstant:280].active = YES;
    __weak SettingsWindowController* weakSelf = self;
    _curveView.onChange = ^(const AxisCurve& curve) {
        [weakSelf curveEdited:curve];
    };
    _axisEnabled = [NSButton checkboxWithTitle:@"Axis enabled" target:self action:@selector(axisFlags:)];
    _axisInverted = [NSButton checkboxWithTitle:@"Inverted" target:self action:@selector(axisFlags:)];
    _profileLabel = [NSTextField labelWithString:@""];
    NSButton* save = [NSButton buttonWithTitle:@"Save Profile As…" target:self action:@selector(saveProfile:)];
    NSButton* revert = [NSButton buttonWithTitle:@"Revert" target:self action:@selector(revertProfile:)];
    NSStackView* flags = [NSStackView stackViewWithViews:@[ _axisEnabled, _axisInverted ]];
    NSStackView* bottom = [NSStackView stackViewWithViews:@[ _profileLabel, revert, save ]];
    NSStackView* all = [NSStackView stackViewWithViews:@[ _axisPicker, _curveView, flags, bottom ]];
    all.orientation = NSUserInterfaceLayoutOrientationVertical;
    all.alignment = NSLayoutAttributeLeading;
    all.spacing = 10;
    return padded(all);
}

- (NSView*)buildOutput
{
    _udp = [NSButton checkboxWithTitle:@"Send opentrack “UDP over network” data" target:self
                                action:@selector(changed:)];
    _udpHost = [NSTextField textFieldWithString:@"127.0.0.1"];
    _udpPort = [NSTextField textFieldWithString:@"4242"];
    for (NSTextField* f in @[ _udpHost, _udpPort ]) {
        f.target = self;
        f.action = @selector(changed:);
    }
    [_udpHost.widthAnchor constraintEqualToConstant:160].active = YES;
    [_udpPort.widthAnchor constraintEqualToConstant:70].active = YES;
    _keyRecenter = [[NSPopUpButton alloc] init];
    _keyPause = [[NSPopUpButton alloc] init];
    for (NSPopUpButton* p in @[ _keyRecenter, _keyPause ]) {
        [p addItemsWithTitles:functionKeyNames()];
        p.target = self;
        p.action = @selector(changed:);
    }
    NSGridView* grid = [NSGridView gridViewWithViews:@[
        @[ label(@"X-Plane, Wine games:"), note(@"Always on: the X-Plane plugin and the Wine NPClient.dll read "
                                                @"/tmp/TrackIR-macOS.bridge.") ],
        @[ label(@"opentrack:"), _udp ],
        @[ label(@"Host / port:"), [NSStackView stackViewWithViews:@[ _udpHost, _udpPort ]] ],
        @[ label(@"Recenter key:"), _keyRecenter ],
        @[ label(@"Pause key:"), _keyPause ],
        @[ [NSGridCell emptyContentView],
           note(@"Global keys work in every app. On a Mac keyboard press fn with the F key unless “Use F1, F2, "
                @"etc. keys as standard function keys” is on. In X-Plane you can also bind the "
                @"trackir_macos/recenter and trackir_macos/pause commands to joystick buttons.") ],
    ]];
    grid.rowSpacing = 10;
    grid.columnSpacing = 10;
    return padded(grid);
}

- (void)reload
{
    Settings s = [_controller settings];
    [_clipType selectItemAtIndex:s.clipType == ClipType::TrackClipPro ? 1 : 0];
    _irLights.state = s.camera.irIntensity == 0 ? NSControlStateValueOff : NSControlStateValueOn;
    _smoothing.doubleValue = s.smoothing;
    _threshold.integerValue = s.camera.threshold;
    _udp.state = s.udp ? NSControlStateValueOn : NSControlStateValueOff;
    _udpHost.stringValue = ns(s.udpHost);
    _udpPort.integerValue = s.udpPort;
    [_keyRecenter selectItemWithTitle:ns(s.hotkeyRecenter)];
    if (!_keyRecenter.selectedItem)
        [_keyRecenter selectItemAtIndex:0];
    [_keyPause selectItemWithTitle:ns(s.hotkeyPause)];
    if (!_keyPause.selectedItem)
        [_keyPause selectItemAtIndex:0];
    static const char* names[6] = {"yaw", "pitch", "roll", "x", "y", "z"};
    NSMutableString* signs = [NSMutableString string];
    for (int i = 0; i < 6; i++)
        [signs appendFormat:@"%s %@  ", names[i], s.axisSign[size_t(i)] > 0 ? @"+" : @"−"];
    [signs appendFormat:@" lens ×%.3f", s.focalScale];
    _signs.stringValue = signs;
    [self updateLabels];
    [self loadAxis];
}

- (void)updateLabels
{
    _smoothingValue.stringValue = [NSString stringWithFormat:@"%.2f", _smoothing.doubleValue];
    _thresholdValue.stringValue = [NSString stringWithFormat:@"%ld", (long)_threshold.integerValue];
    NSString* path = [_controller profilePath];
    NSString* name = path ? path.lastPathComponent.stringByDeletingPathExtension : @"Linear (1:1)";
    _profileLabel.stringValue = [NSString stringWithFormat:@"Profile: %@%@", name,
                                                           [_controller profileModified] ? @" (edited, not saved)" : @""];
}

- (void)loadAxis
{
    Profile p = [_controller profile];
    const AxisCurve& c = p.axes[size_t(_axis)];
    _curveView.rotation = _axis < 3;
    _curveView.curve = c;
    _axisEnabled.state = c.enabled ? NSControlStateValueOn : NSControlStateValueOff;
    _axisInverted.state = c.inverted ? NSControlStateValueOn : NSControlStateValueOff;
}

- (void)refresh
{
    EngineStatus s = [_controller currentStatus];
    const double v[6] = {s.measured.yaw, s.measured.pitch, s.measured.roll, s.measured.x, s.measured.y, s.measured.z};
    _curveView.liveInput = v[_axis];
    _curveView.needsDisplay = YES;
}

- (void)axisPicked:(id)sender
{
    _axis = int(_axisPicker.selectedSegment);
    [self loadAxis];
}

- (void)curveEdited:(const AxisCurve&)curve
{
    Profile p = [_controller profile];
    p.axes[size_t(_axis)].points = curve.points;
    [_controller editProfile:p];
    [self updateLabels];
}

- (void)axisFlags:(id)sender
{
    Profile p = [_controller profile];
    p.axes[size_t(_axis)].enabled = _axisEnabled.state == NSControlStateValueOn;
    p.axes[size_t(_axis)].inverted = _axisInverted.state == NSControlStateValueOn;
    [_controller editProfile:p];
    [self updateLabels];
}

- (void)saveProfile:(id)sender
{
    NSSavePanel* panel = [NSSavePanel savePanel];
    panel.directoryURL = [NSURL fileURLWithPath:ns(profileDirectory())];
    panel.allowedContentTypes = @[ UTTypeXML ];
    NSString* current = [_controller profilePath];
    panel.nameFieldStringValue = current ? current.lastPathComponent : @"My Profile.xml";
    [panel beginSheetModalForWindow:self.window
                  completionHandler:^(NSModalResponse result) {
                      if (result != NSModalResponseOK)
                          return;
                      if (![self->_controller saveProfileAs:panel.URL.path]) {
                          NSAlert* a = [[NSAlert alloc] init];
                          a.messageText = @"Could not save the profile.";
                          [a runModal];
                      }
                      [self updateLabels];
                  }];
}

- (void)revertProfile:(id)sender
{
    [_controller revertProfile];
    [self loadAxis];
    [self updateLabels];
}

- (void)changed:(id)sender
{
    Settings s = [_controller settings];
    s.clipType = _clipType.indexOfSelectedItem == 1 ? ClipType::TrackClipPro : ClipType::TrackClip;
    s.camera.irIntensity = _irLights.state == NSControlStateValueOn ? -1 : 0;
    s.smoothing = std::round(_smoothing.doubleValue * 100) / 100;
    s.camera.threshold = int(_threshold.integerValue);
    s.udp = _udp.state == NSControlStateValueOn;
    s.udpHost = _udpHost.stringValue.length ? _udpHost.stringValue.UTF8String : "127.0.0.1";
    s.udpPort = _udpPort.intValue > 0 && _udpPort.intValue < 65536 ? _udpPort.intValue : 4242;
    s.hotkeyRecenter = _keyRecenter.titleOfSelectedItem.UTF8String ?: "none";
    s.hotkeyPause = _keyPause.titleOfSelectedItem.UTF8String ?: "none";
    [_controller applySettings:s];
    [self updateLabels];
}

@end
