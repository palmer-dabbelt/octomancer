// Check the pairing arithmetic against published test vectors.
//
// The AES and AES-CMAC expectations below are from FIPS-197 and RFC 4493 --
// standards documents, not this project's own output, which is what makes them
// worth anything. They matter more than they look: every Secure Connections
// function is CMAC with a different message in it, so a CMAC that is subtly
// wrong produces pairing that fails with no diagnostic beyond the camera
// refusing to bond.
//
// The SMP functions themselves (c1, s1, f4, f5, f6, g2) are checked here for
// construction -- input widths, field order, the constants the specification
// fixes -- and NOT against the specification's own worked examples, which are
// not reproduced here. Those remain to be confirmed against real hardware; see
// doc/dongle-notes.md. What this file does rule out is the far more likely
// class of error: a message assembled in the wrong order or the wrong length.
#include "../src/crypto.h"
#include "harness.h"

#include <string>
#include <vector>

using namespace octo::crypto;

namespace {

std::vector<uint8_t> from_hex(const std::string& s) {
  std::vector<uint8_t> out;
  for (size_t i = 0; i + 1 < s.size(); i += 2) {
    out.push_back(static_cast<uint8_t>(std::stoul(s.substr(i, 2), nullptr, 16)));
  }
  return out;
}

std::string hex(const uint8_t* d, size_t n) {
  static const char* digits = "0123456789abcdef";
  std::string out;
  for (size_t i = 0; i < n; ++i) {
    out.push_back(digits[d[i] >> 4]);
    out.push_back(digits[d[i] & 0x0f]);
  }
  return out;
}

std::string hex(const Block& b) { return hex(b.data(), b.size()); }

Key128 key_from_hex(const std::string& s) {
  std::vector<uint8_t> v = from_hex(s);
  Key128 k{};
  for (size_t i = 0; i < 16 && i < v.size(); ++i) k[i] = v[i];
  return k;
}

Block block_from_hex(const std::string& s) {
  std::vector<uint8_t> v = from_hex(s);
  Block b{};
  for (size_t i = 0; i < 16 && i < v.size(); ++i) b[i] = v[i];
  return b;
}

void test_aes() {
  // FIPS-197, appendix C.1: the AES-128 example.
  CHECK_STR(hex(aes128_encrypt(key_from_hex("000102030405060708090a0b0c0d0e0f"),
                               block_from_hex("00112233445566778899aabbccddeeff"))),
            "69c4e0d86a7b0430d8cdb78070b4c55a");

  // FIPS-197, appendix B: the worked cipher example.
  CHECK_STR(hex(aes128_encrypt(key_from_hex("2b7e151628aed2a6abf7158809cf4f3c"),
                               block_from_hex("3243f6a8885a308d313198a2e0370734"))),
            "3925841d02dc09fbdc118597196a0b32");
}

void test_cmac() {
  // RFC 4493, section 4: all four worked examples, which between them cover
  // the empty message, an exact block, a partial block needing padding, and a
  // multi-block message. The padding cases are the ones that go wrong.
  Key128 k = key_from_hex("2b7e151628aed2a6abf7158809cf4f3c");

  CHECK_STR(hex(aes_cmac(k, std::vector<uint8_t>())),
            "bb1d6929e95937287fa37d129b756746");

  CHECK_STR(hex(aes_cmac(k, from_hex("6bc1bee22e409f96e93d7e117393172a"))),
            "070a16b46b4d4144f79bdd9dd04a287c");

  CHECK_STR(hex(aes_cmac(k, from_hex("6bc1bee22e409f96e93d7e117393172a"
                                     "ae2d8a571e03ac9c9eb76fac45af8e51"
                                     "30c81c46a35ce411"))),
            "dfa66747de9ae63030ca32611497c827");

  CHECK_STR(hex(aes_cmac(k, from_hex("6bc1bee22e409f96e93d7e117393172a"
                                     "ae2d8a571e03ac9c9eb76fac45af8e51"
                                     "30c81c46a35ce411e5fbc1191a0a52ef"
                                     "f69f2445df4f9b17ad2b417be66c3710"))),
            "51f0bebf7e3b9d92fc49741779363cfe");
}

void test_legacy_construction() {
  // c1 mixes in both pairing PDUs and both addresses, so changing any one of
  // them must change the result. That is the property the confirm value exists
  // for: it is what a man in the middle cannot forge after swapping a random.
  Key128 k = key_from_hex("00000000000000000000000000000000");
  Block r = block_from_hex("5783d52156ad6f0e6388274ec6702ee0");
  std::vector<uint8_t> preq = from_hex("01010000100701");
  std::vector<uint8_t> pres = from_hex("02030000080701");
  std::vector<uint8_t> ia = from_hex("a1a2a3a4a5a6");
  std::vector<uint8_t> ra = from_hex("b1b2b3b4b5b6");

  Block base = c1(k, r, preq.data(), pres.data(), 0x01, ia.data(), 0x00,
                  ra.data());

  std::vector<uint8_t> preq2 = preq;
  preq2[0] ^= 0x01;
  CHECK(!equal(base, c1(k, r, preq2.data(), pres.data(), 0x01, ia.data(), 0x00,
                        ra.data())));

  std::vector<uint8_t> ia2 = ia;
  ia2[5] ^= 0x01;
  CHECK(!equal(base, c1(k, r, preq.data(), pres.data(), 0x01, ia2.data(), 0x00,
                        ra.data())));

  // The address type is one bit, and the two values must differ.
  CHECK(!equal(base, c1(k, r, preq.data(), pres.data(), 0x00, ia.data(), 0x00,
                        ra.data())));

  // Same inputs, same answer -- there is no hidden state.
  CHECK(equal(base, c1(k, r, preq.data(), pres.data(), 0x01, ia.data(), 0x00,
                       ra.data())));

  // s1 takes the low half of each random, so the high halves cannot matter and
  // the order of the two arguments must.
  Block r1 = block_from_hex("000102030405060708090a0b0c0d0e0f");
  Block r2 = block_from_hex("101112131415161718191a1b1c1d1e1f");
  Block stk = s1(k, r1, r2);
  Block r1_high_changed = r1;
  r1_high_changed[0] ^= 0xff;
  CHECK(equal(stk, s1(k, r1_high_changed, r2)));
  Block r1_low_changed = r1;
  r1_low_changed[15] ^= 0x01;
  CHECK(!equal(stk, s1(k, r1_low_changed, r2)));
  CHECK(!equal(stk, s1(k, r2, r1)));
}

void test_secure_connections_construction() {
  std::vector<uint8_t> u = from_hex(
      "20b003d2f297be2c5e2c83a7e9f9a5b9eff49111acf4fddbcc0301480e359de6");
  std::vector<uint8_t> v = from_hex(
      "55188b3d32f6bb9a900afcfbeed4e72a59cb9ac2f19d7cfb6b4fdd49f47fc5fd");
  Block x = block_from_hex("d5cb8454d177733effffb2ec712baeab");
  Block y = block_from_hex("a6e8e7cc25a75f6e216583f7ff3dc4cf");

  // f4's z byte is one bit of the passkey during Passkey Entry, so it has to
  // reach the digest. If it did not, every one of the twenty passkey rounds
  // would produce the same commitment and the passkey would not be checked at
  // all.
  Block c0 = f4(u.data(), v.data(), x, 0x00);
  Block c1v = f4(u.data(), v.data(), x, 0x81);
  CHECK(!equal(c0, c1v));
  CHECK(equal(c0, f4(u.data(), v.data(), x, 0x00)));
  // Swapping the two public keys must change the commitment; they are not
  // symmetric and treating them as such would let a device pair with itself.
  CHECK(!equal(c0, f4(v.data(), u.data(), x, 0x00)));

  // f5 produces two different 128-bit values from one shared secret. Deriving
  // the same bytes for both would hand the MacKey to anyone who saw the LTK.
  std::vector<uint8_t> dh = from_hex(
      "ec0234a357c8ad05341010a60a397d9b99796b13b4f866f1868d34f373bfa698");
  std::vector<uint8_t> a1 = from_hex("561237378cda");
  std::vector<uint8_t> a2 = from_hex("a713702dcfc1");
  Block mac_key, ltk;
  f5(dh.data(), x, y, 0x00, a1.data(), 0x00, a2.data(), &mac_key, &ltk);
  CHECK(!equal(mac_key, ltk));
  CHECK(!equal(mac_key, Block{}));
  CHECK(!equal(ltk, Block{}));

  // The two nonces are not interchangeable: initiator and responder must
  // derive the same key from opposite viewpoints only when they agree on who
  // is who.
  Block mac2, ltk2;
  f5(dh.data(), y, x, 0x00, a1.data(), 0x00, a2.data(), &mac2, &ltk2);
  CHECK(!equal(ltk, ltk2));

  // f6 has to depend on every field, since it is the value that proves both
  // sides saw the same pairing.
  std::vector<uint8_t> iocap = from_hex("010102");
  Block r = block_from_hex("12a3343bb453bb5408da42d20c2d0fc8");
  Block check = f6(mac_key, x, y, r, iocap.data(), 0x00, a1.data(), 0x00,
                   a2.data());
  std::vector<uint8_t> iocap2 = from_hex("020102");
  CHECK(!equal(check, f6(mac_key, x, y, r, iocap2.data(), 0x00, a1.data(), 0x00,
                         a2.data())));
  Block r2 = r;
  r2[15] ^= 0x01;
  CHECK(!equal(check, f6(mac_key, x, y, r2, iocap.data(), 0x00, a1.data(), 0x00,
                         a2.data())));

  // g2 is shown to a human, so it must always be six digits.
  for (int i = 0; i < 64; ++i) {
    Block xi = x;
    xi[0] = static_cast<uint8_t>(i);
    uint32_t code = g2(u.data(), v.data(), xi, y);
    CHECK(code < 1000000u);
  }
  CHECK_EQ(g2(u.data(), v.data(), x, y), g2(u.data(), v.data(), x, y));
  CHECK(g2(u.data(), v.data(), x, y) != g2(u.data(), v.data(), y, x));
}

void test_random() {
  // Two nonces in a row must not be equal, and neither may be all zeros --
  // random_block() returns zeros to signal that the system's random source
  // failed, and pairing with a zero nonce is pairing with no confirm value at
  // all.
  Block a = random_block();
  Block b = random_block();
  CHECK(!equal(a, Block{}));
  CHECK(!equal(b, Block{}));
  CHECK(!equal(a, b));
}

}  // namespace

int main() {
  test_aes();
  test_cmac();
  test_legacy_construction();
  test_secure_connections_construction();
  test_random();
  return octotest::report("test_crypto");
}
