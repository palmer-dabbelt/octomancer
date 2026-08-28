// Octomancer -- the window and the menu-bar item.
//
// This process holds no state worth losing and decides nothing about clocks.
// It asks the two daemons what they can see, draws it, and hands back what a
// person clicks. It can be quit, restarted or never run at all without
// affecting what is being measured.
//
// It talks to both sockets, because there are two daemons and they know
// different things. octomancerd knows the timecode bench and which boxes have
// drifted; octomancer-sync knows the cameras, and is the only one that can be
// told to do anything. Losing either is drawn as a missing panel rather than
// as an error, because one daemon being down is not a reason to stop showing
// what the other one says.
//
// The one thing this process adds is notifications, which need a bundled,
// signed application and a login session -- none of which a launchd agent has.
#import <AppKit/AppKit.h>
#import <UserNotifications/UserNotifications.h>

#include <algorithm>
#include <set>
#include <string>
#include <vector>

#include "agents.h"
#include "bmd.h"
#include "camconf.h"
#include "client.h"
#include "control.h"
#include "devices.h"
#include "proto.h"
#include "registry.h"
#include "render.h"
#include "server.h"
#include "timeutil.h"

namespace {

constexpr double kRefreshSeconds = 2.0;

// How many of those ticks go by between one look at launchd and the next.
//
// Asking is a fork and an exec of /bin/launchctl, twice, and nothing it says
// changes in two seconds: a daemon that is running was running last time and
// will be running next time. Ten seconds is soon enough to notice a crash and
// cheap enough to leave running while the window is open.
constexpr int kDaemonStateEveryTicks = 5;

// The three things a person can ask to be told about, and where the answer is
// remembered. Defaults on: someone who installs a clock-sync tool wants to
// know when the clock did not get synced.
NSString* const kPrefSyncFailed = @"notify.sync-failed";
NSString* const kPrefFirstSync = @"notify.first-sync";
NSString* const kPrefCameraLost = @"notify.camera-lost";

// ...and two about the bench rather than a camera.
NSString* const kPrefBenchDisagree = @"notify.bench-disagree";
NSString* const kPrefBenchDrift = @"notify.bench-drift";

// The menu-bar item is a shortcut to the window, not the program. Someone who
// drives all of this from `octomancer` on the command line has no use for it,
// and the menu bar is short.
NSString* const kPrefShowStatusItem = @"ui.show-status-item";

// Which tab was open last. Remembered because the four pages answer four
// different questions, and somebody who came here to watch the bench does not
// want to be put back on the camera page every time the app restarts.
NSString* const kPrefTab = @"ui.tab";

// How far apart the boxes have to be before they are not jammed to the same
// source any more.
//
// Boxes actually jammed together sit within a few milliseconds of each other;
// this bench runs at about 12 ms, which is where advertisement timing alone
// puts it. A box that has been left unjammed is out by seconds, not tens of
// milliseconds, so the threshold only has to clear the noise floor by enough
// not to cry wolf. The gap between the two is hysteresis, so a bench sitting
// on the line does not alternate.
constexpr double kSpreadAlertEnter = 0.050;
constexpr double kSpreadAlertExit = 0.035;

// How fast the bench and this Mac may pull apart before it is worth saying so.
//
// Parts per million, because the absolute offset between them is a constant
// with no meaning (timecode-of-day against wall clock) and only its *rate* of
// change says anything. 25 ppm is about two seconds a day: fast enough to
// matter over a shoot, slow enough that an ordinary crystal will not trip it.
// The daemon has already refused to report drift measured over too short an
// arm, which is the part that would otherwise invent hundreds of ppm.
constexpr double kDriftAlertEnter = 25.0;
constexpr double kDriftAlertExit = 15.0;


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

// The same spans, said as a duration rather than as a time in the past.
//
// Negative is clamped to zero on purpose. What this is usually given is the
// difference between now and a process's start, both absolute wall clocks, and
// a clock step -- NTP catching up, or somebody dragging the date -- can leave a
// daemon looking as though it starts three hours from now. "up 0s" is wrong in
// a boring way; "up -3.0h" is wrong in a way that gets reported as a bug in
// launchd.
NSString* elapsed_text(double seconds) {
  const double d = seconds > 0.0 ? seconds : 0.0;
  if (d < 90.0) return [NSString stringWithFormat:@"%.0fs", d];
  if (d < 5400.0) return [NSString stringWithFormat:@"%.0fm", d / 60.0];
  return [NSString stringWithFormat:@"%.1fh", d / 3600.0];
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

// What a section of a page is called, now that the sections are inside tabs
// rather than inside boxes with titles drawn on them.
NSTextField* heading(NSString* text) {
  NSTextField* f = label(text);
  f.font = [NSFont boldSystemFontOfSize:NSFont.systemFontSize];
  return f;
}

// A label for a sentence rather than a value: it wraps where the others
// truncate. Wrapping needs a width to wrap to and autolayout will not invent
// one -- an unconstrained label reports the whole sentence on one line as its
// natural width, and the window grows to match -- so every caller pins it to
// something. See -pinWidth:to:inset:.
NSTextField* wrapped_label(NSString* text) {
  NSTextField* f = [NSTextField wrappingLabelWithString:text];
  f.selectable = NO;
  f.textColor = [NSColor secondaryLabelColor];
  f.font = [NSFont systemFontOfSize:NSFont.smallSystemFontSize];
  return f;
}

// One camera as `octomancer-sync --scan-only` prints it, from
// print_camera_row in octomancer-sync.cc:
//
//     09EE26AF-1D5C-4E5B-9C2A-...   A:1EAE18A7   rssi=-52   (service uuid)
//
// Read by landmark rather than by column: the columns are only as wide as
// printf padded them, and one long name pushes everything after it to the
// right. A line with no rssi= in it is not a camera row at all -- it is the
// tool saying something in English -- and the caller shows those as they were
// written, because the interesting failures ("no LE devices at all") are all
// sentences.
bool parse_scan_row(const std::string& line, std::string* id,
                    std::string* name) {
  const size_t mark = line.find("rssi=");
  if (mark == std::string::npos) return false;
  std::string head = line.substr(0, mark);
  const size_t start = head.find_first_not_of(" \t");
  if (start == std::string::npos) return false;
  head = head.substr(start);
  const size_t gap = head.find_first_of(" \t");
  if (gap == std::string::npos) return false;
  *id = head.substr(0, gap);
  const size_t rest = head.find_first_not_of(" \t", gap);
  std::string said = rest == std::string::npos ? std::string()
                                               : head.substr(rest);
  while (!said.empty() && (said.back() == ' ' || said.back() == '\t')) {
    said.pop_back();
  }
  // The tool writes "(no name)" where a camera advertised none. Carrying that
  // through as a name would put it in the picker as though it were one.
  if (said == "(no name)") said.clear();
  *name = said;
  return !id->empty();
}

// One line of the Configuration page's device list.
//
// Kept as a list of its own rather than looked up in the two daemons when a
// checkbox is clicked, because the checkbox has to keep working for a device
// neither daemon can see -- which is exactly the device somebody switched off.
struct ConfigEntry {
  bool camera = false;
  std::string id;
  std::string name;
  bool enabled = false;
  // Whether somebody asked to be told when this one is wrong. Read from the
  // same file and drawn on the same row, because the two questions are asked
  // about the same device and answering one usually means thinking about the
  // other.
  bool warn = false;
  bool present = false;  // is either daemon hearing it right now
};

// One camera the pairing sheet's scan turned up.
struct ScanHit {
  std::string id;
  std::string name;
};

}  // namespace

@interface OctoController : NSObject <NSApplicationDelegate, NSMenuDelegate,
                                      NSTabViewDelegate>
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
  bool _benchDisagreeing;
  bool _benchDrifting;

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
  NSButton* _notifyBenchDisagree;
  NSButton* _notifyBenchDrift;
  NSButton* _startAtBoot;
  NSTextField* _bootNote;
  NSButton* _showStatusItem;
  NSTextField* _statusNote;
  NSButton* _writesEnabled;

  // The configuration file, read on the polling queue and kept here. The draw
  // path runs every two seconds and must not touch a disk to do it.
  octo::CamConf _conf;
  bool _confLoaded;

  // --- the Devices page ------------------------------------------------
  //
  // The rows are keyed by device rather than by position, and the views are
  // kept between ticks: a page that rebuilds itself on a timer is a page whose
  // text cannot be selected and whose scroll position will not stay put.
  NSTabView* _tabs;
  NSTextField* _canonicalLine;
  NSTextField* _hiddenLine;
  NSGridView* _deviceGrid;
  NSArray<NSString*>* _deviceKeys;
  NSMutableDictionary<NSString*, NSArray<NSTextField*>*>* _deviceCells;

  // --- the Configuration page ------------------------------------------
  NSGridView* _configGrid;
  NSArray<NSString*>* _configKeys;
  NSMutableDictionary<NSString*, NSArray<NSView*>*>* _configCells;
  std::vector<ConfigEntry> _configEntries;
  NSTextField* _configNote;
  NSTextField* _benchAgentLine;
  NSTextField* _syncAgentLine;
  int _daemonTick;

  // --- the pairing sheet -------------------------------------------------
  NSWindow* _pairSheet;
  NSTask* _pairTask;
  NSTextView* _pairLog;
  NSScrollView* _pairScroll;
  NSPopUpButton* _pairPicker;
  NSButton* _pairButton;
  NSButton* _pairSearchButton;
  NSButton* _pairStopDaemonButton;
  NSTextField* _pairWarning;
  NSTextField* _pairActivity;
  NSProgressIndicator* _pairSpinner;
  NSMutableString* _pairPending;
  std::vector<ScanHit> _pairFound;
}

