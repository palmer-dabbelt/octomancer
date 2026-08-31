// See firmware/src/blepeer.h.
#include "blepeer.h"

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/sys/util.h>

#include <vector>

// Nordic's UART Service. See the header for why these rather than a private
// set: it is the one "bytes in, bytes out" GATT service every generic
// Bluetooth tool already knows how to open, and this device has no other way
// of being looked at.
//
// BT_UUID_128_ENCODE rather than a hand-written byte list, because a 128-bit
// UUID goes on the wire least-significant byte first and writing that out by
// hand is the kind of mistake that produces a service nothing can find and no
// error anywhere.
#define OCTO_NUS_SVC_VAL \
  BT_UUID_128_ENCODE(0x6E400001, 0xB5A3, 0xF393, 0xE0A9, 0xE50E24DCCA9E)
#define OCTO_NUS_RX_VAL \
  BT_UUID_128_ENCODE(0x6E400002, 0xB5A3, 0xF393, 0xE0A9, 0xE50E24DCCA9E)
#define OCTO_NUS_TX_VAL \
  BT_UUID_128_ENCODE(0x6E400003, 0xB5A3, 0xF393, 0xE0A9, 0xE50E24DCCA9E)

static struct bt_uuid_128 octo_nus_svc = BT_UUID_INIT_128(OCTO_NUS_SVC_VAL);
// RX is what the *central* writes to; TX is what the box notifies on. The
// names are from the central's point of view, which is Nordic's convention and
// worth keeping even though it reads backwards from in here.
static struct bt_uuid_128 octo_nus_rx = BT_UUID_INIT_128(OCTO_NUS_RX_VAL);
static struct bt_uuid_128 octo_nus_tx = BT_UUID_INIT_128(OCTO_NUS_TX_VAL);

// The one peer, because the table below is a static definition with C
// callbacks and there is nowhere in it to put a `this`. Exactly one BlePeer
// ever exists -- there is one radio -- so a file-scope pointer is the honest
// spelling rather than a limitation being worked around.
static octo::BlePeer* g_ble_peer = nullptr;

static ssize_t octo_nus_write(struct bt_conn* conn,
                              const struct bt_gatt_attr* attr, const void* buf,
                              uint16_t len, uint16_t offset, uint8_t flags) {
  ARG_UNUSED(conn);
  ARG_UNUSED(attr);
  ARG_UNUSED(flags);
  // A long write arrives in pieces with a growing offset. Reassembling it here
  // would mean a buffer per connection for no gain -- the protocol is lines,
  // and a line too long for one write can simply arrive as two. So the offset
  // is ignored and the bytes are appended in the order they came, which is the
  // order they were sent.
  ARG_UNUSED(offset);
  if (g_ble_peer != nullptr) {
    g_ble_peer->on_written(static_cast<const uint8_t*>(buf), len);
  }
  return len;
}

static void octo_nus_ccc(const struct bt_gatt_attr* attr, uint16_t value) {
  ARG_UNUSED(attr);
  if (g_ble_peer != nullptr) {
    g_ble_peer->on_subscribed(value == BT_GATT_CCC_NOTIFY);
  }
}

