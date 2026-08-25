// Octomancer -- the window and the menu-bar item.
//
// This process holds no state worth losing and decides nothing about clocks.
// It asks the two daemons what they can see, draws it, and hands back what a
// person clicks. It can be quit, restarted or never run at all without
// affecting what is being measured.
//
// It talks to both sockets, because there are two daemons and they know
// different things. octomancerd knows the Tentacle bench and which boxes have
// drifted; octomancer-sync knows the cameras, and is the only one that can be
// told to do anything. Losing either is drawn as a missing panel rather than
// as an error, because one daemon being down is not a reason to stop showing
// what the other one says.
//
// The one thing this process adds is notifications, which need a bundled,
// signed application and a login session -- none of which a launchd agent has.
#import <AppKit/AppKit.h>
#import <UserNotifications/UserNotifications.h>

#include <set>
#include <string>
#include <vector>

#include "bmd.h"
#include "client.h"
#include "control.h"
#include "proto.h"
#include "registry.h"
#include "render.h"
#include "server.h"
#include "timeutil.h"

namespace {

constexpr double kRefreshSeconds = 2.0;

// The three things a person can ask to be told about, and where the answer is
// remembered. Defaults on: someone who installs a clock-sync tool wants to
// know when the clock did not get synced.
NSString* const kPrefSyncFailed = @"notify.sync-failed";
NSString* const kPrefFirstSync = @"notify.first-sync";
NSString* const kPrefCameraLost = @"notify.camera-lost";

NSString* const kAgentLabels[] = {
    @"com.dabbelt.octomancerd",
    @"com.dabbelt.octomancer-sync",
};

NSString* ns(const std::string& s) {
  return [NSString stringWithUTF8String:s.c_str()] ?: @"";
}

std::string cpp(NSString* s) {
  return s == nil ? std::string() : std::string(s.UTF8String ?: "");
}

// Offsets run from microseconds to minutes; pick the unit that shows the
// interesting digits without inventing precision.
NSString* offset_text(double seconds) {
  const double mag = fabs(seconds);
  if (mag < 1.0) return [NSString stringWithFormat:@"%+.0f ms", seconds * 1000.0];
  if (mag < 60.0) return [NSString stringWithFormat:@"%+.3f s", seconds];
  return [NSString stringWithFormat:@"%+.1f s", seconds];
}

NSString* ago_text(double wall, double now) {
  if (wall <= 0.0) return @"never";
  const double d = now - wall;
  if (d < 90.0) return [NSString stringWithFormat:@"%.0fs ago", d];
  if (d < 5400.0) return [NSString stringWithFormat:@"%.0fm ago", d / 60.0];
  return [NSString stringWithFormat:@"%.1fh ago", d / 3600.0];
}

// Whether this process has an identity macOS will accept.
//
// UNUserNotificationCenter does not fail politely without one: asking for the
// current notification centre from a bare executable raises
// NSInternalInconsistencyException and takes the process with it, so the check
// has to happen before the first call rather than around its result.
bool have_bundle_identity() {
  NSBundle* main = [NSBundle mainBundle];
  if (main == nil) return false;
  if (main.bundleIdentifier == nil) return false;
  return [[main.bundleURL pathExtension] isEqualToString:@"app"];
}

NSTextField* label(NSString* text) {
  NSTextField* f = [NSTextField labelWithString:text];
  f.lineBreakMode = NSLineBreakByTruncatingTail;
  return f;
}

NSTextField* dim_label(NSString* text) {
  NSTextField* f = label(text);
  f.textColor = [NSColor secondaryLabelColor];
  return f;
}

NSTextField* mono_label(NSString* text) {
  NSTextField* f = label(text);
  f.font = [NSFont monospacedDigitSystemFontOfSize:NSFont.systemFontSize
                                            weight:NSFontWeightRegular];
  return f;
}

}  // namespace

@interface OctoController : NSObject <NSApplicationDelegate, NSMenuDelegate>
@end

