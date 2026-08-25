// Octomancer -- a menu-bar view of what octomancerd can hear.
//
// This process holds no state worth losing and makes no decisions. It asks the
// agent for a snapshot every couple of seconds and draws it, which means the
// UI can be quit, restarted or never run at all without affecting what is
// being measured. The one thing it adds is notifications, because posting
// those requires a bundled, signed application and a login session, none of
// which a launchd agent has.
#import <AppKit/AppKit.h>
#import <UserNotifications/UserNotifications.h>

#include <set>
#include <string>

#include "client.h"
#include "registry.h"
#include "render.h"
#include "server.h"
#include "timeutil.h"

namespace {

constexpr double kRefreshSeconds = 2.0;

NSString* ns(const std::string& s) {
  return [NSString stringWithUTF8String:s.c_str()] ?: @"";
}

// Offsets run from microseconds to minutes; pick the unit that shows the
// interesting digits without inventing precision.
NSString* offset_text(double seconds) {
  const double mag = fabs(seconds);
  if (mag < 1.0) return [NSString stringWithFormat:@"%+.0f ms", seconds * 1000.0];
  if (mag < 60.0) return [NSString stringWithFormat:@"%+.3f s", seconds];
  return [NSString stringWithFormat:@"%+.1f s", seconds];
}

}  // namespace

@interface OctoController : NSObject <NSApplicationDelegate, NSMenuDelegate>
@end

@implementation OctoController {
  NSStatusItem* _statusItem;
  NSMenu* _menu;
  NSWindow* _window;
  NSTextView* _text;
  NSTimer* _timer;
  dispatch_queue_t _queue;

  octo::Snapshot _snapshot;
  std::string _socketPath;
  std::string _error;
  bool _connected;
  std::set<std::string> _alerting;
  bool _notificationsReady;
}

- (instancetype)init {
  self = [super init];
  if (self) {
    _connected = false;
    _notificationsReady = false;
    _socketPath = octo::default_socket_path();
    if (const char* override = getenv("OCTOMANCER_SOCKET")) {
      if (*override) _socketPath = override;
    }
    _queue = dispatch_queue_create("dev.dabbelt.octomancer.ui", DISPATCH_QUEUE_SERIAL);
  }
  return self;
}

- (void)applicationDidFinishLaunching:(NSNotification*)note {
  (void)note;
  _statusItem = [[NSStatusBar systemStatusBar]
      statusItemWithLength:NSVariableStatusItemLength];
  _statusItem.button.title = @"◷";
  _statusItem.button.toolTip = @"Octomancer";

  _menu = [[NSMenu alloc] init];
  _menu.delegate = self;
  _statusItem.menu = _menu;

  [self requestNotificationPermission];
  [self rebuildMenu];

  _timer = [NSTimer scheduledTimerWithTimeInterval:kRefreshSeconds
                                           repeats:YES
                                             block:^(NSTimer* t) {
                                               (void)t;
                                               [self refresh];
                                             }];
  [self refresh];
}

- (void)requestNotificationPermission {
  UNUserNotificationCenter* center = [UNUserNotificationCenter currentNotificationCenter];
  [center requestAuthorizationWithOptions:(UNAuthorizationOptionAlert |
                                           UNAuthorizationOptionSound)
                        completionHandler:^(BOOL granted, NSError* error) {
    if (error != nil) {
      NSLog(@"octomancer: notification authorization failed: %@", error);
    }
    // Not fatal. Without permission the menu-bar item still turns into a
    // warning, so the information is never actually lost -- it just stops
    // arriving unprompted.
    dispatch_async(dispatch_get_main_queue(), ^{
      self->_notificationsReady = granted ? true : false;
    });
  }];
}

// Fetching talks to a socket, so it happens off the main thread; a wedged
// agent must not freeze the menu bar.
- (void)refresh {
  const std::string path = _socketPath;
  dispatch_async(_queue, ^{
    octo::Snapshot snap;
    std::string err;
    const bool ok = octo::fetch(path, &snap, &err);
    dispatch_async(dispatch_get_main_queue(), ^{
      self->_connected = ok;
      if (ok) {
        self->_snapshot = snap;
        self->_error.clear();
      } else {
        self->_error = err;
      }
      [self applySnapshot];
    });
  });
}

