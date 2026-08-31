// See src/boxble.h.
#import <CoreBluetooth/CoreBluetooth.h>
#import <Foundation/Foundation.h>

#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "boxble.h"
#include "boxmsg.h"

namespace {

std::string to_std(NSString* s) {
  if (s == nil) return std::string();
  const char* utf8 = [s UTF8String];
  return utf8 ? std::string(utf8) : std::string();
}

// Nordic's UART Service, the same UUIDs firmware/src/blepeer.cc registers.
// RX is what we write to and TX is what we subscribe to: the names are from
// the central's point of view, which is Nordic's convention.
NSString* const kNusService = @"6E400001-B5A3-F393-E0A9-E50E24DCCA9E";
NSString* const kNusRx = @"6E400002-B5A3-F393-E0A9-E50E24DCCA9E";
NSString* const kNusTx = @"6E400003-B5A3-F393-E0A9-E50E24DCCA9E";

}  // namespace

// Everything CoreBluetooth touches lives behind this lock and is reached only
// through the shared state below. The delegate runs on a private queue; the
// loop runs on its own; and the only thing either may do to the other's data
// is take the lock for as long as it takes to move bytes.
struct OctoBleShared {
  std::mutex lock;
  std::vector<uint8_t> rx;        // notified bytes, awaiting the loop
  bool connected = false;
  bool subscribed = false;
  bool gone = false;
  std::string why_gone;
  std::string peer_name;
  // Bytes that arrived with nowhere to put them. A radio link drops where a
  // cable does not, so this is expected to be nonzero on a busy bench and is
  // not by itself a fault.
  uint64_t dropped_rx = 0;
};

// A cap on what may pile up between two pumps. Generous -- a device list is a
// few kilobytes -- and finite, because an unattended daemon must not grow a
// buffer for as long as nothing drains it.
static const size_t kMaxPending = 64 * 1024;

@interface OctoBoxBleDelegate
    : NSObject <CBCentralManagerDelegate, CBPeripheralDelegate>
@property(nonatomic, strong) CBCentralManager* central;
@property(nonatomic, strong) CBPeripheral* peripheral;
@property(nonatomic, strong) CBCharacteristic* rxChar;
@property(nonatomic, strong) NSString* wantName;
@property(nonatomic, assign) std::shared_ptr<OctoBleShared>* state;
@end

@implementation OctoBoxBleDelegate

- (std::shared_ptr<OctoBleShared>)shared {
  return self.state != nullptr ? *self.state : nullptr;
}

- (void)centralManagerDidUpdateState:(CBCentralManager*)central {
  if (central.state != CBManagerStatePoweredOn) {
    auto st = [self shared];
    if (st && central.state != CBManagerStateUnknown &&
        central.state != CBManagerStateResetting) {
      std::lock_guard<std::mutex> g(st->lock);
      st->gone = true;
      st->why_gone = central.state == CBManagerStateUnauthorized
                         ? "this program is not allowed to use Bluetooth"
                         : "the Bluetooth radio is not available";
    }
    return;
  }
  // Filtered by service rather than swept broadly: a control daemon has no
  // business waking every peripheral in the building, and CoreBluetooth
  // matches this against the scan response, which is where the dongle puts
  // its UUID (a 128-bit UUID and a name do not both fit in one advertisement).
  [central scanForPeripheralsWithServices:@[ [CBUUID UUIDWithString:kNusService] ]
                                  options:nil];
}

- (void)centralManager:(CBCentralManager*)central
    didDiscoverPeripheral:(CBPeripheral*)peripheral
        advertisementData:(NSDictionary<NSString*, id>*)advertisementData
                     RSSI:(NSNumber*)RSSI {
  if (self.peripheral != nil) return;  // already committed to one

  NSString* name = advertisementData[CBAdvertisementDataLocalNameKey];
  if (name == nil) name = peripheral.name;
  if (self.wantName.length > 0 &&
      ![self.wantName isEqualToString:(name ?: @"")]) {
    return;
  }

  self.peripheral = peripheral;
  peripheral.delegate = self;
  auto st = [self shared];
  if (st) {
    std::lock_guard<std::mutex> g(st->lock);
    st->peer_name = to_std(name);
  }
  // Stop scanning before connecting. Scanning and connecting at once is a
  // radio doing two things badly, and there is nothing else here to find.
  [central stopScan];
  [central connectPeripheral:peripheral options:nil];
}