@implementation OctoController {
  NSStatusItem* _statusItem;
  NSMenu* _menu;
  NSWindow* _window;
  dispatch_queue_t _queue;

  // --- the bench, from octomancerd -----------------------------------
  octo::Snapshot _snapshot;
  std::string _benchSocket;
  std::string _benchError;
  bool _benchUp;
  std::set<std::string> _alerting;

  // --- the cameras, from octomancer-sync -----------------------------
  octo::Status _status;
  std::string _controlSocket;
  std::string _controlError;
  bool _controlUp;
  int64_t _eventCursor;
  bool _eventCursorPrimed;

  bool _notificationsReady;
  bool _busy;

  // --- the window's controls -----------------------------------------
  NSTextField* _daemonLine;
  NSTextField* _benchLine;
  NSPopUpButton* _cameraPicker;
  NSGridView* _detail;
  NSTextField* _tcValue;
  NSTextField* _errorValue;
  NSTextField* _rateValue;
  NSPopUpButton* _sourcePicker;
  NSTextField* _cycleValue;
  NSTextField* _writeValue;
  NSTextField* _leadValue;
  NSButton* _syncButton;
  NSTextField* _activity;
  NSButton* _notifyFailed;
  NSButton* _notifyFirst;
  NSButton* _notifyLost;
  NSButton* _startAtBoot;
  NSTextField* _bootNote;
}

