// The camera, over the dongle, on the loop.
//
// This is what src/camera_hci.cc used to implement behind camera.h's blocking
// CameraLink, and the reason it could not stay that way is worth recording.
// Its old shape was: hold a mutex, send something, and wait on a condition
// variable for hcilink's reader thread to signal it. Take the reader thread
// away -- which the box requires, because the Zephyr SDK's libstdc++ has no
// std::thread in any multilib -- and the last remaining thread parks on a
// condition variable that nothing alive can signal. Not a slow path: a
// guaranteed deadlock, at src/camera_hci.cc:366 and :440 as it was written.
//
// So the wait is gone rather than moved. Everything here posts work and
// returns; the answer arrives as a completion on the loop's one thread, and
// there is nothing to lock because there is nothing else running.
//
// The one contract that genuinely changed shape is await_state(). A caller
// used to block until the camera had volunteered both a timecode and a
// transport mode. There is nobody to block, so instead the camera says what it
// knows as it learns it -- set_view_handler -- and the caller decides when it
// has enough. That is a better fit for what a sync daemon actually does with
// the information anyway: it does not want a snapshot at a moment of its
// choosing, it wants to be told.
//
// Pairing is the other thing that cannot be hidden and should not be.
// CoreBluetooth pairs by putting a panel on the screen and remembers the bond
// in the system keychain, which is why doc/ble-write-failure-report.md could
// observe that everything "worked immediately". The dongle has no keychain and
// no screen. It arrives as a stranger with an address the camera has never
// seen, so the camera displays a six-digit passkey and waits -- and the passkey
// has to come from somewhere the caller supplies. See doc/dongle-notes.md.
#ifndef OCTO_CAMHCI_H
#define OCTO_CAMHCI_H

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "att.h"
#include "camasync.h"
#include "camera.h"
#include "hci.h"
#include "hcilink.h"
#include "loop.h"
#include "smp.h"

namespace octo {

// The interface is src/camasync.h's, so that the sync daemon can be driven by
// a fake camera in a test and by this one on a rig without knowing which it
// has. Everything below that is not in AsyncCamera -- opening the dongle,
// supplying a passkey -- is here because it has no counterpart on any other
// backend: CoreBluetooth has no dongle to open and pairs by putting a panel on
// the screen.
class HciCamera : public AsyncCamera {
 public:
  // Asked for the six-digit number the camera is displaying. Returning false
  // abandons the pairing, which is the honest outcome when there is nobody to
  // ask -- under launchd there is not.
  using PasskeyProvider = std::function<bool(uint32_t* passkey)>;

  // Opens the dongle. As with hci::Link, the port either opens or does not and
  // says so straight away, while the controller coming up is several round
  // trips later and arrives at `on_ready`.
  static std::unique_ptr<HciCamera> open(Loop* loop, DoneHandler on_ready,
                                         std::string* err);
  ~HciCamera() override;

  HciCamera(const HciCamera&) = delete;
  HciCamera& operator=(const HciCamera&) = delete;

  void set_passkey_provider(PasskeyProvider provider);
  void set_view_handler(ViewHandler on_change) override;
  // Called when the camera goes away by itself, which it does whenever it is
  // switched off or walks out of range mid-sync.
  void set_disconnect_handler(std::function<void()> on_gone) override;

  // Listen for `seconds`, then report. Unlike the CoreBluetooth backend this
  // one cannot say "there it is" during the scan: it collects advertisements
  // and classifies them afterwards, so there is no moment during the scan at
  // which it knows it has found a camera.
  void scan(double seconds, const std::string& name_hint, bool want_all,
            ScanHandler done) override;

  // Over the dongle a camera is named by its Bluetooth address, not by the
  // opaque per-host identifier CoreBluetooth invents. Connecting includes
  // negotiating an MTU and discovering the control service, so `done` means
  // "ready to be used", not "the radio link is up".
  void connect(const std::string& id, double timeout,
               DoneHandler done) override;
  void disconnect() override;
  bool connected() const override;

  // Subscribe to the Timecode and Incoming Control characteristics. Once per
  // connection: subscribing twice is an error. This is also where pairing
  // happens, because those characteristics are encrypted and the subscription
  // is what first demands an encrypted link.
  void subscribe(double timeout, DoneHandler done) override;
  bool subscribed() const override;

  void write_control(const std::vector<uint8_t>& packet, double timeout,
                     DoneHandler done) override;

  const CameraView& view() const override;
  // Drop the timecode so the next reading observed is a fresh one rather than
  // the one that arrived before a write landed.
  void forget_timecode() override;

 private:
  // Where a characteristic lives once discovery has found it.
  struct CharHandles {
    uint16_t value = 0;
    uint16_t cccd = 0;
    uint8_t properties = 0;
    bool found() const { return value != 0; }
  };

  explicit HciCamera(Loop* loop);

  void on_ready(bool ok, const std::string& err);
  void on_att(uint16_t conn, const std::vector<uint8_t>& pdu);
  void on_smp(uint16_t conn, const std::vector<uint8_t>& pdu);
  void on_gone(uint16_t handle);

  // Discovery, as a chain of continuations. Each step asks one ATT question
  // and decides from the answer whether to ask another, which is the same walk
  // the blocking version did with a for loop -- the difference is only that
  // the loop is spelled out rather than held on a stack.
  // Same cycle hazard as hci::Link::run_sequence: a lambda that captures the
  // shared_ptr owning it never dies. A member function taking the position as
  // an argument has no cycle to break.
  void try_connect(const hci::Address& peer, size_t type_index, double timeout,
                   DoneHandler done);

  void discover(DoneHandler done);
  void walk_services(uint16_t start, int round, DoneHandler done);
  void walk_chars(uint16_t cursor, int round, DoneHandler done);
  void walk_cccds(size_t index, DoneHandler done);
  void find_cccd(size_t index, uint16_t cursor, uint16_t end, int round,
                 DoneHandler done);
  void finish_discovery(DoneHandler done);

  void subscribe_next(size_t index, double timeout, DoneHandler done);
  void write_cccd(uint16_t cccd, double timeout, DoneHandler done);

  // Pair, then turn on encryption. At most once per connection.
  void ensure_encrypted(double timeout, DoneHandler done);
  void finish_pairing(bool ok, const std::string& err);

  static bool needs_encryption(uint8_t att_error);
  void note(const CameraView& v);

  Loop* loop_ = nullptr;
  std::unique_ptr<hci::Link> link_;
  DoneHandler on_ready_;
  PasskeyProvider passkey_;
  ViewHandler on_view_;
  std::function<void()> on_gone_;

  uint16_t conn_ = 0;
  hci::Address peer_;
  // The timer that ends a scan. Held so the destructor can cancel it: it
  // captures `this`, and a camera torn down mid-scan would otherwise be called
  // back into after it was freed.
  TimerId scan_timer_ = kNoTimer;
  uint16_t mtu_ = att::kDefaultMtu;
  bool subscribed_ = false;

  CharHandles outgoing_;
  CharHandles incoming_;
  CharHandles timecode_;
  CameraView live_;

  // Discovery scratch, live only while discover() is running.
  uint16_t svc_start_ = 0;
  uint16_t svc_end_ = 0;
  std::vector<att::CharDecl> chars_;
  std::map<size_t, uint16_t> chars_cccd_;

  std::unique_ptr<smp::Initiator> pairing_;
  DoneHandler pairing_done_;
  TimerId pairing_timer_ = kNoTimer;
  double pairing_timeout_ = 0.0;
};

}  // namespace octo

#endif  // OCTO_CAMHCI_H