- (void)centralManager:(CBCentralManager*)central
    didConnectPeripheral:(CBPeripheral*)peripheral {
  auto st = [self shared];
  if (st) {
    std::lock_guard<std::mutex> g(st->lock);
    st->connected = true;
  }
  [peripheral discoverServices:@[ [CBUUID UUIDWithString:kNusService] ]];
}

- (void)centralManager:(CBCentralManager*)central
    didFailToConnectPeripheral:(CBPeripheral*)peripheral
                         error:(NSError*)error {
  [self giveUp:"the dongle would not accept a connection"];
}

- (void)centralManager:(CBCentralManager*)central
    didDisconnectPeripheral:(CBPeripheral*)peripheral
                      error:(NSError*)error {
  [self giveUp:"the dongle went away"];
}

- (void)giveUp:(const char*)why {
  auto st = [self shared];
  if (st) {
    std::lock_guard<std::mutex> g(st->lock);
    st->connected = false;
    st->subscribed = false;
    st->gone = true;
    if (st->why_gone.empty()) st->why_gone = why;
  }
}

- (void)peripheral:(CBPeripheral*)peripheral
    didDiscoverServices:(NSError*)error {
  if (error != nil || peripheral.services.count == 0) {
    [self giveUp:"the dongle has no box-protocol service"];
    return;
  }
  for (CBService* service in peripheral.services) {
    [peripheral discoverCharacteristics:@[
      [CBUUID UUIDWithString:kNusRx], [CBUUID UUIDWithString:kNusTx]
    ]
                             forService:service];
  }
}

- (void)peripheral:(CBPeripheral*)peripheral
    didDiscoverCharacteristicsForService:(CBService*)service
                                   error:(NSError*)error {
  if (error != nil) {
    [self giveUp:"the dongle's service would not describe itself"];
    return;
  }
  for (CBCharacteristic* ch in service.characteristics) {
    if ([ch.UUID isEqual:[CBUUID UUIDWithString:kNusTx]]) {
      [peripheral setNotifyValue:YES forCharacteristic:ch];
    } else if ([ch.UUID isEqual:[CBUUID UUIDWithString:kNusRx]]) {
      self.rxChar = ch;
    }
  }
}

- (void)peripheral:(CBPeripheral*)peripheral
    didUpdateNotificationStateForCharacteristic:(CBCharacteristic*)ch
                                          error:(NSError*)error {
  if (error != nil) {
    [self giveUp:"the dongle would not send notifications"];
    return;
  }
  auto st = [self shared];
  // Both halves, because a link that can be heard and not spoken to is not a
  // link: the daemon's first act is to ask a question.
  if (st && ch.isNotifying && self.rxChar != nil) {
    std::lock_guard<std::mutex> g(st->lock);
    st->subscribed = true;
  }
}

- (void)peripheral:(CBPeripheral*)peripheral
    didUpdateValueForCharacteristic:(CBCharacteristic*)ch
                              error:(NSError*)error {
  if (error != nil || ch.value == nil) return;
  auto st = [self shared];
  if (!st) return;
  const uint8_t* bytes = static_cast<const uint8_t*>(ch.value.bytes);
  const size_t len = ch.value.length;
  std::lock_guard<std::mutex> g(st->lock);
  if (st->rx.size() + len > kMaxPending) {
    st->dropped_rx += len;
    return;
  }
  st->rx.insert(st->rx.end(), bytes, bytes + len);
}

@end

namespace octo {
namespace {

class BleBoxLink : public BoxTransport {
 public:
  BleBoxLink() : state_(std::make_shared<OctoBleShared>()) {}

  ~BleBoxLink() override { shutdown(); }

  bool start(const std::string& want_name, std::string* err) {
    delegate_ = [[OctoBoxBleDelegate alloc] init];
    delegate_.wantName = want_name.empty()
                             ? @""
                             : [NSString stringWithUTF8String:want_name.c_str()];
    held_ = state_;
    delegate_.state = &held_;
    queue_ = dispatch_queue_create("com.dabbelt.octomancer.boxble", nullptr);
    delegate_.central = [[CBCentralManager alloc] initWithDelegate:delegate_
                                                             queue:queue_];
    if (delegate_.central == nil) {
      if (err != nullptr) *err = "no CoreBluetooth on this host";
      return false;
    }
    return true;
  }

