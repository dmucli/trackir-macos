// Live View: what the camera sees (marker blobs) and the six output axes.
#import "MacApp.h"

#include <cmath>

using namespace tir;

@interface CameraFieldView : NSView
@property(nonatomic) EngineStatus status;
@end

@implementation CameraFieldView

- (BOOL)isFlipped
{
    return YES;  // sensor rows grow downwards
}

- (void)drawRect:(NSRect)dirty
{
    const EngineStatus& s = _status;
    [[NSColor colorWithWhite:0.08 alpha:1] setFill];
    NSRectFill(self.bounds);

    double sw = s.sensorWidth > 0 ? s.sensorWidth : 640, sh = s.sensorHeight > 0 ? s.sensorHeight : 480;
    double scale = std::min(self.bounds.size.width / sw, self.bounds.size.height / sh);
    NSRect field = NSMakeRect((self.bounds.size.width - sw * scale) / 2, (self.bounds.size.height - sh * scale) / 2,
                              sw * scale, sh * scale);
    [[NSColor colorWithWhite:0.14 alpha:1] setFill];
    NSRectFill(field);
    NSBezierPath* grid = [NSBezierPath bezierPath];
    for (int i = 1; i < 4; i++) {
        CGFloat x = field.origin.x + field.size.width * i / 4, y = field.origin.y + field.size.height * i / 4;
        [grid moveToPoint:NSMakePoint(x, field.origin.y)];
        [grid lineToPoint:NSMakePoint(x, NSMaxY(field))];
        [grid moveToPoint:NSMakePoint(field.origin.x, y)];
        [grid lineToPoint:NSMakePoint(NSMaxX(field), y)];
    }
    [[NSColor colorWithWhite:0.25 alpha:1] setStroke];
    grid.lineWidth = 0.5;
    [grid stroke];

    // The camera faces the user, so mirror x: moving right moves the dots right, like a mirror.
    for (size_t i = 0; i < s.blobs.size(); i++) {
        const Blob& b = s.blobs[i];
        double r = std::max(3.0, std::sqrt(b.area / M_PI) * scale * 1.5);
        NSPoint c = NSMakePoint(NSMaxX(field) - b.x * scale, field.origin.y + b.y * scale);
        NSColor* color = i < 3 ? (s.clipVisible ? NSColor.systemGreenColor : NSColor.systemOrangeColor)
                               : NSColor.systemGrayColor;
        [color setFill];
        [[NSBezierPath bezierPathWithOvalInRect:NSMakeRect(c.x - r, c.y - r, 2 * r, 2 * r)] fill];
    }

    NSString* text;
    if (s.state != EngineStatus::State::Streaming)
        text = [NSString stringWithUTF8String:s.message.c_str()] ?: @"";
    else
        text = [NSString stringWithFormat:@"%u fps   %zu marker%@   %@", s.framesPerSecond, s.blobs.size(),
                                          s.blobs.size() == 1 ? @"" : @"s",
                                          s.clipVisible ? [NSString stringWithFormat:@"clip %.0f cm away", s.distanceCm]
                                                        : @"clip not found (need exactly 3 markers)"];
    if (s.paused)
        text = [text stringByAppendingString:@"   PAUSED"];
    NSDictionary* attrs = @{
        NSFontAttributeName : [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular],
        NSForegroundColorAttributeName : [NSColor colorWithWhite:0.85 alpha:1]
    };
    [text drawAtPoint:NSMakePoint(field.origin.x + 8, field.origin.y + 6) withAttributes:attrs];
}

@end

@interface PoseBarsView : NSView
@property(nonatomic) EngineStatus status;
@end

@implementation PoseBarsView

- (void)drawRect:(NSRect)dirty
{
    static const char* names[6] = {"Yaw", "Pitch", "Roll", "X", "Y", "Z"};
    static const double range[6] = {180, 90, 90, 30, 30, 30};
    NSString* const units[6] = {@"°", @"°", @"°", @" cm", @" cm", @" cm"};
    const HeadPose& o = _status.output;
    const HeadPose& m = _status.measured;
    const double out[6] = {o.yaw, o.pitch, o.roll, o.x, o.y, o.z};
    const double raw[6] = {m.yaw, m.pitch, m.roll, m.x, m.y, m.z};

    NSDictionary* label = @{
        NSFontAttributeName : [NSFont systemFontOfSize:12 weight:NSFontWeightMedium],
        NSForegroundColorAttributeName : NSColor.labelColor
    };
    NSDictionary* value = @{
        NSFontAttributeName : [NSFont monospacedDigitSystemFontOfSize:11 weight:NSFontWeightRegular],
        NSForegroundColorAttributeName : _status.clipVisible ? NSColor.secondaryLabelColor : NSColor.tertiaryLabelColor
    };
    CGFloat rowH = self.bounds.size.height / 6, labelW = 46, valueW = 150;
    CGFloat barW = self.bounds.size.width - labelW - valueW - 16;
    for (int i = 0; i < 6; i++) {
        CGFloat y = self.bounds.size.height - (i + 1) * rowH;
        [[NSString stringWithUTF8String:names[i]] drawAtPoint:NSMakePoint(4, y + rowH / 2 - 8) withAttributes:label];
        NSRect track = NSMakeRect(labelW, y + rowH / 2 - 5, barW, 10);
        [[NSColor quaternaryLabelColor] setFill];
        [[NSBezierPath bezierPathWithRoundedRect:track xRadius:5 yRadius:5] fill];
        CGFloat mid = NSMidX(track);
        double f = std::clamp(out[i] / range[i], -1.0, 1.0);
        NSRect fill = f >= 0 ? NSMakeRect(mid, track.origin.y, f * barW / 2, 10)
                             : NSMakeRect(mid + f * barW / 2, track.origin.y, -f * barW / 2, 10);
        // Greyed while the clip is out of view: the outputs hold the last pose.
        [(_status.clipVisible || _status.paused ? NSColor.controlAccentColor : NSColor.tertiaryLabelColor) setFill];
        NSRectFill(fill);
        [[NSColor tertiaryLabelColor] setFill];
        NSRectFill(NSMakeRect(mid - 0.5, track.origin.y - 3, 1, 16));
        NSString* text = [NSString stringWithFormat:@"%+7.1f%@  (raw %+.1f)", out[i], units[i], raw[i]];
        [text drawAtPoint:NSMakePoint(labelW + barW + 12, y + rowH / 2 - 8) withAttributes:value];
    }
}

