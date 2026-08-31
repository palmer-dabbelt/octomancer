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

// A view whose origin is its top-left corner, used for exactly one thing: the
// document view of a scrolling page.
//
// AppKit's default is the other way up, and NSClipView takes its own
// flippedness from whatever it is scrolling. An unflipped clip view whose
// document is shorter than itself puts the document at the *bottom* -- so
// every page in this window with less content than the tab was tall drew
// against the bottom edge with a band of nothing above it, which is what it
// looked like: the Devices page is 72 points tall inside a 630-point tab, and
// all 558 points of the difference went above it.
//
// Pinning the page to the clip view's top does not fix this on its own, which
// is what made it confusing -- the constraint was there and was satisfied.
// The clip view scrolls to the origin *it* considers the start, and for an
// unflipped view that is the bottom.
@interface OctoFlippedView : NSView
@end

@implementation OctoFlippedView
- (BOOL)isFlipped {
  return YES;
}
@end

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

// A device's identity in the Details menu: its id with its kind in front.
//
// The prefix is not decoration. Ids are only unique within a kind -- a camera
// and a timecode box may perfectly well answer to the same string -- and the
// menu now holds both, so an id alone can no longer say which row of
// `_configEntries` a selection means.
NSString* device_key(bool camera, const std::string& id) {
  return [NSString stringWithFormat:@"%s:%s", camera ? "c" : "b", id.c_str()];
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
// The width these are told to wrap at when nothing else has told them yet.
// Roughly the window's content width less the page insets; see kWindowWidth.
const CGFloat kWrapWidth = 400.0;

NSTextField* wrapped_label(NSString* text) {
  NSTextField* f = [NSTextField wrappingLabelWithString:text];
  f.selectable = NO;
  f.textColor = [NSColor secondaryLabelColor];
  f.font = [NSFont systemFontOfSize:NSFont.smallSystemFontSize];
  // Without this a wrapping label's *intrinsic* width is the width of the
  // whole sentence on one line, however wide that is. Every one of these is
  // also pinned to its container, so the pin and the intrinsic size disagree
  // and the container's fitting width goes to the unwrapped sentence -- which
  // is how a window sized to fit its content opened half again as wide as it
  // was meant to, and got wider every time a note gained a clause.
  f.preferredMaxLayoutWidth = kWrapWidth;
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

// One line of the merged list of devices this Mac knows of.
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
  // One picker for every device of every kind. See -rebuildDevicePicker.
  //
  // The selection is remembered by key, not by the title showing in the menu:
  // a title gains and loses "(off the air)" as the device comes and goes, and
  // a page that jumped back to the first device every time one went quiet
  // would be useless in exactly the moment somebody was watching it.
  // `_deviceKeys` is the menu's index-to-key map, rebuilt whenever the menu
  // is.
  //
  // A key is the device's id with "c:" or "b:" in front of it, and the prefix
  // is doing real work: ids are not unique across kinds, and every question
  // this page asks -- which readings to draw, which half to show, which
  // configuration row to write -- needs the kind as well as the id. Carrying
  // them together means there is no second array to keep in step.
  NSPopUpButton* _devicePicker;
  NSArray<NSString*>* _pickerKeys;
  NSString* _deviceSelectedKey;
  // Says which kind the selection is, because the menu no longer does. The
  // readings below it are different for a camera and a box, and a page that
  // changed shape with nothing naming the reason would just look unstable.
  NSTextField* _deviceKind;
  // The two halves, one of which is always hidden. NSStackView leaves a
  // hidden arranged subview out of the layout entirely, so this costs no
  // space rather than leaving a gap where the other kind would have been.
  NSStackView* _cameraHalf;
  NSStackView* _boxHalf;
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
  NSButton* _cameraWarn;
  NSButton* _removeCameraButton;
  NSButton* _refreshCameraButton;
  NSTextField* _cameraNote;

  // --- the Details page, timecode-box half ------------------------------
  NSTextField* _boxTcValue;
  NSTextField* _boxOffValue;
  NSTextField* _boxMacValue;
  NSTextField* _boxResValue;
  NSTextField* _boxDriftValue;
  NSTextField* _boxSignalValue;
  NSTextField* _boxHeardValue;
  NSTextField* _boxStateValue;
  NSTextField* _boxNote;
  NSButton* _boxEnabled;
  NSButton* _boxWarn;
  NSButton* _removeBoxButton;
  NSButton* _refreshBoxButton;

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

  // --- the known-device list -------------------------------------------
  // Every device either daemon has heard of plus every device the
  // configuration file mentions, merged and in a fixed order. The picker on
  // the Details page indexes into this, and so do the checkboxes: a device
  // switched off is left out of the merged DeviceView entirely, so a page that
  // took its list from there would have no way to switch one back on.
  std::vector<ConfigEntry> _configEntries;
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
    // This machine's radio only. The condition is "the bench and *this Mac*
    // are pulling apart", and a dongle's rows are quoted against the dongle's
    // own clock -- folding them in would measure the drift between two
    // crystals neither of which is the one in this notification.
    if (!d.radio.empty()) continue;
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
    // The body below says "from this Mac", which is only true of this Mac's
    // rows. A dongle measures against the clock it started at boot, so the
    // same sentence about one of its rows would name a number of hours and
    // send somebody to re-jam a box that is fine.
    if (!d.radio.empty()) continue;
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
// One circle, always, and only its colour changes.
//
// It used to be a clock glyph that grew a second dot beside it when something
// was wrong, which meant the icon changed width -- the menu bar shuffled
// whenever a box went quiet -- and meant reading two symbols to learn one
// thing. One shape that changes colour says the same thing in the space of a
// full stop, and the eye notices a colour change in the corner of a screen far
// better than it notices an extra character.
//
// `nil` is the ordinary state, and it draws in `labelColor` rather than in
// literal white. The menu bar follows the system appearance, so white is only
// white half the time and is invisible the other half; `labelColor` is the
// colour that means "ordinary text here", which is exactly what is wanted.
- (void)setStatusBlip:(NSColor*)color tip:(NSString*)tip {
  if (_statusItem == nil) return;
  _statusItem.button.attributedTitle = [[NSAttributedString alloc]
      initWithString:@"●"
          attributes:@{
            NSForegroundColorAttributeName : color != nil ? color
                                                          : [NSColor labelColor]
          }];
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
    // Grey rather than red: red is reserved for a device that is wrong, and
    // spending it here would send somebody to the cameras when the thing to
    // restart is on this Mac. Dimmed is the honest shape for "this program is
    // not currently able to tell you anything".
    [self setStatusBlip:[NSColor tertiaryLabelColor]
                    tip:@"Octomancer: no daemon answering"];
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
    // From the view rather than from the snapshot, because the snapshot's
    // median_offset is measured against whichever radio heard the row. For
    // this Mac's rows those are the same number; for a dongle's they differ by
    // however long ago it was plugged in, and the menu used to print that --
    // eleven hours, in the column where milliseconds belong. The view quotes
    // every row against its own radio's bench, which is the only form in
    // which two radios' rows can sit in one list.
    const octo::DeviceView view = [self deviceView];
    for (const octo::DeviceRow& r : view.rows) {
      if (r.kind != octo::DeviceKind::kTentacle) continue;
      NSString* where =
          r.radio.empty() ? @"" : [NSString stringWithFormat:@" (%@)",
                                                             ns(r.radio)];
      NSString* line =
          [NSString stringWithFormat:@"%@%@%@  %@", r.alerting ? @"⚠ " : @"",
                                     ns(r.name), where,
                                     r.has_offset ? offset_text(r.offset_s)
                                                  : @"--"];
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

// The width the window opens at. Chosen for the two status lines above the
// tabs, which are longer than anything inside them, and used again to measure
// the pages -- a page has to be measured at the width it will be given or its
// wrapping labels answer for a shape it will never have.
const CGFloat kWindowWidth = 460.0;

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
// The page goes inside a scroll view, and that is not about scrolling. A plain
// NSView does not clip its subviews, so a page taller than the tab simply drew
// past the bottom of it -- over the window, and outside the rectangle AppKit
// invalidates when the selected tab changes. The overflow was therefore never
// erased, and switching tabs left the tall page's text lying under the short
// page's: two pages legibly on top of each other. A clip view clips, which
// ends it. That the leftovers can now be scrolled to instead of lost is the
// second-best thing about it.
- (void)tabView:(NSTabView*)tabView
    didSelectTabViewItem:(NSTabViewItem*)item {
  [[NSUserDefaults standardUserDefaults]
      setInteger:[tabView indexOfTabViewItem:item]
          forKey:kPrefTab];
}

- (NSTabViewItem*)tabTitled:(NSString*)title content:(NSView*)content {
  NSTabViewItem* item = [[NSTabViewItem alloc] initWithIdentifier:title];
  item.label = title;

  // Deliberately left with its autoresizing mask intact: the tab view sets
  // this view's frame itself, and taking that away leaves the page a zero-
  // sized nothing in the corner.
  NSScrollView* scroll = [[NSScrollView alloc] init];
  scroll.hasVerticalScroller = YES;
  scroll.hasHorizontalScroller = NO;
  scroll.drawsBackground = NO;
  scroll.borderType = NSNoBorder;
  scroll.scrollerStyle = NSScrollerStyleOverlay;

  // The page goes inside a flipped wrapper rather than straight into the
  // scroll view; see OctoFlippedView for what that is worth. The wrapper is
  // the document view, and hugs the page exactly, so it changes nothing about
  // the layout except which end of the tab a short page settles against.
  OctoFlippedView* document = [[OctoFlippedView alloc] init];
  document.translatesAutoresizingMaskIntoConstraints = NO;
  content.translatesAutoresizingMaskIntoConstraints = NO;
  [document addSubview:content];
  [NSLayoutConstraint activateConstraints:@[
    [content.leadingAnchor constraintEqualToAnchor:document.leadingAnchor],
    [content.trailingAnchor constraintEqualToAnchor:document.trailingAnchor],
    [content.topAnchor constraintEqualToAnchor:document.topAnchor],
    [content.bottomAnchor constraintEqualToAnchor:document.bottomAnchor],
  ]];

  scroll.documentView = document;
  NSClipView* clip = scroll.contentView;
  [NSLayoutConstraint activateConstraints:@[
    [document.leadingAnchor constraintEqualToAnchor:clip.leadingAnchor],
    [document.trailingAnchor constraintEqualToAnchor:clip.trailingAnchor],
    // Top only. Pinning the bottom as well would stretch a short page down to
    // fill the tab; leaving it free lets the page keep its own height and sit
    // at the top, which is where every one of them starts reading from.
    [document.topAnchor constraintEqualToAnchor:clip.topAnchor],
  ]];
  item.view = scroll;
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

  _devicePicker = [[NSPopUpButton alloc] init];
  _devicePicker.target = self;
  _devicePicker.action = @selector(devicePicked:);
  _deviceKind = dim_label(@"");

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

  // The two settings live next to the readings they govern rather than in a
  // preferences pane, which is the whole reason this page absorbed them: "why
  // will this not sync?" and "let it sync" should be the same glance. They
  // were a grid of bare checkboxes on another tab, where reading a row meant
  // counting across to find out which column was which.
  _writesEnabled = [NSButton checkboxWithTitle:@"Enabled"
                                        target:self
                                        action:@selector(deviceEnabledToggled:)];
  _cameraWarn = [NSButton checkboxWithTitle:@"Warn if out of sync"
                                     target:self
                                     action:@selector(deviceWarnToggled:)];
  _cameraNote = wrapped_label(@"");

  // "Jam sync" rather than "Sync Now" because that is what the operation is
  // called by everybody who does it for a living, and this window is the one
  // place the program speaks to them rather than about them.
  _syncButton = [NSButton buttonWithTitle:@"Jam Sync"
                                   target:self
                                   action:@selector(syncNow:)];
  _syncButton.keyEquivalent = @"\r";
  _removeCameraButton = [NSButton buttonWithTitle:@"Remove…"
                                           target:self
                                           action:@selector(removeDevice:)];
  // No ellipsis: this asks nothing and destroys nothing a person chose. It
  // forgets what the device said it was called so the device is asked again,
  // which is what somebody wants after renaming it at its own end.
  _refreshCameraButton = [NSButton buttonWithTitle:@"Refresh Name"
                                            target:self
                                            action:@selector(refreshDevice:)];
  _activity = dim_label(@"");
  // Whatever the last action had to say, which can be a sentence. It shares a
  // row with two buttons, so it truncates rather than wrapping -- and it gives
  // up its width before anything else does, so a long message widens the
  // window not at all.
  _activity.lineBreakMode = NSLineBreakByTruncatingTail;
  [_activity
      setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow - 1
                               forOrientation:
                                   NSLayoutConstraintOrientationHorizontal];

  NSStackView* cameraFlags =
      [NSStackView stackViewWithViews:@[ _writesEnabled, _cameraWarn ]];
  cameraFlags.orientation = NSUserInterfaceLayoutOrientationHorizontal;
  cameraFlags.spacing = 18;

  NSStackView* actions = [NSStackView
      stackViewWithViews:@[
        _syncButton, _refreshCameraButton, _removeCameraButton, _activity
      ]];
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

  // --- the Details page, timecode-box half --------------------------------
  //
  // Same shape as the camera half above, on purpose. This page answers one
  // question -- tell me everything about one device -- and somebody should not
  // have to learn two layouts to ask it about two kinds of device. Devices is
  // the list; this is the single thing looked at closely.
  //
  // A box gets a picker and readings and no controls, because there are no
  // controls to give: nothing in this program can set a Tentacle's clock. The
  // note at the bottom says so rather than leaving the empty space to be read
  // as something unfinished.

  _boxTcValue = mono_label(@"--");
  _boxOffValue = mono_label(@"--");
  _boxMacValue = mono_label(@"--");
  _boxResValue = label(@"--");
  _boxDriftValue = mono_label(@"--");
  _boxSignalValue = mono_label(@"--");
  _boxHeardValue = mono_label(@"--");
  _boxStateValue = label(@"--");
  _boxNote = wrapped_label(@"");

  // "Off by" is the first number on both halves and means the same thing on
  // both: distance from the time everything else is being held to, never from
  // this Mac. The Mac appears once, further down, labelled as itself.
  NSGridView* boxDetail = [NSGridView gridViewWithViews:@[
    @[ dim_label(@"Timecode"), _boxTcValue ],
    @[ dim_label(@"Off by"), _boxOffValue ],
    @[ dim_label(@"Counting in"), _boxResValue ],
    @[ dim_label(@"Against this Mac"), _boxMacValue ],
    @[ dim_label(@"Drift"), _boxDriftValue ],
    @[ dim_label(@"Signal"), _boxSignalValue ],
    @[ dim_label(@"Last heard"), _boxHeardValue ],
    @[ dim_label(@"State"), _boxStateValue ],
  ]];
  boxDetail.columnSpacing = 12;
  boxDetail.rowSpacing = 6;
  [boxDetail columnAtIndex:0].xPlacement = NSGridCellPlacementTrailing;

  NSStackView* boxDetailRow =
      [NSStackView stackViewWithViews:@[ boxDetail, [[NSView alloc] init] ]];
  boxDetailRow.orientation = NSUserInterfaceLayoutOrientationHorizontal;
  boxDetailRow.spacing = 0;
  [boxDetailRow setHuggingPriority:NSLayoutPriorityDefaultLow
                    forOrientation:NSLayoutConstraintOrientationHorizontal];

  // The same two settings, in the same place on the page, meaning the same
  // thing to the same file. A box has no third control because there is no
  // third thing anybody can do to it from here.
  _boxEnabled = [NSButton checkboxWithTitle:@"Enabled"
                                     target:self
                                     action:@selector(deviceEnabledToggled:)];
  _boxWarn = [NSButton checkboxWithTitle:@"Warn if out of sync"
                                  target:self
                                  action:@selector(deviceWarnToggled:)];
  _removeBoxButton = [NSButton buttonWithTitle:@"Remove…"
                                        target:self
                                        action:@selector(removeDevice:)];
  _refreshBoxButton = [NSButton buttonWithTitle:@"Refresh Name"
                                         target:self
                                         action:@selector(refreshDevice:)];

  NSStackView* boxFlags =
      [NSStackView stackViewWithViews:@[ _boxEnabled, _boxWarn ]];
  boxFlags.orientation = NSUserInterfaceLayoutOrientationHorizontal;
  boxFlags.spacing = 18;

  NSStackView* boxActions =
      [NSStackView stackViewWithViews:@[ _refreshBoxButton, _removeBoxButton ]];
  boxActions.orientation = NSUserInterfaceLayoutOrientationHorizontal;
  boxActions.spacing = 12;

  // The two halves, each gathered into a container so the page can show one
  // and collapse the other. They keep the shape they had when they were two
  // sections of one long page -- readings, then settings, then whatever can be
  // done, then a note -- because that is the order somebody reads them in and
  // it is the same order for both kinds.
  _cameraHalf = [NSStackView stackViewWithViews:@[
    detailRow, cameraFlags, actions, _cameraNote,
  ]];
  _boxHalf = [NSStackView stackViewWithViews:@[
    boxDetailRow, boxFlags, boxActions, _boxNote,
  ]];
  for (NSStackView* half in @[ _cameraHalf, _boxHalf ]) {
    half.orientation = NSUserInterfaceLayoutOrientationVertical;
    half.alignment = NSLayoutAttributeLeading;
    half.spacing = 10;
  }

  NSStackView* detailsStack = [NSStackView stackViewWithViews:@[
    heading(@"Device"), _devicePicker, _deviceKind, _cameraHalf, _boxHalf,
  ]];
  detailsStack.orientation = NSUserInterfaceLayoutOrientationVertical;
  detailsStack.alignment = NSLayoutAttributeLeading;
  detailsStack.spacing = 10;
  detailsStack.edgeInsets = NSEdgeInsetsMake(8, 8, 8, 8);
  [detailsStack setCustomSpacing:16 afterView:_deviceKind];
  [self pinWidth:_boxNote to:_boxHalf inset:24];
  [self pinWidth:_cameraNote to:_cameraHalf inset:24];
  // The halves are as wide as the page, so the one that is showing wraps its
  // note to the window rather than to whatever its widest row happened to be.
  for (NSStackView* half in @[ _cameraHalf, _boxHalf ]) {
    [self pinWidth:half to:detailsStack inset:16];
  }

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

  // Wrapping, because this line names every box that voted and is the longest
  // sentence in the window. As a single-line label its intrinsic width was the
  // whole sentence, and a window sized to fit its content is a window that
  // sentence gets to decide the width of.
  _canonicalLine = wrapped_label(@"…");
  _canonicalLine.textColor = [NSColor labelColor];
  _deviceGrid = [NSGridView gridViewWithNumberOfColumns:5 rows:0];
  _deviceGrid.columnSpacing = 16;
  _deviceGrid.rowSpacing = 5;
  // The three numeric columns hang off their right edge, which is the only way
  // digits of different lengths line up under each other.
  for (NSInteger i = 1; i <= 3; ++i) {
    [_deviceGrid columnAtIndex:i].xPlacement = NSGridCellPlacementTrailing;
  }
  _hiddenLine = wrapped_label(@"");

  NSStackView* devicesStack = [NSStackView stackViewWithViews:@[
    _canonicalLine, _deviceGrid, _hiddenLine,
  ]];
  
  devicesStack.orientation = NSUserInterfaceLayoutOrientationVertical;
  devicesStack.alignment = NSLayoutAttributeLeading;
  devicesStack.spacing = 10;
  devicesStack.edgeInsets = NSEdgeInsetsMake(12, 12, 12, 12);
  for (NSTextField* note in @[ _canonicalLine, _hiddenLine ]) {
    [self pinWidth:note to:devicesStack inset:24];
  }

  // --- the System page ----------------------------------------------------
  //
  // What is true of the whole installation rather than of any one device: the
  // two daemons, whether they come back at login, and the one thing that has
  // to happen before a camera exists to have settings at all.
  //
  // The per-device checkboxes used to be here, in a grid. They are on Details
  // now, beside the readings they explain, which is where somebody is standing
  // when the question comes up. What is left is genuinely about the system, so
  // that is what the tab is called.

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
    heading(@"Add a camera"), pairButton, pairNote,
    heading(@"Daemons"), agentGrid, daemonButtons, _startAtBoot, _bootNote,
  ]];
  configStack.orientation = NSUserInterfaceLayoutOrientationVertical;
  configStack.alignment = NSLayoutAttributeLeading;
  configStack.spacing = 8;
  configStack.edgeInsets = NSEdgeInsetsMake(12, 12, 12, 12);
  [configStack setCustomSpacing:18 afterView:pairNote];
  for (NSTextField* note in @[ pairNote, _bootNote ]) {
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
  [_tabs addTabViewItem:[self tabTitled:@"Details" content:detailsStack]];
  [_tabs addTabViewItem:[self tabTitled:@"System" content:configStack]];
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
  // happens to be showing.
  [[_tabs.widthAnchor constraintEqualToAnchor:root.widthAnchor
                                     constant:-32] setActive:YES];

  // Tall enough for the tallest page, asked of each page directly. A scroll
  // view is content to be any size at all, so the window can no longer take
  // its height from the view hierarchy the way it did when the pages were
  // pinned straight into the tab -- it would open every page scrolled.
  //
  // Each page is measured with the width it will actually be given, because a
  // wrapping label's height is a function of its width: asking before fixing
  // the width answers for a shape the page will never have, and the answer is
  // always too short.
  //
  // Both of the numbers this needs are asked of the tab view rather than
  // guessed, which is the change that mattered. The guesses were "the page is
  // 32 points narrower than the window" and "the tab strip is 40 points" --
  // and they were out by six each, in the direction that makes a page too
  // tall for the space measured for it. Six points is one line of a wrapped
  // note, so the Details page came up with a scroll bar it did not need and
  // nothing on screen said why.
  //
  // Two separate insets are in play and they are easy to conflate. A tab
  // view's *frame* is bigger than its *alignment rect*, which is what the
  // width constraint above is really setting; and its contentRect is smaller
  // again, by the strip and the border. So the frame is built from the
  // alignment rect the constraint will give it, and the answer read back.
  const CGFloat tabs_align_width = kWindowWidth - 32.0;
  _tabs.frame = [_tabs frameForAlignmentRect:NSMakeRect(0, 0, tabs_align_width,
                                                        400.0)];
  const NSRect tab_content = [_tabs contentRect];
  const CGFloat page_width = NSWidth(tab_content);
  // How much taller the tab view's alignment rect must be than the page it
  // holds. Expressed against the alignment rect, because that is what the
  // height constraint below is measured in.
  const CGFloat tab_chrome =
      NSHeight([_tabs alignmentRectForFrame:_tabs.frame]) -
      NSHeight(tab_content);

  CGFloat tallest = 320.0;
  for (NSView* page in @[ devicesStack, detailsStack, configStack, notifyPage ]) {
    NSLayoutConstraint* fixed =
        [page.widthAnchor constraintEqualToConstant:page_width];
    fixed.active = YES;
    if (page == detailsStack) {
      // Only ever one half of the Details page is on screen. Measuring it with
      // both showing would size the window for a shape it never takes, and
      // leave a band of nothing under whichever half is up -- which is the
      // same complaint the flipped document view was fixing, arrived at from
      // the other direction.
      for (int camera = 0; camera < 2; ++camera) {
        _cameraHalf.hidden = camera == 0;
        _boxHalf.hidden = camera != 0;
        [page layoutSubtreeIfNeeded];
        tallest = MAX(tallest, page.fittingSize.height);
      }
    } else {
      [page layoutSubtreeIfNeeded];
      tallest = MAX(tallest, page.fittingSize.height);
    }
    fixed.active = NO;
  }
  // Left as -updateDetail: will find them; it is called before the window is
  // shown and decides which half belongs up.
  _cameraHalf.hidden = YES;
  _boxHalf.hidden = YES;
  // A ceiling, because "as tall as the tallest page" is only a good rule while
  // the tallest page fits on a screen. It used to be a flat 620, which was
  // less than the Details page needs on any display made this decade -- so the
  // one page that had to scroll was the one somebody opens to read a number
  // off. Ask the screen instead, and leave room for the title bar, the two
  // status lines above the tabs and a margin.
  // mainScreen rather than the window's own: there is no window yet, this
  // being what decides how big to make it.
  CGFloat ceiling = 620.0;
  NSScreen* screen = [NSScreen mainScreen];
  if (screen != nil) ceiling = NSHeight(screen.visibleFrame) - 200.0;
  tallest = MIN(tallest, MAX(320.0, ceiling));
  [[_tabs.heightAnchor
      constraintGreaterThanOrEqualToConstant:tallest + tab_chrome]
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
  [_window setContentSize:NSMakeSize(MAX(kWindowWidth, fits.width),
                                     fits.height)];
  // The floor on the height is deliberately not `fits.height`: a page can now
  // scroll, so a window somebody has dragged shorter is a window they can
  // still read, and refusing to let them make it smaller than its tallest page
  // is refusing them the thing the scroll view was added for.
  [_window setContentMinSize:NSMakeSize(MAX(420, fits.width), 360.0)];
  [_window center];

  [self refreshDaemonState];
}

- (void)updateWindow {
  if (_window == nil || !_window.isVisible) return;

  // How long the daemon has been up is not what anybody opened this window to
  // find out, and it sat above every page. So the line is shown only when it
  // has something to say -- which is when the daemon is not answering, or is
  // not going to touch anything. A hidden arranged subview is left out of an
  // NSStackView's layout entirely, so this costs no blank row.
  if (_controlUp && !_status.daemon.dry_run) {
    _daemonLine.hidden = YES;
  } else if (_controlUp) {
    _daemonLine.hidden = NO;
    _daemonLine.stringValue = @"DRY RUN — nothing will be written";
    _daemonLine.textColor = [NSColor systemOrangeColor];
  } else {
    _daemonLine.hidden = NO;
    _daemonLine.stringValue = @"Sync daemon not answering";
    _daemonLine.textColor = [NSColor systemRedColor];
  }

  // What the bench is, in its own words. No "Bench:" label -- it is the only
  // thing on the line -- and no distance from this Mac's clock, which is a
  // number about the laptop rather than about the boxes. The spread is the one
  // worth watching: it is how far the boxes are from each other, and it is
  // what goes wrong first.
  if (_controlUp && _status.bench.has) {
    _benchLine.stringValue =
        [NSString stringWithFormat:@"%d timecode box%s, spread %.0f ms",
                                   _status.bench.boxes,
                                   _status.bench.boxes == 1 ? "" : "es",
                                   _status.bench.spread_s * 1000.0];
  } else if (_benchUp && _snapshot.has_bench) {
    // The other daemon does not report a spread, so this says less rather
    // than inventing one.
    _benchLine.stringValue =
        [NSString stringWithFormat:@"%d timecode box%s", _snapshot.live,
                                   _snapshot.live == 1 ? "" : "es"];
  } else {
    _benchLine.stringValue = @"No timecode boxes heard yet";
  }

  // The merged list of known devices first: the picker indexes into it, and so
  // do the checkboxes underneath it.
  [self updateKnownDevices];
  [self rebuildDevicePicker];
  // Built once and handed to both pages that read it. They are two views of
  // one list and must not be able to disagree about what is in it.
  const octo::DeviceView view = [self deviceView];
  [self updateDetail:view];
  [self updateDevices:view];
}

// Rebuilt in place: replacing the whole menu on a two-second timer would fight
// anyone trying to use it.
// One menu, every device, both kinds mixed together.
//
// It used to be two pickers under two headings, which meant the page asked
// "which camera" and "which timecode box" as separate questions when the
// question somebody actually has is "tell me about this thing". Boxes and
// cameras appear in the order updateKnownDevices built them, which is the
// order everything else in the program lists them in.
//
// The list is `_configEntries`, not the merged DeviceView, and that is the
// load-bearing part: a device somebody switched off is deliberately absent
// from the DeviceView, so a picker built from there would have no entry for
// the one device whose settings somebody most likely came here to change back.
//
// Rebuilt in place, and only when the titles actually differ: replacing the
// whole menu on a two-second timer would fight anybody trying to use it.
- (void)rebuildDevicePicker {
  if (_devicePicker == nil) return;
  NSMutableArray<NSString*>* titles = [NSMutableArray array];
  NSMutableArray<NSString*>* keys = [NSMutableArray array];
  for (const ConfigEntry& e : _configEntries) {
    NSString* title = ns(e.name.empty() ? e.id : e.name);
    if (!e.present) title = [title stringByAppendingString:@" (off the air)"];
    if (!e.enabled) title = [title stringByAppendingString:@" — switched off"];
    // NSPopUpButton treats a title as an identity and drops the older of two
    // that match, which would leave `keys` a row longer than the menu and
    // every selection past the collision pointing at the wrong device. More
    // likely now than it was with two menus: a camera and a box can only
    // collide once they are in the same list.
    if ([titles containsObject:title]) {
      title = [NSString stringWithFormat:@"%@ [%@]", title,
                                         ns(e.id.substr(0, 8))];
    }
    [titles addObject:title];
    [keys addObject:device_key(e.camera, e.id)];
  }
  _pickerKeys = keys;

  // The empty case goes before the "nothing changed" guard below, and has to.
  // With no devices at all, `titles` and the menu's own titles are both empty
  // and compare equal, so the guard returned with the menu exactly as it
  // found it -- which on the first tick is no items at all, leaving an enabled
  // drop-down that opens onto nothing.
  if (titles.count == 0) {
    if (_devicePicker.numberOfItems == 1 && !_devicePicker.enabled) return;
    [_devicePicker removeAllItems];
    [_devicePicker addItemWithTitle:@"No devices"];
    _devicePicker.enabled = NO;
    return;
  }
  if ([titles isEqualToArray:[_devicePicker itemTitles]]) return;

  [_devicePicker removeAllItems];
  [_devicePicker addItemsWithTitles:titles];
  _devicePicker.enabled = YES;
  const NSUInteger want =
      _deviceSelectedKey == nil ? NSNotFound
                                : [keys indexOfObject:_deviceSelectedKey];
  [_devicePicker selectItemAtIndex:want == NSNotFound ? 0 : (NSInteger)want];
  if (want == NSNotFound) _deviceSelectedKey = keys.firstObject;
}

// The key under the picker's current selection, or nil when there is none.
- (NSString*)selectedDeviceKey {
  const NSInteger index = _devicePicker.indexOfSelectedItem;
  if (_pickerKeys == nil || index < 0 || index >= (NSInteger)_pickerKeys.count) {
    return nil;
  }
  return _pickerKeys[index];
}

- (BOOL)selectedIsCamera {
  NSString* key = [self selectedDeviceKey];
  return key != nil && [key hasPrefix:@"c:"];
}

// The selected device's id, but only when it is of the kind asked for. Every
// caller wants one kind or the other -- a camera's readings are meaningless
// for a box -- and returning an id of the wrong kind would have them look it
// up, fail, and draw "no camera" when a camera is not what is selected.
- (std::string)selectedIdIfCamera:(BOOL)camera {
  NSString* key = [self selectedDeviceKey];
  if (key == nil) return std::string();
  if ([key hasPrefix:@"c:"] != (camera == YES)) return std::string();
  return [key substringFromIndex:2].UTF8String;
}

// Which camera the controls act on, as the id the daemon knows it by. Empty
// when the selection is a timecode box, which is the whole point: the buttons
// that use this are in the half that is hidden then.
- (std::string)selectedCameraId {
  return [self selectedIdIfCamera:YES];
}

// Where the selected device sits in `_configEntries`, which is what a
// checkbox's tag carries and what the save path indexes into. -1 when there
// is no selection, which every caller has to be able to mean "no device".
- (NSInteger)configIndexFor:(const std::string&)id camera:(BOOL)camera {
  if (id.empty()) return -1;
  for (size_t i = 0; i < _configEntries.size(); ++i) {
    const ConfigEntry& e = _configEntries[i];
    if (e.camera == (camera == YES) && e.id == id) {
      return static_cast<NSInteger>(i);
    }
  }
  return -1;
}

// The daemon's view of the selected camera, when it has one. A camera that is
// only in the configuration file has no status at all, and that is not an
// error -- its readings are blank and its settings still work, which is the
// whole reason it is still listed.
- (const octo::CameraStatus*)selectedCamera {
  const std::string want = [self selectedCameraId];
  if (want.empty()) return nullptr;
  for (const octo::CameraStatus& c : _status.cameras) {
    if (c.id == want) return &c;
  }
  return nullptr;
}

// Which half of the Details page is showing, and the readings in it.
//
// Both halves are updated whichever is visible. It costs nothing -- they are
// reading numbers already in memory -- and it means the hidden half is never
// stale for the one frame after somebody switches to it.
- (void)updateDetail:(const octo::DeviceView&)view {
  if (_devicePicker == nil) return;

  const BOOL camera = [self selectedIsCamera];
  const BOOL any = [self selectedDeviceKey] != nil;

  // Hidden rather than emptied: an arranged subview that is hidden is left out
  // of an NSStackView's layout entirely, so the page is exactly as tall as the
  // half being shown. With nothing selected at all, both halves go -- there is
  // no kind to show readings for, and a page of dashes would be pretending
  // there is.
  _cameraHalf.hidden = !any || !camera;
  _boxHalf.hidden = !any || camera;

  if (!any) {
    _deviceKind.stringValue =
        _benchUp || _controlUp
            ? @"Nothing has been heard yet."
            : @"Neither daemon is answering.";
  } else {
    _deviceKind.stringValue = camera ? @"Camera" : @"Timecode box";
  }

  [self updateCameraDetail];
  [self updateBoxDetail:view];
}

- (void)updateCameraDetail {
  const std::string picked = [self selectedCameraId];
  const NSInteger which = [self configIndexFor:picked camera:YES];
  const ConfigEntry* entry =
      which < 0 ? nullptr : &_configEntries[static_cast<size_t>(which)];
  // Before the early return below, deliberately. A camera that is only in the
  // configuration file has no status to draw, but its two settings still work
  // and removing it still works -- and those are the only things anybody can
  // do about a camera that has stopped turning up.
  [self setFlags:_writesEnabled
            warn:_cameraWarn
          remove:_removeCameraButton
         refresh:_refreshCameraButton
              to:entry
              at:which];
  [self updateCameraNote:entry];

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



// --------------------------------------------- the Details page, box half

// The readings for the selected box, when the merged view has any. A box that
// has been switched off is not in the view at all -- that is what switching it
// off means -- so this returns nothing and the page says so rather than
// drawing a row of dashes that could equally mean the radio is dead.
- (const octo::DeviceRow*)boxIn:(const octo::DeviceView&)view {
  const std::string want = [self selectedIdIfCamera:NO];
  if (want.empty()) return nullptr;
  for (const octo::DeviceRow& r : view.rows) {
    if (r.kind == octo::DeviceKind::kTentacle && r.id == want) return &r;
  }
  return nullptr;
}

// The two settings and the remove button for whichever device a half of the
// page is showing.
//
// The tag is how a click finds its way back to a device: the control itself
// knows nothing, and the list it indexes into is rebuilt in the same order
// every tick. -1 means no device, and every control goes dead rather than
// acting on whatever happens to be first.
- (void)setFlags:(NSButton*)enabled
            warn:(NSButton*)warn
          remove:(NSButton*)remove
         refresh:(NSButton*)refresh
              to:(const ConfigEntry*)entry
              at:(NSInteger)which {
  for (NSButton* b in @[ enabled, warn, remove, refresh ]) b.tag = which;
  if (entry == nullptr) {
    enabled.state = NSControlStateValueOff;
    warn.state = NSControlStateValueOff;
    enabled.enabled = NO;
    warn.enabled = NO;
    remove.enabled = NO;
    refresh.enabled = NO;
    return;
  }
  enabled.state = entry->enabled ? NSControlStateValueOn : NSControlStateValueOff;
  warn.state = entry->warn ? NSControlStateValueOn : NSControlStateValueOff;
  enabled.enabled = _confLoaded && !_busy;
  // A switched-off device is left out of the merged view entirely, so its
  // warning could never fire. Greyed rather than cleared: the setting is still
  // in the file and comes back the moment the device is switched on again, and
  // silently unticking somebody's box to mean "this has no effect right now"
  // would lose what they asked for.
  warn.enabled = _confLoaded && !_busy && entry->enabled;
  remove.enabled = !_busy;
  // Needs octomancerd rather than octomancer-sync, whichever kind of device
  // this is: the name book lives there, because it has to cover devices heard
  // by a dongle and octomancer-sync has never seen one of those.
  refresh.enabled = !_busy && _benchUp;
}

// What the two settings on the camera half actually mean, said in words,
// because "Enabled" is the same label on both halves of this page and does not
// mean the same thing on each: a box that is enabled is one that counts, and a
// camera that is enabled is one this program is allowed to write to.
- (void)updateCameraNote:(const ConfigEntry*)entry {
  if (_cameraNote == nil) return;
  if (entry == nullptr) {
    _cameraNote.stringValue =
        _controlUp ? @"No camera is known yet. Pair one on the System page and "
                     @"it appears here."
                   : @"octomancer-sync is not answering, so nothing here knows "
                     @"about any camera.";
    _cameraNote.textColor = [NSColor secondaryLabelColor];
    return;
  }
  NSString* what =
      entry->enabled
          ? @"Enabled: octomancer may set this camera's clock."
          : @"Not enabled: octomancer will read this camera and never write "
            @"to it.";
  _cameraNote.stringValue =
      _confLoaded
          ? [NSString stringWithFormat:@"%@ Saved in %@.", what,
                                       ns(_conf.path())]
          : @"The configuration file could not be read, so a change here will "
            @"not stick.";
  _cameraNote.textColor = [NSColor secondaryLabelColor];
}

- (void)devicePicked:(id)sender {
  (void)sender;
  NSString* key = [self selectedDeviceKey];
  if (key != nil) _deviceSelectedKey = key;
  [self updateDetail:[self deviceView]];
}

- (void)updateBoxDetail:(const octo::DeviceView&)view {
  if (_devicePicker == nil) return;
  NSArray<NSTextField*>* values = @[
    _boxTcValue, _boxOffValue, _boxResValue, _boxMacValue, _boxDriftValue,
    _boxSignalValue, _boxHeardValue,
  ];

  const std::string picked = [self selectedIdIfCamera:NO];
  const NSInteger which = [self configIndexFor:picked camera:NO];
  const ConfigEntry* entry =
      which < 0 ? nullptr : &_configEntries[static_cast<size_t>(which)];
  [self setFlags:_boxEnabled
            warn:_boxWarn
          remove:_removeBoxButton
         refresh:_refreshBoxButton
              to:entry
              at:which];

  const octo::DeviceRow* r = [self boxIn:view];
  if (r == nullptr) {
    for (NSTextField* f in values) {
      f.stringValue = @"--";
      f.textColor = [NSColor labelColor];
    }
    // Three different silences, and a page that drew them the same way would
    // be answering a question it had not been asked.
    _boxStateValue.stringValue =
        entry == nullptr
            ? (_benchUp ? @"no timecode box heard yet"
                        : @"octomancerd is not answering")
        : !entry->enabled ? @"switched off — nothing is being read from it"
                          : @"not heard from since the daemon started";
    _boxStateValue.textColor = [NSColor secondaryLabelColor];
    _boxNote.stringValue =
        entry == nullptr
            ? @""
            : @"Nothing here can set a timecode box's clock; only the "
              @"Tentacle app can re-jam one.";
    _boxNote.textColor = [NSColor secondaryLabelColor];
    return;
  }
  for (NSTextField* f in values) f.textColor = [NSColor labelColor];

  _boxTcValue.stringValue = r->timecode.empty() ? @"--" : ns(r->timecode);

  // Blank while the box is away rather than blank full stop, and the two are
  // said differently. See DeviceRow::has_offset for why the number is
  // withheld: subtracting a current canonical time from an old reading
  // measures the silence, not the box.
  if (r->has_offset) {
    _boxOffValue.stringValue = offset_text(r->offset_s);
    _boxOffValue.textColor = fabs(r->offset_s) < octo::kWarnOffset
                                 ? [NSColor systemGreenColor]
                                 : [NSColor systemOrangeColor];
  } else if (r->offset_is_stale) {
    _boxOffValue.stringValue = @"not while it is off the air";
    _boxOffValue.textColor = [NSColor secondaryLabelColor];
  } else {
    _boxOffValue.stringValue = @"--";
  }

  _boxResValue.stringValue =
      r->resolution.empty() ? @"--" : ns(r->resolution);
  // Only for rows this Mac heard. The raw median is quoted against whichever
  // radio took the reading, so under a label that says "this Mac" a dongle's
  // row would be answering a question nobody asked -- and answering it with a
  // number that looks like a catastrophic drift.
  if (!r->radio.empty()) {
    _boxMacValue.stringValue = @"not measured from here";
    _boxMacValue.textColor = [NSColor secondaryLabelColor];
  } else {
    _boxMacValue.stringValue =
        r->has_median ? offset_text(r->median_offset_s) : @"--";
  }

  if (r->has_drift) {
    _boxDriftValue.stringValue =
        [NSString stringWithFormat:@"%+.1f ppm", r->drift_ppm];
  } else if (r->has_drift_span) {
    // Not a measurement, and said so. Drift needs a long lever arm rather
    // than more samples, so this row stays empty for a good while and a bare
    // "--" would read as broken.
    _boxDriftValue.stringValue = [NSString
        stringWithFormat:@"not yet — watched for %@",
                         elapsed_text(r->drift_span_s)];
    _boxDriftValue.textColor = [NSColor secondaryLabelColor];
  } else {
    _boxDriftValue.stringValue = @"--";
  }

  _boxSignalValue.stringValue =
      r->has_rssi ? [NSString stringWithFormat:@"%d dBm", r->rssi] : @"--";
  _boxHeardValue.stringValue =
      r->has_age ? [elapsed_text(r->age_s) stringByAppendingString:@" ago"]
                 : @"--";

  NSString* state = @(octo::link_state_name(r->link));
  state = [state stringByAppendingString:
                     r->contributes ? @", voting on the canonical time"
                                    : @", not voting on the canonical time"];
  _boxStateValue.stringValue = state;
  _boxStateValue.textColor = octo::link_is_live(r->link)
                                 ? [NSColor labelColor]
                                 : [NSColor secondaryLabelColor];

  // The only instruction this half can offer, and the reason it has no
  // controls. A box that has drifted out of its alert band cannot be fixed
  // from here by anybody -- re-jamming is the Tentacle app's job.
  NSMutableArray<NSString*>* note = [NSMutableArray array];
  if (r->alerting) {
    [note addObject:[NSString stringWithFormat:
                                  @"%@ is out on its own and needs re-jamming "
                                  @"in the Tentacle app.",
                                  ns(r->name)]];
  }
  if (!octo::link_is_live(r->link)) {
    [note addObject:@"Nothing is being heard from it now, so the readings "
                    @"above are the last ones it gave."];
  }
  [note addObject:@"Nothing here can set a timecode box's clock; only the "
                  @"Tentacle app can re-jam one."];
  _boxNote.stringValue = [note componentsJoinedByString:@" "];
  _boxNote.textColor = r->alerting ? [NSColor systemRedColor]
                                   : [NSColor secondaryLabelColor];
}

// Send a request and watch it to the end, off the main thread. The button is
// disabled meanwhile: a second sync queued behind the first would just make
// someone wait twice as long for the same answer.
- (void)send:(const std::string&)command describing:(NSString*)what {
  if (_busy) return;
  _busy = true;
  _activity.stringValue = [what stringByAppendingString:@"…"];
  _activity.textColor = [NSColor secondaryLabelColor];
  [self updateDetail:[self deviceView]];

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
                   @"on the System page."];
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
      [self updateDetail:[self deviceView]];  // put the picker back
      return;
    }
  }

  [self send:("source value=" + std::to_string(value) + [self cameraArgument])
      describing:@"Setting timecode source"];
}

// Permission is written to the configuration file, not sent over the socket:
// the daemon only ever reads that file. Then it is told to re-read it, which
// it does within a quarter of a second, rather than at the next restart.
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

- (void)updateDevices:(const octo::DeviceView&)view {
  if (_deviceGrid == nil) return;

  // Nothing when there is a canonical time: the line above the tabs already
  // says how many boxes there are and how far apart they sit, and this said it
  // again in longer words. What it is kept for is the case below, where the
  // OFFSET column is empty and the reason is not otherwise visible anywhere.
  if (view.has_canonical) {
    _canonicalLine.hidden = YES;
  } else {
    _canonicalLine.hidden = NO;
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
    // Which radio heard it, when it was not this one. The terminal spends a
    // column on this; here there is room to say it in words, and without it a
    // bench in earshot of a dongle lists "BMPCC" twice with nothing to tell
    // the two apart -- which reads as the page repeating itself rather than
    // as two radios agreeing.
    NSString* via =
        r.radio.empty()
            ? @""
            : [NSString stringWithFormat:@" (via %@)", ns(r.radio)];
    cells[0].stringValue =
        [[ns(r.name) stringByAppendingString:via] stringByAppendingString:mark];
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
    cells[3].stringValue =
        r.has_rssi ? [NSString stringWithFormat:@"%d dBm", r.rssi] : @"--";
    cells[3].textColor = faint;
    cells[4].stringValue = said;
    cells[4].textColor = faint;
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
  // Take the old rows out of the view hierarchy, not just out of the grid.
  //
  // removeRowAtIndex: unbinds a row from the grid's layout. Whether it also
  // removes that row's content views from the view hierarchy is not something
  // NSGridView.h states, and here it does not: the old labels stayed on as
  // subviews at whatever frame they last held, the rebuilt ones were added
  // over the top, and after a few changes to the device list the page was text
  // drawn on text. That reads as a blur rather than as a mistake, which is why
  // it survived being looked at.
  //
  // Removing them explicitly is right whichever way AppKit behaves: a no-op if
  // it already took them out, and the fix if it did not. Depending on the
  // answer is what produced the bug.
  for (NSInteger i = _deviceGrid.numberOfRows - 1; i >= 0; --i) {
    NSGridRow* row = [_deviceGrid rowAtIndex:i];
    for (NSInteger c = 0; c < row.numberOfCells; ++c) {
      [[row cellAtIndex:c].contentView removeFromSuperview];
    }
    [_deviceGrid removeRowAtIndex:i];
  }

  // Signal sits before Link rather than after it, which is the one place this
  // table's column order departs from `octomancer status`. The terminal keeps
  // its notes under the table and so has a fixed-width LINK; here the note
  // rides in the same cell, so Link is the column that can be a sentence, and
  // a sentence belongs at the end of a row rather than in the middle of one.
  NSArray<NSTextField*>* header = @[
    dim_label(@"Device"), dim_label(@"Last seen"),
    dim_label(@"From canonical"), dim_label(@"Signal"), dim_label(@"Link"),
  ];
  for (NSTextField* f in header) {
    f.font = [NSFont systemFontOfSize:NSFont.smallSystemFontSize];
  }
  [_deviceGrid addRowWithViews:header];

  for (NSString* key in keys) {
    NSArray<NSTextField*>* cells = _deviceCells[key];
    if (cells == nil) {
      cells = @[
        label(@""), mono_label(@""), mono_label(@""), mono_label(@""),
        label(@""),
      ];
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
- (void)updateKnownDevices {
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
}

- (void)deviceEnabledToggled:(id)sender {
  [self saveDeviceFlag:(NSButton*)sender warning:NO];
}

- (void)deviceWarnToggled:(id)sender {
  [self saveDeviceFlag:(NSButton*)sender warning:YES];
}

// Take a device off the list entirely: out of the configuration file, and out
// of whichever daemon is holding what it has learned about it.
//
// Not the same as switching it off, and the alert says so, because the two are
// one click apart and only one of them can be undone by clicking again. What
// this throws away is history -- an hour of drift measurement for a box, a
// camera body's RTC bias and apply delay, both of which took hours to converge.
// What it does not throw away is the device: it is not blacklisted, so the next
// advertisement puts it straight back on the list at its defaults. That is the
// honest thing to promise, and it is also why removing something that is still
// in the room is a temporary condition rather than a mistake.
// Forget what a device said it was called, so it is asked again.
//
// No confirmation, deliberately: this destroys nothing a person chose. A name
// somebody typed here is kept -- see src/naming.h, where the reasoning is --
// and everything else it drops is re-learned within seconds of the device
// being heard again. A dialog guarding an action that cannot lose anything
// teaches people to click through dialogs.
- (void)refreshDevice:(id)sender {
  const NSInteger which = ((NSButton*)sender).tag;
  if (which < 0 || static_cast<size_t>(which) >= _configEntries.size()) return;
  const ConfigEntry entry = _configEntries[static_cast<size_t>(which)];
  NSString* label = ns(entry.name.empty() ? entry.id : entry.name);

  // Always octomancerd, whichever kind of device it is: the name book lives
  // there because it has to cover devices heard by a dongle as well as by this
  // Mac, and octomancer-sync has never seen one of those.
  const std::string benchPath = _benchSocket;
  const std::string id = entry.id;

  _busy = true;
  dispatch_async(_queue, ^{
    std::string reply, err;
    const bool told =
        octo::query(benchPath, "refresh " + id, &reply, &err, 5.0);
    dispatch_async(dispatch_get_main_queue(), ^{
      self->_busy = false;
      if (!told) {
        [self complain:@"Could not refresh that device."
                  info:ns(err)];
        return;
      }
      self->_activity.stringValue = [NSString
          stringWithFormat:@"%@ will be asked again what it is called", label];
      [self refresh];
    });
  });
}

- (void)removeDevice:(id)sender {
  const NSInteger which = ((NSButton*)sender).tag;
  if (which < 0 || static_cast<size_t>(which) >= _configEntries.size()) return;
  const ConfigEntry entry = _configEntries[static_cast<size_t>(which)];
  NSString* name = ns(entry.name.empty() ? entry.id : entry.name);

  NSAlert* alert = [[NSAlert alloc] init];
  alert.alertStyle = NSAlertStyleWarning;
  alert.messageText = [NSString stringWithFormat:@"Remove %@?", name];
  NSMutableString* why = [NSMutableString string];
  [why appendString:@"Its settings and everything measured about it are "
                    @"deleted."];
  [why appendString:entry.camera
                        ? @" That means the learned RTC bias and apply delay "
                          @"for this body, which took hours to settle."
                        : @" That means its drift history, which needs hours "
                          @"of listening to build up again."];
  [why appendString:@"\n\nIt is not blocked: if it is still switched on and "
                    @"in range it will reappear on this list, at its "
                    @"defaults. To stop hearing about it instead, switch it "
                    @"off."];
  if (entry.camera) {
    // The one part of removing a camera this program cannot do, said plainly
    // rather than left for somebody to discover when pairing fails oddly.
    [why appendString:@"\n\nThe Bluetooth pairing is not octomancer's to "
                      @"undo. To unpair properly, remove this Mac from the "
                      @"camera's Bluetooth setup menu, and remove the camera "
                      @"in System Settings ▸ Bluetooth."];
  }
  alert.informativeText = why;
  NSButton* go = [alert addButtonWithTitle:@"Remove"];
  NSButton* cancel = [alert addButtonWithTitle:@"Cancel"];
  // Return is Cancel. The default on a destructive question should be the one
  // that changes nothing.
  go.keyEquivalent = @"";
  cancel.keyEquivalent = @"\r";
  if ([alert runModal] != NSAlertFirstButtonReturn) return;

  const std::string configPath = _status.daemon.config_path.empty()
                                     ? octo::default_camera_config_path()
                                     : _status.daemon.config_path;
  const std::string controlPath = _controlSocket;
  const std::string benchPath = _benchSocket;
  const bool camera = entry.camera;
  const std::string id = entry.id;

  _busy = true;
  dispatch_async(_queue, ^{
    // The file first. A daemon that is asked to forget and then restarted
    // would read the line straight back in, so the line has to go first for
    // the two halves to end up agreeing however the order works out.
    octo::CamConf conf;
    std::string err;
    bool ok = conf.load(configPath, &err);
    if (ok) {
      ok = camera ? conf.forget_camera(id, &err) : conf.forget_box(id, &err);
    }

    // Then the daemon holding what was learned. Reported but not fatal: the
    // settings are already gone, and a daemon that did not answer will have
    // nothing to say about the device once it restarts either.
    std::string reply, derr;
    const std::string ask =
        camera ? "forget camera=" + id : "forget " + id;
    const bool told =
        octo::query(camera ? controlPath : benchPath, ask, &reply, &derr, 5.0);

    dispatch_async(dispatch_get_main_queue(), ^{
      self->_busy = false;
      if (!ok) {
        [self complain:@"Could not remove that device."
                  info:[NSString stringWithFormat:@"%@\n\n%@", ns(err),
                                                  ns(configPath)]];
        return;
      }
      // The selection pointed at something that no longer exists; letting the
      // picker keep it would leave the page showing a device it can no longer
      // find and cannot act on.
      self->_deviceSelectedKey = nil;
      self->_activity.stringValue =
          told ? [NSString stringWithFormat:@"Removed %@.", name]
               : [NSString stringWithFormat:
                               @"Removed %@ from the configuration. The daemon "
                               @"did not answer, so what it had learned is "
                               @"still in memory until it restarts.",
                               name];
      self->_activity.textColor = told ? [NSColor secondaryLabelColor]
                                       : [NSColor systemOrangeColor];
      [self refresh];
    });
  });
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
              @"be started again afterwards, from the System page.",
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
// the "Start at boot" checkbox on the System page is where it is made.
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
