// CoreBluetooth passive scanner.
//
// Tentacles put the clock in the advertisement itself, so this never connects
// to anything: no pairing, no GATT, no limit on how many boxes are listened to
// at once, and nothing that could interfere with the Tentacle app or with the
// camera holding its own connection.
#import <CoreBluetooth/CoreBluetooth.h>
#import <Foundation/Foundation.h>

#include <string>
#include <vector>

#include "bmd.h"
#include "radio.h"
#include "scanner.h"
#include "tentacle.h"
#include "timeutil.h"

namespace {

std::string to_std(NSString* s) {
  if (s == nil) return std::string();
  const char* utf8 = [s UTF8String];
  return utf8 ? std::string(utf8) : std::string();
}

}  // namespace

@interface OctoScannerDelegate : NSObject <CBCentralManagerDelegate>
@property(nonatomic, assign) octo::Scanner::AdvertHandler* onAdvert;
@property(nonatomic, assign) octo::Scanner::SightingHandler* onCamera;
@property(nonatomic, assign) octo::Scanner::StateHandler* onState;
@property(nonatomic, strong) CBUUID* fdac;
@property(nonatomic, strong) CBUUID* camera;
@end

@implementation OctoScannerDelegate

- (void)reportState:(NSString*)state {
  if (self.onState && *self.onState) (*self.onState)(to_std(state));
}

- (void)centralManagerDidUpdateState:(CBCentralManager*)central {
  switch (central.state) {
    case CBManagerStatePoweredOn:
      [self reportState:@"poweredOn"];
      // Scan unfiltered rather than filtering on FDAC. The filter matches
      // advertised service *UUIDs*, and a Tentacle carries its payload as
      // service *data*, which does not reliably put the UUID in the list a
      // filter looks at. Unfiltered costs a few extra callbacks per second and
      // cannot silently miss a box.
      [central scanForPeripheralsWithServices:nil
                                      options:@{
                                        CBCentralManagerScanOptionAllowDuplicatesKey : @YES
                                      }];
      break;
    case CBManagerStatePoweredOff: [self reportState:@"poweredOff"]; break;
    case CBManagerStateUnauthorized: [self reportState:@"unauthorized"]; break;
    case CBManagerStateUnsupported: [self reportState:@"unsupported"]; break;
    case CBManagerStateResetting: [self reportState:@"resetting"]; break;
    case CBManagerStateUnknown:
    default: [self reportState:@"unknown"]; break;
  }
}

- (void)centralManager:(CBCentralManager*)central
    didDiscoverPeripheral:(CBPeripheral*)peripheral
        advertisementData:(NSDictionary<NSString*, id>*)advertisementData
                     RSSI:(NSNumber*)RSSI {
  (void)central;
  // A camera is recognised by the service it advertises, not by its name.
  // Tentacle boxes are named after the camera they ride on -- there is one on
  // this bench called "BMPCC" -- so a name match would hand the sync daemon a
  // box to connect to and it would spend the night failing to find a control
  // characteristic on it.
  if (self.onCamera && *self.onCamera) {
    NSArray<CBUUID*>* uuids = advertisementData[CBAdvertisementDataServiceUUIDsKey];
    bool is_camera = false;
    if ([uuids isKindOfClass:[NSArray class]]) {
      // Spelled out rather than -containsObject:, to match the comparison in
      // camera_mac.mm exactly. That one is known to identify this bench's body
      // correctly, and two ways of asking the same question is one way too
      // many for something whose failure mode is a camera that never syncs.
      for (CBUUID* u in uuids) {
        if ([u isEqual:self.camera]) { is_camera = true; break; }
      }
    }
    if (is_camera) {
      octo::Sighting seen;
      seen.id = to_std(peripheral.identifier.UUIDString);
      NSString* camName = advertisementData[CBAdvertisementDataLocalNameKey];
      seen.name =
          to_std([camName isKindOfClass:[NSString class]] ? camName : peripheral.name);
      seen.rssi = RSSI ? RSSI.intValue : 0;
      seen.mono = octo::mono_now();
      seen.wall = octo::wall_now();
      (*self.onCamera)(seen);
    }
  }

  NSDictionary<CBUUID*, NSData*>* serviceData =
      advertisementData[CBAdvertisementDataServiceDataKey];
  if (![serviceData isKindOfClass:[NSDictionary class]]) return;

  NSData* payload = serviceData[self.fdac];
  if (payload == nil) {
    // CBUUID equality across 16-bit and 128-bit spellings is not something to
    // rely on, so fall back to comparing the expanded string form.
    for (CBUUID* key in serviceData) {
      if ([[key.UUIDString lowercaseString] hasPrefix:@"fdac"] ||
          [[key.UUIDString lowercaseString] isEqualToString:@(octo::kFdacUuid)]) {
        payload = serviceData[key];
        break;
      }
    }
  }
  if (payload == nil || payload.length == 0) return;  // not a Tentacle

  octo::Advert advert;
  advert.id = to_std(peripheral.identifier.UUIDString);
  NSString* local = advertisementData[CBAdvertisementDataLocalNameKey];
  advert.name = to_std([local isKindOfClass:[NSString class]] ? local : peripheral.name);
  advert.rssi = RSSI ? RSSI.intValue : 0;
  const uint8_t* bytes = static_cast<const uint8_t*>(payload.bytes);
  advert.data.assign(bytes, bytes + payload.length);
  advert.mono = octo::mono_now();
  advert.wall = octo::wall_now();

  if (self.onAdvert && *self.onAdvert) (*self.onAdvert)(advert);
}