- (instancetype)init {
  self = [super init];
  if (self) {
    _benchUp = false;
    _benchDisagreeing = false;
    _benchDrifting = false;
    _controlUp = false;
    _notificationsReady = false;
    _busy = false;
    _eventCursor = 0;
    _eventCursorPrimed = false;
    _confLoaded = false;
    _daemonTick = 0;
    _deviceKeys = @[];
    _deviceCells = [NSMutableDictionary dictionary];
    _configKeys = @[];
    _configCells = [NSMutableDictionary dictionary];
    _pairPending = [NSMutableString string];
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
      kPrefBenchDisagree : @YES,
      kPrefBenchDrift : @YES,
      kPrefShowStatusItem : @YES,
    }];
  }
  return self;
}

- (void)applicationDidFinishLaunching:(NSNotification*)note {
  (void)note;
  _menu = [[NSMenu alloc] init];
  _menu.delegate = self;

  [self applyStatusItemVisibility];
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

  // launchd is asked about on its own, slower schedule, and only while
  // somebody is looking at the page that says what it answered.
  if (_window != nil && _window.isVisible &&
      (_daemonTick++ % kDaemonStateEveryTicks) == 0) {
    [self refreshDaemonState];
  }

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

    // Who is enabled, read here rather than in the draw path: it is a file,
    // and the daemon is the one that knows which file it was told to use.
    const std::string conf_path =
        control_ok && !status.daemon.config_path.empty()
            ? status.daemon.config_path
            : octo::default_camera_config_path();
    octo::CamConf conf;
    std::string conf_err;
    const bool conf_ok = conf.load(conf_path, &conf_err);

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

      self->_confLoaded = conf_ok;
      if (conf_ok) self->_conf = conf;

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
      if (bench_ok) {
        [self notifyForNewAlerts];
        [self notifyForBenchConditions];
      }
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

// Two conditions about the bench as a whole, rather than about one box.
//
// Both are edge-triggered with hysteresis, for the same reason the daemon's
// per-box alerts are: a bench parked on a threshold must not be able to send a
// notification every two seconds. They are computed here rather than in
// octomancerd because both are one line of arithmetic over a snapshot it
// already publishes, and adding a second alerting mechanism to the daemon to
// save that line would be the worse trade.
- (void)notifyForBenchConditions {
  NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];

  // 1. The boxes disagree with each other. Nothing about the Mac is involved:
  //    this is the bench failing to be one bench, which no amount of syncing
  //    against it can fix.
  if (_snapshot.has_bench && _snapshot.live >= 2) {
    const double spread = _snapshot.bench_spread;
    const bool over = spread > kSpreadAlertEnter;
    const bool under = spread < kSpreadAlertExit;
    if (over && !_benchDisagreeing) {
      _benchDisagreeing = true;
      if ([defaults boolForKey:kPrefBenchDisagree]) {
        [self postNotification:@"Timecode boxes disagree"
                          body:[NSString stringWithFormat:
                                             @"%d timecode boxes are spread over %@. "
                                             @"They are not all jammed to the "
                                             @"same source.",
                                             _snapshot.live,
                                             offset_text(spread)]
                    identifier:@"bench-disagree"];
      }
    } else if (under && _benchDisagreeing) {
      _benchDisagreeing = false;
    }
  }

  // 2. The bench and this Mac are pulling apart. The median across boxes that
  //    have a drift figure at all -- the daemon refuses to produce one from a
  //    short arm, so anything here has a real lever behind it.
  std::vector<double> ppm;
  for (const octo::DeviceSnapshot& d : _snapshot.device) {
    if (d.live && d.has_drift) ppm.push_back(d.drift_ppm);
  }
  if (!ppm.empty()) {
    std::sort(ppm.begin(), ppm.end());
    const double median = ppm[ppm.size() / 2];
    const double mag = fabs(median);
    if (mag > kDriftAlertEnter && !_benchDrifting) {
      _benchDrifting = true;
      if ([defaults boolForKey:kPrefBenchDrift]) {
        [self postNotification:@"Bench drifting from this Mac"
                          body:[NSString stringWithFormat:
                                             @"%+.0f ppm — about %.1f s a day. "
                                             @"One of the two clocks is wrong.",
                                             median, fabs(median) * 0.0864]
                    identifier:@"bench-drift"];
      }
    } else if (mag < kDriftAlertExit && _benchDrifting) {
      _benchDrifting = false;
    }
  }
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

// Show or hide the menu-bar item.
//
// Hidden, the app is still running -- it just has nothing on screen. That is
// the useful state for someone who drives everything from `octomancer` on the
// command line: no clutter, and notifications keep arriving, which they could
// not do otherwise. Posting one needs a bundled, signed application, so this
// process is the only thing here that can, and quitting it is what turns them
// off.
//
// Getting back to the window is opening Octomancer.app again. A second launch
// does not start a second copy; macOS reactivates this one, and
// applicationShouldHandleReopen below puts the window up.
//
// None of this touches the daemons. They hold the clocks and they do not care
// whether anybody is watching.
- (void)applyStatusItemVisibility {
  const BOOL show =
      [[NSUserDefaults standardUserDefaults] boolForKey:kPrefShowStatusItem];

  if (show) {
    if (_statusItem == nil) {
      _statusItem = [[NSStatusBar systemStatusBar]
          statusItemWithLength:NSVariableStatusItemLength];
      _statusItem.button.title = @"◷";
      _statusItem.button.toolTip = @"Octomancer";
      _statusItem.menu = _menu;
      [self updateStatusItem];
    }
    _statusNote.stringValue =
        @"Uncheck to keep the menu bar clear. Octomancer keeps running and "
        @"keeps notifying; open the app again to get this window back.";
    return;
  }

  if (_statusItem != nil) {
    [[NSStatusBar systemStatusBar] removeStatusItem:_statusItem];
    _statusItem = nil;
  }
  _statusNote.stringValue =
      @"Nothing in the menu bar. Open Octomancer.app again for this window. "
      @"Syncing is the daemon's job either way.";
}

// Opening the app while it is already running. Without this a second launch of
// an accessory app does nothing visible, which reads as the app being broken.
- (BOOL)applicationShouldHandleReopen:(NSApplication*)app
                    hasVisibleWindows:(BOOL)visible {
  (void)app;
  (void)visible;
  [self showWindow:nil];
  return YES;
}

- (void)showStatusItemToggled:(id)sender {
  (void)sender;
  [[NSUserDefaults standardUserDefaults]
      setBool:_showStatusItem.state == NSControlStateValueOn
       forKey:kPrefShowStatusItem];
  [self applyStatusItemVisibility];
  [self rebuildMenu];
}

// The one glyph in the menu bar, and the colour it is drawn in.
//
// An NSStatusItem's title is a plain string and renders in whatever colour the
// menu bar is using, so a coloured blip has to be an attributed string. The
// clock stays in the label colour and only the blip is tinted: a lone dot in
// the menu bar says nothing about which app it belongs to, and the point of the
// thing is to be identified at a glance from across the room.
//
// systemRedColor and systemYellowColor rather than literal components, because
// the menu bar follows the appearance and a fixed yellow that reads on a dark
// bar vanishes into a light one.
- (void)setStatusBlip:(NSColor*)color tip:(NSString*)tip {
  if (_statusItem == nil) return;
  NSMutableAttributedString* title = [[NSMutableAttributedString alloc]
      initWithString:@"◷"
          attributes:@{NSForegroundColorAttributeName : [NSColor labelColor]}];
  if (color != nil) {
    [title appendAttributedString:
               [[NSAttributedString alloc]
                   initWithString:@" ●"
                       attributes:@{NSForegroundColorAttributeName : color}]];
  }
  _statusItem.button.attributedTitle = title;
  _statusItem.button.toolTip = tip;
}

// What the blip is about, spelled out for the place there is room to spell it.
//
// A colour can carry "something is wrong" and nothing else, so the names go in
// the tooltip: "one device out of sync" only sends somebody looking, and the
// device they are looking for is the whole answer.
- (NSString*)warningTooltipFor:(const octo::DeviceView&)view {
  NSMutableArray<NSString*>* lines = [NSMutableArray array];
  for (int pass = 0; pass < 2; ++pass) {
    const octo::WarnLevel want = pass == 0 ? octo::WarnLevel::kOutOfSync
                                           : octo::WarnLevel::kUnsure;
    NSMutableArray<NSString*>* names = [NSMutableArray array];
    for (const octo::DeviceRow& r : view.rows) {
      if (r.warn_level == want) [names addObject:ns(r.name)];
    }
    if (names.count == 0) continue;
    NSString* why = pass == 0
                        ? @"Out of sync with the bench"
                        : @"Not heard from recently enough to say";
    [lines addObject:[NSString stringWithFormat:@"%@: %@", why,
                                                [names componentsJoinedByString:
                                                           @", "]]];
  }
  return [lines componentsJoinedByString:@"\n"];
}

// The menu-bar item.
//
// It used to carry a count of the timecode boxes being heard, which looks like
// information and is not: five boxes on the bench says 5 whether all five are
// jammed to each other or one of them walked out of the building an hour ago.
// The question somebody glancing up actually has is "is anything wrong", so
// that is the only thing this answers -- and only about the devices somebody
// asked to be warned about, which is why the blip is usually absent.
- (void)updateStatusItem {
  if (_statusItem == nil) return;

  // Neither daemon answering is kept its own shape rather than folded into the
  // warnings. "Nobody is watching the bench" and "the bench is wrong" are
  // different problems with different fixes, and a red dot for the first would
  // send somebody to the cameras when the thing to restart is on this Mac.
  if (!_benchUp && !_controlUp) {
    _statusItem.button.attributedTitle = [[NSAttributedString alloc]
        initWithString:@"◷ ?"
            attributes:@{
              NSForegroundColorAttributeName : [NSColor labelColor]
            }];
    _statusItem.button.toolTip = @"Octomancer: no daemon answering";
    return;
  }

  const octo::DeviceView view = [self deviceView];
  NSString* detail = [self warningTooltipFor:view];
  switch (view.worst_warning) {
    case octo::WarnLevel::kOutOfSync:
      [self setStatusBlip:[NSColor systemRedColor] tip:detail];
      return;
    case octo::WarnLevel::kUnsure:
      [self setStatusBlip:[NSColor systemYellowColor] tip:detail];
      return;
    case octo::WarnLevel::kNone:
      break;
  }
  [self setStatusBlip:nil
                  tip:view.has_canonical
                          ? [NSString stringWithFormat:
                                          @"Octomancer: bench %@ vs this Mac",
                                          offset_text(view.canonical_offset_s)]
                          : @"Octomancer"];
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
  // Two ways out, because there were two things behind the one that used to be
  // here and only one of them was "quit". This process is a window onto the
  // daemons; closing it stops notifications and nothing else, and the clocks
  // go on being set. Somebody who means "stop setting my cameras' clocks" has
  // to be able to say that, and somebody who means "get this out of my menu
  // bar" must not say the other by accident. Hence two items whose titles are
  // each unmistakable, and a note under the harmless one.
  //
  // Cmd-Q stays on the harmless one. It is the reflex, and a reflex should not
  // be able to stop the bench.
  [_menu addItem:[NSMenuItem separatorItem]];
  [_menu addItemWithTitle:@"Quit the Menu Bar App"
                   action:@selector(quit:)
            keyEquivalent:@"q"].target = self;
  [_menu addItem:[self disabledItem:@"    The daemons keep running and keep "
                                    @"syncing."]];
  [_menu addItem:[NSMenuItem separatorItem]];
  [_menu addItemWithTitle:@"Stop Octomancer and Its Daemons…"
                   action:@selector(stopEverything:)
            keyEquivalent:@""].target = self;
}

// ------------------------------------------------------------------- window

// One page of the window.
//
// The sections used to be NSBoxes, which cannot constrain a content view that
// manages its own layout: they had to be handed a view that had already been
// sized and left to lay it out the old springs-and-struts way. A tab view item
// is the opposite -- it hosts an ordinary autolayout view -- so the page is
// pinned to the item's own view and given its width from there. Sizing it
// first, out of habit from the box days, is what would leave a page frozen at
// the width it happened to have when the window was built.
//
// The bottom is pinned loosely so that a short page sits at the top of the tab
// rather than being stretched down to fill it.
- (void)tabView:(NSTabView*)tabView
    didSelectTabViewItem:(NSTabViewItem*)item {
  [[NSUserDefaults standardUserDefaults]
      setInteger:[tabView indexOfTabViewItem:item]
          forKey:kPrefTab];
}

- (NSTabViewItem*)tabTitled:(NSString*)title content:(NSView*)content {
  NSTabViewItem* item = [[NSTabViewItem alloc] initWithIdentifier:title];
  item.label = title;
  NSView* host = [[NSView alloc] init];
  content.translatesAutoresizingMaskIntoConstraints = NO;
  [host addSubview:content];
  [NSLayoutConstraint activateConstraints:@[
    [content.leadingAnchor constraintEqualToAnchor:host.leadingAnchor],
    [content.trailingAnchor constraintEqualToAnchor:host.trailingAnchor],
    [content.topAnchor constraintEqualToAnchor:host.topAnchor],
    [content.bottomAnchor constraintLessThanOrEqualToAnchor:host.bottomAnchor],
  ]];
  item.view = host;
  return item;
}

// A wrapping label has to be told how wide it is allowed to be, and the honest
// answer is always "as wide as the thing it is sitting in".
- (void)pinWidth:(NSView*)view to:(NSView*)container inset:(CGFloat)inset {
  [[view.widthAnchor constraintEqualToAnchor:container.widthAnchor
                                    constant:-inset] setActive:YES];
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

  // The permission switch, next to the controls it governs rather than in a
  // preferences pane: "why will this not sync?" and "let it sync" should be
  // the same glance.
  _writesEnabled =
      [NSButton checkboxWithTitle:@"Let octomancer change this camera"
                           target:self
                           action:@selector(writesToggled:)];

  _syncButton = [NSButton buttonWithTitle:@"Sync Now"
                                   target:self
                                   action:@selector(syncNow:)];
  _syncButton.keyEquivalent = @"\r";
  _activity = dim_label(@"");

  NSStackView* actions = [NSStackView stackViewWithViews:@[ _syncButton, _activity ]];
  actions.orientation = NSUserInterfaceLayoutOrientationHorizontal;
  actions.spacing = 12;
  actions.alignment = NSLayoutAttributeCenterY;

  // The box is as wide as the window; the grid is not. Parking it against a
  // spacer keeps it at its natural width, so its own column placement still
  // decides where the labels sit.
  NSStackView* detailRow =
      [NSStackView stackViewWithViews:@[ _detail, [[NSView alloc] init] ]];
  detailRow.orientation = NSUserInterfaceLayoutOrientationHorizontal;
  detailRow.spacing = 0;
  [detailRow setHuggingPriority:NSLayoutPriorityDefaultLow
                 forOrientation:NSLayoutConstraintOrientationHorizontal];

  NSStackView* cameraStack = [NSStackView stackViewWithViews:@[
    _cameraPicker, detailRow, _writesEnabled, actions,
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

  _notifyBenchDisagree =
      [NSButton checkboxWithTitle:@"the timecode boxes disagree with each other"
                           target:self
                           action:@selector(notifyToggled:)];
  _notifyBenchDrift =
      [NSButton checkboxWithTitle:@"the bench drifts away from this Mac"
                           target:self
                           action:@selector(notifyToggled:)];
  _notifyBenchDisagree.state = [defaults boolForKey:kPrefBenchDisagree]
                                   ? NSControlStateValueOn
                                   : NSControlStateValueOff;
  _notifyBenchDrift.state = [defaults boolForKey:kPrefBenchDrift]
                                ? NSControlStateValueOn
                                : NSControlStateValueOff;

  NSStackView* notifyStack = [NSStackView stackViewWithViews:@[
    _notifyFailed, _notifyFirst, _notifyLost,
    _notifyBenchDisagree, _notifyBenchDrift,
  ]];
  notifyStack.orientation = NSUserInterfaceLayoutOrientationVertical;
  notifyStack.alignment = NSLayoutAttributeLeading;
  notifyStack.spacing = 4;
  notifyStack.edgeInsets = NSEdgeInsetsMake(8, 8, 8, 8);

  // --- the Devices page -------------------------------------------------
  //
  // One line per device that is switched on, with the two numbers worth
  // reading: how long ago we heard it, and how far it is from the canonical
  // time. Which devices those are, and what that offset is measured against,
  // is decided in src/devices.h -- nothing on this page decides anything.

  _canonicalLine = label(@"…");
  _deviceGrid = [NSGridView gridViewWithNumberOfColumns:4 rows:0];
  _deviceGrid.columnSpacing = 16;
  _deviceGrid.rowSpacing = 5;
  // The two numeric columns hang off their right edge, which is the only way
  // digits of different lengths line up under each other.
  [_deviceGrid columnAtIndex:1].xPlacement = NSGridCellPlacementTrailing;
  [_deviceGrid columnAtIndex:2].xPlacement = NSGridCellPlacementTrailing;
  _hiddenLine = wrapped_label(@"");

  NSStackView* devicesStack = [NSStackView stackViewWithViews:@[
    _canonicalLine, _deviceGrid, _hiddenLine,
  ]];
  devicesStack.orientation = NSUserInterfaceLayoutOrientationVertical;
  devicesStack.alignment = NSLayoutAttributeLeading;
  devicesStack.spacing = 10;
  devicesStack.edgeInsets = NSEdgeInsetsMake(12, 12, 12, 12);
  [self pinWidth:_hiddenLine to:devicesStack inset:24];

  // --- the Configuration page --------------------------------------------
  //
  // Everything a person decides, as opposed to everything they watch: which
  // devices count, whether the daemons are running, and the one thing that has
  // to have happened before a camera can be talked to at all.

  // Two checkboxes per device and so two columns of them, headed, because
  // "on" and "warn" are different questions and a row of bare boxes would make
  // somebody count across to work out which is which. Centred under their
  // headings for the same reason.
  _configGrid = [NSGridView gridViewWithNumberOfColumns:4 rows:0];
  _configGrid.columnSpacing = 10;
  _configGrid.rowSpacing = 5;
  [_configGrid columnAtIndex:0].xPlacement = NSGridCellPlacementCenter;
  [_configGrid columnAtIndex:1].xPlacement = NSGridCellPlacementCenter;
  _configNote = wrapped_label(@"…");

  NSButton* pairButton = [NSButton buttonWithTitle:@"Pair Camera…"
                                            target:self
                                            action:@selector(openPairSheet:)];
  NSTextField* pairNote = wrapped_label(
      @"A camera has to be bonded with this Mac once before anything can read "
      @"or set its clock. The camera shows six digits on its own screen and "
      @"macOS asks for them.");

  _startAtBoot = [NSButton checkboxWithTitle:@"Start at boot"
                                      target:self
                                      action:@selector(startAtBootToggled:)];
  _bootNote =
      wrapped_label(@"Runs both daemons as LaunchAgents in your session.");

  _benchAgentLine = mono_label(@"…");
  _syncAgentLine = mono_label(@"…");
  for (NSTextField* f in @[ _benchAgentLine, _syncAgentLine ]) {
    f.font = [NSFont monospacedDigitSystemFontOfSize:NSFont.smallSystemFontSize
                                              weight:NSFontWeightRegular];
    f.textColor = [NSColor secondaryLabelColor];
  }
  // The two daemons are named by src/agents.cc rather than spelled out here,
  // for the same reason none of the launchd handling is written twice.
  NSTextField* benchName =
      dim_label(@(octo::agent_program(octo::Agent::kBench)));
  NSTextField* syncName =
      dim_label(@(octo::agent_program(octo::Agent::kSync)));
  NSGridView* agentGrid = [NSGridView gridViewWithViews:@[
    @[ benchName, _benchAgentLine ],
    @[ syncName, _syncAgentLine ],
  ]];
  agentGrid.columnSpacing = 12;
  agentGrid.rowSpacing = 2;
  [agentGrid columnAtIndex:0].xPlacement = NSGridCellPlacementTrailing;

  NSStackView* daemonButtons = [NSStackView stackViewWithViews:@[
    [NSButton buttonWithTitle:@"Start" target:self action:@selector(startDaemons:)],
    [NSButton buttonWithTitle:@"Stop" target:self action:@selector(stopDaemons:)],
    [NSButton buttonWithTitle:@"Restart" target:self action:@selector(restartDaemons:)],
  ]];
  daemonButtons.orientation = NSUserInterfaceLayoutOrientationHorizontal;
  daemonButtons.spacing = 8;

  NSStackView* configStack = [NSStackView stackViewWithViews:@[
    heading(@"Devices"), _configGrid, _configNote,
    pairButton, pairNote,
    heading(@"Daemons"), agentGrid, daemonButtons, _startAtBoot, _bootNote,
  ]];
  configStack.orientation = NSUserInterfaceLayoutOrientationVertical;
  configStack.alignment = NSLayoutAttributeLeading;
  configStack.spacing = 8;
  configStack.edgeInsets = NSEdgeInsetsMake(12, 12, 12, 12);
  [configStack setCustomSpacing:18 afterView:pairNote];
  for (NSTextField* note in @[ _configNote, pairNote, _bootNote ]) {
    [self pinWidth:note to:configStack inset:24];
  }

  // --- the Notifications page ---------------------------------------------

  _showStatusItem = [NSButton checkboxWithTitle:@"Show the icon in the menu bar"
                                         target:self
                                         action:@selector(showStatusItemToggled:)];
  _showStatusItem.state =
      [defaults boolForKey:kPrefShowStatusItem] ? NSControlStateValueOn
                                                : NSControlStateValueOff;
  _statusNote = wrapped_label(@"");

  NSStackView* menuBarStack =
      [NSStackView stackViewWithViews:@[ _showStatusItem, _statusNote ]];
  menuBarStack.orientation = NSUserInterfaceLayoutOrientationVertical;
  menuBarStack.alignment = NSLayoutAttributeLeading;
  menuBarStack.spacing = 2;
  menuBarStack.edgeInsets = NSEdgeInsetsMake(8, 8, 8, 8);

  // The menu-bar switch shares this page with the notification switches
  // because it is the same question -- how much of itself this process puts on
  // screen -- and because until now it was built and then never added to
  // anything, which made it a checkbox nobody could find.
  NSStackView* notifyPage = [NSStackView stackViewWithViews:@[
    heading(@"Notify me when…"), notifyStack,
    heading(@"Menu bar"), menuBarStack,
  ]];
  notifyPage.orientation = NSUserInterfaceLayoutOrientationVertical;
  notifyPage.alignment = NSLayoutAttributeLeading;
  notifyPage.spacing = 8;
  notifyPage.edgeInsets = NSEdgeInsetsMake(12, 12, 12, 12);
  [self pinWidth:_statusNote to:notifyPage inset:40];

  // --- the window ---------------------------------------------------------

  _tabs = [[NSTabView alloc] init];
  _tabs.translatesAutoresizingMaskIntoConstraints = NO;
  [_tabs addTabViewItem:[self tabTitled:@"Devices" content:devicesStack]];
  [_tabs addTabViewItem:[self tabTitled:@"Camera" content:cameraStack]];
  [_tabs addTabViewItem:[self tabTitled:@"Configuration" content:configStack]];
  [_tabs addTabViewItem:[self tabTitled:@"Notifications" content:notifyPage]];
  _tabs.delegate = self;
  {
    // Clamped rather than trusted: the number in the defaults was written by
    // some earlier version of this program, which may have had more tabs.
    const NSInteger want =
        [[NSUserDefaults standardUserDefaults] integerForKey:kPrefTab];
    if (want > 0 && want < (NSInteger)_tabs.numberOfTabViewItems) {
      [_tabs selectTabViewItemAtIndex:want];
    }
  }

  // The two status lines stay above the tabs rather than becoming a page of
  // their own: they are the only thing here worth seeing without choosing to
  // look, and the window's width was picked for them.
  NSStackView* root = [NSStackView stackViewWithViews:@[
    _daemonLine, _benchLine, _tabs,
  ]];
  root.orientation = NSUserInterfaceLayoutOrientationVertical;
  root.alignment = NSLayoutAttributeLeading;
  root.spacing = 12;
  root.edgeInsets = NSEdgeInsetsMake(16, 16, 16, 16);
  root.translatesAutoresizingMaskIntoConstraints = NO;

  // The tab view is the one thing here that should take the window's width; a
  // leading-aligned stack would otherwise leave it as wide as whichever page
  // happens to be showing. The floor is so that the shortest page does not
  // open a window too small to read the others in.
  [[_tabs.widthAnchor constraintEqualToAnchor:root.widthAnchor
                                     constant:-32] setActive:YES];
  [[_tabs.heightAnchor constraintGreaterThanOrEqualToConstant:320]
      setActive:YES];

  _window = [[NSWindow alloc]
      initWithContentRect:NSMakeRect(0, 0, 460, 620)
                styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                           NSWindowStyleMaskResizable)
                  backing:NSBackingStoreBuffered
                    defer:NO];
  _window.title = @"Octomancer";
  _window.releasedWhenClosed = NO;
  _window.contentView = root;
  // The height is whatever the pages actually add up to rather than a number
  // guessed before any of them existed. The width stays as chosen: the two
  // status lines above the tabs are longer than anything inside them, and
  // fitting the window to the pages alone would open it with those truncated.
  NSSize fits = root.fittingSize;
  [_window setContentSize:NSMakeSize(MAX(460, fits.width), fits.height)];
  [_window setContentMinSize:NSMakeSize(MAX(420, fits.width), fits.height)];
  [_window center];

  [self refreshDaemonState];
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
        [NSString stringWithFormat:@"Bench: %d timecode box%s, %@, spread %.0f ms",
                                   _status.bench.boxes,
                                   _status.bench.boxes == 1 ? "" : "es",
                                   offset_text(_status.bench.offset_s),
                                   _status.bench.spread_s * 1000.0];
  } else if (_benchUp && _snapshot.has_bench) {
    _benchLine.stringValue =
        [NSString stringWithFormat:@"Bench: %d timecode box%s, %@", _snapshot.live,
                                   _snapshot.live == 1 ? "" : "es",
                                   offset_text(_snapshot.bench_offset)];
  } else {
    _benchLine.stringValue = @"Bench: nothing heard yet";
  }

  [self rebuildCameraPicker];
  [self updateCameraDetail];
  [self updateDevices];
  [self updateConfiguration];
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

  _writesEnabled.state =
      c->writes_enabled ? NSControlStateValueOn : NSControlStateValueOff;
  _writesEnabled.enabled = _controlUp && !_busy;
  // Nothing acts on a camera that has not been permitted, so offering the
  // buttons would be offering something that will be refused.
  _syncButton.enabled = _syncButton.enabled && c->writes_enabled;
  _sourcePicker.enabled = _sourcePicker.enabled && c->writes_enabled;

  NSString* cycle = c->action.empty() ? @"--" : ns(c->action);
  if (!c->writes_enabled) cycle = @"writes disabled — reading only";
  if (c->recording) cycle = @"RECORDING — the clock will not be touched";
  if (c->has_source && c->source != octo::bmd::kTimecodeSourceTimeOfDay) {
    cycle = @"timecode does not follow the clock — cannot sync";
  }
  _cycleValue.stringValue = cycle;
  _cycleValue.textColor =
      (c->recording || !c->writes_enabled ||
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
              info:@"Start it with `make install-agent`, or with the buttons "
                   @"on the Configuration page."];
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

// Permission is written to the configuration file, not sent over the socket:
// the daemon only ever reads that file. Then it is told to re-read it, which
// it does within a quarter of a second, rather than at the next restart.
- (void)writesToggled:(id)sender {
  (void)sender;
  const octo::CameraStatus* c = [self selectedCamera];
  if (c == nullptr) return;
  const bool wanted = _writesEnabled.state == NSControlStateValueOn;
  const std::string id = c->id;
  const std::string name = c->name;
  const std::string configPath = _status.daemon.config_path.empty()
                                     ? octo::default_camera_config_path()
                                     : _status.daemon.config_path;
  const std::string socketPath = _controlSocket;

  dispatch_async(_queue, ^{
    octo::CamConf conf;
    std::string err;
    bool ok = conf.load(configPath, &err);
    if (ok) ok = conf.set_writes(id, name, wanted, &err);

    if (ok) {
      std::string reply, ignored;
      octo::query(socketPath, "reload", &reply, &ignored, 3.0);
    }
    dispatch_async(dispatch_get_main_queue(), ^{
      if (!ok) {
        [self complain:@"Could not save that setting."
                  info:[NSString stringWithFormat:@"%@\n\n%@", ns(err),
                                                  ns(configPath)]];
        // Put the checkbox back where the daemon says it is.
        [self updateCameraDetail];
        return;
      }
      self->_activity.stringValue =
          wanted ? @"Writes enabled for this camera."
                 : @"Writes disabled — octomancer will read it and not touch it.";
      self->_activity.textColor = [NSColor secondaryLabelColor];
      [self refresh];
    });
  });
}

- (void)notifyToggled:(id)sender {
  NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
  if (sender == _notifyFailed) {
    [defaults setBool:_notifyFailed.state == NSControlStateValueOn
               forKey:kPrefSyncFailed];
  } else if (sender == _notifyFirst) {
    [defaults setBool:_notifyFirst.state == NSControlStateValueOn
               forKey:kPrefFirstSync];
  } else if (sender == _notifyBenchDisagree) {
    [defaults setBool:_notifyBenchDisagree.state == NSControlStateValueOn
               forKey:kPrefBenchDisagree];
  } else if (sender == _notifyBenchDrift) {
    [defaults setBool:_notifyBenchDrift.state == NSControlStateValueOn
               forKey:kPrefBenchDrift];
  } else if (sender == _notifyLost) {
    [defaults setBool:_notifyLost.state == NSControlStateValueOn
               forKey:kPrefCameraLost];
  }
}

// ------------------------------------------------------------ start at boot

// All of the launchd handling lives in src/agents.cc, which `octomancer` uses
// too. Doing it twice -- once in C++ and once here in Objective-C -- is how the
// two would end up disagreeing about which labels exist.
// Off the main thread, because agent_state runs /bin/launchctl and a window
// that stops redrawing while it waits for a fork is a window that looks
// crashed. The answer therefore arrives a moment after it was asked for, which
// nothing here minds: it is polled on a timer anyway.
- (void)refreshDaemonState {
  dispatch_async(_queue, ^{
    const octo::AgentState bench = octo::agent_state(octo::Agent::kBench);
    const octo::AgentState sync = octo::agent_state(octo::Agent::kSync);
    const double now = octo::wall_now();
    dispatch_async(dispatch_get_main_queue(), ^{
      [self showAgent:bench in:self->_benchAgentLine now:now];
      [self showAgent:sync in:self->_syncAgentLine now:now];
      // Both, or the checkbox would claim a half-installed pair is installed.
      self->_startAtBoot.state = (bench.installed && sync.installed)
                                     ? NSControlStateValueOn
                                     : NSControlStateValueOff;
    });
  });
}

- (void)showAgent:(const octo::AgentState&)state
               in:(NSTextField*)field
              now:(double)now {
  if (field == nil) return;
  if (state.running) {
    // How long it has been up comes from the kernel rather than from the
    // daemon's own socket, so it is still there for a daemon that is running
    // and has stopped answering -- which is exactly the daemon somebody is
    // about to restart.
    NSString* up =
        state.has_started
            ? [NSString stringWithFormat:@", up %@",
                                         elapsed_text(now - state.started_wall)]
            : @"";
    field.stringValue =
        [NSString stringWithFormat:@"running (%d)%@", state.pid, up];
  } else if (state.loaded) {
    field.stringValue = @"loaded, not running";
  } else if (state.installed) {
    field.stringValue = @"stopped";
  } else {
    field.stringValue = @"not installed";
  }
}

// launchctl can take a moment, and a frozen window looks like a crash.
- (void)runAgentAction:(NSString*)verb {
  NSString* doing =
      [NSString stringWithFormat:@"%@ing…", [verb capitalizedString]];
  _benchAgentLine.stringValue = doing;
  _syncAgentLine.stringValue = doing;
  const std::string what = cpp(verb);

  dispatch_async(_queue, ^{
    std::string failure;
    for (const octo::Agent a : {octo::Agent::kBench, octo::Agent::kSync}) {
      std::string err;
      bool ok = false;
      if (what == "start") {
        ok = octo::agent_start(a, &err);
      } else if (what == "stop") {
        ok = octo::agent_stop(a, &err);
      } else {
        ok = octo::agent_restart(a, &err);
      }
      if (!ok && failure.empty()) failure = err;
    }
    dispatch_async(dispatch_get_main_queue(), ^{
      if (!failure.empty()) {
        [self complain:[NSString stringWithFormat:@"Could not %@ the daemons.",
                                                  verb]
                  info:ns(failure)];
      }
      [self refreshDaemonState];
      [self refresh];
    });
  });
}

- (void)startDaemons:(id)sender {
  (void)sender;
  [self runAgentAction:@"start"];
}

- (void)stopDaemons:(id)sender {
  (void)sender;
  [self runAgentAction:@"stop"];
}

- (void)restartDaemons:(id)sender {
  (void)sender;
  [self runAgentAction:@"restart"];
}

- (void)startAtBootToggled:(id)sender {
  (void)sender;
  const BOOL wanted = _startAtBoot.state == NSControlStateValueOn;

  dispatch_async(_queue, ^{
    std::string failure;
    for (const octo::Agent a : {octo::Agent::kBench, octo::Agent::kSync}) {
      std::string err;
      const bool ok = wanted ? octo::agent_install(a, &err)
                             : octo::agent_uninstall(a, &err);
      if (!ok && failure.empty()) failure = err;
    }
    dispatch_async(dispatch_get_main_queue(), ^{
      if (!failure.empty()) {
        [self complain:@"Could not change what starts at boot."
                  info:ns(failure)];
      } else {
        self->_bootNote.stringValue =
            wanted ? @"Both daemons will start when you log in."
                   : @"Neither daemon will start on its own. Syncing stops "
                     @"until you start them again.";
      }
      [self refreshDaemonState];
      [self refresh];
    });
  });
}

// ------------------------------------------------------------------ devices

// The Devices page.
//
// The merge itself belongs to src/devices.h -- which daemon heard what, which
// boxes get a vote on the canonical time, and what an offset is measured
// against. All that happens here is turning its answer into labels, and the
// labels are what is kept: the view is rebuilt only when the list of devices
// changes, so a two-second tick moves the numbers rather than the window.
// The merged list, built from whatever the last poll came back with.
//
// Two callers want it -- this page and the menu-bar blip -- and the menu-bar
// item exists whether or not the window has ever been opened, so it cannot be
// a side effect of drawing the page. Cheap enough to build twice a tick: it is
// arithmetic over two structures already in memory, with no socket and no file
// anywhere in it.
- (octo::DeviceView)deviceView {
  octo::DeviceSources src;
  src.bench = _benchUp ? &_snapshot : nullptr;
  src.cameras = _controlUp ? &_status : nullptr;
  // A configuration that could not be read is not a configuration that says
  // no. Null means "everything is enabled", which shows every device rather
  // than an empty page that explains nothing. It also means nothing warns,
  // which is the right way round: a file we could not read is not permission
  // to light something up on somebody's behalf.
  src.conf = _confLoaded ? &_conf : nullptr;
  return octo::build_device_view(src);
}

- (void)updateDevices {
  if (_deviceGrid == nil) return;

  const octo::DeviceView view = [self deviceView];

  if (view.has_canonical) {
    _canonicalLine.stringValue =
        [NSString stringWithFormat:
                      @"Canonical time: %d timecode box%s on the air, via %s, "
                      @"%@ from this Mac, spread %.0f ms",
                      view.contributing, view.contributing == 1 ? "" : "es",
                      view.canonical_source.c_str(),
                      offset_text(view.canonical_offset_s),
                      view.canonical_spread_s * 1000.0];
    _canonicalLine.textColor = [NSColor labelColor];
  } else {
    _canonicalLine.stringValue =
        @"No canonical time: nothing is voting on one, so there is nothing "
        @"for the offsets to be measured against.";
    _canonicalLine.textColor = [NSColor systemOrangeColor];
  }

  NSMutableArray<NSString*>* keys = [NSMutableArray array];
  for (const octo::DeviceRow& r : view.rows) {
    [keys addObject:ns((r.kind == octo::DeviceKind::kCamera ? "c:" : "t:") +
                       r.id)];
  }
  if (![keys isEqualToArray:_deviceKeys]) {
    [self rebuildDeviceGrid:keys];
    _deviceKeys = [keys copy];
  }

  for (size_t i = 0; i < view.rows.size(); ++i) {
    const octo::DeviceRow& r = view.rows[i];
    NSArray<NSTextField*>* cells = _deviceCells[_deviceKeys[i]];
    if (cells == nil) continue;
    // A row we are not hearing from is dimmed rather than hidden or coloured:
    // it is still a device somebody owns, and its last known numbers are worth
    // reading -- they are just no longer news.
    const bool live = octo::link_is_live(r.link);
    NSColor* ink = live ? [NSColor labelColor] : [NSColor secondaryLabelColor];
    NSColor* faint = live ? [NSColor secondaryLabelColor]
                          : [NSColor tertiaryLabelColor];

    // A warning is drawn on the name, in the same red and yellow as the
    // menu-bar blip and with the same `!` and `?` the terminal uses. The
    // marker is there as well as the colour because the blip that sent
    // somebody to this page only carried a colour, and a row that answers
    // "which device, and which of the two things" in text answers it for
    // whoever cannot tell the two colours apart.
    NSString* mark = @"";
    NSColor* named = ink;
    if (r.warn_level == octo::WarnLevel::kOutOfSync) {
      mark = @" !";
      named = [NSColor systemRedColor];
    } else if (r.warn_level == octo::WarnLevel::kUnsure) {
      mark = @" ?";
      named = [NSColor systemYellowColor];
    }
    cells[0].stringValue = [ns(r.name) stringByAppendingString:mark];
    cells[0].textColor = named;

    // A held link ages from nothing: the camera stopped advertising because we
    // are connected to it, so "now" is the honest word.
    NSString* seen = @"--";
    if (r.has_age) {
      seen = r.age_s < 1.0
                 ? @"now"
                 : [elapsed_text(r.age_s) stringByAppendingString:@" ago"];
    }
    cells[1].stringValue = seen;
    cells[1].textColor = faint;

    cells[2].stringValue = r.has_offset ? offset_text(r.offset_s) : @"--";
    cells[2].textColor = r.alerting ? [NSColor systemOrangeColor] : ink;

    // "held" against "off the air" is the distinction this column exists for.
    // Drawn the same way, they would have somebody power-cycling a camera that
    // is being talked to at that moment.
    NSString* said = @(octo::link_state_name(r.link));
    if (!r.note.empty()) {
      said = [NSString stringWithFormat:@"%@ — %@", said, ns(r.note)];
    }
    cells[3].stringValue = said;
    cells[3].textColor = faint;
  }

  if (view.rows.empty()) {
    _hiddenLine.stringValue =
        (_benchUp || _controlUp)
            ? @"Nothing to show yet. A timecode box has to be advertising, or a "
              @"camera enabled in Configuration, before it appears here."
            : @"Neither daemon is answering, so nothing here knows anything.";
  } else {
    // Counted rather than dropped silently: a bench that quietly lists fewer
    // boxes than are in the room is not an honest bench, and neither is one
    // that counts four boxes in the header while showing six rows without
    // saying which two did not count.
    NSMutableArray<NSString*>* notes = [NSMutableArray array];
    if (view.has_canonical && view.silent > 0) {
      [notes addObject:[NSString stringWithFormat:
                            @"%d timecode box%s off the air, so not voting on "
                            @"the canonical time and not in the spread.",
                            view.silent, view.silent == 1 ? "" : "es"]];
    }
    if (view.hidden > 0) {
      [notes addObject:[NSString stringWithFormat:
                            @"%d device%s switched off and left out of this "
                            @"list. Configuration has them.",
                            view.hidden, view.hidden == 1 ? "" : "s"]];
    }
    _hiddenLine.stringValue = [notes componentsJoinedByString:@" "];
  }
}

// Views are made here and nowhere else. A device that comes and goes keeps its
// row across the gap, so the page does not flinch every time a camera drops
// off the air and comes back.
- (void)rebuildDeviceGrid:(NSArray<NSString*>*)keys {
  while (_deviceGrid.numberOfRows > 0) [_deviceGrid removeRowAtIndex:0];

  NSArray<NSTextField*>* header = @[
    dim_label(@"Device"), dim_label(@"Last seen"),
    dim_label(@"From canonical"), dim_label(@"Link"),
  ];
  for (NSTextField* f in header) {
    f.font = [NSFont systemFontOfSize:NSFont.smallSystemFontSize];
  }
  [_deviceGrid addRowWithViews:header];

  for (NSString* key in keys) {
    NSArray<NSTextField*>* cells = _deviceCells[key];
    if (cells == nil) {
      cells = @[ label(@""), mono_label(@""), mono_label(@""), label(@"") ];
      _deviceCells[key] = cells;
    }
    [_deviceGrid addRowWithViews:cells];
  }

  // Rows for devices that are not merely off the air but gone -- from both
  // daemons and from the configuration file as well.
  NSMutableArray<NSString*>* stale = [NSMutableArray array];
  for (NSString* key in _deviceCells) {
    if (![keys containsObject:key]) [stale addObject:key];
  }
  [_deviceCells removeObjectsForKeys:stale];
}

// ------------------------------------------------------------ configuration

// Every device this Mac knows of, whether or not anything is hearing it.
//
// The Devices page shows what is working; this one shows what exists. They are
// different lists on purpose: a box somebody switched off has stopped
// appearing anywhere else, and the only way back is a checkbox with its name
// on it. So this is built from the two daemons *and* from the configuration
// file, and the file is what supplies the rows neither daemon can see.
- (void)updateConfiguration {
  if (_configGrid == nil) return;

  std::vector<ConfigEntry> entries;
  std::set<std::string> known;

  if (_benchUp) {
    for (const octo::DeviceSnapshot& d : _snapshot.device) {
      ConfigEntry e;
      e.camera = false;
      e.id = d.id;
      e.name = d.name.empty() ? d.id : d.name;
      e.present = d.live;
      e.enabled = _confLoaded ? _conf.box_enabled(d.id) : true;
      e.warn = _confLoaded && _conf.warn_enabled(d.id);
      entries.push_back(e);
      known.insert("t:" + d.id);
    }
  }
  if (_confLoaded) {
    for (const octo::BoxConfig& b : _conf.boxes()) {
      if (known.count("t:" + b.id)) continue;
      ConfigEntry e;
      e.camera = false;
      e.id = b.id;
      e.name = b.name.empty() ? b.id : b.name;
      e.enabled = b.enabled;
      e.warn = b.warn;
      entries.push_back(e);
      known.insert("t:" + b.id);
    }
  }
  if (_controlUp) {
    for (const octo::CameraStatus& c : _status.cameras) {
      ConfigEntry e;
      e.camera = true;
      e.id = c.id;
      e.name = c.name.empty() ? c.id : c.name;
      // Held counts as present: a camera we are connected to has stopped
      // advertising precisely because we are connected to it.
      e.present = c.present || c.connected;
      e.enabled = _confLoaded ? _conf.writes_enabled(c.id) : c.writes_enabled;
      e.warn = _confLoaded && _conf.warn_enabled(c.id);
      entries.push_back(e);
      known.insert("c:" + c.id);
    }
  }
  if (_confLoaded) {
    for (const octo::CameraConfig& c : _conf.cameras()) {
      if (known.count("c:" + c.id)) continue;
      ConfigEntry e;
      e.camera = true;
      e.id = c.id;
      e.name = c.name.empty() ? c.id : c.name;
      e.enabled = c.writes_enabled;
      e.warn = c.warn;
      entries.push_back(e);
      known.insert("c:" + c.id);
    }
  }

  _configEntries = entries;

  NSMutableArray<NSString*>* keys = [NSMutableArray array];
  for (const ConfigEntry& e : entries) {
    [keys addObject:ns((e.camera ? "c:" : "t:") + e.id)];
  }
  if (![keys isEqualToArray:_configKeys]) {
    [self rebuildConfigGrid:keys];
    _configKeys = [keys copy];
  }

  for (size_t i = 0; i < entries.size(); ++i) {
    const ConfigEntry& e = entries[i];
    NSArray<NSView*>* cells = _configCells[_configKeys[i]];
    if (cells == nil) continue;
    NSButton* box = (NSButton*)cells[0];
    NSButton* warn = (NSButton*)cells[1];
    NSTextField* name = (NSTextField*)cells[2];
    NSTextField* who = (NSTextField*)cells[3];
    // The tag is how a click finds its way back to a device: the checkbox
    // itself knows nothing, and the list it indexes into is rebuilt in the
    // same order every tick.
    box.tag = static_cast<NSInteger>(i);
    warn.tag = static_cast<NSInteger>(i);
    box.state = e.enabled ? NSControlStateValueOn : NSControlStateValueOff;
    warn.state = e.warn ? NSControlStateValueOn : NSControlStateValueOff;
    // A switched-off device is left out of the merged view entirely, so its
    // warning could never fire -- see build_device_view. Greyed rather than
    // cleared: the setting is still in the file and comes back the moment the
    // device is switched on again, and silently unticking somebody's box to
    // represent "this has no effect right now" would lose what they asked for.
    warn.enabled = e.enabled;
    name.stringValue = ns(e.name);
    name.textColor = e.present ? [NSColor labelColor]
                               : [NSColor secondaryLabelColor];
    // Eight characters of the id: enough to tell two bodies of the same model
    // apart, short enough not to own the window.
    who.stringValue =
        [NSString stringWithFormat:@"%@ %s", ns(e.id.substr(0, 8)),
                                   e.camera ? "camera" : "timecode box"];
  }

  if (entries.empty()) {
    _configNote.stringValue =
        @"No devices known yet. They appear here as soon as either daemon "
        @"hears one.";
  } else if (_confLoaded) {
    _configNote.stringValue =
        ns("Saved in " + _conf.path() +
           ". A camera has to be enabled before anything will write to it; a "
           "box is heard unless it is switched off here. Warn puts a blip in "
           "the menu bar -- red when that device is out of sync, yellow when "
           "it has been quiet too long to say -- and it is off everywhere "
           "until asked for, so that the light still means something on the "
           "day it comes on.");
  } else {
    _configNote.stringValue =
        @"The configuration file could not be read, so everything is shown as "
        @"enabled and a change here will not stick.";
  }
}

- (void)rebuildConfigGrid:(NSArray<NSString*>*)keys {
  while (_configGrid.numberOfRows > 0) [_configGrid removeRowAtIndex:0];

  NSArray<NSTextField*>* header = @[
    dim_label(@"On"), dim_label(@"Warn if out of sync"), dim_label(@"Device"),
    dim_label(@""),
  ];
  for (NSTextField* f in header) {
    f.font = [NSFont systemFontOfSize:NSFont.smallSystemFontSize];
  }
  // The first column's heading has to cover two different things -- a camera
  // may be written to, a timecode box is listened to -- and no single word
  // covers both without lying about one of them. "On" is the word people use
  // for a checkbox anyway, and the note under the grid says what it means for
  // each kind.
  header[0].toolTip = @"Cameras: may octomancer set this one's clock. "
                      @"Timecode boxes: is this one listened to at all.";
  header[1].toolTip =
      @"Show a blip in the menu bar when this device is too far from the "
      @"bench, or when it has not been heard from recently enough to say.";
  [_configGrid addRowWithViews:header];

  for (NSString* key in keys) {
    NSArray<NSView*>* cells = _configCells[key];
    if (cells == nil) {
      SEL onOff = @selector(deviceEnabledToggled:);
      SEL warned = @selector(deviceWarnToggled:);
      NSButton* on = [NSButton checkboxWithTitle:@"" target:self action:onOff];
      NSButton* warn = [NSButton checkboxWithTitle:@""
                                            target:self
                                            action:warned];
      on.toolTip = header[0].toolTip;
      warn.toolTip = header[1].toolTip;
      cells = @[ on, warn, label(@""), dim_label(@"") ];
      _configCells[key] = cells;
    }
    [_configGrid addRowWithViews:cells];
  }

  NSMutableArray<NSString*>* stale = [NSMutableArray array];
  for (NSString* key in _configCells) {
    if (![keys containsObject:key]) [stale addObject:key];
  }
  [_configCells removeObjectsForKeys:stale];
}

// A device switched on or off, or asked to start warning.
//
// Written to the configuration file rather than sent over a socket, because
// that file is the one a person owns and the daemons only ever read it -- see
// camconf.h. The reload afterwards is what turns a saved line into behaviour
// inside a quarter of a second rather than at the next restart.
//
// Both checkboxes come here. They set different keys on the same line and are
// otherwise the same job -- write, complain and snap back if it did not take,
// refresh -- and two copies of that would be two chances to get the
// snapping-back wrong on one of them.
- (void)deviceEnabledToggled:(id)sender {
  [self saveDeviceFlag:(NSButton*)sender warning:NO];
}

- (void)deviceWarnToggled:(id)sender {
  [self saveDeviceFlag:(NSButton*)sender warning:YES];
}

- (void)saveDeviceFlag:(NSButton*)box warning:(BOOL)warning {
  const NSInteger which = box.tag;
  if (which < 0 || static_cast<size_t>(which) >= _configEntries.size()) return;
  const ConfigEntry entry = _configEntries[static_cast<size_t>(which)];
  const bool wanted = box.state == NSControlStateValueOn;
  const bool isWarn = warning == YES;
  const std::string configPath = _status.daemon.config_path.empty()
                                     ? octo::default_camera_config_path()
                                     : _status.daemon.config_path;
  const std::string socketPath = _controlSocket;

  dispatch_async(_queue, ^{
    octo::CamConf conf;
    std::string err;
    bool ok = conf.load(configPath, &err);
    if (ok) {
      if (isWarn) {
        ok = entry.camera
                 ? conf.set_camera_warn(entry.id, entry.name, wanted, &err)
                 : conf.set_box_warn(entry.id, entry.name, wanted, &err);
      } else {
        ok = entry.camera ? conf.set_writes(entry.id, entry.name, wanted, &err)
                          : conf.set_box_enabled(entry.id, entry.name, wanted,
                                                 &err);
      }
    }
    // Only octomancer-sync reads this file, and only its camera permissions
    // change what it does: switching a box off is a decision about what gets
    // counted and drawn here, and a warning is a decision about this window
    // alone -- no daemon has ever read that key. Telling it anyway would break
    // its poll for nothing.
    if (ok && entry.camera && !isWarn) {
      std::string reply, ignored;
      octo::query(socketPath, "reload", &reply, &ignored, 3.0);
    }
    dispatch_async(dispatch_get_main_queue(), ^{
      if (!ok) {
        [self complain:@"Could not save that setting."
                  info:[NSString stringWithFormat:@"%@\n\n%@", ns(err),
                                                  ns(configPath)]];
        // Nothing was written, so nothing changed: put it back.
        box.state = wanted ? NSControlStateValueOff : NSControlStateValueOn;
        return;
      }
      [self refresh];
    });
  });
}

// ------------------------------------------------------------------ pairing

// Pairing is the one thing in this window that is not a request to a daemon.
//
// There is no CoreBluetooth call that means "bond with this peripheral". macOS
// negotiates encryption only when something touches a characteristic that
// insists on it, and then it puts up its own passkey dialog -- so the six
// digits on the camera's screen are answered to the system, not to us. The
// binary with the radio in it does the work, exactly as `octomancer pair`
// does, and this sheet is a window around its output rather than a second
// implementation of it. src/pairing.h is where the outcomes are told apart,
// and it only runs inside that binary.
- (void)openPairSheet:(id)sender {
  (void)sender;
  [self ensureWindow];
  if (_window == nil) return;
  if (!_window.isVisible) [self showWindow:nil];
  if (_pairSheet == nil) _pairSheet = [self makePairSheet];

  _pairFound.clear();
  [_pairPicker removeAllItems];
  [_pairPicker addItemWithTitle:@"Nothing found yet"];
  _pairPicker.enabled = NO;
  _pairButton.enabled = NO;
  _pairActivity.stringValue = @"";
  _pairPending = [NSMutableString string];
  [_pairLog.textStorage
      setAttributedString:[[NSAttributedString alloc] initWithString:@""]];

  [_window beginSheet:_pairSheet
    completionHandler:^(NSModalResponse response) {
      (void)response;
    }];
  [self checkSyncDaemonForPairing];
}

- (NSWindow*)makePairSheet {
  _pairWarning = wrapped_label(@"");
  _pairWarning.textColor = [NSColor systemOrangeColor];
  _pairWarning.hidden = YES;
  _pairStopDaemonButton =
      [NSButton buttonWithTitle:@"Stop the sync daemon"
                         target:self
                         action:@selector(pairStopDaemon:)];
  _pairStopDaemonButton.hidden = YES;

  NSTextField* explain = wrapped_label(
      @"Searching takes about twenty seconds. When pairing starts, the camera "
      @"puts six digits on its own screen and macOS asks for them — the code "
      @"is on the camera, not here. What the tool says as it goes is shown "
      @"below, verdict and all.");

  _pairSearchButton = [NSButton buttonWithTitle:@"Search for Cameras"
                                         target:self
                                         action:@selector(pairSearch:)];
  _pairSpinner = [[NSProgressIndicator alloc] init];
  _pairSpinner.style = NSProgressIndicatorStyleSpinning;
  _pairSpinner.controlSize = NSControlSizeSmall;
  _pairSpinner.displayedWhenStopped = NO;
  _pairActivity = dim_label(@"");

  NSStackView* searchRow = [NSStackView
      stackViewWithViews:@[ _pairSearchButton, _pairSpinner, _pairActivity ]];
  searchRow.orientation = NSUserInterfaceLayoutOrientationHorizontal;
  searchRow.spacing = 10;
  searchRow.alignment = NSLayoutAttributeCenterY;

  _pairPicker = [[NSPopUpButton alloc] init];
  [_pairPicker addItemWithTitle:@"Nothing found yet"];
  _pairPicker.enabled = NO;

  // The old springs-and-struts recipe inside, autolayout outside: an NSTextView
  // wants to size itself against its clip view the way it always has, and the
  // scroll view is what the surrounding stack is given.
  _pairScroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 0, 480, 180)];
  _pairScroll.hasVerticalScroller = YES;
  _pairScroll.borderType = NSBezelBorder;
  _pairScroll.translatesAutoresizingMaskIntoConstraints = NO;
  _pairLog = [[NSTextView alloc] initWithFrame:_pairScroll.contentView.bounds];
  _pairLog.editable = NO;
  _pairLog.font = [NSFont userFixedPitchFontOfSize:NSFont.smallSystemFontSize];
  _pairLog.minSize = NSMakeSize(0, 0);
  _pairLog.maxSize = NSMakeSize(FLT_MAX, FLT_MAX);
  _pairLog.verticallyResizable = YES;
  _pairLog.horizontallyResizable = NO;
  _pairLog.autoresizingMask = NSViewWidthSizable;
  _pairLog.textContainer.widthTracksTextView = YES;
  _pairLog.textContainer.containerSize =
      NSMakeSize(_pairScroll.contentSize.width, FLT_MAX);
  _pairScroll.documentView = _pairLog;
  [[_pairScroll.heightAnchor constraintEqualToConstant:180] setActive:YES];
  [[_pairScroll.widthAnchor constraintEqualToConstant:480] setActive:YES];

  _pairButton = [NSButton buttonWithTitle:@"Pair"
                                   target:self
                                   action:@selector(pairStart:)];
  _pairButton.keyEquivalent = @"\r";
  _pairButton.enabled = NO;
  NSButton* close = [NSButton buttonWithTitle:@"Close"
                                       target:self
                                       action:@selector(closePairSheet:)];
  close.keyEquivalent = @"\033";  // escape, and it kills the child on the way

  NSStackView* buttons =
      [NSStackView stackViewWithViews:@[ close, _pairButton ]];
  buttons.orientation = NSUserInterfaceLayoutOrientationHorizontal;
  buttons.spacing = 12;

  NSStackView* stack = [NSStackView stackViewWithViews:@[
    heading(@"Pair a camera"), _pairWarning, _pairStopDaemonButton, explain,
    searchRow, _pairPicker, _pairScroll, buttons,
  ]];
  stack.orientation = NSUserInterfaceLayoutOrientationVertical;
  stack.alignment = NSLayoutAttributeLeading;
  stack.spacing = 10;
  stack.edgeInsets = NSEdgeInsetsMake(20, 20, 20, 20);
  stack.translatesAutoresizingMaskIntoConstraints = NO;
  [self pinWidth:explain to:stack inset:40];
  [self pinWidth:_pairWarning to:stack inset:40];

  NSWindow* sheet =
      [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 520, 540)
                                  styleMask:NSWindowStyleMaskTitled
                                    backing:NSBackingStoreBuffered
                                      defer:NO];
  sheet.contentView = stack;
  NSSize fits = stack.fittingSize;
  [sheet setContentSize:NSMakeSize(MAX(520, fits.width), fits.height)];
  return sheet;
}