@end

@implementation LiveWindowController {
    __weak id<TrackerController> _controller;
    CameraFieldView* _field;
    PoseBarsView* _bars;
    NSTextField* _log;
    NSButton* _pause;
}

- (instancetype)initWithController:(id<TrackerController>)controller
{
    NSWindow* w = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 900, 470)
                                              styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                                        NSWindowStyleMaskResizable | NSWindowStyleMaskMiniaturizable
                                                backing:NSBackingStoreBuffered
                                                  defer:NO];
    w.title = @"TrackIR Live View";
    w.releasedWhenClosed = NO;
    w.contentMinSize = NSMakeSize(700, 400);
    if (!(self = [super initWithWindow:w]))
        return nil;
    _controller = controller;

    _field = [[CameraFieldView alloc] init];
    _bars = [[PoseBarsView alloc] init];
    _log = [NSTextField wrappingLabelWithString:@""];
    _log.font = [NSFont monospacedSystemFontOfSize:10 weight:NSFontWeightRegular];
    _log.textColor = NSColor.secondaryLabelColor;
    _log.maximumNumberOfLines = 4;
    _log.lineBreakMode = NSLineBreakByTruncatingTail;

    NSButton* recenter = [NSButton buttonWithTitle:@"Recenter" target:self action:@selector(recenter:)];
    _pause = [NSButton buttonWithTitle:@"Pause" target:self action:@selector(pause:)];
    NSStackView* buttons = [NSStackView stackViewWithViews:@[ recenter, _pause ]];

    NSStackView* right = [NSStackView stackViewWithViews:@[ _bars, buttons ]];
    right.orientation = NSUserInterfaceLayoutOrientationVertical;
    right.alignment = NSLayoutAttributeLeading;

    NSStackView* top = [NSStackView stackViewWithViews:@[ _field, right ]];
    top.distribution = NSStackViewDistributionFill;
    top.spacing = 16;

    NSStackView* all = [NSStackView stackViewWithViews:@[ top, _log ]];
    all.orientation = NSUserInterfaceLayoutOrientationVertical;
    all.edgeInsets = NSEdgeInsetsMake(16, 16, 16, 16);
    all.alignment = NSLayoutAttributeLeading;
    all.translatesAutoresizingMaskIntoConstraints = NO;
    w.contentView = [[NSView alloc] init];
    [w.contentView addSubview:all];
    [NSLayoutConstraint activateConstraints:@[
        [all.leadingAnchor constraintEqualToAnchor:w.contentView.leadingAnchor],
        [all.trailingAnchor constraintEqualToAnchor:w.contentView.trailingAnchor],
        [all.topAnchor constraintEqualToAnchor:w.contentView.topAnchor],
        [all.bottomAnchor constraintEqualToAnchor:w.contentView.bottomAnchor],
        [top.widthAnchor constraintEqualToAnchor:all.widthAnchor constant:-32],
        [_log.widthAnchor constraintEqualToAnchor:top.widthAnchor],
        [_field.widthAnchor constraintEqualToAnchor:_field.heightAnchor multiplier:4.0 / 3.0],
        [_field.heightAnchor constraintGreaterThanOrEqualToConstant:300],
        [_bars.widthAnchor constraintGreaterThanOrEqualToConstant:360],
        [_bars.heightAnchor constraintEqualToAnchor:_field.heightAnchor constant:-40],
    ]];
    [w center];
    return self;
}

- (void)recenter:(id)sender
{
    [_controller recenter];
}

- (void)pause:(id)sender
{
    [_controller togglePause];
    [self refresh];
}

- (void)refresh
{
    EngineStatus s = [_controller currentStatus];
    _field.status = s;
    _bars.status = s;
    _field.needsDisplay = YES;
    _bars.needsDisplay = YES;
    _pause.title = s.paused ? @"Resume" : @"Pause";
    NSArray* log = [_controller recentLog];
    NSRange tail = NSMakeRange(log.count > 4 ? log.count - 4 : 0, MIN(log.count, (NSUInteger)4));
    _log.stringValue = [[log subarrayWithRange:tail] componentsJoinedByString:@"\n"];
}

@end
