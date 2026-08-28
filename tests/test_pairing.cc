#include "../src/pairing.h"
#include "harness.h"

#include <string>

using namespace octo::pairing;

namespace {

bool mentions(const std::string& haystack, const char* needle) {
  return haystack.find(needle) != std::string::npos;
}

}  // namespace

int main() {
  // The reason this file exists. A camera that has lost its bond connects
  // happily and subscribes happily and then says nothing at all, so silence
  // after a good connection has to be read as "not paired" rather than as
  // "nothing to report".
  Attempt lost_bond;
  lost_bond.connected = true;
  lost_bond.subscribed = true;
  lost_bond.saw_state = false;
  CHECK(judge(lost_bond) == Verdict::kSilent);

  // Subscribing is not the thing that proves a bond; hearing something is.
  // A camera that never even accepted the subscription is in the same
  // position as one that did, and must not be reported differently.
  Attempt no_subscribe = lost_bond;
  no_subscribe.subscribed = false;
  CHECK(judge(no_subscribe) == Verdict::kSilent);

  Attempt working;
  working.connected = true;
  working.subscribed = true;
  working.saw_state = true;
  CHECK(judge(working) == Verdict::kBonded);

  Attempt never_got_there;
  never_got_there.connected = false;
  CHECK(judge(never_got_there) == Verdict::kNotConnected);

  // An error that names authentication outranks everything else, because it
  // is the one case where the cause is stated rather than inferred. Even
  // when the connection itself succeeded.
  Attempt refused;
  refused.connected = true;
  refused.subscribed = true;
  refused.radio_error = "Insufficient Authentication";
  CHECK(judge(refused) == Verdict::kRefused);

  // ...and even when nothing connected, so the better message wins over the
  // vaguer one.
  Attempt refused_early;
  refused_early.connected = false;
  refused_early.radio_error = "The attribute requires encryption";
  CHECK(judge(refused_early) == Verdict::kRefused);

  // The strings come from two unrelated layers, so the matcher has to cope
  // with however each of them phrases it.
  CHECK(names_authentication_failure("Insufficient Authentication"));
  CHECK(names_authentication_failure("insufficient encryption"));
  CHECK(names_authentication_failure("Peer removed pairing information"));
  CHECK(names_authentication_failure("device is not paired"));
  CHECK(!names_authentication_failure(""));
  CHECK(!names_authentication_failure("Connection timed out"));
  CHECK(!names_authentication_failure("Write Not Permitted"));

  // A verdict word is what a script would match on, so it is part of the
  // interface and pinned here.
  CHECK_STR(word(Verdict::kBonded), "bonded");
  CHECK_STR(word(Verdict::kNotConnected), "not-connected");
  CHECK_STR(word(Verdict::kRefused), "refused");
  CHECK_STR(word(Verdict::kSilent), "silent");

  // The advice is the whole value of the silent case: it has to say that the
  // code appears on the camera, because a person staring at the Mac waiting
  // for a number will wait forever.
  const std::string silent_advice = explain(Verdict::kSilent, "A:1EAE18A7");
  CHECK(mentions(silent_advice, "A:1EAE18A7"));
  CHECK(mentions(silent_advice, "six-digit"));
  CHECK(mentions(silent_advice, "displayed on the camera"));

  // An unnamed camera still has to produce a sentence that reads.
  const std::string anonymous = explain(Verdict::kNotConnected, "");
  CHECK(mentions(anonymous, "the camera"));
  CHECK(!mentions(anonymous, "  ."));

  CHECK(mentions(explain(Verdict::kBonded, "A:1EAE18A7"), "A:1EAE18A7"));

  return octotest::report("test_pairing");
}