- (void)applySnapshot {
  [self updateStatusItem];
  [self rebuildMenu];
  [self updateWindow];
  if (_connected) [self notifyForNewAlerts];
}

- (void)updateStatusItem {
  if (!_connected) {
    _statusItem.button.title = @"◷ ?";
    _statusItem.button.toolTip = ns("Octomancer: " + _error);
    return;
  }
  if (_snapshot.alerting > 0) {
    _statusItem.button.title =
        [NSString stringWithFormat:@"⚠ %d", _snapshot.alerting];
    _statusItem.button.toolTip =
        [NSString stringWithFormat:@"%d box%s out of sync", _snapshot.alerting,
                                   _snapshot.alerting == 1 ? "" : "es"];
    return;
  }
  _statusItem.button.title = [NSString stringWithFormat:@"◷ %d", _snapshot.live];
  _statusItem.button.toolTip =
      _snapshot.has_bench
          ? [NSString stringWithFormat:@"bench %@ vs this Mac",
                                       offset_text(_snapshot.bench_offset)]
          : @"Octomancer";
}

- (void)notifyForNewAlerts {
  std::set<std::string> current;
  for (const octo::DeviceSnapshot& d : _snapshot.device) {
    if (d.alerting) current.insert(d.id);
  }

  for (const octo::DeviceSnapshot& d : _snapshot.device) {
    if (!d.alerting) continue;
    if (_alerting.count(d.id)) continue;  // already told them about this one
    [self postNotificationForBox:d];
  }
  _alerting = current;
}

- (void)postNotificationForBox:(const octo::DeviceSnapshot&)box {
  if (!_notificationsReady) return;
  UNMutableNotificationContent* content = [[UNMutableNotificationContent alloc] init];
  content.title = [NSString stringWithFormat:@"%@ has drifted", ns(box.name)];
  content.body =
      [NSString stringWithFormat:@"%@ is %@ from this Mac. Re-jam it in the "
                                 @"Tentacle app.",
                                 ns(box.name), offset_text(box.median_offset)];
  content.sound = [UNNotificationSound defaultSound];

  // A stable identifier per box, so a box that keeps drifting replaces its own
  // notification rather than stacking up a column of them.
  UNNotificationRequest* request =
      [UNNotificationRequest requestWithIdentifier:ns("drift-" + box.id)
                                           content:content
                                           trigger:nil];
  [[UNUserNotificationCenter currentNotificationCenter]
      addNotificationRequest:request
       withCompletionHandler:^(NSError* error) {
         if (error != nil) NSLog(@"octomancer: notification failed: %@", error);
       }];
}

- (NSMenuItem*)disabledItem:(NSString*)title {
  NSMenuItem* item = [[NSMenuItem alloc] initWithTitle:title action:nil keyEquivalent:@""];
  item.enabled = NO;
  return item;
}

