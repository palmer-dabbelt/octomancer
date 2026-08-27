// Pair with something, without a radio.
//
// The responder below is written out longhand from the specification rather
// than reusing any part of smp.cc, so this is not the initiator agreeing with
// itself: it is two independent implementations of the same exchange arriving
// at the same key. That is the property that matters, because the symptom of
// getting it wrong on real hardware is a camera that refuses to bond and says
// nothing about why.
//
// What this cannot check is whether a Blackmagic camera agrees with the
// specification as read here. That waits for the dongle.
#include "../src/smp.h"
#include "harness.h"

#include <string>
#include <vector>

using namespace octo;
using namespace octo::smp;

namespace {

std::vector<uint8_t> from_hex(const std::string& s) {
  std::vector<uint8_t> out;
  for (size_t i = 0; i + 1 < s.size(); i += 2) {
    out.push_back(static_cast<uint8_t>(std::stoul(s.substr(i, 2), nullptr, 16)));
  }
  return out;
}

hci::Address addr(const char* text, uint8_t type) {
  hci::Address a;
  a.type = type;
  hci::address_from_string(text, &a);
  a.type = type;
  return a;
}

// The other half of the exchange, as a peripheral would run it. Written from
// the specification directly.
class Responder {
 public:
  Responder(const hci::Address& initiator, const hci::Address& self,
            uint32_t passkey, uint8_t io = kDisplayOnly)
      : initiator_(initiator), self_(self), io_(io) {
    tk_.fill(0);
    tk_[12] = static_cast<uint8_t>((passkey >> 24) & 0xff);
    tk_[13] = static_cast<uint8_t>((passkey >> 16) & 0xff);
    tk_[14] = static_cast<uint8_t>((passkey >> 8) & 0xff);
    tk_[15] = static_cast<uint8_t>(passkey & 0xff);
    // A fixed nonce, so a failure here is reproducible rather than occasional.
    for (size_t i = 0; i < 16; ++i) rand_[i] = static_cast<uint8_t>(0xa0 + i);
  }

  std::vector<uint8_t> handle(const std::vector<uint8_t>& pdu) {
    if (pdu.empty()) return {};
    switch (pdu[0]) {
      case kPairingRequest: {
        preq_.assign(pdu.begin(), pdu.begin() + 7);
        PairingParams rsp;
        rsp.code = kPairingResponse;
        rsp.io_capability = io_;
        rsp.oob_flag = 0;
        rsp.auth_req = kBondingFlag | kMitmFlag;
        rsp.max_key_size = 16;
        rsp.initiator_keys = 0;
        rsp.responder_keys = 0;
        pres_ = rsp.encode();
        return pres_;
      }
      case kPairingConfirm: {
        peer_confirm_ = block_from_wire(pdu.data() + 1, 16);
        std::vector<uint8_t> out = {kPairingConfirm};
        std::vector<uint8_t> wire = block_to_wire(confirm(rand_));
        out.insert(out.end(), wire.begin(), wire.end());
        return out;
      }
      case kPairingRandom: {
        peer_rand_ = block_from_wire(pdu.data() + 1, 16);
        if (!crypto::equal(confirm(peer_rand_), peer_confirm_)) {
          failed_ = true;
          return {kPairingFailed, kConfirmValueFailed};
        }
        // The responder's own random goes first into s1.
        stk_ = crypto::s1(tk_, rand_, peer_rand_);
        done_ = true;
        std::vector<uint8_t> out = {kPairingRandom};
        std::vector<uint8_t> wire = block_to_wire(rand_);
        out.insert(out.end(), wire.begin(), wire.end());
        return out;
      }
      default:
        return {};
    }
  }

  bool done() const { return done_; }
  bool failed() const { return failed_; }
  const crypto::Block& stk() const { return stk_; }

 private:
  crypto::Block confirm(const crypto::Block& r) const {
    return crypto::c1(tk_, r, preq_.data(), pres_.data(), initiator_.type,
                      initiator_.bytes.data(), self_.type, self_.bytes.data());
  }