@end

namespace octo {

namespace {

class MacScanner : public Scanner {
 public:
  MacScanner(AdvertHandler on_advert, SightingHandler on_camera,
             StateHandler on_state)
      : on_advert_(std::move(on_advert)),
        on_camera_(std::move(on_camera)),
        on_state_(std::move(on_state)) {}

  ~MacScanner() override { stop(); }

  bool start(std::string* err) override {
    @autoreleasepool {
      queue_ = dispatch_queue_create("dev.dabbelt.octomancer.ble",
                                     DISPATCH_QUEUE_SERIAL);
      delegate_ = [[OctoScannerDelegate alloc] init];
      delegate_.onAdvert = &on_advert_;
      delegate_.onCamera = on_camera_ ? &on_camera_ : nullptr;
      delegate_.onState = &on_state_;
      delegate_.fdac = [CBUUID UUIDWithString:@"FDAC"];
      delegate_.camera = [CBUUID UUIDWithString:@(octo::bmd::kServiceCamera)];

      // No power alert: this runs unattended under launchd, where a modal
      // asking about Bluetooth has nobody to answer it.
      central_ = [[CBCentralManager alloc]
          initWithDelegate:delegate_
                     queue:queue_
                   options:@{CBCentralManagerOptionShowPowerAlertKey : @NO}];
      if (central_ == nil) {
        if (err) *err = "could not create a CBCentralManager";
        return false;
      }
    }
    return true;
  }

  void stop() override {
    @autoreleasepool {
      if (central_ != nil) {
        [central_ stopScan];
        central_.delegate = nil;
        central_ = nil;
      }
      if (delegate_ != nil) {
        delegate_.onAdvert = nullptr;
        delegate_.onCamera = nullptr;
        delegate_.onState = nullptr;
        delegate_ = nil;
      }
      queue_ = nil;
    }
  }

 private:
  AdvertHandler on_advert_;
  SightingHandler on_camera_;
  StateHandler on_state_;
  CBCentralManager* central_ = nil;
  OctoScannerDelegate* delegate_ = nil;
  dispatch_queue_t queue_ = nil;
};

}  // namespace

// Renamed from make_ble_scanner: that name now belongs to radio.cc, which
// picks between this and the dongle. Nothing else about this file changed.
std::unique_ptr<Scanner> make_corebluetooth_scanner(
    Scanner::AdvertHandler on_advert, Scanner::SightingHandler on_camera,
    Scanner::StateHandler on_state) {
  return std::unique_ptr<Scanner>(new MacScanner(
      std::move(on_advert), std::move(on_camera), std::move(on_state)));
}

}  // namespace octo
