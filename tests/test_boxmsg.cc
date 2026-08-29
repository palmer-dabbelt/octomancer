// The message layer the box speaks over USB, BLE and a unix socket.
//
// What is pinned here is mostly round trips and mostly hostile input. Names
// arrive from the air and go straight into a message, and a box on a rig is
// reachable by anyone in range, so nothing here may assume a well-formed peer.

#include <string>
#include <vector>

#include "boxmsg.h"
#include "harness.h"

using octo::decode;
using octo::encode;
using octo::LineReader;
using octo::Message;

namespace {

void test_a_message_round_trips() {
  Message m;
  m.verb = "dev";
  m.set("id", "C7:49:78:1A:39:60");
  m.set("name", "Krysta");
  m.set_double("offset", -6.2314);
  m.set_int("rssi", -60);
  m.set_bool("live", true);

  const std::string line = encode(m);
  CHECK_STR(line,
            "dev id=C7:49:78:1A:39:60 name=Krysta offset=-6.2314 rssi=-60 "
            "live=1");

  Message back;
  std::string err;
  CHECK(decode(line, &back, &err));
  CHECK_STR(back.verb, "dev");
  CHECK_STR(back.get("id"), "C7:49:78:1A:39:60");
  CHECK_STR(back.get("name"), "Krysta");

  double offset = 0.0;
  CHECK(back.get_double("offset", &offset));
  CHECK_NEAR(offset, -6.2314, 1e-9);

  int64_t rssi = 0;
  CHECK(back.get_int("rssi", &rssi));
  CHECK_EQ(rssi, static_cast<int64_t>(-60));

  bool live = false;
  CHECK(back.get_bool("live", &live));
  CHECK(live);
}

void test_a_hostile_name_cannot_break_the_line() {
  // Box names are user-set and arrive over the air. This one contains a
  // space, an '=', a newline and a percent -- every character the tokenizer
  // uses -- and must come back byte for byte.
  const std::string nasty = "a b=c%d\ne\tf";
  Message m;
  m.verb = "dev";
  m.set("name", nasty);

  const std::string line = encode(m);
  CHECK(line.find('\n') == std::string::npos);
  CHECK(line.find(' ') != std::string::npos);  // the one separating the field

  Message back;
  CHECK(decode(line, &back, nullptr));
  CHECK_STR(back.get("name"), nasty);
}

void test_an_empty_value_survives() {
  Message m;
  m.verb = "dev";
  m.set("name", "");
  m.set("id", "x");

  Message back;
  CHECK(decode(encode(m), &back, nullptr));
  CHECK(back.has("name"));
  CHECK_STR(back.get("name"), "");
  CHECK_STR(back.get("id"), "x");
}

void test_unknown_fields_are_kept_not_rejected() {
  // A Mac and a box will be at different versions almost always. A field the
  // reader has never heard of must not make it give up on the line.
  Message back;
  std::string err;
  CHECK(decode("status ver=1 somethingnew=42 up=15", &back, &err));
  CHECK_STR(back.verb, "status");
  int64_t up = 0;
  CHECK(back.get_int("up", &up));
  CHECK_EQ(up, static_cast<int64_t>(15));
  CHECK_STR(back.get("somethingnew"), "42");
}

void test_malformed_lines_are_refused_with_a_reason() {
  Message m;
  std::string err;

  CHECK(!decode("", &m, &err));
  CHECK(!err.empty());
  CHECK(!decode("   ", &m, &err));
  CHECK(!decode("\r\n", &m, &err));

  // A bare word where a field belongs is a typo. Reading it as a flag is how
  // a mistyped command quietly does something else.
  CHECK(!decode("status verbose", &m, &err));

  // A verb with a character the tokenizer would not survive.
  CHECK(!decode("st@tus x=1", &m, &err));
  CHECK(!decode("status na=1 m e=x", &m, &err));

  // A space inside what was meant as one verb is not an error, it is a
  // different verb: "st atus=1" is the verb "st" with a field. Worth pinning
  // so nobody later "fixes" it into a guess about what was intended.
  Message split;
  CHECK(decode("st atus=1", &split, &err));
  CHECK_STR(split.verb, "st");
  CHECK_STR(split.get("atus"), "1");
}

void test_a_repeated_key_is_a_list() {
  Message m;
  m.verb = "roster";
  m.add("id", "one");
  m.add("id", "two");
  m.add("id", "three");

  Message back;
  CHECK(decode(encode(m), &back, nullptr));
  const std::vector<std::string> ids = back.all("id");
  CHECK_EQ(ids.size(), static_cast<size_t>(3));
  CHECK_STR(ids[0], "one");
  CHECK_STR(ids[2], "three");
  // get() takes the first, because a repeated key is a list and the reader
  // that wants one wants the one sent first.
  CHECK_STR(back.get("id"), "one");
}

void test_set_replaces_and_add_appends() {
  Message m;
  m.verb = "x";
  m.set("k", "1");
  m.set("k", "2");
  CHECK_EQ(m.all("k").size(), static_cast<size_t>(1));
  CHECK_STR(m.get("k"), "2");

  m.add("k", "3");
  CHECK_EQ(m.all("k").size(), static_cast<size_t>(2));
}

void test_absent_is_not_the_same_as_zero() {
  // 000000 is a passkey a camera might really be displaying, so a reader has
  // to be able to tell "no passkey given" from "the passkey is zero".
  Message m;
  CHECK(decode("pair passkey=000000", &m, nullptr));
  int64_t pk = -1;
  CHECK(m.get_int("passkey", &pk));
  CHECK_EQ(pk, static_cast<int64_t>(0));

  Message none;
  CHECK(decode("pair", &none, nullptr));
  int64_t untouched = -1;
  CHECK(!none.get_int("passkey", &untouched));
  CHECK_EQ(untouched, static_cast<int64_t>(-1));
}

void test_a_number_with_trailing_junk_does_not_parse() {
  Message m;
  CHECK(decode("x n=12abc", &m, nullptr));
  int64_t n = 0;
  CHECK(!m.get_int("n", &n));
  double d = 0.0;
  CHECK(!m.get_double("n", &d));
}

void test_bools_accept_what_a_person_would_type() {
  Message m;
  CHECK(decode("cfg a=yes b=off c=true d=0 e=maybe", &m, nullptr));
  bool v = false;
  CHECK(m.get_bool("a", &v));
  CHECK(v);
  CHECK(m.get_bool("b", &v));
  CHECK(!v);
  CHECK(m.get_bool("c", &v));
  CHECK(v);
  CHECK(m.get_bool("d", &v));
  CHECK(!v);
  CHECK(!m.get_bool("e", &v));
}

void test_a_trailing_carriage_return_is_tolerated() {
  // A person typing into a terminal emulator over USB serial sends CRLF.
  Message m;
  CHECK(decode("status ver=1\r", &m, nullptr));
  CHECK_STR(m.verb, "status");
  CHECK_STR(m.get("ver"), "1");
}

void test_the_line_reader_reassembles_split_writes() {
  // No transport here delivers a message: a serial port delivers whatever the
  // driver had, and a GATT write delivers at most one MTU.
  LineReader r;
  std::vector<std::string> lines;

  CHECK(r.feed("sta", &lines));
  CHECK_EQ(lines.size(), static_cast<size_t>(0));
  CHECK(r.feed("tus ver=1\nros", &lines));
  CHECK_EQ(lines.size(), static_cast<size_t>(1));
  CHECK_STR(lines[0], "status ver=1");

  CHECK(r.feed("ter n=2\n", &lines));
  CHECK_EQ(lines.size(), static_cast<size_t>(2));
  CHECK_STR(lines[1], "roster n=2");
}

void test_the_line_reader_splits_several_lines_in_one_write() {
  LineReader r;
  std::vector<std::string> lines;
  CHECK(r.feed("a=1\nb=2\nc=3\n", &lines));
  CHECK_EQ(lines.size(), static_cast<size_t>(3));
  CHECK_STR(lines[1], "b=2");
}

void test_the_line_reader_caps_an_endless_line() {
  // A peer that never sends a newline must not be an unbounded allocation on
  // a device with 256 KB of RAM.
  LineReader r;
  std::vector<std::string> lines;
  const std::string big(LineReader::kMaxLine + 100, 'x');

  CHECK(r.feed(big, &lines));  // nothing dropped yet: no newline seen
  CHECK_EQ(lines.size(), static_cast<size_t>(0));
  CHECK(r.buffered() <= LineReader::kMaxLine);

  // The newline ends the doomed line and reports the loss once.
  CHECK(!r.feed("\n", &lines));
  CHECK_EQ(lines.size(), static_cast<size_t>(0));

  // And the reader is usable again immediately afterwards.
  CHECK(r.feed("status ver=1\n", &lines));
  CHECK_EQ(lines.size(), static_cast<size_t>(1));
  CHECK_STR(lines[0], "status ver=1");
}

void test_the_line_reader_ignores_empty_lines_at_the_message_layer() {
  // Blank lines are legal on the wire and become nothing: a person pressing
  // return on a console should not produce an error.
  LineReader r;
  std::vector<std::string> lines;
  CHECK(r.feed("\n\nstatus\n", &lines));
  CHECK_EQ(lines.size(), static_cast<size_t>(3));

  Message m;
  CHECK(!decode(lines[0], &m, nullptr));
  CHECK(decode(lines[2], &m, nullptr));
}

}  // namespace

int main() {
  test_a_message_round_trips();
  test_a_hostile_name_cannot_break_the_line();
  test_an_empty_value_survives();
  test_unknown_fields_are_kept_not_rejected();
  test_malformed_lines_are_refused_with_a_reason();
  test_a_repeated_key_is_a_list();
  test_set_replaces_and_add_appends();
  test_absent_is_not_the_same_as_zero();
  test_a_number_with_trailing_junk_does_not_parse();
  test_bools_accept_what_a_person_would_type();
  test_a_trailing_carriage_return_is_tolerated();
  test_the_line_reader_reassembles_split_writes();
  test_the_line_reader_splits_several_lines_in_one_write();
  test_the_line_reader_caps_an_endless_line();
  test_the_line_reader_ignores_empty_lines_at_the_message_layer();
  return octotest::report("test_boxmsg");
}