// The warning `octomancer pair` prints, said in a window.
//
// The sync daemon connects to a camera whenever it notices one, and a BLE
// peripheral takes a single connection at a time. Pairing alongside it usually
// works, because it spends most of its life not connected -- but when it does
// not work it fails as "could not connect", which is the least informative
// outcome there is and the easiest to read as the wrong thing.
- (void)checkSyncDaemonForPairing {
  dispatch_async(_queue, ^{
    const octo::AgentState state = octo::agent_state(octo::Agent::kSync);
    dispatch_async(dispatch_get_main_queue(), ^{
      const BOOL holding = state.running ? YES : NO;
      self->_pairWarning.hidden = !holding;
      self->_pairStopDaemonButton.hidden = !holding;
      if (!holding) return;
      self->_pairWarning.stringValue = [NSString
          stringWithFormat:
              @"%s is running (pid %d) and connects to the camera on its own. "
              @"A camera takes one connection at a time, so this may fail as "
              @"\"could not connect\", which is the least informative way it "
              @"can fail. Stopping it first is the reliable order — it has to "
              @"be started again afterwards, from the Configuration page.",
              octo::agent_program(octo::Agent::kSync), state.pid];
    });
  });
}

- (void)pairStopDaemon:(id)sender {
  (void)sender;
  _pairStopDaemonButton.enabled = NO;
  dispatch_async(_queue, ^{
    std::string err;
    const bool ok = octo::agent_stop(octo::Agent::kSync, &err);
    dispatch_async(dispatch_get_main_queue(), ^{
      self->_pairStopDaemonButton.enabled = YES;
      if (!ok) {
        [self complain:@"Could not stop the sync daemon." info:ns(err)];
        return;
      }
      [self pairAppendText:@"stopped the sync daemon -- nothing is syncing "
                           @"until it is started again.\n"];
      [self checkSyncDaemonForPairing];
      [self refreshDaemonState];
    });
  });
}

