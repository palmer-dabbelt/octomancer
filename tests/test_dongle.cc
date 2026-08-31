// A dongle attaching, answering, going quiet and being unplugged, with no
// dongle in the building.
//
// The cases worth arranging here are the ones that are awkward on a bench: an
// answer that arrives in pieces, an answer that never finishes, a box that
// stops being heard, a cable pulled mid-sentence. Each of those has a way of
// looking like the room changed when what changed was the cable.
#include "../src/dongle.h"

#include <string>
#include <vector>

#include "harness.h"

using octo::DeviceSnapshot;
using octo::DongleView;
using octo::Message;

namespace {

Message msg_of(const std::string& line) {
  Message m;
  std::string err;
  CHECK(octo::decode(line, &m, &err));
  return m;
}

// The three boxes a dongle in this room would report: addresses rather than
// CoreBluetooth UUIDs, because that is what a radio that can see the hardware
// address calls them, and offsets displaced by an unset clock.
const char* kBoxA = "dev id=C4:1E:AE:18:A7:01 rssi=-47 live=1 age=0.4"
                    " offset=39599.500 median=39599.500 samples=826";
const char* kBoxB = "dev id=C4:1E:AE:18:A7:02 rssi=-79 live=1 age=1.3"
                    " offset=39599.486 median=39599.486 samples=411";
const char* kBoxC = "dev id=C4:1E:AE:18:A7:03 rssi=-84 live=0 age=99.4"
                    " offset=39599.514 median=39599.514 samples=4";

void feed(DongleView* v, const std::vector<std::string>& lines, double now) {
  for (const std::string& l : lines) v->observe(msg_of(l), now);
}

void answer(DongleView* v, const std::vector<std::string>& boxes, double now) {
  feed(v, boxes, now);
  v->observe(msg_of("end what=devices n=" + std::to_string(boxes.size())), now);
}

const DeviceSnapshot* find(const std::vector<DeviceSnapshot>& rows,
                           const std::string& id) {
  for (const DeviceSnapshot& d : rows) {
    if (d.id == id) return &d;
  }
  return nullptr;
}

// ------------------------------------------------------------------ the tests

void test_nothing_is_reported_before_the_dongle_is_attached() {
  DongleView v;
  CHECK(!v.attached());
  CHECK_EQ(static_cast<int>(v.devices(1000.0).size()), 0);
  // ...and it is not asked for anything either.
  Message out;
  CHECK(!v.wants_poll(1000.0, &out));
}

void test_a_finished_answer_becomes_rows() {
  DongleView v;
  v.opened(1000.0);
  answer(&v, {kBoxA, kBoxB, kBoxC}, 1001.0);

  const std::vector<DeviceSnapshot> rows = v.devices(1001.0);
  CHECK_EQ(static_cast<int>(rows.size()), 3);
  CHECK_EQ(static_cast<long long>(v.answers()), 1LL);

  const DeviceSnapshot* a = find(rows, "C4:1E:AE:18:A7:01");
  CHECK(a != nullptr);
  // Tagged, every one of them. This is what stops the row being mistaken for
  // something this machine heard.
  CHECK_STR(a->radio, "dongle");
  CHECK(a->live);
  CHECK(a->has_time);
  CHECK_NEAR(a->median_offset, 39599.500, 1e-6);
  CHECK_EQ(a->samples, 826);
  CHECK_EQ(a->rssi, -47);
  CHECK(a->heard_this_run);

  // A box the dongle is no longer hearing is still listed, and still says so.
  const DeviceSnapshot* c = find(rows, "C4:1E:AE:18:A7:03");
  CHECK(c != nullptr);
  CHECK(!c->live);
  CHECK_NEAR(c->age, 99.4, 1e-6);
}

// A batch arrives every kPollEvery seconds and the ages in it are true at the
// instant the dongle built it. Handed back unchanged, every dongle row's age
// advances in five-second steps and sits up to five seconds short in between --
// which is what a person watching the table sees, and what they reported.
//
// The caller cannot fix this: it has no way to know when the batch arrived.
void test_ages_advance_between_answers() {
  DongleView v;
  v.opened(1000.0);
  answer(&v, {kBoxA, kBoxC}, 1001.0);

  // Two seconds after the batch, everything in it is two seconds older.
  const std::vector<DeviceSnapshot> later = v.devices(1003.0);
  CHECK_NEAR(find(later, "C4:1E:AE:18:A7:01")->age, 2.4, 1e-6);
  CHECK_NEAR(find(later, "C4:1E:AE:18:A7:03")->age, 101.4, 1e-6);

  // And a fresh batch resets them to what the dongle actually measured,
  // rather than to what we had extrapolated.
  answer(&v, {kBoxA}, 1006.0);
  CHECK_NEAR(find(v.devices(1006.0), "C4:1E:AE:18:A7:01")->age, 0.4, 1e-6);
}

// A `dev` list is a complete statement about the room, so showing it partway
// through would report every box not yet mentioned as having gone away -- a
// bench that flickered down to one box and back on every poll.
void test_a_half_arrived_answer_is_not_shown() {
  DongleView v;
  v.opened(1000.0);
  answer(&v, {kBoxA, kBoxB, kBoxC}, 1001.0);
  CHECK_EQ(static_cast<int>(v.devices(1001.0).size()), 3);

  // The next answer starts arriving...
  feed(&v, {kBoxA}, 1006.0);
  // ...and until it finishes, the last complete one still stands.
  CHECK_EQ(static_cast<int>(v.devices(1006.0).size()), 3);

  v.observe(msg_of("end what=devices n=1"), 1006.0);
  CHECK_EQ(static_cast<int>(v.devices(1006.0).size()), 1);
}

// The other half of that: an empty room is a real answer and has to replace
// the last one, or a dongle carried out of range would go on reporting the
// last thing it heard for as long as it stayed plugged in.
void test_an_empty_answer_clears_the_room() {
  DongleView v;
  v.opened(1000.0);
  answer(&v, {kBoxA, kBoxB}, 1001.0);
  CHECK_EQ(static_cast<int>(v.devices(1001.0).size()), 2);

  answer(&v, {}, 1006.0);
  CHECK_EQ(static_cast<int>(v.devices(1006.0).size()), 0);
  CHECK_EQ(static_cast<long long>(v.answers()), 2LL);
}

// A dongle that greets us and then stops finishing batches is not a dongle
// that is telling us about an empty room. Its last answer is kept and marked
// dead rather than dropped: a row that vanishes says the box has gone, and
// what has actually gone quiet is the dongle.
void test_an_answer_that_goes_stale_stops_being_live() {
  DongleView v;
  v.opened(1000.0);
  answer(&v, {kBoxA, kBoxB}, 1001.0);

  const std::vector<DeviceSnapshot> edge =
      v.devices(1001.0 + DongleView::kStaleAfter);
  CHECK_EQ(static_cast<int>(edge.size()), 2);
  CHECK(edge[0].live);

  const std::vector<DeviceSnapshot> past =
      v.devices(1001.0 + DongleView::kStaleAfter + 0.1);
  CHECK_EQ(static_cast<int>(past.size()), 2);
  CHECK(!past[0].live);
  CHECK(!past[1].live);
  // And still ageing, which is what makes the row readable as a memory rather
  // than as a reading that has stopped moving.
  CHECK_NEAR(past[0].age, edge[0].age + 0.1, 1e-6);

  // Still attached: not answering and not being there are different, and a
  // caller may want to say which.
  CHECK(v.attached());
}

// The cable coming out mid-sentence. The half-arrived batch goes, because it
// was never a complete statement about the room. The last complete one stays,
// dead: emptying the page of every box the dongle had heard says the room
// changed, when the only thing that changed is what we are plugged into.
void test_unplugging_keeps_the_last_answer_but_kills_it() {
  DongleView v;
  v.opened(1000.0);
  answer(&v, {kBoxA, kBoxB}, 1001.0);
  feed(&v, {kBoxC}, 1006.0);  // a batch in flight

  v.closed(1007.0);
  CHECK(!v.attached());
  const std::vector<DeviceSnapshot> rows = v.devices(1007.0);
  CHECK_EQ(static_cast<int>(rows.size()), 2);
  CHECK(!rows[0].live);
  CHECK(!rows[1].live);
  // The one that was in flight is not among them: an incomplete `dev` list
  // would report every box it had not reached yet as having gone away.
  CHECK(find(rows, "C4:1E:AE:18:A7:03") == nullptr);

  // Plugging it back in does clear them, and that is where the rule belongs.
  // The thing plugged back in need not be the same dongle, and showing the
  // last one's boxes under the new one's name would be a bench assembled from
  // two different rooms.
  v.opened(1008.0);
  CHECK_EQ(static_cast<int>(v.devices(1008.0).size()), 0);
  v.observe(msg_of("end what=devices n=1"), 1008.0);
  CHECK_EQ(static_cast<int>(v.devices(1008.0).size()), 0);
}

// A box that advertised without a readable clock in it. Zero would be a
// measurement; the absence of the field is not.
void test_a_box_with_no_clock_has_no_time() {
  DongleView v;
  v.opened(1000.0);
  answer(&v, {"dev id=C4:1E:AE:18:A7:09 rssi=-60 live=1 age=0.2"}, 1001.0);

  const std::vector<DeviceSnapshot> rows = v.devices(1001.0);
  CHECK_EQ(static_cast<int>(rows.size()), 1);
  CHECK(!rows[0].has_time);
  CHECK_EQ(rows[0].median_offset, 0.0);
  CHECK_EQ(rows[0].samples, 0);
}

// "(unnamed)" is what src/registry.cc puts in a snapshot when a device has
// never said what it is called, so it arrives over the wire as though it were
// a name -- and four boxes that have never said arrive wearing the same one.
void test_the_registrys_stand_in_for_a_name_is_not_a_name() {
  DongleView v;
  v.opened(1000.0);
  answer(&v, {"dev id=C4:1E:AE:18:A7:01 name=(unnamed) rssi=-50 live=1"
              " age=0.3 median=39599.5 samples=240"}, 1001.0);
  CHECK_STR(v.devices(1001.0)[0].name, "");
}

// An alert means "this device is more than a minute from the truth", which is
// not a sentence a box with no idea what time it is can say. A dongle that
// has never been told measures everything against a clock that started at
// zero when it was plugged in, so by its own reckoning every box in the room
// is hours out -- and every row would arrive red.
void test_alarms_from_a_free_running_box_are_not_imported() {
  DongleView v;
  v.opened(1000.0);
  const char* alarmed = "dev id=C4:1E:AE:18:A7:01 rssi=-50 live=1 age=0.3"
                        " median=-14499.85 samples=240 alerting=1";
  answer(&v, {alarmed}, 1001.0);
  CHECK(!v.clock_is_real());
  CHECK(!v.devices(1001.0)[0].alerting);

  // Told the time, the same line is believed: now it is a claim the box is in
  // a position to make.
  v.observe(msg_of("status phase=idle clock=real"), 1002.0);
  answer(&v, {alarmed}, 1002.0);
  CHECK(v.devices(1002.0)[0].alerting);
}

void test_a_named_box_names_its_own_radio() {
  DongleView v;
  v.opened(1000.0);
  CHECK_STR(v.radio(), "dongle");
  v.observe(msg_of("hello proto=1 role=sync name=cart-left"), 1000.0);
  CHECK_STR(v.radio(), "cart-left");

  answer(&v, {kBoxA}, 1001.0);
  CHECK_STR(v.devices(1001.0)[0].radio, "cart-left");
}

// A dongle nobody has told the time says so, and that is the ordinary
// standalone state rather than a fault. An older box that does not carry the
// field is taken as free, which only ever withholds a claim.
void test_the_clock_is_free_until_the_box_says_otherwise() {
  DongleView v;
  v.opened(1000.0);
  CHECK(!v.clock_is_real());
  v.observe(msg_of("status phase=idle radio=on devices=3 clock=free"), 1000.0);
  CHECK(!v.clock_is_real());
  v.observe(msg_of("status phase=idle radio=on devices=3 clock=real"), 1000.0);
  CHECK(v.clock_is_real());
  v.observe(msg_of("status phase=idle radio=on devices=3"), 1000.0);
  CHECK(!v.clock_is_real());
}

// A box with no console says why its last run ended this way, and it says it
// once. Dropping it would throw away the only account there is.
void test_what_the_box_said_is_kept() {
  DongleView v;
  v.opened(1000.0);
  Message said;
  said.verb = "say";
  said.set("text", "last run stopped: the watchdog fired");
  v.observe(said, 1000.0);
  CHECK_STR(v.last_words(), "last run stopped: the watchdog fired");
}

// Asked at once on attaching, then on an interval. A dongle that has just
// greeted us should not sit unquestioned for the first interval.
// Nothing is said to an open port until it has said who it is. The port was
// found by opening whatever looked like a USB serial device, and that list
// includes everybody else's microcontroller.
void test_a_port_that_has_not_greeted_is_never_written_to() {
  DongleView v;
  Message out;
  v.opened(1000.0);
  CHECK(!v.greeted());
  CHECK(!v.wants_poll(1000.0, &out));
  CHECK(!v.wants_poll(1000.0 + 10 * DongleView::kPollEvery, &out));

  v.observe(msg_of("hello proto=1 role=sync"), 1010.0);
  CHECK(v.greeted());
  CHECK(v.wants_poll(1010.0, &out));
}

// ...and a greeting does not survive the cable. What gets plugged in next
// need not be the same thing, or a dongle at all.
void test_a_greeting_does_not_survive_unplugging() {
  DongleView v;
  v.opened(1000.0);
  v.observe(msg_of("hello proto=1 role=sync"), 1000.0);
  CHECK(v.greeted());
  v.closed(1001.0);
  CHECK(!v.greeted());
  v.opened(1002.0);
  CHECK(!v.greeted());
}

void test_polling_starts_at_once_and_then_waits() {
  DongleView v;
  Message out;
  v.opened(1000.0);
  v.observe(msg_of("hello proto=1 role=sync"), 1000.0);
  CHECK(v.wants_poll(1000.0, &out));
  CHECK_STR(out.verb, "devices");
  v.polled(1000.0);

  CHECK(!v.wants_poll(1000.0 + DongleView::kPollEvery - 0.01, &out));
  CHECK(v.wants_poll(1000.0 + DongleView::kPollEvery, &out));
}

// A request that could not be written must not start an interval, or a link
// that is failing to write goes quiet for as long as it keeps failing.
void test_a_poll_that_was_not_sent_does_not_count() {
  DongleView v;
  Message out;
  v.opened(1000.0);
  v.observe(msg_of("hello proto=1 role=sync"), 1000.0);
  CHECK(v.wants_poll(1000.0, &out));
  // ...the write fails, so polled() is never called.
  CHECK(v.wants_poll(1000.1, &out));
}

// Unknown verbs are ignored rather than refused. A Mac and a dongle run
// different versions most of the time, and a reader that gave up on the
// conversation because of one unrecognised line would be useless on the day
// it mattered.
void test_unknown_verbs_are_ignored() {
  DongleView v;
  v.opened(1000.0);
  feed(&v, {kBoxA}, 1001.0);
  v.observe(msg_of("bench ok=1 offset=39599.5 spread=0.028 boxes=3"), 1001.0);
  v.observe(msg_of("weather sunny=1 outlook=fair"), 1001.0);
  v.observe(msg_of("end what=devices n=1"), 1001.0);

  CHECK_EQ(static_cast<int>(v.devices(1001.0).size()), 1);
}

// An `end` for something else must not be taken as the end of a device list.
void test_an_end_for_something_else_does_not_finish_a_batch() {
  DongleView v;
  v.opened(1000.0);
  feed(&v, {kBoxA}, 1001.0);
  v.observe(msg_of("end what=log n=4"), 1001.0);
  CHECK_EQ(static_cast<int>(v.devices(1001.0).size()), 0);
  CHECK_EQ(static_cast<long long>(v.answers()), 0LL);

  v.observe(msg_of("end what=devices n=1"), 1001.0);
  CHECK_EQ(static_cast<int>(v.devices(1001.0).size()), 1);
}

// ----------------------------------------------------------- the date

octo::DateStamp stamp(int y, int mo, int d) {
  octo::DateStamp s;
  s.year = y;
  s.month = mo;
  s.day = d;
  return s;
}

// The one fact that genuinely has to travel between two radios. Everything
// else about a clock each end works out for itself.
void test_a_greeted_box_is_told_todays_date_once() {
  DongleView v;
  Message out;
  v.opened(1000.0);
  // Not before the greeting: nothing is said to a port that has not said who
  // it is, and that rule does not get an exception for being helpful.
  CHECK(!v.wants_date(stamp(2026, 8, 31), &out));

  v.observe(msg_of("hello proto=1 role=sync"), 1000.0);
  CHECK(v.wants_date(stamp(2026, 8, 31), &out));
  CHECK_STR(out.verb, "date");
  CHECK_STR(out.get("y"), "2026");
  CHECK_STR(out.get("mo"), "8");
  CHECK_STR(out.get("d"), "31");

  // Once told, not told again -- until tomorrow.
  v.dated(stamp(2026, 8, 31));
  CHECK(!v.wants_date(stamp(2026, 8, 31), &out));
  CHECK(v.wants_date(stamp(2026, 9, 1), &out));
  CHECK_STR(out.get("mo"), "9");
  CHECK_STR(out.get("d"), "1");
}

// The case this exists for. A dongle has no battery-backed clock, so it
// forgets the date whenever it loses power -- and a dongle in a phone charger
// is a device that loses power. A box that went away and came back must be
// told again, or it silently cannot set a camera.
void test_a_box_that_came_back_is_told_the_date_again() {
  DongleView v;
  Message out;
  v.opened(1000.0);
  v.observe(msg_of("hello proto=1 role=sync"), 1000.0);
  v.dated(stamp(2026, 8, 31));
  CHECK(!v.wants_date(stamp(2026, 8, 31), &out));

  // Unplugged, replugged, greeted afresh.
  v.closed(1001.0);
  v.opened(1002.0);
  v.observe(msg_of("hello proto=1 role=sync"), 1002.0);
  CHECK(v.wants_date(stamp(2026, 8, 31), &out));
}

// A host that does not know the date says nothing, rather than pushing a
// zero that would be range-refused at the far end and look like a protocol
// fault.
void test_a_host_with_no_date_says_nothing() {
  DongleView v;
  Message out;
  v.opened(1000.0);
  v.observe(msg_of("hello proto=1 role=sync"), 1000.0);
  CHECK(!v.wants_date(octo::DateStamp(), &out));
  CHECK(!v.wants_date(stamp(2026, 0, 31), &out));
  CHECK(!v.wants_date(stamp(0, 8, 31), &out));
}

// ------------------------------------------------------- which way in

// USB wins whenever it is there, and the reason is not that the radio link is
// untrusted. It is that a Bluetooth link costs a connection slot on a box
// whose whole job is listening, and airtime in a room already full of
// advertisements, to carry lines that are already arriving down a cable.
void test_usb_wins_whenever_it_is_there() {
  CHECK(octo::carrier(true, true) == octo::LinkWay::kUsb);
  CHECK(octo::carrier(true, false) == octo::LinkWay::kUsb);
  CHECK(octo::carrier(false, true) == octo::LinkWay::kBluetooth);
  CHECK(octo::carrier(false, false) == octo::LinkWay::kNone);
}

// ...and only one of them carries the conversation, even when both are up.
// Two links feeding one view would deliver every device list twice, and since
// a list replaces the last one wholesale, that is not a doubled bench -- it is
// a bench that alternates between two radios' answers on no schedule anybody
// chose.
void test_exactly_one_link_carries_the_conversation() {
  CHECK(octo::carrier(true, true) != octo::LinkWay::kBluetooth);
}

void test_bluetooth_comes_up_only_when_it_is_needed() {
  CHECK(!octo::want_bluetooth(true, false));
  CHECK(octo::want_bluetooth(false, false));
}

// The debug mode exists because the obvious way to test the radio path -- pull
// the cable -- tests it with no way left to see what the box thinks is
// happening. Both links up, USB still carrying.
void test_debug_brings_bluetooth_up_alongside_usb() {
  CHECK(octo::want_bluetooth(true, true));
  CHECK(octo::want_bluetooth(false, true));
  // ...and does not change which one is in charge.
  CHECK(octo::carrier(true, true) == octo::LinkWay::kUsb);
}

// A misspelling that silently means "off" is a debug session spent testing
// nothing, so anything unrecognised is refused rather than defaulted.
void test_the_bluetooth_setting_refuses_what_it_does_not_know() {
  octo::BleUse use = octo::BleUse::kAuto;
  CHECK(octo::parse_ble_use("off", &use));
  CHECK(use == octo::BleUse::kOff);
  CHECK(octo::parse_ble_use("auto", &use));
  CHECK(use == octo::BleUse::kAuto);
  CHECK(octo::parse_ble_use("both", &use));
  CHECK(use == octo::BleUse::kBoth);
  // A synonym a person would reasonably type for the debug setting.
  CHECK(octo::parse_ble_use("debug", &use));
  CHECK(use == octo::BleUse::kBoth);

  octo::BleUse untouched = octo::BleUse::kAuto;
  CHECK(!octo::parse_ble_use("yes", &untouched));
  CHECK(!octo::parse_ble_use("", &untouched));
  CHECK(!octo::parse_ble_use("BOTH", &untouched));
  CHECK(untouched == octo::BleUse::kAuto);
}

void test_link_way_names() {
  CHECK_STR(octo::link_way_name(octo::LinkWay::kUsb), "usb");
  CHECK_STR(octo::link_way_name(octo::LinkWay::kBluetooth), "bluetooth");
  CHECK_STR(octo::link_way_name(octo::LinkWay::kNone), "none");
}

}  // namespace