  hci::Address initiator_;
  hci::Address self_;
  uint8_t io_;
  crypto::Key128 tk_{};
  crypto::Block rand_{};
  crypto::Block peer_rand_{};
  crypto::Block peer_confirm_{};
  crypto::Block stk_{};
  std::vector<uint8_t> preq_;
  std::vector<uint8_t> pres_;
  bool done_ = false;
  bool failed_ = false;
};

// Run the two against each other. Returns true when both reached a key.
bool run_pairing(uint32_t camera_shows, uint32_t we_type, crypto::Block* ours,
                 crypto::Block* theirs, std::string* error) {
  hci::Address us = addr("C0:11:22:33:44:55", hci::kAddrRandom);
  hci::Address them = addr("A0:B1:C2:D3:E4:F5", hci::kAddrPublic);

  Initiator::Config cfg;
  cfg.local = us;
  cfg.remote = them;
  cfg.io_capability = kKeyboardOnly;
  cfg.want_mitm = true;
  Initiator initiator(cfg);
  initiator.set_passkey_provider([we_type](uint32_t* out) {
    *out = we_type;
    return true;
  });

  Responder responder(us, them, camera_shows);

  std::vector<uint8_t> pdu = initiator.begin();
  for (int step = 0; step < 8 && !pdu.empty(); ++step) {
    std::vector<uint8_t> reply = responder.handle(pdu);
    if (reply.empty()) break;
    std::vector<uint8_t> next;
    if (!initiator.handle(reply, &next, error)) return false;
    if (initiator.state() == Initiator::State::kReadyToEncrypt) break;
    pdu = next;
  }

  if (initiator.state() != Initiator::State::kReadyToEncrypt) {
    if (error && error->empty()) *error = "pairing did not complete";
    return false;
  }
  if (!responder.done()) {
    if (error) *error = "the responder never derived a key";
    return false;
  }
  if (ours) *ours = initiator.stk();
  if (theirs) *theirs = responder.stk();
  return true;
}

void test_pdu_encoding() {
  PairingParams p;
  p.code = kPairingRequest;
  p.io_capability = kKeyboardOnly;
  p.oob_flag = 0;
  p.auth_req = kBondingFlag | kMitmFlag;
  p.max_key_size = 16;
  p.initiator_keys = 0;
  p.responder_keys = 0;
  CHECK_STR(hci::to_hex(p.encode()), "01" "02" "00" "05" "10" "00" "00");

  PairingParams back;
  CHECK(PairingParams::decode(p.encode(), &back));
  CHECK_EQ(back.io_capability, static_cast<uint8_t>(kKeyboardOnly));
  CHECK_EQ(back.auth_req, static_cast<uint8_t>(0x05));
  CHECK(!PairingParams::decode(from_hex("0102"), &back));

  // 128-bit values go out least significant byte first, which is the reverse
  // of how crypto.h holds them.
  crypto::Block b{};
  for (size_t i = 0; i < 16; ++i) b[i] = static_cast<uint8_t>(i);
  CHECK_STR(hci::to_hex(block_to_wire(b)), "0f0e0d0c0b0a09080706050403020100");
  std::vector<uint8_t> wire = block_to_wire(b);
  CHECK(crypto::equal(block_from_wire(wire.data(), wire.size()), b));
}

void test_method_selection() {
  // The camera displays, we type. This is the case that matters.
  CHECK(choose_method(kKeyboardOnly, kDisplayOnly, true) ==
        Method::kPasskeyInput);
  CHECK(choose_method(kKeyboardOnly, kDisplayYesNo, true) ==
        Method::kPasskeyInput);
  // We display, they type.
  CHECK(choose_method(kDisplayOnly, kKeyboardOnly, true) ==
        Method::kPasskeyDisplay);
  // Nobody can show or take a number, so there is nothing better than Just
  // Works no matter who wants protection.
  CHECK(choose_method(kNoInputNoOutput, kDisplayOnly, true) ==
        Method::kJustWorks);
  CHECK(choose_method(kKeyboardOnly, kNoInputNoOutput, true) ==
        Method::kJustWorks);
  // Without a demand for protection there is no reason to involve a person.
  CHECK(choose_method(kKeyboardOnly, kDisplayOnly, false) == Method::kJustWorks);
}

void test_successful_pairing() {
  crypto::Block ours, theirs;
  std::string err;
  CHECK(run_pairing(424242, 424242, &ours, &theirs, &err));
  // The point of the whole exercise: both sides computed the same key.
  CHECK(crypto::equal(ours, theirs));
  CHECK(!crypto::equal(ours, crypto::Block{}));
}

void test_wrong_passkey_is_caught() {
  // A wrong passkey must fail at the confirm check rather than producing two
  // different keys and an encryption setup that fails later with no
  // explanation.
  crypto::Block ours, theirs;
  std::string err;
  CHECK(!run_pairing(424242, 123456, &ours, &theirs, &err));
  CHECK(err.find("confirm") != std::string::npos);
}

void test_no_passkey_available() {
  // A daemon with nobody at the keyboard has to give up, not guess at 000000 --
  // which would pair with anything willing to accept it.
  Initiator::Config cfg;
  cfg.local = addr("C0:11:22:33:44:55", hci::kAddrRandom);
  cfg.remote = addr("A0:B1:C2:D3:E4:F5", hci::kAddrPublic);
  Initiator init(cfg);
  init.set_passkey_provider([](uint32_t*) { return false; });

  init.begin();
  PairingParams rsp;
  rsp.code = kPairingResponse;
  rsp.io_capability = kDisplayOnly;
  rsp.auth_req = kBondingFlag | kMitmFlag;
  rsp.max_key_size = 16;

  std::vector<uint8_t> out;
  std::string err;
  CHECK(!init.handle(rsp.encode(), &out, &err));
  CHECK(init.state() == Initiator::State::kFailed);
  // A peer left waiting stays unpaired until it times out, so the refusal has
  // to be sent rather than merely recorded.
  CHECK_EQ(out.size(), static_cast<size_t>(2));
  CHECK_EQ(out[0], static_cast<uint8_t>(kPairingFailed));
  CHECK_EQ(out[1], static_cast<uint8_t>(kPasskeyEntryFailed));
}

void test_secure_connections_is_refused_clearly() {
  // A peer that insists on Secure Connections gets a specific answer, not a
  // confirm mismatch three PDUs later.
  Initiator::Config cfg;
  cfg.local = addr("C0:11:22:33:44:55", hci::kAddrRandom);
  cfg.remote = addr("A0:B1:C2:D3:E4:F5", hci::kAddrPublic);
  Initiator init(cfg);
  init.begin();

  PairingParams rsp;
  rsp.code = kPairingResponse;
  rsp.io_capability = kDisplayYesNo;
  rsp.auth_req = kBondingFlag | kMitmFlag | kSecureConnectionsFlag;
  rsp.max_key_size = 16;

  std::vector<uint8_t> out;
  std::string err;
  CHECK(!init.handle(rsp.encode(), &out, &err));
  CHECK(err.find("Secure Connections") != std::string::npos);
  CHECK_EQ(out[1], static_cast<uint8_t>(kAuthenticationRequirements));
}

void test_peer_failure_is_reported() {
  Initiator::Config cfg;
  cfg.local = addr("C0:11:22:33:44:55", hci::kAddrRandom);
  cfg.remote = addr("A0:B1:C2:D3:E4:F5", hci::kAddrPublic);
  Initiator init(cfg);
  init.begin();

  std::vector<uint8_t> out;
  std::string err;
  CHECK(!init.handle({kPairingFailed, kRepeatedAttempts}, &out, &err));
  CHECK(err.find("repeated attempts") != std::string::npos);
}

void test_just_works_still_agrees() {
  // With no demand for protection both sides settle on Just Works and a zero
  // temporary key, and must still arrive at the same short term key.
  hci::Address us = addr("C0:11:22:33:44:55", hci::kAddrRandom);
  hci::Address them = addr("A0:B1:C2:D3:E4:F5", hci::kAddrPublic);

  Initiator::Config cfg;
  cfg.local = us;
  cfg.remote = them;
  cfg.io_capability = kNoInputNoOutput;
  cfg.want_mitm = false;
  Initiator init(cfg);

  Responder responder(us, them, 0, kNoInputNoOutput);
  std::vector<uint8_t> pdu = init.begin();
  std::string err;
  for (int step = 0; step < 8 && !pdu.empty(); ++step) {
    std::vector<uint8_t> reply = responder.handle(pdu);
    if (reply.empty()) break;
    std::vector<uint8_t> next;
    if (!init.handle(reply, &next, &err)) break;
    if (init.state() == Initiator::State::kReadyToEncrypt) break;
    pdu = next;
  }
  CHECK(init.state() == Initiator::State::kReadyToEncrypt);
  CHECK(init.method() == Method::kJustWorks);
  CHECK(crypto::equal(init.stk(), responder.stk()));
}

}  // namespace

int main() {
  test_pdu_encoding();
  test_method_selection();
  test_successful_pairing();
  test_wrong_passkey_is_caught();
  test_no_passkey_available();
  test_secure_connections_is_refused_clearly();
  test_peer_failure_is_reported();
  test_just_works_still_agrees();
  return octotest::report("test_smp");
}