- (void)pairSearch:(id)sender {
  (void)sender;
  // --log '' because the default writes a JSONL file into whatever directory
  // this process happens to be running in, and a scan is a question rather
  // than a session worth leaving a file behind for.
  [self runPairTool:@[ @"--scan-only", @"--log", @"" ]
         describing:@"Searching — about twenty seconds…"];
}

- (void)pairStart:(id)sender {
  (void)sender;
  const NSInteger index = _pairPicker.indexOfSelectedItem;
  if (index < 0 || static_cast<size_t>(index) >= _pairFound.size()) return;
  const std::string picked = _pairFound[static_cast<size_t>(index)].id;
  [self runPairTool:@[ @"--pair", @"--camera", ns(picked), @"--log", @"" ]
         describing:@"Pairing — watch the camera's screen…"];
}

// Run the binary that has the radio, and stream what it says into the sheet.
//
// One child at a time, and it is a child rather than a replaced process only
// because there is a window to keep drawing; `octomancer pair` execs it for
// the opposite reason.
- (void)runPairTool:(NSArray<NSString*>*)args describing:(NSString*)what {
  if (_pairTask != nil) return;
  const std::string program = octo::sibling_program_path("octomancer-sync");

  NSTask* task = [[NSTask alloc] init];
  task.executableURL = [NSURL fileURLWithPath:ns(program)];
  task.arguments = args;
  // One stream for both: the tool's running commentary and its complaints are
  // equally worth reading, and interleaving them is how they were written.
  NSPipe* pipe = [NSPipe pipe];
  task.standardOutput = pipe;
  task.standardError = pipe;
  pipe.fileHandleForReading.readabilityHandler = ^(NSFileHandle* handle) {
    NSData* data = handle.availableData;
    if (data.length == 0) {
      // End of file. Left installed it would spin on the closed pipe.
      handle.readabilityHandler = nil;
      return;
    }
    dispatch_async(dispatch_get_main_queue(), ^{
      [self pairAppendData:data];
    });
  };
  task.terminationHandler = ^(NSTask* finished) {
    const int status = static_cast<int>(finished.terminationStatus);
    dispatch_async(dispatch_get_main_queue(), ^{
      [self pairFinished:status];
    });
  };

  NSError* err = nil;
  if (![task launchAndReturnError:&err]) {
    pipe.fileHandleForReading.readabilityHandler = nil;
    [self complain:@"Could not run octomancer-sync."
              info:[NSString stringWithFormat:@"%@\n\n%@", ns(program),
                                              err.localizedDescription ?: @""]];
    return;
  }
  _pairTask = task;
  _pairSearchButton.enabled = NO;
  _pairButton.enabled = NO;
  _pairPicker.enabled = NO;
  _pairActivity.stringValue = what;
  [_pairSpinner startAnimation:nil];
  [self pairAppendText:ns("$ " + program + " " +
                          cpp([args componentsJoinedByString:@" "]) + "\n")];
}

