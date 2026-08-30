// What a scanner makes of one advertising report -- see src/advert.h.
//
// This is the classifier every radio shares, so the cases worth pinning are
// the ones where a second implementation would plausibly have drifted: a box
// that is named after a camera, an empty payload, and an advertisement that
// runs off the end of its own buffer.
#include "advert.h"

#include <cstdint>
#include <string>
#include <vector>

#include "bmd.h"
#include "harness.h"
#include "hci.h"

namespace {

using octo::classify_ad;

// Type numbers from the Core Specification Supplement, part A.
constexpr uint8_t kAdFlags = 0x01;
constexpr uint8_t kAdServices128 = 0x07;
constexpr uint8_t kAdName = 0x09;
constexpr uint8_t kAdServiceData16 = 0x16;

void add_name(std::vector<uint8_t>* ad, const std::string& name) {
  CHECK(octo::hci::append_ad(ad, kAdName,
                             reinterpret_cast<const uint8_t*>(name.data()),
                             name.size()));
}

// Service data under 0xFDAC, which is the only place a Tentacle's clock ever
// appears. The UUID travels little-endian, so 0xFDAC is AC FD on the wire.
void add_fdac(std::vector<uint8_t>* ad, const std::vector<uint8_t>& payload) {
  std::vector<uint8_t> value{0xac, 0xfd};
  value.insert(value.end(), payload.begin(), payload.end());
  CHECK(octo::hci::append_ad(ad, kAdServiceData16, value.data(), value.size()));
}

void add_camera_service(std::vector<uint8_t>* ad) {
  octo::hci::Uuid u = octo::hci::uuid_const(octo::bmd::kServiceCamera);
  // A 128-bit UUID goes out least-significant byte first, the reverse of how
  // it is written.
  std::vector<uint8_t> le(u.bytes.rbegin(), u.bytes.rend());
  CHECK(octo::hci::append_ad(ad, kAdServices128, le.data(), le.size()));
}

void test_tentacle() {
  const std::vector<uint8_t> payload{0x22, 0x01, 0x02, 0x03, 0x04, 0x05};
  std::vector<uint8_t> ad;
  const uint8_t flags = 0x06;
  CHECK(octo::hci::append_ad(&ad, kAdFlags, &flags, 1));
  add_fdac(&ad, payload);
  add_name(&ad, "Tentacle");

  const octo::AdvertMatch m = classify_ad(ad);
  CHECK(m.is_box);
  CHECK(!m.is_camera);
  CHECK_STR(m.name, "Tentacle");
  CHECK(m.box_data == payload);
}

void test_camera() {
  std::vector<uint8_t> ad;
  add_camera_service(&ad);
  add_name(&ad, "BMPCC");

  const octo::AdvertMatch m = classify_ad(ad);
  CHECK(m.is_camera);
  CHECK(!m.is_box);
  CHECK_STR(m.name, "BMPCC");
  CHECK(m.box_data.empty());
}

// The rule the header states, and the one a hand-written second classifier
// would most plausibly get wrong: there is a Tentacle on this bench called
// "BMPCC", and matching a camera by name would hand the sync daemon a box
// with no control characteristic on it.
void test_box_named_like_a_camera() {
  std::vector<uint8_t> ad;
  add_fdac(&ad, {0x32, 0x11, 0x22, 0x33});
  add_name(&ad, "BMPCC");

  const octo::AdvertMatch m = classify_ad(ad);
  CHECK(m.is_box);
  CHECK(!m.is_camera);
  CHECK_STR(m.name, "BMPCC");
}

// An FDAC entry with nothing in it is not a sighting of a box: the decoder
// downstream has nothing to read, so accepting it would put a device on the
// roster that can never acquire a time.
void test_empty_payload_is_not_a_box() {
  std::vector<uint8_t> ad;
  add_fdac(&ad, {});
  add_name(&ad, "Empty");

  const octo::AdvertMatch m = classify_ad(ad);
  CHECK(!m.is_box);
  CHECK(!m.is_camera);
  CHECK_STR(m.name, "Empty");
}

// Service data under some other UUID is somebody else's business.
void test_other_service_data_ignored() {
  std::vector<uint8_t> ad;
  std::vector<uint8_t> value{0x0d, 0x18, 0x01, 0x02};  // 0x180d, heart rate
  CHECK(octo::hci::append_ad(&ad, kAdServiceData16, value.data(), value.size()));

  const octo::AdvertMatch m = classify_ad(ad);
  CHECK(!m.is_box);
  CHECK(!m.is_camera);
}

// Nothing here may read off the end. An advertisement whose last structure
// claims more bytes than are present is what a lossy radio delivers, and the
// decoder is documented to stop rather than guess -- so whatever was already
// decoded still stands.
void test_truncated() {
  std::vector<uint8_t> ad;
  add_fdac(&ad, {0x42, 0x01});
  ad.push_back(0x20);  // a structure claiming 32 bytes
  ad.push_back(kAdName);
  ad.push_back('x');

  const octo::AdvertMatch m = classify_ad(ad);
  CHECK(m.is_box);
  CHECK(m.name.empty());

  // And the degenerate inputs, which a radio really does deliver.
  const octo::AdvertMatch none = classify_ad(nullptr, 0);
  CHECK(!none.is_box);
  CHECK(!none.is_camera);
  const std::vector<uint8_t> empty;
  CHECK(!classify_ad(empty).is_box);
}

// Both questions are asked of the same packet and neither excludes the other.
void test_both() {
  std::vector<uint8_t> ad;
  add_fdac(&ad, {0x22, 0x09});
  add_camera_service(&ad);

  const octo::AdvertMatch m = classify_ad(ad);
  CHECK(m.is_box);
  CHECK(m.is_camera);
}

}  // namespace

int main() {
  test_tentacle();
  test_camera();
  test_box_named_like_a_camera();
  test_empty_payload_is_not_a_box();
  test_other_service_data_ignored();
  test_truncated();
  test_both();
  return octotest::report("test_advert");
}
