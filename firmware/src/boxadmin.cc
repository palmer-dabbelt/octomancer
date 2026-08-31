// See firmware/src/boxadmin.h.
#include "boxadmin.h"

#include <hal/nrf_power.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/reboot.h>

#include "boxmsg.h"
#include "faultlog.h"
#include "scanner_zephyr.h"

namespace octo {
namespace {

}  // namespace

bool handle_box_admin(const std::string& line, CdcPeer* peer) {
  Message msg;
  std::string err;
  if (!decode(line, &msg, &err)) return false;  // the daemon reports bad lines

  if (msg.verb == "boxstats") {
    Message out;
    out.verb = "boxstats";
    if (msg.has("id")) out.set("id", msg.get("id"));
    // Everything that can be dropped, and was. All cumulative: the question a
    // person asks is "has this ever happened", not "is it happening now".
    out.set_int("adverts_dropped", zephyr_scanner_dropped());
    out.set_int("tx_dropped", peer->dropped_tx());
    out.set_int("rx_dropped", peer->dropped_rx());
    out.set_int("long_lines", peer->long_lines());
    // Whether this build can print the numbers it is being asked for. If this
    // is 0 then every other figure in this message is an integer that happened
    // to survive, and every double the box has sent is empty.
    out.set_bool("floats", can_format_doubles());
    // Free heap is not reported. There is no supported way to ask picolibc's
    // arena how much of it is left, and a figure invented here would be worse
    // than the silence -- it is exactly the kind of number that gets believed.
    peer->send(encode(out));
    return true;
  }

  if (msg.verb == "dfu") {
    // Confirmation required. This is the one command that ends the
    // conversation, and a client retrying a garbled line should not be able to
    // take the radio off the air by accident.
    bool confirm = false;
    if (!msg.get_bool("confirm", &confirm) || !confirm) {
      Message bad;
      bad.verb = "err";
      if (msg.has("id")) bad.set("id", msg.get("id"));
      bad.set("reason", "needs-confirm");
      bad.set("hint", "dfu confirm=1");
      peer->send(encode(bad));
      return true;
    }

    Message out;
    out.verb = "ok";
    if (msg.has("id")) out.set("id", msg.get("id"));
    out.set("what", "dfu");
    peer->send(encode(out));

    // Let the reply reach the host before the USB device disappears. There is
    // no way to ask the CDC ring whether the endpoint has drained, and a
    // reboot that beats the acknowledgement out of the door looks exactly like
    // a crash. A tenth of a second is far longer than a bulk transfer and far
    // shorter than a person's patience.
    k_sleep(K_MSEC(100));

    // A person replacing the image is a person saying the old one's troubles
    // are over. Without this, a box that failed its way into safe mode stays
    // in safe mode across the reflash that was meant to fix it -- the count
    // lives in memory that a soft reset does not clear -- and the only way out
    // is a power cycle, which is the trip to the desk this whole mechanism
    // exists to avoid.
    forget_boot_failures();
    enter_bootloader();
    return true;  // not reached
  }

  return false;
}

}  // namespace octo
