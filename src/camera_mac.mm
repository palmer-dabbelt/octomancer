// CoreBluetooth central for the camera: the only part of octomancer that
// connects to a device and writes to it.
//
// The awkward part of CoreBluetooth is that everything is a callback on a
// private queue, while the program above wants to say "connect, then write,
// then read back". So each operation here posts its request to the queue and
// then blocks on a condition variable until the matching delegate callback
// fires or the deadline passes. Timeouts are mandatory rather than optional:
// a camera that goes to sleep mid-connection produces no callback at all, and
// an unattended overnight run must not wedge on it.
#import <CoreBluetooth/CoreBluetooth.h>
#import <Foundation/Foundation.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <string>
#include <vector>

#include "bmd.h"
#include "camera.h"
#include "tentacle.h"
#include "timeutil.h"

namespace {

std::string to_std(NSString* s) {
  if (s == nil) return std::string();
  const char* utf8 = [s UTF8String];
  return utf8 ? std::string(utf8) : std::string();
}

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return s;
}

// A name is only ever a guess, but it is the guess that finds a camera whose
// service UUID is not in the advertisement -- which is most of them, since the
// camera service is only listed once connected on some firmware.
bool looks_like_a_camera(const std::string& name) {
  static const char* kHints[] = {"blackmagic", "bmpcc",         "bmcc",
                                 "bmd",        "ursa",          "pocket",
                                 "pyxis",      "cinema camera", "studio camera"};
  const std::string n = lower(name);
  if (n.empty()) return false;
  for (const char* hint : kHints) {
    if (n.find(hint) != std::string::npos) return true;
  }
  return false;
}

struct Seen {
  octo::CameraDevice device;
  bool is_tentacle = false;
  bool is_camera = false;
};

}  // namespace

@class OctoCameraDelegate;

namespace octo {
namespace {

class MacCameraLink : public CameraLink {
 public:
  MacCameraLink() = default;
  ~MacCameraLink() override;

  bool start(std::string* err);

  bool ready(double timeout, std::string* err) override;
  ScanResult scan(double seconds, const std::string& name_hint,
                  bool want_all) override;
  bool connect(const std::string& id, double timeout, std::string* err) override;
  void disconnect() override;
  bool connected() const override;
  bool subscribe(double timeout, std::string* err) override;
  bool write_control(const std::vector<uint8_t>& packet, double timeout,
                     std::string* err) override;
  CameraView view() override;
  void forget_timecode() override;
  CameraView await_state(double seconds) override;

  // --- called from the delegate, on the CoreBluetooth queue ---------------
  void on_state(const std::string& state);
  void on_advert(const std::string& id, const std::string& name, int rssi,
                 bool is_camera, bool is_tentacle);
  void on_connected(bool ok, const std::string& err);
  void on_disconnected();
  void on_subscribed(bool is_outgoing);
  void on_written(bool ok, const std::string& err);
  void on_timecode(const uint8_t* data, size_t len);
  void on_incoming(const uint8_t* data, size_t len);

  CBCentralManager* central = nil;
  OctoCameraDelegate* delegate = nil;
  dispatch_queue_t queue = nil;

  std::mutex mu;
  std::condition_variable cv;

  std::string radio_state;
  bool scanning = false;
  std::map<std::string, Seen> seen;

  bool link_up = false;
  bool connect_done = false;
  bool connect_ok = false;
  std::string connect_error;

  int notify_ready = 0;
  // Tracked here rather than read off the delegate: the predicate below runs
  // on the caller's thread while the delegate is mutated on CoreBluetooth's,
  // and a nonatomic property read across that boundary is a data race.
  bool have_outgoing = false;
  bool write_done = false;
  bool write_ok = false;
  std::string write_error;

  CameraView live;

 private:
  template <class Pred>
  bool wait_for(std::unique_lock<std::mutex>& lock, double seconds, Pred pred) {
    return cv.wait_for(lock, std::chrono::duration<double>(seconds), pred);
  }
};

}  // namespace
}  // namespace octo

@interface OctoCameraDelegate
    : NSObject <CBCentralManagerDelegate, CBPeripheralDelegate>
@property(nonatomic, assign) octo::MacCameraLink* link;
@property(nonatomic, strong) CBPeripheral* peripheral;
@property(nonatomic, strong) NSMutableDictionary<NSString*, CBPeripheral*>* found;
@property(nonatomic, strong) CBCharacteristic* outgoing;
@property(nonatomic, strong) CBUUID* svcCamera;
@property(nonatomic, strong) CBUUID* chOutgoing;
@property(nonatomic, strong) CBUUID* chIncoming;
@property(nonatomic, strong) CBUUID* chTimecode;
@end