- (void)pairFinished:(int)status {
  NSTask* task = _pairTask;
  _pairTask = nil;
  if (task != nil) {
    NSPipe* pipe = (NSPipe*)task.standardOutput;
    pipe.fileHandleForReading.readabilityHandler = nil;
    // The child is gone but the pipe is not empty: what it wrote last is
    // usually the verdict, which is the one line anybody wanted.
    NSData* rest = [pipe.fileHandleForReading availableData];
    if (rest.length > 0) [self pairAppendData:rest];
  }
  if (_pairPending.length > 0) {
    [self pairHandleLine:[_pairPending copy]];
    _pairPending = [NSMutableString string];
  }

  [_pairSpinner stopAnimation:nil];
  _pairSearchButton.enabled = YES;
  _pairPicker.enabled = !_pairFound.empty();
  _pairButton.enabled = !_pairFound.empty();
  _pairActivity.stringValue =
      status == 0 ? @"Done."
                  : @"It did not finish cleanly — the last lines say why.";
}

// Line assembly happens here, on the main thread, rather than in the pipe's
// handler: one place that owns the buffer is one fewer thing to get wrong, and
// a read can land in the middle of a line.
- (void)pairAppendData:(NSData*)data {
  NSString* chunk = [[NSString alloc] initWithData:data
                                          encoding:NSUTF8StringEncoding];
  if (chunk == nil) return;
  [_pairPending appendString:chunk];
  for (;;) {
    const NSRange nl = [_pairPending rangeOfString:@"\n"];
    if (nl.location == NSNotFound) break;
    NSString* line = [_pairPending substringToIndex:nl.location];
    [_pairPending deleteCharactersInRange:NSMakeRange(0, nl.location + 1)];
    [self pairHandleLine:line];
  }
}