int main() {
  test_nothing_is_reported_before_the_dongle_is_attached();
  test_a_finished_answer_becomes_rows();
  test_ages_advance_between_answers();
  test_a_half_arrived_answer_is_not_shown();
  test_an_empty_answer_clears_the_room();
  test_an_answer_that_goes_stale_stops_being_live();
  test_unplugging_keeps_the_last_answer_but_kills_it();
  test_a_box_with_no_clock_has_no_time();
  test_the_registrys_stand_in_for_a_name_is_not_a_name();
  test_alarms_from_a_free_running_box_are_not_imported();
  test_a_named_box_names_its_own_radio();
  test_the_clock_is_free_until_the_box_says_otherwise();
  test_what_the_box_said_is_kept();
  test_a_port_that_has_not_greeted_is_never_written_to();
  test_a_greeting_does_not_survive_unplugging();
  test_polling_starts_at_once_and_then_waits();
  test_a_poll_that_was_not_sent_does_not_count();
  test_unknown_verbs_are_ignored();
  test_an_end_for_something_else_does_not_finish_a_batch();
  test_a_greeted_box_is_told_todays_date_once();
  test_a_box_that_came_back_is_told_the_date_again();
  test_a_host_with_no_date_says_nothing();
  test_usb_wins_whenever_it_is_there();
  test_exactly_one_link_carries_the_conversation();
  test_bluetooth_comes_up_only_when_it_is_needed();
  test_debug_brings_bluetooth_up_alongside_usb();
  test_the_bluetooth_setting_refuses_what_it_does_not_know();
  test_link_way_names();
  return octotest::report("test_dongle");
}