- (instancetype)init {
  self = [super init];
  if (self) {
    _benchUp = false;
    _controlUp = false;
    _notificationsReady = false;
    _busy = false;
    _eventCursor = 0;
    _eventCursorPrimed = false;
    _benchSocket = octo::default_socket_path();
    _controlSocket = octo::default_control_socket_path();
    if (const char* override = getenv("OCTOMANCER_SOCKET")) {
      if (*override) _benchSocket = override;
    }
    if (const char* override = getenv("OCTOMANCER_CONTROL_SOCKET")) {
      if (*override) _controlSocket = override;
    }
    _queue = dispatch_queue_create("dev.dabbelt.octomancer.ui",
                                   DISPATCH_QUEUE_SERIAL);

    [[NSUserDefaults standardUserDefaults] registerDefaults:@{
      kPrefSyncFailed : @YES,
      kPrefFirstSync : @YES,
      kPrefCameraLost : @YES,
    }];
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

  [NSTimer scheduledTimerWithTimeInterval:kRefreshSeconds
                                  repeats:YES
                                    block:^(NSTimer* t) {
                                      (void)t;
                                      [self refresh];
                                    }];
  [self refresh];
}

- (void)requestNotificationPermission {
  if (!have_bundle_identity()) {
    fprintf(stderr,
            "octomancer-ui: running as a bare executable, so macOS will not\n"
            "  deliver notifications -- they need a bundle identity. The\n"
            "  window and the menu bar item work either way.\n"
            "  For notifications: open ./Octomancer.app  (or make install-app)\n");
    return;
  }
  UNUserNotificationCenter* center =
      [UNUserNotificationCenter currentNotificationCenter];
  [center requestAuthorizationWithOptions:(UNAuthorizationOptionAlert |
                                           UNAuthorizationOptionSound)
                        completionHandler:^(BOOL granted, NSError* error) {
    if (error != nil) {
      NSLog(@"octomancer: notification authorization failed: %@", error);
    }
    dispatch_async(dispatch_get_main_queue(), ^{
      self->_notificationsReady = granted ? true : false;
    });
  }];
}

// ------------------------------------------------------------------ polling

// Both sockets are read off the main thread; a wedged daemon must not freeze
// the menu bar or the window.
- (void)refresh {
  const std::string bench_path = _benchSocket;
  const std::string control_path = _controlSocket;
  const int64_t since = _eventCursor;

  dispatch_async(_queue, ^{
    octo::Snapshot snap;
    std::string bench_err;
    const bool bench_ok = octo::fetch(bench_path, &snap, &bench_err);

    octo::Status status;
    std::string control_err;
    std::string reply;
    bool control_ok = octo::query(control_path, "status", &reply, &control_err, 3.0);
    if (control_ok) {
      control_ok = octo::parse_status(reply, &status, &control_err);
    }

    std::vector<octo::Event> events;
    int64_t next = since;
    if (control_ok) {
      std::string ereply, eerr;
      const std::string ask = "events since=" + std::to_string(since);
      if (octo::query(control_path, ask, &ereply, &eerr, 3.0)) {
        octo::parse_events(ereply, &events, &next, &eerr);
      }
    }

    dispatch_async(dispatch_get_main_queue(), ^{
      self->_benchUp = bench_ok;
      if (bench_ok) {
        self->_snapshot = snap;
        self->_benchError.clear();
      } else {
        self->_benchError = bench_err;
      }

      self->_controlUp = control_ok;
      if (control_ok) {
        self->_status = status;
        self->_controlError.clear();
        [self consumeEvents:events upTo:next];
      } else {
        self->_controlError = control_err;
      }

      [self updateStatusItem];
      [self rebuildMenu];
      [self updateWindow];
      if (bench_ok) [self notifyForNewAlerts];
    });
  });
}

// Events are delivered once and then the cursor moves, which is what stops the
// same failure being announced every two seconds.
- (void)consumeEvents:(const std::vector<octo::Event>&)events
                 upTo:(int64_t)next {
  const bool primed = _eventCursorPrimed;
  _eventCursor = next;
  _eventCursorPrimed = true;

  // The first fetch adopts the cursor without announcing anything. A daemon
  // that has been up all night has a backlog, and opening the app is not a
  // reason to be told about every camera that came and went since breakfast.
  if (!primed) return;

  NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
  for (const octo::Event& e : events) {
    NSString* pref = nil;
    NSString* title = nil;
    switch (e.kind) {
      case octo::EventKind::kSyncFailed:
        pref = kPrefSyncFailed;
        title = @"Camera not synced";
        break;
      case octo::EventKind::kFirstSync:
        pref = kPrefFirstSync;
        title = @"Camera synced";
        break;
      case octo::EventKind::kCameraLost:
        pref = kPrefCameraLost;
        title = @"Camera off the air";
        break;
    }
    if (pref == nil || ![defaults boolForKey:pref]) continue;

    NSString* who = e.camera_name.empty() ? @"The camera" : ns(e.camera_name);
    NSString* body = e.message.empty()
                         ? who
                         : [NSString stringWithFormat:@"%@ -- %@", who,
                                                      ns(e.message)];
    // One identifier per camera per kind, so a camera that keeps failing
    // replaces its own notification rather than stacking a column of them.
    [self postNotification:title
                      body:body
                identifier:ns(std::string(octo::event_kind_name(e.kind)) + "-" +
                              e.camera_id)];
  }
}

- (void)postNotification:(NSString*)title
                    body:(NSString*)body
              identifier:(NSString*)identifier {
  if (!_notificationsReady) return;
  UNMutableNotificationContent* content =
      [[UNMutableNotificationContent alloc] init];
  content.title = title;
  content.body = body;
  content.sound = [UNNotificationSound defaultSound];
  UNNotificationRequest* request =
      [UNNotificationRequest requestWithIdentifier:identifier
                                           content:content
                                           trigger:nil];
  [[UNUserNotificationCenter currentNotificationCenter]
      addNotificationRequest:request
       withCompletionHandler:^(NSError* error) {
         if (error != nil) NSLog(@"octomancer: notification failed: %@", error);
       }];
}

- (void)notifyForNewAlerts {
  std::set<std::string> current;
  for (const octo::DeviceSnapshot& d : _snapshot.device) {
    if (d.alerting) current.insert(d.id);
  }
  for (const octo::DeviceSnapshot& d : _snapshot.device) {
    if (!d.alerting) continue;
    if (_alerting.count(d.id)) continue;
    [self postNotification:[NSString stringWithFormat:@"%@ has drifted",
                                                      ns(d.name)]
                      body:[NSString stringWithFormat:
                                         @"%@ is %@ from this Mac. Re-jam it "
                                         @"in the Tentacle app.",
                                         ns(d.name),
                                         offset_text(d.median_offset)]
                identifier:ns("drift-" + d.id)];
  }
  _alerting = current;
}

// ------------------------------------------------------------------ menu bar

- (void)updateStatusItem {
  if (!_benchUp && !_controlUp) {
    _statusItem.button.title = @"◷ ?";
    _statusItem.button.toolTip = @"Octomancer: no daemon answering";
    return;
  }
  if (_benchUp && _snapshot.alerting > 0) {
    _statusItem.button.title =
        [NSString stringWithFormat:@"⚠ %d", _snapshot.alerting];
    _statusItem.button.toolTip =
        [NSString stringWithFormat:@"%d box%s out of sync", _snapshot.alerting,
                                   _snapshot.alerting == 1 ? "" : "es"];
    return;
  }
  _statusItem.button.title =
      [NSString stringWithFormat:@"◷ %d", _benchUp ? _snapshot.live : 0];
  _statusItem.button.toolTip =
      _benchUp && _snapshot.has_bench
          ? [NSString stringWithFormat:@"bench %@ vs this Mac",
                                       offset_text(_snapshot.bench_offset)]
          : @"Octomancer";
}

- (NSMenuItem*)disabledItem:(NSString*)title {
  NSMenuItem* item = [[NSMenuItem alloc] initWithTitle:title
                                                action:nil
                                         keyEquivalent:@""];
  item.enabled = NO;
  return item;
}

- (void)rebuildMenu {
  [_menu removeAllItems];

  if (_controlUp && !_status.cameras.empty()) {
    for (const octo::CameraStatus& c : _status.cameras) {
      NSString* line =
          c.has_error
              ? [NSString stringWithFormat:@"%@  %@", ns(c.name), offset_text(c.error_s)]
              : ns(c.name);
      [_menu addItem:[self disabledItem:line]];
    }
    [_menu addItem:[NSMenuItem separatorItem]];
  } else if (!_controlUp) {
    [_menu addItem:[self disabledItem:@"Sync daemon not answering"]];
    [_menu addItem:[NSMenuItem separatorItem]];
  }

  if (_benchUp) {
    for (const octo::DeviceSnapshot& d : _snapshot.device) {
      NSString* line = [NSString stringWithFormat:@"%@%@  %@",
                                                  d.alerting ? @"⚠ " : @"",
                                                  ns(d.name),
                                                  offset_text(d.median_offset)];
      [_menu addItem:[self disabledItem:line]];
    }
  } else {
    [_menu addItem:[self disabledItem:@"Bench daemon not answering"]];
  }

  [_menu addItem:[NSMenuItem separatorItem]];
  [_menu addItemWithTitle:@"Open Octomancer…"
                   action:@selector(showWindow:)
            keyEquivalent:@""].target = self;
  [_menu addItemWithTitle:@"Sync Camera Now"
                   action:@selector(syncNow:)
            keyEquivalent:@""].target = self;
  [_menu addItem:[NSMenuItem separatorItem]];
  [_menu addItemWithTitle:@"Quit Octomancer"
                   action:@selector(quit:)
            keyEquivalent:@"q"].target = self;
}

// ------------------------------------------------------------------- window

- (NSView*)boxTitled:(NSString*)title content:(NSView*)content {
  NSBox* box = [[NSBox alloc] init];
  box.title = title;
  box.titlePosition = NSAtTop;
  box.contentView = content;
  box.translatesAutoresizingMaskIntoConstraints = NO;
  return box;
}

- (void)ensureWindow {
  if (_window != nil) return;

  _daemonLine = label(@"…");
  _benchLine = dim_label(@"…");

  _cameraPicker = [[NSPopUpButton alloc] init];
  _cameraPicker.target = self;
  _cameraPicker.action = @selector(cameraPicked:);
  [_cameraPicker addItemWithTitle:@"All cameras"];

  _tcValue = mono_label(@"--");
  _errorValue = mono_label(@"--");
  _rateValue = mono_label(@"--");
  _cycleValue = label(@"--");
  _writeValue = label(@"--");
  _leadValue = mono_label(@"--");

  _sourcePicker = [[NSPopUpButton alloc] init];
  [_sourcePicker addItemWithTitle:@"Time of day"];
  [_sourcePicker addItemWithTitle:@"Clip"];
  _sourcePicker.target = self;
  _sourcePicker.action = @selector(sourcePicked:);

  _detail = [NSGridView gridViewWithViews:@[
    @[ dim_label(@"Timecode"), _tcValue ],
    @[ dim_label(@"Off by"), _errorValue ],
    @[ dim_label(@"Frame rate"), _rateValue ],
    @[ dim_label(@"Timecode source"), _sourcePicker ],
    @[ dim_label(@"Last cycle"), _cycleValue ],
    @[ dim_label(@"Last write"), _writeValue ],
    @[ dim_label(@"Send lead"), _leadValue ],
  ]];
  _detail.columnSpacing = 12;
  _detail.rowSpacing = 6;
  [_detail columnAtIndex:0].xPlacement = NSGridCellPlacementTrailing;

  _syncButton = [NSButton buttonWithTitle:@"Sync Now"
                                   target:self
                                   action:@selector(syncNow:)];
  _syncButton.keyEquivalent = @"\r";
  _activity = dim_label(@"");

  NSStackView* actions = [NSStackView stackViewWithViews:@[ _syncButton, _activity ]];
  actions.orientation = NSUserInterfaceLayoutOrientationHorizontal;
  actions.spacing = 12;
  actions.alignment = NSLayoutAttributeCenterY;

  NSStackView* cameraStack = [NSStackView stackViewWithViews:@[
    _cameraPicker, _detail, actions,
  ]];
  cameraStack.orientation = NSUserInterfaceLayoutOrientationVertical;
  cameraStack.alignment = NSLayoutAttributeLeading;
  cameraStack.spacing = 10;
  cameraStack.edgeInsets = NSEdgeInsetsMake(8, 8, 8, 8);

  _notifyFailed = [NSButton checkboxWithTitle:@"a sync fails"
                                       target:self
                                       action:@selector(notifyToggled:)];
  _notifyFirst = [NSButton checkboxWithTitle:@"a camera syncs for the first time"
                                      target:self
                                      action:@selector(notifyToggled:)];
  _notifyLost = [NSButton checkboxWithTitle:@"a camera drops off the air"
                                     target:self
                                     action:@selector(notifyToggled:)];

  NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
  _notifyFailed.state = [defaults boolForKey:kPrefSyncFailed] ? NSControlStateValueOn
                                                             : NSControlStateValueOff;
  _notifyFirst.state = [defaults boolForKey:kPrefFirstSync] ? NSControlStateValueOn
                                                            : NSControlStateValueOff;
  _notifyLost.state = [defaults boolForKey:kPrefCameraLost] ? NSControlStateValueOn
                                                            : NSControlStateValueOff;

  NSStackView* notifyStack = [NSStackView stackViewWithViews:@[
    _notifyFailed, _notifyFirst, _notifyLost,
  ]];
  notifyStack.orientation = NSUserInterfaceLayoutOrientationVertical;
  notifyStack.alignment = NSLayoutAttributeLeading;
  notifyStack.spacing = 4;
  notifyStack.edgeInsets = NSEdgeInsetsMake(8, 8, 8, 8);

  _startAtBoot = [NSButton checkboxWithTitle:@"Start at boot"
                                      target:self
                                      action:@selector(startAtBootToggled:)];
  _bootNote = dim_label(@"Runs both daemons as LaunchAgents in your session.");
  _bootNote.font = [NSFont systemFontOfSize:NSFont.smallSystemFontSize];

  NSStackView* bootStack = [NSStackView stackViewWithViews:@[ _startAtBoot, _bootNote ]];
  bootStack.orientation = NSUserInterfaceLayoutOrientationVertical;
  bootStack.alignment = NSLayoutAttributeLeading;
  bootStack.spacing = 2;
  bootStack.edgeInsets = NSEdgeInsetsMake(8, 8, 8, 8);

  NSStackView* root = [NSStackView stackViewWithViews:@[
    _daemonLine,
    _benchLine,
    [self boxTitled:@"Camera" content:cameraStack],
    [self boxTitled:@"Notify me when…" content:notifyStack],
    [self boxTitled:@"Startup" content:bootStack],
  ]];
  root.orientation = NSUserInterfaceLayoutOrientationVertical;
  root.alignment = NSLayoutAttributeLeading;
  root.spacing = 12;
  root.edgeInsets = NSEdgeInsetsMake(16, 16, 16, 16);
  root.translatesAutoresizingMaskIntoConstraints = NO;

  _window = [[NSWindow alloc]
      initWithContentRect:NSMakeRect(0, 0, 460, 620)
                styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                           NSWindowStyleMaskResizable)
                  backing:NSBackingStoreBuffered
                    defer:NO];
  _window.title = @"Octomancer";
  _window.releasedWhenClosed = NO;
  _window.contentView = root;
  [_window setContentMinSize:NSMakeSize(420, 520)];
  [_window center];

  [self refreshStartAtBootCheckbox];
}