- (void)pairHandleLine:(NSString*)line {
  std::string found, name;
  if (parse_scan_row(cpp(line), &found, &name)) {
    [self pairFound:found named:name];
  }
  // Shown either way. The lines that do not parse are the ones that explain
  // why nothing was found, and dropping them would leave an empty picker and
  // no reason for it.
  [self pairAppendText:[line stringByAppendingString:@"\n"]];
}

- (void)pairFound:(const std::string&)deviceId named:(const std::string&)name {
  for (const ScanHit& hit : _pairFound) {
    if (hit.id == deviceId) return;
  }
  ScanHit hit;
  hit.id = deviceId;
  hit.name = name;
  _pairFound.push_back(hit);

  if (_pairFound.size() == 1) [_pairPicker removeAllItems];
  [_pairPicker addItemWithTitle:
      ns(name.empty() ? deviceId : name + "  " + deviceId.substr(0, 8))];
  _pairPicker.enabled = YES;
  // Still enabled only when nothing is running: the scan that found this is
  // very probably still going.
  _pairButton.enabled = _pairTask == nil;
}

- (void)pairAppendText:(NSString*)text {
  if (_pairLog == nil) return;
  NSDictionary* attributes = @{
    NSFontAttributeName : _pairLog.font ?: [NSFont
        userFixedPitchFontOfSize:NSFont.smallSystemFontSize],
    NSForegroundColorAttributeName : [NSColor labelColor],
  };
  NSAttributedString* piece =
      [[NSAttributedString alloc] initWithString:text attributes:attributes];
  [_pairLog.textStorage appendAttributedString:piece];
  [_pairLog scrollRangeToVisible:NSMakeRange(_pairLog.string.length, 0)];
}