BT_GATT_SERVICE_DEFINE(
    octo_nus, BT_GATT_PRIMARY_SERVICE(&octo_nus_svc),
    BT_GATT_CHARACTERISTIC(&octo_nus_tx.uuid, BT_GATT_CHRC_NOTIFY,
                           BT_GATT_PERM_NONE, NULL, NULL, NULL),
    BT_GATT_CCC(octo_nus_ccc, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
    BT_GATT_CHARACTERISTIC(&octo_nus_rx.uuid,
                           BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP,
                           BT_GATT_PERM_WRITE, NULL, octo_nus_write, NULL));

// Attribute 2 is the TX characteristic's *value*, which is what a notification
// goes on -- attribute 1 is its declaration. Named, because getting it wrong
// notifies the wrong attribute and fails quietly.
static const int kTxValueAttr = 2;

static void octo_connected(struct bt_conn* conn, uint8_t err) {
  if (err != 0) return;
  if (g_ble_peer != nullptr) g_ble_peer->on_connected(bt_conn_ref(conn));
}

static void octo_disconnected(struct bt_conn* conn, uint8_t reason) {
  ARG_UNUSED(conn);
  ARG_UNUSED(reason);
  if (g_ble_peer != nullptr) g_ble_peer->on_disconnected();
}

BT_CONN_CB_DEFINE(octo_conn_cb) = {
    .connected = octo_connected,
    .disconnected = octo_disconnected,
};

// Connectable and named. The name is all a person scanning with a phone has to
// go on, and CONFIG_BT_DEVICE_NAME is where it comes from.
static const struct bt_data kAdvert[] = {
    BT_DATA_BYTES(BT_DATA_FLAGS, BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR),
    BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME,
            sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};

// The service UUID goes in the scan response rather than the advertisement. A
// 128-bit UUID is sixteen bytes and the name takes most of what is left of the
// legacy thirty-one; putting both in one packet leaves room for neither.
static const struct bt_data kScanResponse[] = {
    BT_DATA_BYTES(BT_DATA_UUID128_ALL, OCTO_NUS_SVC_VAL),
};

namespace octo {

BlePeer::BlePeer(Loop* loop) : loop_(loop) {
  k_poll_signal_init(&rx_signal_);
  ring_buf_init(&rx_, sizeof rx_storage_, rx_storage_);
  g_ble_peer = this;
}

BlePeer::~BlePeer() {
  stop();
  if (g_ble_peer == this) g_ble_peer = nullptr;
}

bool BlePeer::start(std::string* err) {
  if (started_) return true;

  Handle handle;
  handle.object = &rx_signal_;
  source_ = loop_->add_source(
      handle, kRead, [this](int) { drain(); }, [](const std::string&) {});

  started_ = true;
  advertise();
  if (!advertising_) {
    if (err != nullptr) *err = "the radio would not advertise";
    stop();
    return false;
  }
  return true;
}

void BlePeer::stop() {
  if (!started_) return;
  if (advertising_) {
    bt_le_adv_stop();
    advertising_ = false;
  }
  if (source_ != kNoSource) loop_->remove_source(source_);
  source_ = kNoSource;
  started_ = false;
}

void BlePeer::advertise() {
  if (advertising_ || !started_ || connected_) return;
  // Fast rather than the slow default: this is a device somebody is trying to
  // reach on purpose, not a beacon, and seconds spent waiting for it to become
  // discoverable are seconds spent wondering whether the firmware is running
  // at all -- which, on a box with no console, is not a cheap doubt.
  const int rc =
      bt_le_adv_start(BT_LE_ADV_CONN_FAST_1, kAdvert, ARRAY_SIZE(kAdvert),
                      kScanResponse, ARRAY_SIZE(kScanResponse));
  advertising_ = rc == 0;
}

// ---------------------------------------------------- Bluetooth's thread

void BlePeer::on_written(const uint8_t* data, uint16_t len) {
  if (len == 0) return;
  const uint32_t put = ring_buf_put(&rx_, data, len);
  if (put < len) dropped_rx_ += len - put;
  // Idempotent: a signal already raised stays raised, so a burst of writes is
  // one wake rather than a queue of them.
  k_poll_signal_raise(&rx_signal_, 1);
}

void BlePeer::on_connected(struct bt_conn* conn) {
  // The reference is taken by the caller and released by the loop, not here.
  // See settle(): the loop needs the connection to ask what the MTU is, and
  // dropping the last reference on this thread would free it out from under a
  // send() that had already started.
  conn_ = conn;
  connected_ = true;
  ++sessions_;
  advertising_ = false;  // the controller stops advertising on connect
  k_poll_signal_raise(&rx_signal_, 1);
}

void BlePeer::on_disconnected() {
  connected_ = false;
  notify_on_ = false;
  k_poll_signal_raise(&rx_signal_, 1);
}

void BlePeer::on_subscribed(bool on) {
  notify_on_ = on;
  k_poll_signal_raise(&rx_signal_, 1);
}

// ---------------------------------------------------------- loop thread

void BlePeer::drain() {
  settle();

  for (;;) {
    uint8_t buf[128];
    const uint32_t got = ring_buf_get(&rx_, buf, sizeof buf);
    if (got == 0) break;
    std::vector<std::string> lines;
    if (!reader_.feed(reinterpret_cast<const char*>(buf), got, &lines)) {
      ++long_lines_;
    }
    for (const std::string& line : lines) {
      if (on_line_) on_line_(line);
    }
  }
}

// A central that has connected but not subscribed cannot be talked to, so it
// is not a peer yet. Treating it as one would send the greeting into a
// notification nobody had enabled, and the daemon would then believe it had
// introduced itself -- and never do it again.
void BlePeer::settle() {
  const bool ready = connected_ && notify_on_;
  if (ready != attached_) {
    attached_ = ready;
    if (attached_) {
      reader_.reset();
      if (on_open_) on_open_();
    } else if (on_close_) {
      on_close_();
    }
  }

  if (!connected_) {
    // The reference the connection callback took, released here rather than
    // there so that a send() already running on this thread cannot be left
    // holding a freed connection.
    if (conn_ != nullptr) {
      bt_conn_unref(conn_);
      conn_ = nullptr;
    }
    // The controller stopped advertising when it accepted the connection, so
    // without this the box is unreachable until somebody power-cycles it.
    advertise();
  }
}

void BlePeer::send(const std::string& line) {
  if (!attached_) {
    ++dropped_tx_;
    return;
  }
  std::string out = line;
  out += '\n';

  // A notification carries at most the MTU minus three bytes of ATT header,
  // and the MTU is whatever the central agreed to -- 23 until it asks for
  // more, which leaves twenty bytes. So a line is split, and the far end puts
  // it back together with the same LineReader the cable uses: a notification
  // is not a message boundary, any more than a serial read is.
  size_t chunk = 20;
  if (conn_ != nullptr) {
    const uint16_t mtu = bt_gatt_get_mtu(conn_);
    if (mtu > 3) chunk = static_cast<size_t>(mtu - 3);
  }

  for (size_t at = 0; at < out.size(); at += chunk) {
    const size_t n = out.size() - at < chunk ? out.size() - at : chunk;
    // A null connection means "everyone who subscribed", which with one
    // connection allowed is the same thing and saves reasoning about the
    // reference here.
    const int rc = bt_gatt_notify(nullptr, &octo_nus.attrs[kTxValueAttr],
                                  out.data() + at, static_cast<uint16_t>(n));
    if (rc != 0) {
      // Out of buffers, or the link went while we were writing. Counted and
      // abandoned rather than queued: an announcement is a perishable
      // statement about the present, and there is nowhere on this device to
      // keep one that has gone stale.
      //
      // The far end sees a truncated line, which its LineReader will fail to
      // decode and count as a bad line. That is the right outcome -- a
      // half-message must not be acted on -- and it is why both ends count
      // what they could not read.
      ++dropped_tx_;
      return;
    }
  }
}

}  // namespace octo