- (void)updateWindow {
  if (_window == nil || !_window.isVisible) return;

  if (_controlUp) {
    _daemonLine.stringValue = [NSString
        stringWithFormat:@"Sync daemon %@ — up since %@%@",
                         ns(_status.daemon.version),
                         ago_text(_status.daemon.started_wall,
                                  _status.daemon.now_wall),
                         _status.daemon.dry_run ? @" — DRY RUN" : @""];
    _daemonLine.textColor = [NSColor labelColor];
  } else {
    _daemonLine.stringValue = @"Sync daemon not answering";
    _daemonLine.textColor = [NSColor systemRedColor];
  }

  if (_controlUp && _status.bench.has) {
    _benchLine.stringValue =
        [NSString stringWithFormat:@"Bench: %d box%s, %@, spread %.0f ms",
                                   _status.bench.boxes,
                                   _status.bench.boxes == 1 ? "" : "es",
                                   offset_text(_status.bench.offset_s),
                                   _status.bench.spread_s * 1000.0];
  } else if (_benchUp && _snapshot.has_bench) {
    _benchLine.stringValue =
        [NSString stringWithFormat:@"Bench: %d box%s, %@", _snapshot.live,
                                   _snapshot.live == 1 ? "" : "es",
                                   offset_text(_snapshot.bench_offset)];
  } else {
    _benchLine.stringValue = @"Bench: nothing heard yet";
  }

  [self rebuildCameraPicker];
  [self updateCameraDetail];
}