- (void)rebuildMenu {
  [_menu removeAllItems];

  if (!_connected) {
    [_menu addItem:[self disabledItem:@"Agent not answering"]];
    [_menu addItem:[self disabledItem:ns(_error)]];
    [_menu addItem:[NSMenuItem separatorItem]];
    [_menu addItemWithTitle:@"Quit Octomancer"
                     action:@selector(quit:)
              keyEquivalent:@"q"].target = self;
    return;
  }

  NSString* header =
      _snapshot.has_bench
          ? [NSString stringWithFormat:@"%d live · bench %@ · spread %@",
                                       _snapshot.live,
                                       offset_text(_snapshot.bench_offset),
                                       offset_text(_snapshot.bench_spread)]
          : [NSString stringWithFormat:@"%d box(es), none heard recently",
                                       _snapshot.devices];
  [_menu addItem:[self disabledItem:header]];

  if (_snapshot.radio != "poweredOn") {
    [_menu addItem:[self disabledItem:ns("Bluetooth: " + _snapshot.radio)]];
  }
  [_menu addItem:[NSMenuItem separatorItem]];

  if (_snapshot.device.empty()) {
    [_menu addItem:[self disabledItem:@"No Tentacle boxes seen yet"]];
  }

  for (const octo::DeviceSnapshot& d : _snapshot.device) {
    NSString* detail;
    if (!d.has_time) {
      detail = @"no timecode";
    } else if (d.has_drift) {
      detail = [NSString stringWithFormat:@"%@   %+.1f ppm",
                                          offset_text(d.median_offset), d.drift_ppm];
    } else {
      detail = offset_text(d.median_offset);
    }

    NSString* mark = d.alerting ? @"⚠︎ " : (d.live ? @"" : @"· ");
    NSMenuItem* item = [[NSMenuItem alloc]
        initWithTitle:[NSString stringWithFormat:@"%@%-10s  %@", mark,
                                                 d.name.c_str(), detail]
               action:nil
        keyEquivalent:@""];
    // Stale boxes are shown but greyed: a remembered offset is not evidence
    // about the present, and the menu should not imply that it is.
    item.enabled = NO;
    if (!d.live) {
      item.attributedTitle = [[NSAttributedString alloc]
          initWithString:item.title
              attributes:@{NSForegroundColorAttributeName : NSColor.tertiaryLabelColor}];
    }
    item.toolTip = [NSString stringWithFormat:@"%s · %s · rssi %d · last seen %s ago",
                                              d.display.c_str(),
                                              d.resolution.c_str(), d.rssi,
                                              octo::format_age(d.age).c_str()];
    [_menu addItem:item];
  }

  [_menu addItem:[NSMenuItem separatorItem]];
  [_menu addItemWithTitle:@"Show Details…"
                   action:@selector(showWindow:)
            keyEquivalent:@""].target = self;
  [_menu addItemWithTitle:@"Refresh Now"
                   action:@selector(refreshNow:)
            keyEquivalent:@"r"].target = self;
  [_menu addItem:[NSMenuItem separatorItem]];
  [_menu addItemWithTitle:@"Quit Octomancer"
                   action:@selector(quit:)
            keyEquivalent:@"q"].target = self;
}

- (void)ensureWindow {
  if (_window != nil) return;
  const NSRect frame = NSMakeRect(0, 0, 760, 320);
  _window = [[NSWindow alloc]
      initWithContentRect:frame
                styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                           NSWindowStyleMaskResizable)
                  backing:NSBackingStoreBuffered
                    defer:NO];
  _window.title = @"Octomancer";
  _window.releasedWhenClosed = NO;
  [_window center];

  NSScrollView* scroll = [[NSScrollView alloc] initWithFrame:frame];
  scroll.hasVerticalScroller = YES;
  scroll.hasHorizontalScroller = YES;
  scroll.autohidesScrollers = YES;
  scroll.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;

  _text = [[NSTextView alloc] initWithFrame:frame];
  _text.editable = NO;
  _text.richText = NO;
  _text.font = [NSFont monospacedSystemFontOfSize:12 weight:NSFontWeightRegular];
  _text.textContainer.widthTracksTextView = NO;
  _text.textContainer.containerSize = NSMakeSize(FLT_MAX, FLT_MAX);
  _text.horizontallyResizable = YES;
  scroll.documentView = _text;

  _window.contentView = scroll;
}

- (void)updateWindow {
  if (_window == nil || !_window.isVisible) return;
  const std::string body =
      _connected ? octo::render_human(_snapshot, false)
                 : ("octomancer: " + _error + "\n");
  _text.string = ns(body);
}

- (void)showWindow:(id)sender {
  (void)sender;
  [self ensureWindow];
  [self updateWindow];
  [NSApp activateIgnoringOtherApps:YES];
  [_window makeKeyAndOrderFront:nil];
  [self updateWindow];
}

- (void)refreshNow:(id)sender {
  (void)sender;
  [self refresh];
}

- (void)quit:(id)sender {
  (void)sender;
  [NSApp terminate:nil];
}

@end

int main(int argc, const char* argv[]) {
  (void)argc;
  (void)argv;
  @autoreleasepool {
    NSApplication* app = [NSApplication sharedApplication];
    OctoController* controller = [[OctoController alloc] init];
    app.delegate = controller;
    // Accessory, not regular: no Dock tile, and opening the details window
    // does not permanently promote it into one.
    [app setActivationPolicy:NSApplicationActivationPolicyAccessory];
    [app run];
  }
  return 0;
}
