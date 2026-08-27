#include "smp.h"

#include <cstdio>
#include <cstring>

namespace octo {
namespace smp {
namespace {

char* scratch() {
  static thread_local char buf[32];
  return buf;
}

}  // namespace

const char* reason_name(uint8_t reason) {
  switch (reason) {
    case kPasskeyEntryFailed: return "passkey entry failed";
    case kOobNotAvailable: return "out-of-band data not available";
    case kAuthenticationRequirements: return "authentication requirements not met";
    case kConfirmValueFailed: return "confirm value did not match";
    case kPairingNotSupported: return "pairing not supported";
    case kEncryptionKeySize: return "encryption key size too small";
    case kCommandNotSupported: return "command not supported";
    case kUnspecifiedReason: return "unspecified reason";
    case kRepeatedAttempts: return "repeated attempts; try again later";
    case kInvalidParameters: return "invalid parameters";
    case kDhKeyCheckFailed: return "DHKey check failed";
    case kNumericComparisonFailed: return "numeric comparison failed";
    default: break;
  }
  std::snprintf(scratch(), 32, "reason 0x%02x", reason);
  return scratch();
}

const char* method_name(Method m) {
  switch (m) {
    case Method::kJustWorks: return "just works";
    case Method::kPasskeyInput: return "passkey entry (they display)";
    case Method::kPasskeyDisplay: return "passkey entry (we display)";
    case Method::kOob: return "out of band";
  }
  return "unknown";
}

Method choose_method(uint8_t local_io, uint8_t remote_io, bool mitm_wanted) {
  // Without a demand for protection against a relay there is nothing to
  // choose: Just Works is what both sides settle for, and it is what pairs
  // silently.
  if (!mitm_wanted) return Method::kJustWorks;
  // Neither side able to show or take a number leaves nothing better either.
  if (local_io == kNoInputNoOutput || remote_io == kNoInputNoOutput) {
    return Method::kJustWorks;
  }

  bool local_can_input = local_io == kKeyboardOnly || local_io == kKeyboardDisplay;
  bool local_can_display = local_io == kDisplayOnly ||
                           local_io == kDisplayYesNo ||
                           local_io == kKeyboardDisplay;
  bool remote_can_input =
      remote_io == kKeyboardOnly || remote_io == kKeyboardDisplay;
  bool remote_can_display = remote_io == kDisplayOnly ||
                            remote_io == kDisplayYesNo ||
                            remote_io == kKeyboardDisplay;

  // A camera showing a number and us typing it is the case that matters here,
  // so it is tried first.
  if (remote_can_display && local_can_input) return Method::kPasskeyInput;
  if (local_can_display && remote_can_input) return Method::kPasskeyDisplay;
  return Method::kJustWorks;
}

std::vector<uint8_t> PairingParams::encode() const {
  return {code,         io_capability,   oob_flag,       auth_req,
          max_key_size, initiator_keys, responder_keys};
}

bool PairingParams::decode(const std::vector<uint8_t>& pdu, PairingParams* out) {
  if (!out || pdu.size() < 7) return false;
  out->code = pdu[0];
  out->io_capability = pdu[1];
  out->oob_flag = pdu[2];
  out->auth_req = pdu[3];
  out->max_key_size = pdu[4];
  out->initiator_keys = pdu[5];
  out->responder_keys = pdu[6];
  return true;
}

std::vector<uint8_t> block_to_wire(const crypto::Block& b) {
  std::vector<uint8_t> out(16);
  for (size_t i = 0; i < 16; ++i) out[i] = b[15 - i];
  return out;
}

crypto::Block block_from_wire(const uint8_t* data, size_t len) {
  crypto::Block b{};
  size_t n = len < 16 ? len : 16;
  for (size_t i = 0; i < n; ++i) b[15 - i] = data[i];
  return b;
}

// ---------------------------------------------------------------- Initiator

Initiator::Initiator(const Config& config) : config_(config) {
  request_.code = kPairingRequest;
  request_.io_capability = config.io_capability;
  request_.oob_flag = 0x00;
  request_.auth_req = 0;
  if (config.want_bonding) request_.auth_req |= kBondingFlag;
  if (config.want_mitm) request_.auth_req |= kMitmFlag;
  // The Secure Connections bit stays clear on purpose. Offering a mode this
  // file cannot complete would let a peer choose it and leave pairing stuck
  // half way through, which is worse than not offering it at all.
  request_.max_key_size = 16;
  // No keys distributed after pairing. Bonding would need somewhere to keep a
  // long term key across runs, and nothing here has that yet -- so every
  // connection pairs afresh, which costs a passkey each time but never leaves
  // a stale key that silently stops working.
  request_.initiator_keys = 0x00;
  request_.responder_keys = 0x00;
}

std::vector<uint8_t> Initiator::begin() {
  request_bytes_ = request_.encode();
  state_ = State::kWaitingForResponse;
  return request_bytes_;
}

std::vector<uint8_t> Initiator::fail(uint8_t reason, const char* text,
                                     std::vector<uint8_t>* out,
                                     std::string* error) {
  state_ = State::kFailed;
  if (error) *error = text;
  std::vector<uint8_t> pdu = {kPairingFailed, reason};
  if (out) *out = pdu;
  return pdu;
}

crypto::Block Initiator::confirm_for(const crypto::Block& rand) const {
  // c1 hashes the two pairing PDUs exactly as they went over the wire, along
  // with both addresses and their types.
  return crypto::c1(tk_, rand, request_bytes_.data(), response_bytes_.data(),
                    config_.local.type, config_.local.bytes.data(),
                    config_.remote.type, config_.remote.bytes.data());
}

bool Initiator::handle(const std::vector<uint8_t>& pdu,
                       std::vector<uint8_t>* out, std::string* error) {
  if (out) out->clear();
  if (pdu.empty()) {
    fail(kInvalidParameters, "empty SMP PDU", out, error);
    return false;
  }

  // A failure from the peer can arrive at any point, and it is the only useful
  // diagnostic there is going to be.
  if (pdu[0] == kPairingFailed) {
    state_ = State::kFailed;
    if (error) {
      *error = std::string("the peer refused pairing: ") +
               reason_name(pdu.size() > 1 ? pdu[1] : kUnspecifiedReason);
    }
    return false;
  }

  switch (state_) {
    case State::kWaitingForResponse: {
      if (pdu[0] != kPairingResponse) {
        fail(kCommandNotSupported, "expected a Pairing Response", out, error);
        return false;
      }
      if (!PairingParams::decode(pdu, &response_)) {
        fail(kInvalidParameters, "malformed Pairing Response", out, error);
        return false;
      }
      response_bytes_.assign(pdu.begin(), pdu.begin() + 7);

      if (response_.auth_req & kSecureConnectionsFlag) {
        // The peer wants Secure Connections. We did not offer it, so a peer
        // that sets it anyway is one this file cannot finish with. Saying so
        // is far better than proceeding and failing at the confirm step for
        // reasons nobody could work out.
        fail(kAuthenticationRequirements,
             "the peer requires LE Secure Connections, which is not "
             "implemented; see doc/dongle-notes.md",
             out, error);
        return false;
      }
      if (response_.max_key_size < 7 || response_.max_key_size > 16) {
        fail(kEncryptionKeySize, "the peer asked for an unusable key size", out,
             error);
        return false;
      }

      bool mitm = (request_.auth_req & kMitmFlag) &&
                  (response_.auth_req & kMitmFlag);
      method_ = choose_method(request_.io_capability, response_.io_capability,
                              mitm);

      tk_.fill(0);
      if (method_ == Method::kPasskeyInput || method_ == Method::kPasskeyDisplay) {
        uint32_t passkey = 0;
        if (!passkey_ || !passkey_(&passkey)) {
          fail(kPasskeyEntryFailed, "no passkey was supplied", out, error);
          return false;
        }
        if (passkey > 999999u) {
          fail(kPasskeyEntryFailed, "the passkey must be six digits", out,
               error);
          return false;
        }
        // The temporary key is the passkey as a 128-bit big-endian number,
        // which puts it in the last four bytes and leaves the rest zero.
        tk_[12] = static_cast<uint8_t>((passkey >> 24) & 0xff);
        tk_[13] = static_cast<uint8_t>((passkey >> 16) & 0xff);
        tk_[14] = static_cast<uint8_t>((passkey >> 8) & 0xff);
        tk_[15] = static_cast<uint8_t>(passkey & 0xff);
      }

      local_rand_ = crypto::random_block();
      if (crypto::equal(local_rand_, crypto::Block{})) {
        fail(kUnspecifiedReason, "no randomness available for the pairing nonce",
             out, error);
        return false;
      }

      crypto::Block confirm = confirm_for(local_rand_);
      std::vector<uint8_t> reply = {kPairingConfirm};
      std::vector<uint8_t> wire = block_to_wire(confirm);
      reply.insert(reply.end(), wire.begin(), wire.end());
      if (out) *out = reply;
      state_ = State::kWaitingForConfirm;
      return true;
    }

    case State::kWaitingForConfirm: {
      if (pdu[0] != kPairingConfirm || pdu.size() < 17) {
        fail(kInvalidParameters, "expected a Pairing Confirm", out, error);
        return false;
      }
      remote_confirm_ = block_from_wire(pdu.data() + 1, 16);
      // Only now is our random revealed. Sending it before the peer has
      // committed to theirs would let them choose one to match.
      std::vector<uint8_t> reply = {kPairingRandom};
      std::vector<uint8_t> wire = block_to_wire(local_rand_);
      reply.insert(reply.end(), wire.begin(), wire.end());
      if (out) *out = reply;
      state_ = State::kWaitingForRandom;
      return true;
    }

    case State::kWaitingForRandom: {
      if (pdu[0] != kPairingRandom || pdu.size() < 17) {
        fail(kInvalidParameters, "expected a Pairing Random", out, error);
        return false;
      }
      remote_rand_ = block_from_wire(pdu.data() + 1, 16);

      // The check the whole exchange exists for. A wrong passkey lands here,
      // and so does a relay attack.
      crypto::Block expect = confirm_for(remote_rand_);
      if (!crypto::equal(expect, remote_confirm_)) {
        fail(kConfirmValueFailed,
             method_ == Method::kJustWorks
                 ? "the peer's confirm value did not match"
                 : "the peer's confirm value did not match; the passkey was "
                   "probably wrong",
             out, error);
        return false;
      }

      // The responder's random goes first.
      stk_ = crypto::s1(tk_, remote_rand_, local_rand_);
      state_ = State::kReadyToEncrypt;
      return true;
    }

    case State::kIdle:
      fail(kCommandNotSupported, "pairing has not been started", out, error);
      return false;

    case State::kReadyToEncrypt:
      // Anything arriving now is key distribution we did not ask for. Ignoring
      // it is correct: we requested no keys, so there is nothing to store.
      return true;

    case State::kFailed:
    default:
      if (error) *error = "pairing has already failed";
      return false;
  }
}

}  // namespace smp
}  // namespace octo
