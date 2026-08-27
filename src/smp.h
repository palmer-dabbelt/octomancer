// The Security Manager: pairing, and the key that comes out of it.
//
// Needed for exactly one reason. The Blackmagic control characteristics are
// encrypted, and doc/ble-write-failure-report.md records that they worked
// "against an already-paired camera" -- the Mac had bonded with it long
// before. The dongle is a different device with a different address, so it
// arrives as a stranger and has to pair, which means the camera puts a
// six-digit number on its screen and waits for it to be typed back.
//
// Scope, stated plainly. This implements **LE Legacy pairing**: Just Works and
// Passkey Entry. It does not implement LE Secure Connections, and does not
// offer it -- the AuthReq goes out with the SC bit clear, so a peer that can
// do both will choose legacy. A peer that requires Secure Connections is
// refused with a message that says so rather than failing obscurely. The
// crypto for Secure Connections is already written and tested in crypto.h
// (f4, f5, f6, g2); what is missing is the public key exchange, which needs
// the controller's ECDH commands wired through hcilink. See doc/dongle-notes.md.
//
// Byte order, once more. SMP puts its 128-bit values on the wire least
// significant byte first; crypto.h works most significant byte first. Every
// reversal in this project happens at a PDU boundary, and for SMP that
// boundary is this file.
#ifndef OCTO_SMP_H
#define OCTO_SMP_H

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "crypto.h"
#include "hci.h"

namespace octo {
namespace smp {

enum Code : uint8_t {
  kPairingRequest = 0x01,
  kPairingResponse = 0x02,
  kPairingConfirm = 0x03,
  kPairingRandom = 0x04,
  kPairingFailed = 0x05,
  kEncryptionInformation = 0x06,
  kMasterIdentification = 0x07,
  kIdentityInformation = 0x08,
  kIdentityAddressInformation = 0x09,
  kSigningInformation = 0x0a,
  kSecurityRequest = 0x0b,
  kPairingPublicKey = 0x0c,
  kPairingDhKeyCheck = 0x0d,
  kKeypressNotification = 0x0e,
};

enum Reason : uint8_t {
  kPasskeyEntryFailed = 0x01,
  kOobNotAvailable = 0x02,
  kAuthenticationRequirements = 0x03,
  kConfirmValueFailed = 0x04,
  kPairingNotSupported = 0x05,
  kEncryptionKeySize = 0x06,
  kCommandNotSupported = 0x07,
  kUnspecifiedReason = 0x08,
  kRepeatedAttempts = 0x09,
  kInvalidParameters = 0x0a,
  kDhKeyCheckFailed = 0x0b,
  kNumericComparisonFailed = 0x0c,
};

const char* reason_name(uint8_t reason);

// What each side can do about a number on a screen. Ours is KeyboardOnly: the
// camera displays a passkey and something types it back, which is exactly the
// arrangement doc/ble-write-failure-report.md describes.
enum IoCapability : uint8_t {
  kDisplayOnly = 0x00,
  kDisplayYesNo = 0x01,
  kKeyboardOnly = 0x02,
  kNoInputNoOutput = 0x03,
  kKeyboardDisplay = 0x04,
};

enum AuthReqBits : uint8_t {
  kBondingFlag = 0x01,
  kMitmFlag = 0x04,
  kSecureConnectionsFlag = 0x08,
  kKeypressFlag = 0x10,
};

// How the two sides' capabilities decide what the user has to do.
enum class Method {
  kJustWorks,       // no user involvement, and no protection against a relay
  kPasskeyInput,    // they display a number, we type it
  kPasskeyDisplay,  // we display a number, they type it
  kOob,             // not supported here
};

Method choose_method(uint8_t local_io, uint8_t remote_io, bool mitm_wanted);
const char* method_name(Method m);

// The seven bytes of a Pairing Request or Response, in wire order. Kept whole
// because c1 hashes them verbatim -- reconstructing them from parsed fields is
// an opportunity to differ from what was actually sent, and the failure is a
// confirm mismatch with no clue as to which side is wrong.
struct PairingParams {
  uint8_t code = kPairingRequest;
  uint8_t io_capability = kKeyboardOnly;
  uint8_t oob_flag = 0x00;
  uint8_t auth_req = kBondingFlag | kMitmFlag;
  uint8_t max_key_size = 16;
  uint8_t initiator_keys = 0x00;
  uint8_t responder_keys = 0x00;