@implementation OctoCameraDelegate

- (void)centralManagerDidUpdateState:(CBCentralManager*)central {
  const char* name = "unknown";
  switch (central.state) {
    case CBManagerStatePoweredOn: name = "poweredOn"; break;
    case CBManagerStatePoweredOff: name = "poweredOff"; break;
    case CBManagerStateUnauthorized: name = "unauthorized"; break;
    case CBManagerStateUnsupported: name = "unsupported"; break;
    case CBManagerStateResetting: name = "resetting"; break;
    default: break;
  }
  if (self.link) self.link->on_state(name);
}

- (void)centralManager:(CBCentralManager*)central
    didDiscoverPeripheral:(CBPeripheral*)peripheral
        advertisementData:(NSDictionary<NSString*, id>*)advertisementData
                     RSSI:(NSNumber*)RSSI {
  (void)central;
  if (!self.link) return;

  NSString* key = peripheral.identifier.UUIDString;
  self.found[key] = peripheral;

  NSString* local = advertisementData[CBAdvertisementDataLocalNameKey];
  NSString* name = [local isKindOfClass:[NSString class]] ? local : peripheral.name;

  bool is_camera = false;
  NSArray<CBUUID*>* uuids = advertisementData[CBAdvertisementDataServiceUUIDsKey];
  if ([uuids isKindOfClass:[NSArray class]]) {
    for (CBUUID* u in uuids) {
      if ([u isEqual:self.svcCamera]) { is_camera = true; break; }
    }
  }

  // A Tentacle broadcasting FDAC service data is positively identified, and
  // must never be offered as a camera however it is named.
  bool is_tentacle = false;
  NSDictionary<CBUUID*, NSData*>* sd =
      advertisementData[CBAdvertisementDataServiceDataKey];
  if ([sd isKindOfClass:[NSDictionary class]]) {
    for (CBUUID* k in sd) {
      NSString* s = [k.UUIDString lowercaseString];
      if ([s hasPrefix:@"fdac"] || [s isEqualToString:@(octo::kFdacUuid)]) {
        is_tentacle = true;
        break;
      }
    }
  }

  self.link->on_advert(to_std(key), to_std(name), RSSI ? RSSI.intValue : 0,
                       is_camera, is_tentacle);
}

- (void)centralManager:(CBCentralManager*)central
    didConnectPeripheral:(CBPeripheral*)peripheral {
  (void)central;
  peripheral.delegate = self;
  self.peripheral = peripheral;
  if (self.link) self.link->on_connected(true, "");
}

- (void)centralManager:(CBCentralManager*)central
    didFailToConnectPeripheral:(CBPeripheral*)peripheral
                         error:(NSError*)error {
  (void)central;
  (void)peripheral;
  if (self.link) {
    self.link->on_connected(false, error ? to_std(error.localizedDescription)
                                         : "connect failed");
  }
}

- (void)centralManager:(CBCentralManager*)central
    didDisconnectPeripheral:(CBPeripheral*)peripheral
                      error:(NSError*)error {
  (void)central;
  (void)peripheral;
  (void)error;
  self.outgoing = nil;
  if (self.link) self.link->on_disconnected();
}

- (void)peripheral:(CBPeripheral*)peripheral
    didDiscoverServices:(NSError*)error {
  // A discovery error is left to time out rather than reported as progress.
  // Unblocking here would let subscribe() succeed with nothing discovered, and
  // the first write would then fail with a misleading reason.
  if (error != nil) return;
  for (CBService* svc in peripheral.services) {
    [peripheral discoverCharacteristics:nil forService:svc];
  }
}

- (void)peripheral:(CBPeripheral*)peripheral
    didDiscoverCharacteristicsForService:(CBService*)service
                                   error:(NSError*)error {
  if (error != nil) return;
  for (CBCharacteristic* ch in service.characteristics) {
    if ([ch.UUID isEqual:self.chOutgoing]) {
      self.outgoing = ch;
      if (self.link) self.link->on_subscribed(true);
    } else if ([ch.UUID isEqual:self.chTimecode] ||
               [ch.UUID isEqual:self.chIncoming]) {
      [peripheral setNotifyValue:YES forCharacteristic:ch];
    }
  }
}

- (void)peripheral:(CBPeripheral*)peripheral
    didUpdateNotificationStateForCharacteristic:(CBCharacteristic*)characteristic
                                          error:(NSError*)error {
  (void)peripheral;
  (void)characteristic;
  (void)error;
  if (self.link) self.link->on_subscribed(false);
}

