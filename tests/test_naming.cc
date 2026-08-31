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

// A passive radio knows exactly what time every box thinks it is and has no
// idea what any of them are called: a Tentacle's clock is in the
// advertisement and its name is in the scan response, which only an active
// scan asks for. So the radio goes active when there is a name to learn, and
// back to passive when there is not -- rather than transmitting a scan request
// for every advertisement in the room forever.
void test_a_radio_scans_actively_only_while_there_is_a_name_to_learn() {
  // Nothing unnamed: stay passive.
  CHECK(!octo::want_active_scan(false, 0, 1000.0));
  // Something unnamed: go active.
  CHECK(octo::want_active_scan(false, 1, 1000.0));
  // Once everything is named, go back.
  CHECK(!octo::want_active_scan(true, 0, 1000.0));
  // ...and stay active while there is still something to learn.
  CHECK(octo::want_active_scan(true, 2, 1000.0));
}

// A box at the edge of range appears and disappears. Without damping, that
// restarts the radio's scan every time it does -- and a radio spending its
// time restarting scans is a radio doing neither kind of listening well.
void test_the_scan_setting_settles_before_it_changes_again() {
  // A change is wanted, but not yet.
  CHECK(!octo::want_active_scan(false, 1, 0.0));
  CHECK(!octo::want_active_scan(false, 1, octo::kScanSettleSeconds - 0.1));
  CHECK(octo::want_active_scan(false, 1, octo::kScanSettleSeconds));
  // ...and the same going the other way.
  CHECK(octo::want_active_scan(true, 0, 1.0));
  CHECK(!octo::want_active_scan(true, 0, octo::kScanSettleSeconds));
}

void test_name_source_names() {
  CHECK_STR(octo::name_source_name(NameSource::kUser), "user");
  CHECK_STR(octo::name_source_name(NameSource::kProbed), "probed");
  CHECK_STR(octo::name_source_name(NameSource::kHeard), "heard");
  CHECK_STR(octo::name_source_name(NameSource::kNone), "none");
}

}  // namespace

// What is worth writing to the roster file, which is the question
// DeviceName::empty() answers and octomancerd's save_devices asks.
//
// The rule used to be "only what a person typed", and that quietly cost a
// dongle its names on every restart: it learns them by scanning actively,
// which it only does while something is unnamed, so an answer thrown away at
// shutdown had to be fetched again by putting the radio on the air for half a
// minute. A heard name is cheap to keep and expensive to re-learn.
void test_a_heard_name_is_worth_keeping() {
  octo::DeviceName heard;
  heard.heard = "FS5";
  CHECK(!heard.empty());

  octo::DeviceName typed;
  typed.user = "B camera";
  CHECK(!typed.empty());

  // A probe that came back with nothing is also worth keeping, because it is
  // the only thing that stops the device being asked again forever.
  octo::DeviceName asked;
  asked.probed_done = true;
  CHECK(!asked.empty());

  // And a device we know nothing about is not written at all, rather than
  // written as a row of empty strings.
  CHECK(octo::DeviceName().empty());
}

// The bug this rule was changed for: a box at the edge of range is heard every
// minute or two and is stale in between, so it was almost never live at the
// moment the decision was made. The count came out zero, the radio stayed
// passive, and the box kept its hardware address forever -- while the boxes
// near enough to be continuously live were exactly the ones already named.
//
// So the caller counts devices heard *lately*, and this is the rule that makes
// that worth doing: one is enough to put the radio on the air.
void test_one_unnamed_device_is_enough_to_scan_actively() {
  CHECK(octo::want_active_scan(false, 1, 1000.0));
  // ...and none is enough to stop, once the setting has settled.
  CHECK(!octo::want_active_scan(true, 0, 1000.0));
}

// Hysteresis in time, because the cost being damped is restarting the scan and
// that cost is per restart however many devices provoked it. A box flickering
// in and out at the edge of range must not be able to restart the radio every
// second.
void test_a_scan_setting_is_left_alone_for_a_while() {
  CHECK(!octo::want_active_scan(false, 1, octo::kScanSettleSeconds - 1.0));
  CHECK(octo::want_active_scan(false, 1, octo::kScanSettleSeconds + 1.0));
  CHECK(octo::want_active_scan(true, 0, octo::kScanSettleSeconds - 1.0));
  CHECK(!octo::want_active_scan(true, 0, octo::kScanSettleSeconds + 1.0));
}

// The window has to be several times a distant box's advertising gap, or the
// rule above is back to being about devices that happen to be live.
void test_the_naming_window_outlasts_a_slow_box() {
  CHECK(octo::kNameWithin > 60.0);
}

// Radios go in the same book as devices, so the thing that has to hold is that
// the two namespaces cannot reach each other. They cannot, and not by luck:
// every device identifier this program deals in is hex, and no hex string
// begins with the word "radio".
void test_a_radio_key_cannot_collide_with_a_device_id() {
  CHECK_STR(octo::radio_name_key("dongle"), "radio:dongle");
  CHECK(octo::is_radio_key(octo::radio_name_key("dongle")));

  // The two shapes a device identifier actually comes in.
  CHECK(!octo::is_radio_key("C1:95:4D:A4:9D:18"));
  CHECK(!octo::is_radio_key("A1B2C3D4-0000-1111-2222-333344445555"));
  // And nothing shorter than the prefix trips it.
  CHECK(!octo::is_radio_key(""));
  CHECK(!octo::is_radio_key("rad"));
}

// This machine's radio is the one whose rows carry no tag, and it keys as the
// empty radio rather than as its hostname. Keying it by hostname would lose
// the name somebody gave it the day the machine was renamed, which is a day
// somebody is already reorganising things and would not connect the two.
void test_the_local_radio_keys_without_a_hostname() {
  CHECK_STR(octo::radio_name_key(""), "radio:");
  CHECK(octo::is_radio_key(octo::radio_name_key("")));
  CHECK(octo::radio_name_key("") != octo::radio_name_key("dongle"));
}

// Only a person can name a radio: nothing advertises a dongle and there is
// nothing to connect to and ask. So the fallback is the caller's, and what
// comes back for an unnamed radio is empty rather than the key -- which is
// what display() would have handed over, prefix and all.
void test_a_radio_has_only_the_name_somebody_gave_it() {
  octo::NameBook book;
  CHECK_STR(octo::radio_user_name(book, "dongle"), "");

  // Not even a heard name counts, if something ever managed to record one.
  book.heard(octo::radio_name_key("dongle"), "Raytac");
  CHECK_STR(octo::radio_user_name(book, "dongle"), "");

  book.rename(octo::radio_name_key("dongle"), "cart left");
  CHECK_STR(octo::radio_user_name(book, "dongle"), "cart left");
  // And the device namespace is untouched by any of it.
  CHECK_STR(octo::radio_user_name(book, "cart left"), "");

  book.rename(octo::radio_name_key("dongle"), "");
  CHECK_STR(octo::radio_user_name(book, "dongle"), "");
}

int main() {
  test_a_radio_key_cannot_collide_with_a_device_id();
  test_the_local_radio_keys_without_a_hostname();
  test_a_radio_has_only_the_name_somebody_gave_it();
  test_one_unnamed_device_is_enough_to_scan_actively();
  test_a_scan_setting_is_left_alone_for_a_while();
  test_the_naming_window_outlasts_a_slow_box();
  test_a_heard_name_is_worth_keeping();
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
  test_a_radio_scans_actively_only_while_there_is_a_name_to_learn();
  test_the_scan_setting_settles_before_it_changes_again();
  test_name_source_names();
  return octotest::report("test_naming");
}