- (void)closePairSheet:(id)sender {
  (void)sender;
  [self stopPairTask];
  if (_pairSheet != nil) [_window endSheet:_pairSheet];
}

// Nothing may outlive the sheet. A scan holds the radio for twenty seconds and
// a pairing attempt holds it for ninety, and either one left running would
// take the connection away from the daemon that was about to be started again.
- (void)stopPairTask {
  NSTask* task = _pairTask;
  _pairTask = nil;
  if (task == nil) return;
  task.terminationHandler = nil;
  NSPipe* pipe = (NSPipe*)task.standardOutput;
  pipe.fileHandleForReading.readabilityHandler = nil;
  if (task.isRunning) [task terminate];
  [_pairSpinner stopAnimation:nil];
  _pairSearchButton.enabled = YES;
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

// The other way out: stop the things that are actually doing the work.
//
// Confirmed first, and worth confirming: somebody may be shooting, and this is
// the switch that stops their cameras' clocks from being corrected. The alert
// says what stopping does and what it does not -- octo::agent_stop is
// `launchctl bootout`, which unloads the agent but leaves the plist in
// ~/Library/LaunchAgents, so an installed agent is back at the next login.
// Taking the plist away is `agent_uninstall`, that is a different decision, and
// the "Start at boot" checkbox on the Configuration page is where it is made.
//
// The daemons go down before this process does. Quitting first and stopping
// afterwards would leave nothing on screen to say that a daemon refused.
- (void)stopEverything:(id)sender {
  (void)sender;

  NSAlert* alert = [[NSAlert alloc] init];
  alert.alertStyle = NSAlertStyleWarning;
  alert.messageText = @"Stop Octomancer and its daemons?";
  alert.informativeText =
      @"Nothing will watch the timecode bench and no camera clock will be "
      @"corrected until the daemons are started again — from Octomancer.app, "
      @"or with `octomancer start`.\n\n"
      @"They stay installed: if “Start at boot” is on they will come back the "
      @"next time you log in.";
  NSButton* stop = [alert addButtonWithTitle:@"Stop Everything"];
  NSButton* cancel = [alert addButtonWithTitle:@"Cancel"];
  // Return is Cancel rather than Stop. The default button on a destructive
  // question should be the one that changes nothing.
  stop.keyEquivalent = @"";
  cancel.keyEquivalent = @"\r";
  if ([alert runModal] != NSAlertFirstButtonReturn) return;

  dispatch_async(_queue, ^{
    std::string failure;
    for (const octo::Agent a : {octo::Agent::kBench, octo::Agent::kSync}) {
      std::string err;
      if (!octo::agent_stop(a, &err) && failure.empty()) failure = err;
    }
    dispatch_async(dispatch_get_main_queue(), ^{
      if (!failure.empty()) {
        // Still running, so say so and stay up. Quitting anyway would leave
        // somebody believing they had stopped a daemon that is at that moment
        // connecting to a camera.
        [self complain:@"Could not stop the daemons."
                  info:[NSString stringWithFormat:
                                     @"%@\n\nNothing has been quit, so this "
                                     @"can be tried again.",
                                     ns(failure)]];
        [self refreshDaemonState];
        [self refresh];
        return;
      }
      [NSApp terminate:nil];
    });
  });
}

- (void)applicationWillTerminate:(NSNotification*)note {
  (void)note;
  [self stopPairTask];
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
