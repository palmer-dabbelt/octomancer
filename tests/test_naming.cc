// Who gets to say what a device is called.
//
// Three claimants -- a person, a probe, an advertisement -- and they disagree
// often enough that the order has to be pinned rather than left to whichever
// code path ran last. The cases below are the ones where getting it wrong is
// visible to somebody: a rename that does not stick, a refresh that eats the
// rename, a placeholder stored as though it were a name.
#include "../src/naming.h"

#include <string>

#include "harness.h"

using octo::DeviceName;
using octo::NameBook;
using octo::NameSource;

namespace {

// A dongle's device: a hardware address, and no name, because a passive
// listener never sends the scan request that would fetch one.
const char* kBoxId = "CF:40:5D:89:32:19";
// This Mac's: a CoreBluetooth UUID, which is per-host and says nothing about
// the hardware.
const char* kMacId = "B80D95C9-7D0B-140A-0351-2F4D55A1114E";

// ------------------------------------------------------------------ the tests

// An identifier is a worse name and a better answer than a blank: something
// has to go in the column, and a row with nothing in it looks like a bug.
void test_an_unknown_device_is_called_by_its_id() {
  NameBook book;
  NameSource from = NameSource::kUser;
  CHECK_STR(book.display(kBoxId, &from), kBoxId);
  CHECK(from == NameSource::kNone);
}

void test_the_order_is_person_then_probe_then_advertisement() {
  NameBook book;
  NameSource from = NameSource::kNone;

  book.heard(kMacId, "BMPCC");
  CHECK_STR(book.display(kMacId, &from), "BMPCC");
  CHECK(from == NameSource::kHeard);

  // A probe outranks an advertisement: it asked the device rather than
  // catching what it happened to broadcast.
  book.probed(kMacId, "Blackmagic Pocket 6K");
  CHECK_STR(book.display(kMacId, &from), "Blackmagic Pocket 6K");
  CHECK(from == NameSource::kProbed);

  // ...and a person outranks both, because theirs is the only one of the three
  // that is a decision rather than an observation.
  book.rename(kMacId, "B camera");
  CHECK_STR(book.display(kMacId, &from), "B camera");
  CHECK(from == NameSource::kUser);

  // The others are not destroyed by being outranked. They come back if the
  // person changes their mind.
  book.rename(kMacId, "");
  CHECK_STR(book.display(kMacId, &from), "Blackmagic Pocket 6K");
  CHECK(from == NameSource::kProbed);
}

// Renaming a device nobody has heard of has to work: naming something you
// cannot currently hear is a reasonable thing to want to do, and the
// alternative is telling somebody to switch a box on before they may label it.
void test_a_device_can_be_named_before_it_is_ever_heard() {
  NameBook book;
  book.rename(kBoxId, "Sound cart");
  CHECK_STR(book.display(kBoxId), "Sound cart");

  // ...and what arrives later does not overwrite it.
  book.heard(kBoxId, "Tentacle E");
  book.probed(kBoxId, "Tentacle E");
  CHECK_STR(book.display(kBoxId), "Sound cart");
}

// "(unnamed)" is what src/registry.cc puts in a snapshot for a device that has
// never said what it is called, and it travels over the wire looking exactly
// like a name. Storing it would put it on a row that a probe could have named
// properly, and would make four unnamed devices share one name.
void test_a_placeholder_is_not_a_name() {
  NameBook book;
  book.heard(kBoxId, "(unnamed)");
  book.heard(kMacId, "(no name)");
  CHECK_STR(book.display(kBoxId), kBoxId);
  CHECK_STR(book.display(kMacId), kMacId);
  CHECK(octo::is_placeholder_name("(unnamed)"));
  CHECK(octo::is_placeholder_name(""));
  CHECK(!octo::is_placeholder_name("BMPCC"));
}

// A device that genuinely has no name must be asked once, not forever. So a
// probe that comes back empty is still a probe that happened.
void test_a_probe_that_found_nothing_still_counts_as_a_probe() {
  NameBook book;
  CHECK(book.needs_probe(kBoxId));
  book.probed(kBoxId, "");
  CHECK(!book.needs_probe(kBoxId));
  // ...and the row falls back to the identifier, which is the truth.
  CHECK_STR(book.display(kBoxId), kBoxId);
}

// A probe costs a connection. A device somebody has already named does not
// need one urgently enough to pay that.
void test_a_named_device_is_not_worth_probing() {
  NameBook book;
  book.rename(kBoxId, "Sound cart");
  CHECK(!book.needs_probe(kBoxId));
}

// The work list a control daemon walks.
void test_the_work_list_is_what_has_not_been_asked() {
  NameBook book;
  book.heard("a", "A");
  book.heard("b", "B");
  book.heard("c", "C");
  book.probed("b", "Box B");
  book.rename("c", "Mine");

  const std::vector<std::string> want = book.unprobed();
  CHECK_EQ(static_cast<int>(want.size()), 1);
  CHECK_STR(want[0], "a");
}

// The point of refresh: devices change their names, so forget what we learned
// and ask again.
void test_refresh_forgets_what_was_learned() {
  NameBook book;
  book.heard(kMacId, "BMPCC");
  book.probed(kMacId, "Old Name");
  CHECK(!book.needs_probe(kMacId));

  CHECK(book.refresh(kMacId));
  CHECK(book.needs_probe(kMacId));
  CHECK_STR(book.display(kMacId), kMacId);
}

// ...and the thing refresh must not do. Somebody who renames a camera and
// then asks for a refresh is asking to re-read the device, not to lose their
// own label -- and losing it silently would be discovered days later, when the
// row they set up came back called something else.
void test_refresh_keeps_the_name_a_person_chose() {
  NameBook book;
  book.heard(kMacId, "BMPCC");
  book.probed(kMacId, "Old Name");
  book.rename(kMacId, "B camera");

  CHECK(book.refresh(kMacId));
  CHECK_STR(book.display(kMacId), "B camera");
  // The learned half is gone, so it will be asked again.
  CHECK(!book.needs_probe(kMacId));  // ...but not urgently: it has a name
  book.rename(kMacId, "");
  CHECK(book.needs_probe(kMacId));
  CHECK_STR(book.display(kMacId), kMacId);
}

// Refreshing something unheard-of is not a failure. The caller asked for the
// device to be forgotten and it is; making them tell "removed" from "was never
// here" only invites treating one of the two as an error.
void test_refreshing_an_unknown_device_is_quiet() {
  NameBook book;
  CHECK(!book.refresh("nobody"));
  CHECK_EQ(static_cast<int>(book.size()), 0);
}

// Clearing the last thing about a device removes the record rather than
// leaving an empty one to be saved and reloaded forever.
void test_an_emptied_record_does_not_linger() {
  NameBook book;
  book.rename(kBoxId, "Sound cart");
  CHECK_EQ(static_cast<int>(book.size()), 1);
  book.rename(kBoxId, "");
  CHECK_EQ(static_cast<int>(book.size()), 0);
}

void test_name_source_names() {
  CHECK_STR(octo::name_source_name(NameSource::kUser), "user");
  CHECK_STR(octo::name_source_name(NameSource::kProbed), "probed");
  CHECK_STR(octo::name_source_name(NameSource::kHeard), "heard");
  CHECK_STR(octo::name_source_name(NameSource::kNone), "none");
}

}  // namespace

int main() {
  test_an_unknown_device_is_called_by_its_id();
  test_the_order_is_person_then_probe_then_advertisement();
  test_a_device_can_be_named_before_it_is_ever_heard();
  test_a_placeholder_is_not_a_name();
  test_a_probe_that_found_nothing_still_counts_as_a_probe();
  test_a_named_device_is_not_worth_probing();
  test_the_work_list_is_what_has_not_been_asked();
  test_refresh_forgets_what_was_learned();
  test_refresh_keeps_the_name_a_person_chose();
  test_refreshing_an_unknown_device_is_quiet();
  test_an_emptied_record_does_not_linger();
  test_name_source_names();
  return octotest::report("test_naming");
}