// Rebuilt in place: replacing the whole menu on a two-second timer would fight
// anyone trying to use it.
- (void)rebuildCameraPicker {
  NSMutableArray<NSString*>* wanted = [NSMutableArray array];
  [wanted addObject:@"All cameras"];
  for (const octo::CameraStatus& c : _status.cameras) {
    NSString* title = c.name.empty() ? ns(c.id) : ns(c.name);
    if (!c.present) title = [title stringByAppendingString:@" (off the air)"];
    [wanted addObject:title];
  }
  if ([wanted isEqualToArray:[_cameraPicker itemTitles]]) return;

  NSString* was = _cameraPicker.titleOfSelectedItem;
  [_cameraPicker removeAllItems];
  [_cameraPicker addItemsWithTitles:wanted];
  if (was != nil && [wanted containsObject:was]) {
    [_cameraPicker selectItemWithTitle:was];
  }
}

// Which camera the controls act on, as the id the daemon knows it by. Empty
// means all of them.
- (std::string)selectedCameraId {
  const NSInteger index = _cameraPicker.indexOfSelectedItem;
  if (index <= 0) return std::string();
  const size_t which = static_cast<size_t>(index - 1);
  if (which >= _status.cameras.size()) return std::string();
  return _status.cameras[which].id;
}