  std::vector<uint8_t> encode() const;
  static bool decode(const std::vector<uint8_t>& pdu, PairingParams* out);
};

// Drives legacy pairing as the initiator -- which is the only role needed
// here, because we are the one connecting to the camera.
//
// No radio, no threads, no sockets: PDUs in, PDUs out. That is what lets
// tests/test_smp.cc run two of these against each other and check that they
// agree on a key.
class Initiator {
 public:
  struct Config {
    hci::Address local;   // our address, and its type
    hci::Address remote;  // the camera's
    uint8_t io_capability = kKeyboardOnly;
    bool want_bonding = true;
    // Insisting on protection against a relay attack is what forces Passkey
    // Entry rather than Just Works. Turning it off would make pairing
    // effortless and meaningless.
    bool want_mitm = true;
  };

  // Called when the passkey the camera is displaying is needed. Returning
  // false abandons pairing -- which is what a daemon with nobody at the
  // keyboard should do, rather than guessing at 000000.
  using PasskeyProvider = std::function<bool(uint32_t* passkey)>;

  explicit Initiator(const Config& config);

  void set_passkey_provider(PasskeyProvider fn) { passkey_ = std::move(fn); }

  // The first PDU to send. Sending this is what starts pairing.
  std::vector<uint8_t> begin();

  enum class State {
    kIdle,
    kWaitingForResponse,
    kWaitingForConfirm,
    kWaitingForRandom,
    kReadyToEncrypt,  // the short term key is available
    kFailed,
  };

  // Feed one incoming SMP PDU. Any PDU to send back is put in `out`, which may
  // be empty. Returns false when pairing has failed, with `error` explaining
  // why in words -- and with `out` holding a Pairing Failed to send, because a
  // peer left waiting is a peer that stays unpaired until it times out.
  bool handle(const std::vector<uint8_t>& pdu, std::vector<uint8_t>* out,
              std::string* error);

  State state() const { return state_; }
  Method method() const { return method_; }

  // Valid once the state is kReadyToEncrypt. This is what goes into LE Start
  // Encryption, with an EDIV and Rand of zero, as legacy pairing requires.
  const crypto::Block& stk() const { return stk_; }

  // For the logs and for tests.
  const PairingParams& request() const { return request_; }
  const PairingParams& response() const { return response_; }

 private:
  std::vector<uint8_t> fail(uint8_t reason, const char* text,
                            std::vector<uint8_t>* out, std::string* error);
  crypto::Block confirm_for(const crypto::Block& rand) const;

  Config config_;
  PasskeyProvider passkey_;
  State state_ = State::kIdle;
  Method method_ = Method::kJustWorks;

  PairingParams request_;
  PairingParams response_;
  std::vector<uint8_t> request_bytes_;
  std::vector<uint8_t> response_bytes_;

  crypto::Key128 tk_{};       // the temporary key: the passkey, or zero
  crypto::Block local_rand_{};
  crypto::Block remote_rand_{};
  crypto::Block remote_confirm_{};
  crypto::Block stk_{};
};

// A 128-bit value as SMP carries it: least significant byte first. Both
// directions, so no caller has to remember which way round it goes.
std::vector<uint8_t> block_to_wire(const crypto::Block& b);
crypto::Block block_from_wire(const uint8_t* data, size_t len);

}  // namespace smp
}  // namespace octo

#endif  // OCTO_SMP_H
