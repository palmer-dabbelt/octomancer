#include "crypto.h"

#include <fcntl.h>
#include <unistd.h>

#include <cstring>

namespace octo {
namespace crypto {
namespace {

// ------------------------------------------------------------------- AES

// A plain textbook AES-128, written for clarity rather than speed. Pairing
// happens once per connection and costs a few dozen block encryptions, so
// there is nothing here worth optimising and a great deal worth being able to
// read.
//
// No attempt is made at constant-time table lookups. That matters for a
// library encrypting attacker-chosen data; here the only secrets are a pairing
// nonce and a long term key held for one session, and the attacker would need
// to be running code on this machine already.

const uint8_t kSbox[256] = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b,
    0xfe, 0xd7, 0xab, 0x76, 0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0,
    0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0, 0xb7, 0xfd, 0x93, 0x26,
    0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
    0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2,
    0xeb, 0x27, 0xb2, 0x75, 0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0,
    0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84, 0x53, 0xd1, 0x00, 0xed,
    0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
    0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f,
    0x50, 0x3c, 0x9f, 0xa8, 0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5,
    0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2, 0xcd, 0x0c, 0x13, 0xec,
    0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
    0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14,
    0xde, 0x5e, 0x0b, 0xdb, 0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c,
    0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79, 0xe7, 0xc8, 0x37, 0x6d,
    0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
    0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f,
    0x4b, 0xbd, 0x8b, 0x8a, 0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e,
    0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e, 0xe1, 0xf8, 0x98, 0x11,
    0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
    0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f,
    0xb0, 0x54, 0xbb, 0x16};

uint8_t xtime(uint8_t x) {
  return static_cast<uint8_t>((x << 1) ^ ((x & 0x80) ? 0x1b : 0x00));
}

uint8_t mul(uint8_t a, uint8_t b) {
  uint8_t result = 0;
  while (b) {
    if (b & 1) result ^= a;
    a = xtime(a);
    b >>= 1;
  }
  return result;
}

// 11 round keys of 16 bytes for AES-128.
void expand_key(const Key128& key, uint8_t out[176]) {
  std::memcpy(out, key.data(), 16);
  uint8_t rcon = 0x01;
  for (int i = 16; i < 176; i += 4) {
    uint8_t t[4];
    std::memcpy(t, out + i - 4, 4);
    if (i % 16 == 0) {
      uint8_t tmp = t[0];
      t[0] = static_cast<uint8_t>(kSbox[t[1]] ^ rcon);
      t[1] = kSbox[t[2]];
      t[2] = kSbox[t[3]];
      t[3] = kSbox[tmp];
      rcon = xtime(rcon);
    }
    for (int j = 0; j < 4; ++j) out[i + j] = out[i - 16 + j] ^ t[j];
  }
}

}  // namespace

Block aes128_encrypt(const Key128& key, const Block& plaintext) {
  uint8_t rk[176];
  expand_key(key, rk);

  uint8_t s[16];
  std::memcpy(s, plaintext.data(), 16);
  for (int i = 0; i < 16; ++i) s[i] ^= rk[i];

  for (int round = 1; round <= 10; ++round) {
    for (int i = 0; i < 16; ++i) s[i] = kSbox[s[i]];

    // ShiftRows, on the column-major state AES defines.
    uint8_t t[16];
    for (int c = 0; c < 4; ++c) {
      for (int r = 0; r < 4; ++r) t[c * 4 + r] = s[((c + r) % 4) * 4 + r];
    }
    std::memcpy(s, t, 16);

    if (round != 10) {
      for (int c = 0; c < 4; ++c) {
        uint8_t* col = s + c * 4;
        uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
        col[0] = static_cast<uint8_t>(mul(a0, 2) ^ mul(a1, 3) ^ a2 ^ a3);
        col[1] = static_cast<uint8_t>(a0 ^ mul(a1, 2) ^ mul(a2, 3) ^ a3);
        col[2] = static_cast<uint8_t>(a0 ^ a1 ^ mul(a2, 2) ^ mul(a3, 3));
        col[3] = static_cast<uint8_t>(mul(a0, 3) ^ a1 ^ a2 ^ mul(a3, 2));
      }
    }
    for (int i = 0; i < 16; ++i) s[i] ^= rk[round * 16 + i];
  }

  Block out;
  std::memcpy(out.data(), s, 16);
  return out;
}

// ------------------------------------------------------------- AES-CMAC

namespace {

void shift_left_one(const uint8_t in[16], uint8_t out[16]) {
  uint8_t carry = 0;
  for (int i = 15; i >= 0; --i) {
    out[i] = static_cast<uint8_t>((in[i] << 1) | carry);
    carry = (in[i] & 0x80) ? 1 : 0;
  }
}

// The two subkeys CMAC derives from the block cipher, per RFC 4493 section 2.3.
void cmac_subkeys(const Key128& key, uint8_t k1[16], uint8_t k2[16]) {
  Block zero{};
  Block l = aes128_encrypt(key, zero);

  shift_left_one(l.data(), k1);
  if (l[0] & 0x80) k1[15] ^= 0x87;

  shift_left_one(k1, k2);
  if (k1[0] & 0x80) k2[15] ^= 0x87;
}

}  // namespace

Block aes_cmac(const Key128& key, const uint8_t* msg, size_t len) {
  uint8_t k1[16], k2[16];
  cmac_subkeys(key, k1, k2);

  size_t blocks = (len + 15) / 16;
  bool complete = len > 0 && len % 16 == 0;
  if (blocks == 0) blocks = 1;

  uint8_t last[16];
  size_t last_off = (blocks - 1) * 16;
  if (complete) {
    for (int i = 0; i < 16; ++i) last[i] = msg[last_off + i] ^ k1[i];
  } else {
    // The incomplete final block is padded with a single one bit and zeros,
    // then mixed with the other subkey. Using the same subkey for both cases
    // is what would let a message and its padded form share a tag.
    size_t rest = len - last_off;
    for (size_t i = 0; i < 16; ++i) {
      uint8_t b = i < rest ? msg[last_off + i] : (i == rest ? 0x80 : 0x00);
      last[i] = b ^ k2[i];
    }
  }

  Block x{};
  for (size_t i = 0; i + 1 < blocks; ++i) {
    Block y;
    for (int j = 0; j < 16; ++j) y[j] = x[j] ^ msg[i * 16 + j];
    x = aes128_encrypt(key, y);
  }
  Block y;
  for (int j = 0; j < 16; ++j) y[j] = x[j] ^ last[j];
  return aes128_encrypt(key, y);
}

Block aes_cmac(const Key128& key, const std::vector<uint8_t>& msg) {
  return aes_cmac(key, msg.data(), msg.size());
}

// -------------------------------------------------------- legacy pairing

Block c1(const Key128& k, const Block& r, const uint8_t preq[7],
         const uint8_t pres[7], uint8_t iat, const uint8_t ia[6], uint8_t rat,
         const uint8_t ra[6]) {
  // p1 = pres || preq || rat' || iat', most significant byte first, so the
  // address type bytes come last in the buffer.
  uint8_t p1[16];
  std::memcpy(p1, pres, 7);
  std::memcpy(p1 + 7, preq, 7);
  p1[14] = static_cast<uint8_t>(rat & 0x01);
  p1[15] = static_cast<uint8_t>(iat & 0x01);

  Block e1;
  for (int i = 0; i < 16; ++i) e1[i] = r[i] ^ p1[i];
  e1 = aes128_encrypt(k, e1);

  // p2 = padding || ia || ra
  uint8_t p2[16];
  std::memset(p2, 0, 4);
  std::memcpy(p2 + 4, ia, 6);
  std::memcpy(p2 + 10, ra, 6);

  Block e2;
  for (int i = 0; i < 16; ++i) e2[i] = e1[i] ^ p2[i];
  return aes128_encrypt(k, e2);
}

Block s1(const Key128& k, const Block& r1, const Block& r2) {
  // The high 64 bits of each random are discarded and the low halves are
  // concatenated with r1's on top: r = r1' || r2'. Callers pass the responder's
  // random as r1 -- see smp.cc, where getting this pair the wrong way round
  // produces a key both sides compute differently and an encryption setup that
  // fails with nothing more specific than "PIN or key missing".
  Block r;
  std::memcpy(r.data(), r1.data() + 8, 8);
  std::memcpy(r.data() + 8, r2.data() + 8, 8);
  return aes128_encrypt(k, r);
}

// ---------------------------------------------------- secure connections

Block f4(const uint8_t u[32], const uint8_t v[32], const Block& x, uint8_t z) {
  std::vector<uint8_t> m;
  m.reserve(65);
  m.insert(m.end(), u, u + 32);
  m.insert(m.end(), v, v + 32);
  m.push_back(z);
  Key128 key;
  std::memcpy(key.data(), x.data(), 16);
  return aes_cmac(key, m);
}

void f5(const uint8_t dhkey[32], const Block& n1, const Block& n2, uint8_t a1t,
        const uint8_t a1[6], uint8_t a2t, const uint8_t a2[6], Block* mac_key,
        Block* ltk) {
  // The salt is a constant the specification fixes; it turns the raw shared
  // secret into a key CMAC can be run under.
  static const uint8_t kSalt[16] = {0x6c, 0x88, 0x83, 0x91, 0xaa, 0xf5,
                                    0xa5, 0x38, 0x60, 0x37, 0x0b, 0xdb,
                                    0x5a, 0x60, 0x83, 0xbe};
  Key128 salt;
  std::memcpy(salt.data(), kSalt, 16);
  Block t = aes_cmac(salt, dhkey, 32);
  Key128 key;
  std::memcpy(key.data(), t.data(), 16);

  // counter || keyID "btle" || N1 || N2 || A1 || A2 || length (256, big-endian)
  auto build = [&](uint8_t counter) {
    std::vector<uint8_t> m;
    m.reserve(53);
    m.push_back(counter);
    m.push_back('b');
    m.push_back('t');
    m.push_back('l');
    m.push_back('e');
    m.insert(m.end(), n1.begin(), n1.end());
    m.insert(m.end(), n2.begin(), n2.end());
    m.push_back(static_cast<uint8_t>(a1t & 0x01));
    m.insert(m.end(), a1, a1 + 6);
    m.push_back(static_cast<uint8_t>(a2t & 0x01));
    m.insert(m.end(), a2, a2 + 6);
    m.push_back(0x01);
    m.push_back(0x00);
    return m;
  };

  if (mac_key) *mac_key = aes_cmac(key, build(0));
  if (ltk) *ltk = aes_cmac(key, build(1));
}

Block f6(const Block& mac_key, const Block& n1, const Block& n2, const Block& r,
         const uint8_t iocap[3], uint8_t a1t, const uint8_t a1[6], uint8_t a2t,
         const uint8_t a2[6]) {
  std::vector<uint8_t> m;
  m.reserve(65);
  m.insert(m.end(), n1.begin(), n1.end());
  m.insert(m.end(), n2.begin(), n2.end());
  m.insert(m.end(), r.begin(), r.end());
  m.insert(m.end(), iocap, iocap + 3);
  m.push_back(static_cast<uint8_t>(a1t & 0x01));
  m.insert(m.end(), a1, a1 + 6);
  m.push_back(static_cast<uint8_t>(a2t & 0x01));
  m.insert(m.end(), a2, a2 + 6);
  Key128 key;
  std::memcpy(key.data(), mac_key.data(), 16);
  return aes_cmac(key, m);
}

uint32_t g2(const uint8_t u[32], const uint8_t v[32], const Block& x,
            const Block& y) {
  std::vector<uint8_t> m;
  m.reserve(80);
  m.insert(m.end(), u, u + 32);
  m.insert(m.end(), v, v + 32);
  m.insert(m.end(), y.begin(), y.end());
  Key128 key;
  std::memcpy(key.data(), x.data(), 16);
  Block t = aes_cmac(key, m);
  // The low 32 bits, taken as a big-endian number, reduced to six digits.
  uint32_t value = (static_cast<uint32_t>(t[12]) << 24) |
                   (static_cast<uint32_t>(t[13]) << 16) |
                   (static_cast<uint32_t>(t[14]) << 8) |
                   static_cast<uint32_t>(t[15]);
  return value % 1000000u;
}

// ------------------------------------------------------------- utilities

bool random_bytes(uint8_t* out, size_t len) {
  if (!out) return false;
  int fd = ::open("/dev/urandom", O_RDONLY);
  if (fd < 0) return false;
  size_t off = 0;
  while (off < len) {
    ssize_t n = ::read(fd, out + off, len - off);
    if (n <= 0) {
      ::close(fd);
      return false;
    }
    off += static_cast<size_t>(n);
  }
  ::close(fd);
  return true;
}

Block random_block() {
  Block b{};
  // A pairing nonce that is not random defeats the confirm exchange outright,
  // so a failure here has to be loud rather than quietly producing zeros. The
  // caller checks for the all-zero block.
  if (!random_bytes(b.data(), b.size())) b.fill(0);
  return b;
}

bool equal(const Block& a, const Block& b) {
  uint8_t diff = 0;
  for (size_t i = 0; i < a.size(); ++i) diff |= a[i] ^ b[i];
  return diff == 0;
}

std::vector<uint8_t> to_vector(const Block& b) {
  return std::vector<uint8_t>(b.begin(), b.end());
}

Block from_vector(const std::vector<uint8_t>& v) {
  Block b{};
  size_t n = v.size() < 16 ? v.size() : 16;
  std::memcpy(b.data(), v.data(), n);
  return b;
}

}  // namespace crypto
}  // namespace octo