- (void)peripheral:(CBPeripheral*)peripheral
    didUpdateValueForCharacteristic:(CBCharacteristic*)characteristic
                              error:(NSError*)error {
  (void)peripheral;
  if (error != nil || characteristic.value == nil || !self.link) return;
  const uint8_t* bytes = static_cast<const uint8_t*>(characteristic.value.bytes);
  const size_t len = characteristic.value.length;
  if ([characteristic.UUID isEqual:self.chTimecode]) {
    self.link->on_timecode(bytes, len);
  } else if ([characteristic.UUID isEqual:self.chIncoming]) {
    self.link->on_incoming(bytes, len);
  }
}

- (void)peripheral:(CBPeripheral*)peripheral
    didWriteValueForCharacteristic:(CBCharacteristic*)characteristic
                             error:(NSError*)error {
  (void)peripheral;
  (void)characteristic;
  if (self.link) {
    self.link->on_written(error == nil,
                          error ? to_std(error.localizedDescription) : "");
  }
}

@end

namespace octo {
namespace {

MacCameraLink::~MacCameraLink() {
  disconnect();
  @autoreleasepool {
    if (central != nil) {
      [central stopScan];
      central.delegate = nil;
      central = nil;
    }
    if (delegate != nil) {
      delegate.link = nullptr;
      delegate = nil;
    }
    queue = nil;
  }
}

bool MacCameraLink::start(std::string* err) {
  @autoreleasepool {
    queue = dispatch_queue_create("dev.dabbelt.octomancer.camera",
                                  DISPATCH_QUEUE_SERIAL);
    delegate = [[OctoCameraDelegate alloc] init];
    delegate.link = this;
    delegate.found = [NSMutableDictionary dictionary];
    delegate.svcCamera = [CBUUID UUIDWithString:@(bmd::kServiceCamera)];
    delegate.chOutgoing = [CBUUID UUIDWithString:@(bmd::kCharOutgoingControl)];
    delegate.chIncoming = [CBUUID UUIDWithString:@(bmd::kCharIncomingControl)];
    delegate.chTimecode = [CBUUID UUIDWithString:@(bmd::kCharTimecode)];

    // No power alert: this can run under launchd, where a modal asking about
    // Bluetooth has nobody to answer it.
    central = [[CBCentralManager alloc]
        initWithDelegate:delegate
                   queue:queue
                 options:@{CBCentralManagerOptionShowPowerAlertKey : @NO}];
    if (central == nil) {
      if (err) *err = "could not create a CBCentralManager";
      return false;
    }
  }
  return true;
}

void MacCameraLink::on_state(const std::string& state) {
  {
    std::lock_guard<std::mutex> lock(mu);
    radio_state = state;
  }
  cv.notify_all();
}

void MacCameraLink::on_advert(const std::string& id, const std::string& name,
                              int rssi, bool is_camera, bool is_tentacle) {
  std::lock_guard<std::mutex> lock(mu);
  if (!scanning) return;
  Seen& s = seen[id];
  s.device.id = id;
  // Later adverts often carry a name where the first did not; never let one
  // overwrite a known name with an empty string.
  if (!name.empty()) s.device.name = name;
  s.device.rssi = rssi;
  s.is_camera = s.is_camera || is_camera;
  s.is_tentacle = s.is_tentacle || is_tentacle;
  s.device.by_service_uuid = s.is_camera;
}

void MacCameraLink::on_connected(bool ok, const std::string& err) {
  {
    std::lock_guard<std::mutex> lock(mu);
    connect_done = true;
    connect_ok = ok;
    connect_error = err;
    link_up = ok;
  }
  cv.notify_all();
}

void MacCameraLink::on_disconnected() {
  {
    std::lock_guard<std::mutex> lock(mu);
    link_up = false;
    notify_ready = 0;
    have_outgoing = false;
    live = CameraView();
  }
  cv.notify_all();
}

void MacCameraLink::on_subscribed(bool is_outgoing) {
  {
    std::lock_guard<std::mutex> lock(mu);
    notify_ready += 1;
    if (is_outgoing) have_outgoing = true;
  }
  cv.notify_all();
}

void MacCameraLink::on_written(bool ok, const std::string& err) {
  {
    std::lock_guard<std::mutex> lock(mu);
    write_done = true;
    write_ok = ok;
    write_error = err;
  }
  cv.notify_all();
}

void MacCameraLink::on_timecode(const uint8_t* data, size_t len) {
  bmd::Timecode tc;
  if (!bmd::parse_timecode(data, len, &tc)) return;
  {
    std::lock_guard<std::mutex> lock(mu);
    live.has_timecode = true;
    live.timecode = tc;
    live.timecode_mono = mono_now();
  }
  cv.notify_all();
}

void MacCameraLink::on_incoming(const uint8_t* data, size_t len) {
  std::vector<bmd::Value> values;
  for (const bmd::Message& msg : bmd::parse_stream(data, len)) {
    bmd::Value v;
    if (bmd::decode_value(msg, &v)) values.push_back(std::move(v));
  }
  if (values.empty()) return;
  {
    std::lock_guard<std::mutex> lock(mu);
    for (const bmd::Value& v : values) {
      live.state[{v.group, v.param}] = v;
      if (v.group == bmd::kGroupMedia && v.param == bmd::kParamTransport &&
          !v.ints.empty()) {
        live.has_transport = true;
        live.transport = v.ints[0];
      }
      if (v.group == bmd::kGroupVideo && v.param == bmd::kParamFrameRate &&
          !v.ints.empty() && v.ints[0] > 0) {
        live.has_fps = true;
        live.fps = static_cast<int>(v.ints[0]);
      }
    }
  }
  cv.notify_all();
}

bool MacCameraLink::ready(double timeout, std::string* err) {
  std::unique_lock<std::mutex> lock(mu);
  wait_for(lock, timeout, [this] { return !radio_state.empty(); });
  if (radio_state == "poweredOn") return true;
  if (err) {
    if (radio_state.empty()) {
      *err = "Bluetooth did not report a state; is this binary allowed to use"
             " it? macOS refuses silently rather than with an error.";
    } else if (radio_state == "unauthorized") {
      *err = "not authorised to use Bluetooth -- grant it in System Settings >"
             " Privacy & Security > Bluetooth";
    } else {
      *err = "Bluetooth is " + radio_state;
    }
  }
  return false;
}

ScanResult MacCameraLink::scan(double seconds, const std::string& name_hint,
                               bool want_all) {
  @autoreleasepool {
    {
      std::lock_guard<std::mutex> lock(mu);
      seen.clear();
      scanning = true;
    }
    [delegate.found removeAllObjects];
    // Unfiltered, for the same reason the Tentacle scanner is: a filter
    // matches advertised service UUIDs, and the camera does not reliably list
    // its service. Unfiltered also gives an honest count of how many LE
    // devices answered, which is what separates "no camera" from "no radio".
    [central scanForPeripheralsWithServices:nil options:nil];
  }

  std::this_thread::sleep_for(std::chrono::duration<double>(seconds));

  @autoreleasepool {
    [central stopScan];
  }

  ScanResult result;
  std::lock_guard<std::mutex> lock(mu);
  scanning = false;
  result.total = static_cast<int>(seen.size());
  for (const auto& entry : seen) {
    const Seen& s = entry.second;
    if (want_all) result.all.push_back(s.device);
    if (s.is_tentacle) {
      result.tentacles += 1;
      continue;  // positively identified as something else
    }
    if (s.is_camera || looks_like_a_camera(s.device.name)) {
      result.cameras.push_back(s.device);
    } else if (!name_hint.empty() &&
               (lower(s.device.name).find(lower(name_hint)) != std::string::npos ||
                lower(s.device.id) == lower(name_hint))) {
      // Allow targeting anything seen by name, not just things that looked
      // like cameras -- but never a Tentacle, which is why this sits below
      // the is_tentacle check rather than above it.
      result.cameras.push_back(s.device);
    }
  }

  // Trust the UUID matches ahead of the name guesses, then the strongest
  // signal, so an unattended run picks the camera in the room over one two
  // floors away.
  std::sort(result.cameras.begin(), result.cameras.end(),
            [](const CameraDevice& a, const CameraDevice& b) {
              if (a.by_service_uuid != b.by_service_uuid)
                return a.by_service_uuid;
              return a.rssi > b.rssi;
            });
  std::sort(result.all.begin(), result.all.end(),
            [](const CameraDevice& a, const CameraDevice& b) {
              return a.rssi > b.rssi;
            });
  return result;
}

bool MacCameraLink::connect(const std::string& id, double timeout,
                            std::string* err) {
  __block CBPeripheral* target = nil;
  @autoreleasepool {
    NSString* key = @(id.c_str());
    target = delegate.found[key];
    if (target == nil) {
      // Not seen this run. CoreBluetooth can often still hand back a
      // peripheral it already knows, which is what makes reconnecting without
      // a 20-second scan possible at all.
      NSUUID* uuid = [[NSUUID alloc] initWithUUIDString:key];
      if (uuid != nil) {
        NSArray<CBPeripheral*>* known =
            [central retrievePeripheralsWithIdentifiers:@[ uuid ]];
        if (known.count > 0) {
          target = known[0];
          delegate.found[key] = target;
        }
      }
    }
    if (target == nil) {
      if (err) *err = "not in range and not known to CoreBluetooth";
      return false;
    }

    {
      std::lock_guard<std::mutex> lock(mu);
      connect_done = false;
      connect_ok = false;
      connect_error.clear();
      notify_ready = 0;
      have_outgoing = false;
      live = CameraView();
    }
    delegate.peripheral = target;
    [central connectPeripheral:target options:nil];
  }

  std::unique_lock<std::mutex> lock(mu);
  if (!wait_for(lock, timeout, [this] { return connect_done; })) {
    lock.unlock();
    @autoreleasepool {
      [central cancelPeripheralConnection:target];
    }
    if (err) *err = "timed out";
    return false;
  }
  if (!connect_ok && err) *err = connect_error;
  return connect_ok;
}

void MacCameraLink::disconnect() {
  @autoreleasepool {
    if (central != nil && delegate != nil && delegate.peripheral != nil) {
      [central cancelPeripheralConnection:delegate.peripheral];
    }
  }
  std::unique_lock<std::mutex> lock(mu);
  wait_for(lock, 2.0, [this] { return !link_up; });
  link_up = false;
}

bool MacCameraLink::connected() const {
  return link_up;
}

bool MacCameraLink::subscribe(double timeout, std::string* err) {
  @autoreleasepool {
    if (delegate.peripheral == nil) {
      if (err) *err = "not connected";
      return false;
    }
    [delegate.peripheral discoverServices:nil];
  }

  std::unique_lock<std::mutex> lock(mu);
  // Three callbacks are expected: the outgoing characteristic being found and
  // the two notifications turning on. Waiting for all three would hang on a
  // body that does not expose one of them, so this waits for the first and
  // then gives the rest a short grace period.
  // The outgoing control characteristic is the one that must exist: without
  // it there is no way to set a clock, and saying so now beats a write that
  // fails later for a reason that sounds like a different problem.
  if (!wait_for(lock, timeout, [this] { return have_outgoing; })) {
    if (err) {
      *err = notify_ready > 0
                 ? "no camera-control characteristic -- this looks like a"
                   " Bluetooth device, but not a Blackmagic camera"
                 : "no camera-control characteristics appeared";
    }
    return false;
  }
  // Then a short grace period for the two notifications to come up, so the
  // first look at the camera is not blind.
  wait_for(lock, 2.0, [this] { return notify_ready >= 3; });
  return true;
}

bool MacCameraLink::write_control(const std::vector<uint8_t>& packet,
                                  double timeout, std::string* err) {
  @autoreleasepool {
    if (delegate.peripheral == nil || delegate.outgoing == nil) {
      if (err) *err = "no outgoing control characteristic";
      return false;
    }
    {
      std::lock_guard<std::mutex> lock(mu);
      write_done = false;
      write_ok = false;
      write_error.clear();
    }
    NSData* data = [NSData dataWithBytes:packet.data() length:packet.size()];
    // With a response, always: an unacknowledged write cannot tell a camera
    // that rejected the packet from one that never heard it.
    [delegate.peripheral writeValue:data
                  forCharacteristic:delegate.outgoing
                               type:CBCharacteristicWriteWithResponse];
  }

  std::unique_lock<std::mutex> lock(mu);
  if (!wait_for(lock, timeout, [this] { return write_done; })) {
    if (err) *err = "write timed out";
    return false;
  }
  if (!write_ok && err) *err = write_error;
  return write_ok;
}

CameraView MacCameraLink::view() {
  std::lock_guard<std::mutex> lock(mu);
  return live;
}

void MacCameraLink::forget_timecode() {
  std::lock_guard<std::mutex> lock(mu);
  live.has_timecode = false;
}

CameraView MacCameraLink::await_state(double seconds) {
  std::unique_lock<std::mutex> lock(mu);
  wait_for(lock, seconds,
           [this] { return live.has_timecode && live.has_transport; });
  return live;
}

}  // namespace

std::unique_ptr<CameraLink> make_camera_link() {
  auto link = std::unique_ptr<MacCameraLink>(new MacCameraLink());
  std::string err;
  if (!link->start(&err)) return nullptr;
  return link;
}

}  // namespace octo