- (const octo::CameraStatus*)selectedCamera {
  const NSInteger index = _cameraPicker.indexOfSelectedItem;
  if (_status.cameras.empty()) return nullptr;
  if (index <= 0) return &_status.cameras[0];  // "All": show the first
  const size_t which = static_cast<size_t>(index - 1);
  if (which >= _status.cameras.size()) return nullptr;
  return &_status.cameras[which];
}

- (void)updateCameraDetail {
  const octo::CameraStatus* c = [self selectedCamera];
  const bool have = c != nullptr;
  _syncButton.enabled = have && _controlUp && !_busy;
  _sourcePicker.enabled = have && _controlUp && !_busy;

  if (!have) {
    _tcValue.stringValue = @"--";
    _errorValue.stringValue = @"--";
    _rateValue.stringValue = @"--";
    _cycleValue.stringValue = _controlUp ? @"no cameras seen yet" : @"--";
    _writeValue.stringValue = @"--";
    _leadValue.stringValue = @"--";
    return;
  }

  _tcValue.stringValue = c->timecode.empty() ? @"--" : ns(c->timecode);
  if (c->has_error) {
    _errorValue.stringValue = offset_text(c->error_s);
    _errorValue.textColor = fabs(c->error_s) < 0.05 ? [NSColor systemGreenColor]
                                                    : [NSColor systemOrangeColor];
  } else {
    _errorValue.stringValue = @"--";
    _errorValue.textColor = [NSColor labelColor];
  }
  _rateValue.stringValue =
      c->has_fps ? [NSString stringWithFormat:@"%d fps", c->fps] : @"--";

  // The picker shows what the camera says, unless someone is mid-change. A
  // camera that has not reported 4.7 at all gets a third, disabled entry
  // rather than being drawn as though it had said "time of day".
  if (!_busy) {
    if (!c->has_source) {
      if (_sourcePicker.numberOfItems < 3) {
        [_sourcePicker addItemWithTitle:@"unknown"];
      }
      [_sourcePicker selectItemWithTitle:@"unknown"];
    } else {
      if (_sourcePicker.numberOfItems > 2) {
        [_sourcePicker removeItemWithTitle:@"unknown"];
      }
      [_sourcePicker selectItemAtIndex:
          c->source == octo::bmd::kTimecodeSourceClip ? 1 : 0];
    }
  }

  NSString* cycle = c->action.empty() ? @"--" : ns(c->action);
  if (c->recording) cycle = @"RECORDING — the clock will not be touched";
  if (c->has_source && c->source != octo::bmd::kTimecodeSourceTimeOfDay) {
    cycle = @"timecode does not follow the clock — cannot sync";
  }
  _cycleValue.stringValue = cycle;
  _cycleValue.textColor =
      (c->recording ||
       (c->has_source && c->source != octo::bmd::kTimecodeSourceTimeOfDay))
          ? [NSColor systemOrangeColor]
          : [NSColor labelColor];

  NSString* write = ago_text(c->has_last_write ? c->last_write_wall : 0.0,
                             _status.daemon.now_wall);
  if (c->writes > 0) {
    write = [write stringByAppendingFormat:@" (%d this session)", c->writes];
  }
  _writeValue.stringValue = write;
  _leadValue.stringValue =
      c->has_lead ? [NSString stringWithFormat:@"%.0f ms", c->lead_s * 1000.0]
                  : @"not measured yet";
}

// ------------------------------------------------------------------ actions

