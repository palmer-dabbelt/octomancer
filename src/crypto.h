// The cryptography Bluetooth pairing is built out of.
//
// This exists because of one line in doc/ble-write-failure-report.md: the
// camera's control characteristics worked immediately "against an already-
// paired camera". They worked because the Mac had bonded with it long ago.
// The dongle is a different device with a different address, so it has to
// pair from scratch, and the Blackmagic control characteristics are encrypted
// -- which means no clock can be written over the dongle until this file
// works.
//
// Everything here is software rather than delegated to the controller. HCI
// does offer LE Encrypt, and the ECDH point multiplication genuinely has to be
// left to the controller, but a software AES costs a hundred lines and buys
// something worth far more than those lines: the whole pairing calculation can
// be checked against published test vectors on a machine with no radio. A
// pairing bug that can only be reproduced with a camera in the room is a
// pairing bug that does not get fixed.
//
// Byte order. Every value here is most-significant-byte-first, the order the
// specification writes them in and the order the test vectors are published
// in. SMP puts them on the wire least-significant-byte-first. The reversal
// happens once, at the PDU boundary in smp.cc, and nowhere else -- the same
// discipline hci.h applies to UUIDs, for the same reason.
//
// References are to the Bluetooth Core Specification 5.4, Vol 3 Part H
// section 2.2 (the key generation functions).
#ifndef OCTO_CRYPTO_H
#define OCTO_CRYPTO_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace octo {
namespace crypto {

using Key128 = std::array<uint8_t, 16>;
using Block = std::array<uint8_t, 16>;

// One AES-128 block encryption. This is `e` in the specification's notation
// and the primitive everything below is made of.
Block aes128_encrypt(const Key128& key, const Block& plaintext);

// AES-CMAC over an arbitrary message (RFC 4493), which the specification calls
// AES-CMAC and uses for every Secure Connections function.
Block aes_cmac(const Key128& key, const uint8_t* msg, size_t len);
Block aes_cmac(const Key128& key, const std::vector<uint8_t>& msg);

// ------------------------------------------------------- legacy pairing

// c1: the confirm value, which is what stops a man in the middle swapping the
// random numbers. Vol 3 Part H 2.2.3.
//
// `pres` and `preq` are the seven-byte pairing response and request PDUs as
// they were sent, `iat`/`rat` the initiator's and responder's address types.
Block c1(const Key128& k, const Block& r, const uint8_t preq[7],
         const uint8_t pres[7], uint8_t iat, const uint8_t ia[6], uint8_t rat,
         const uint8_t ra[6]);

// s1: the short term key, from the two random numbers. Vol 3 Part H 2.2.4.
Block s1(const Key128& k, const Block& r1, const Block& r2);

// ------------------------------------------------- secure connections

// f4: the commitment value. `z` is zero for Just Works and Numeric
// Comparison, and one bit of the passkey for Passkey Entry.
Block f4(const uint8_t u[32], const uint8_t v[32], const Block& x, uint8_t z);

// f5: derives the MacKey and the Long Term Key from the shared secret.
void f5(const uint8_t dhkey[32], const Block& n1, const Block& n2, uint8_t a1t,
        const uint8_t a1[6], uint8_t a2t, const uint8_t a2[6], Block* mac_key,
        Block* ltk);

// f6: the check value each side sends so the other knows the pairing matched.
Block f6(const Block& mac_key, const Block& n1, const Block& n2, const Block& r,
         const uint8_t iocap[3], uint8_t a1t, const uint8_t a1[6], uint8_t a2t,
         const uint8_t a2[6]);

// g2: the six-digit number Numeric Comparison shows to the user.
uint32_t g2(const uint8_t u[32], const uint8_t v[32], const Block& x,
            const Block& y);

// ------------------------------------------------------------- utilities

// Bytes from the system's random source. Used for the pairing nonces, where a
// predictable value would defeat the confirm exchange entirely.
bool random_bytes(uint8_t* out, size_t len);
Block random_block();

// Constant-time comparison, so a confirm value cannot be guessed a byte at a
// time by timing the failure.
bool equal(const Block& a, const Block& b);

std::vector<uint8_t> to_vector(const Block& b);
Block from_vector(const std::vector<uint8_t>& v);  // pads or truncates to 16

}  // namespace crypto
}  // namespace octo

#endif  // OCTO_CRYPTO_H