  // ------------------------------------------------------- BoxTransport

  void send(const std::string& line) override {
    if (!ready()) return;
    std::string out = line;
    out += '\n';
    NSData* data = [NSData dataWithBytes:out.data() length:out.size()];
    OctoBoxBleDelegate* d = delegate_;
    // With response rather than without: these are commands, they are small
    // and rare, and the acknowledgement is the flow control that stops a burst
    // being dropped by a controller with three buffers.
    dispatch_async(queue_, ^{
      if (d.peripheral != nil && d.rxChar != nil) {
        [d.peripheral writeValue:data
               forCharacteristic:d.rxChar
                            type:CBCharacteristicWriteWithResponse];
      }
    });
  }

  void send(const Message& msg) override { send(encode(msg)); }

  void on_line(LineHandler h) override { on_line_ = std::move(h); }
  void on_message(MessageHandler h) override { on_message_ = std::move(h); }
  void on_closed(ClosedHandler h) override { on_closed_ = std::move(h); }

  void close(const std::string& why) override {
    if (closed_) return;
    closed_ = true;
    shutdown();
    if (on_closed_) on_closed_(why);
  }

  bool is_open() const override { return !closed_; }

  bool ready() const override {
    if (closed_) return false;
    std::lock_guard<std::mutex> g(state_->lock);
    return state_->connected && state_->subscribed;
  }

  std::string name() const override {
    std::lock_guard<std::mutex> g(state_->lock);
    return state_->peer_name.empty() ? std::string("bluetooth")
                                     : state_->peer_name + " (bluetooth)";
  }

  uint64_t long_lines() const override { return long_lines_; }
  uint64_t bad_lines() const override { return bad_lines_; }

  // The loop's thread, and the only place a handler ever runs.
  void pump() override {
    if (closed_) return;

    std::vector<uint8_t> bytes;
    bool gone = false;
    std::string why;
    {
      std::lock_guard<std::mutex> g(state_->lock);
      bytes.swap(state_->rx);
      gone = state_->gone;
      why = state_->why_gone;
    }

    // Bytes first, then the break. The last thing a box says before it goes is
    // often the reason it went, and a teardown that discarded it would throw
    // away the only account there is. Same order as src/boxcdc.cc, and for the
    // same reason.
    if (!bytes.empty()) {
      std::vector<std::string> lines;
      if (!reader_.feed(reinterpret_cast<const char*>(bytes.data()),
                        bytes.size(), &lines)) {
        ++long_lines_;
      }
      for (const std::string& line : lines) {
        if (closed_) return;
        if (on_line_) on_line_(line);
        Message msg;
        std::string derr;
        if (decode(line, &msg, &derr)) {
          if (on_message_) on_message_(msg);
        } else {
          ++bad_lines_;
        }
      }
    }

    if (gone && !closed_) close(why.empty() ? "the link went away" : why);
  }

 private:
  void shutdown() {
    if (delegate_ == nil) return;
    OctoBoxBleDelegate* d = delegate_;
    delegate_ = nil;
    // On the delegate's own queue, so a callback in flight finishes before the
    // manager it belongs to is torn down.
    dispatch_sync(queue_, ^{
      if (d.central != nil) {
        [d.central stopScan];
        if (d.peripheral != nil) [d.central cancelPeripheralConnection:d.peripheral];
      }
      d.state = nullptr;
    });
    held_.reset();
  }

  std::shared_ptr<OctoBleShared> state_;
  // A second reference whose *address* the delegate holds, so that the
  // delegate can take its own copy without reaching into this object. Cleared
  // on the delegate's queue before this is destroyed.
  std::shared_ptr<OctoBleShared> held_;
  OctoBoxBleDelegate* delegate_ = nil;
  dispatch_queue_t queue_ = nullptr;

  LineReader reader_;
  LineHandler on_line_;
  MessageHandler on_message_;
  ClosedHandler on_closed_;
  bool closed_ = false;
  uint64_t long_lines_ = 0;
  uint64_t bad_lines_ = 0;
};

}  // namespace

std::unique_ptr<BoxTransport> open_box_ble(const std::string& want_name,
                                           std::string* err) {
  std::unique_ptr<BleBoxLink> link(new BleBoxLink());
  if (!link->start(want_name, err)) return nullptr;
  return link;
}

}  // namespace octo