- (void)cameraPicked:(id)sender {
  (void)sender;
  [self updateCameraDetail];
}

// Send a request and watch it to the end, off the main thread. The button is
// disabled meanwhile: a second sync queued behind the first would just make
// someone wait twice as long for the same answer.
- (void)send:(const std::string&)command describing:(NSString*)what {
  if (_busy) return;
  _busy = true;
  _activity.stringValue = [what stringByAppendingString:@"…"];
  _activity.textColor = [NSColor secondaryLabelColor];
  [self updateCameraDetail];

  const std::string path = _controlSocket;
  const std::string request = command;
  dispatch_async(_queue, ^{
    std::string reply, err;
    octo::RequestResult result;
    bool ok = octo::query(path, request, &reply, &err, 5.0);
    if (ok) ok = octo::parse_result(reply, &result, &err);

    while (ok && !octo::request_finished(result.state)) {
      [NSThread sleepForTimeInterval:0.5];
      const std::string ask = "result id=" + std::to_string(result.id);
      ok = octo::query(path, ask, &reply, &err, 5.0);
      if (ok) ok = octo::parse_result(reply, &result, &err);
    }

    const bool done = ok && result.state == octo::RequestState::kDone;
    const std::string message =
        ok ? (result.message.empty() ? std::string("done") : result.message)
           : err;
    dispatch_async(dispatch_get_main_queue(), ^{
      self->_busy = false;
      self->_activity.stringValue = ns(message);
      self->_activity.textColor =
          done ? [NSColor systemGreenColor] : [NSColor systemOrangeColor];
      [self refresh];
    });
  });
}

- (std::string)cameraArgument {
  const std::string id = [self selectedCameraId];
  return id.empty() ? std::string() : (" camera=" + octo::escape(id));
}

- (void)syncNow:(id)sender {
  (void)sender;
  if (!_controlUp) {
    [self complain:@"The sync daemon is not answering."
              info:@"Start it with `make install-agent`, or tick “Start at "
                   @"boot” below."];
    return;
  }
  [self ensureWindow];
  [self send:("sync" + [self cameraArgument]) describing:@"Syncing"];
}

- (void)sourcePicked:(id)sender {
  (void)sender;
  const octo::CameraStatus* c = [self selectedCamera];
  if (c == nullptr) return;
  const NSInteger index = _sourcePicker.indexOfSelectedItem;
  if (index < 0 || index > 1) return;
  const int64_t value = index == 1 ? octo::bmd::kTimecodeSourceClip
                                   : octo::bmd::kTimecodeSourceTimeOfDay;
  if (c->has_source && c->source == value) return;  // already there

  if (value == octo::bmd::kTimecodeSourceClip) {
    // The setting that makes the camera unsyncable. Someone choosing it may
    // not know that yet, and finding out at midnight is worse than an alert.
    NSAlert* alert = [[NSAlert alloc] init];
    alert.messageText = @"Stop the timecode following the clock?";
    alert.informativeText =
        @"In Clip the camera parks its timecode at 00:00:00:00 and stops. "
        @"Octomancer cannot sync it again until this is set back to Time of "
        @"day.";
    [alert addButtonWithTitle:@"Set to Clip"];
    [alert addButtonWithTitle:@"Cancel"];
    alert.alertStyle = NSAlertStyleWarning;
    if ([alert runModal] != NSAlertFirstButtonReturn) {
      [self updateCameraDetail];  // put the picker back
      return;
    }
  }

  [self send:("source value=" + std::to_string(value) + [self cameraArgument])
      describing:@"Setting timecode source"];
}

- (void)notifyToggled:(id)sender {
  NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
  if (sender == _notifyFailed) {
    [defaults setBool:_notifyFailed.state == NSControlStateValueOn
               forKey:kPrefSyncFailed];
  } else if (sender == _notifyFirst) {
    [defaults setBool:_notifyFirst.state == NSControlStateValueOn
               forKey:kPrefFirstSync];
  } else if (sender == _notifyLost) {
    [defaults setBool:_notifyLost.state == NSControlStateValueOn
               forKey:kPrefCameraLost];
  }
}

// ------------------------------------------------------------ start at boot

- (NSString*)launchAgentsDirectory {
  return [NSHomeDirectory() stringByAppendingPathComponent:@"Library/LaunchAgents"];
}

- (BOOL)agentsInstalled {
  NSFileManager* fm = [NSFileManager defaultManager];
  for (NSString* labelName : kAgentLabels) {
    NSString* path = [[self launchAgentsDirectory]
        stringByAppendingPathComponent:[labelName stringByAppendingPathExtension:@"plist"]];
    if (![fm fileExistsAtPath:path]) return NO;
  }
  return YES;
}

- (void)refreshStartAtBootCheckbox {
  _startAtBoot.state = [self agentsInstalled] ? NSControlStateValueOn
                                             : NSControlStateValueOff;
}

- (BOOL)runLaunchctl:(NSArray<NSString*>*)args {
  NSTask* task = [[NSTask alloc] init];
  task.executableURL = [NSURL fileURLWithPath:@"/bin/launchctl"];
  task.arguments = args;
  task.standardOutput = [NSFileHandle fileHandleWithNullDevice];
  task.standardError = [NSFileHandle fileHandleWithNullDevice];
  NSError* error = nil;
  if (![task launchAndReturnError:&error]) {
    NSLog(@"octomancer: launchctl failed to start: %@", error);
    return NO;
  }
  [task waitUntilExit];
  return task.terminationStatus == 0;
}

- (void)startAtBootToggled:(id)sender {
  (void)sender;
  const BOOL wanted = _startAtBoot.state == NSControlStateValueOn;
  NSString* uid = [NSString stringWithFormat:@"gui/%u", getuid()];

  if (!wanted) {
    for (NSString* labelName : kAgentLabels) {
      [self runLaunchctl:@[ @"bootout",
                            [uid stringByAppendingFormat:@"/%@", labelName] ]];
      NSString* path = [[self launchAgentsDirectory]
          stringByAppendingPathComponent:
              [labelName stringByAppendingPathExtension:@"plist"]];
      [[NSFileManager defaultManager] removeItemAtPath:path error:nil];
    }
    _bootNote.stringValue = @"Both daemons will stay stopped until you start "
                            @"them again.";
    [self refreshStartAtBootCheckbox];
    return;
  }

  // The plists ship inside the bundle, generated by the build from the same
  // templates `make install-agent` uses. Writing them here from scratch would
  // make this the second place their contents are decided, and the two would
  // drift.
  NSFileManager* fm = [NSFileManager defaultManager];
  NSString* dir = [self launchAgentsDirectory];
  [fm createDirectoryAtPath:dir
withIntermediateDirectories:YES
                 attributes:nil
                      error:nil];

  NSMutableArray<NSString*>* missing = [NSMutableArray array];
  for (NSString* labelName : kAgentLabels) {
    NSString* leaf = [labelName stringByAppendingPathExtension:@"plist"];
    NSString* source = [[NSBundle mainBundle] pathForResource:labelName
                                                       ofType:@"plist"];
    if (source == nil) {
      [missing addObject:leaf];
      continue;
    }
    NSString* dest = [dir stringByAppendingPathComponent:leaf];
    [fm removeItemAtPath:dest error:nil];
    NSError* error = nil;
    if (![fm copyItemAtPath:source toPath:dest error:&error]) {
      NSLog(@"octomancer: could not install %@: %@", leaf, error);
      [missing addObject:leaf];
      continue;
    }
    [self runLaunchctl:@[ @"bootout",
                          [uid stringByAppendingFormat:@"/%@", labelName] ]];
    if (![self runLaunchctl:@[ @"bootstrap", uid, dest ]]) {
      NSLog(@"octomancer: launchctl bootstrap %@ failed", leaf);
      [missing addObject:leaf];
    }
  }

  if (missing.count > 0) {
    [self complain:@"Could not set both daemons to start at boot."
              info:[NSString stringWithFormat:
                                 @"%@ could not be loaded. Running `make "
                                 @"install-agent` from the source tree does "
                                 @"the same job and will say why.",
                                 [missing componentsJoinedByString:@", "]]];
  } else {
    _bootNote.stringValue = @"Both daemons will start when you log in.";
  }
  [self refreshStartAtBootCheckbox];
}

- (void)complain:(NSString*)message info:(NSString*)info {
  NSAlert* alert = [[NSAlert alloc] init];
  alert.messageText = message;
  alert.informativeText = info;
  [alert addButtonWithTitle:@"OK"];
  [alert runModal];
}

// -------------------------------------------------------------------- misc

- (void)showWindow:(id)sender {
  (void)sender;
  [self ensureWindow];
  [self updateWindow];
  [NSApp activateIgnoringOtherApps:YES];
  [_window makeKeyAndOrderFront:nil];
  [self updateWindow];
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
    // Accessory, not regular: no Dock tile, and opening the window does not
    // permanently promote it into one.
    [app setActivationPolicy:NSApplicationActivationPolicyAccessory];
    [app run];
  }
  return 0;
}
