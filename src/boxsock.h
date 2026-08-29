// The box protocol over a unix socket, on the loop.
//
// One of three transports for the same message language -- a unix socket when
// the sync daemon is a process on this Mac, a USB CDC serial port when it is a
// Nordic on the end of a cable, and a BLE characteristic when that Nordic is
// powered from something else. They differ in framing and in how much fits in
// one write, and in nothing else, which is why src/boxmsg.h is one piece of
// portable code and this file is thin.
//
// It is deliberately not src/server.h. That one is strictly one command, one
// reply, close, over proto.h's block-of-lines format, and it suits a client
// that asks a question and goes away. This link stays open and carries
// unsolicited announcements, so the framing has to be one message per line and
// a connection has to have state. Two different jobs; the older one is still
// the right shape for octomancerd and is left alone.
//
// Nothing here parses a message. It moves whole lines in both directions and
// hands them to whoever asked, which is what makes it substitutable: the CDC
// console and the GATT characteristic will do the same job with different
// system calls.
#ifndef OCTO_BOXSOCK_H
#define OCTO_BOXSOCK_H

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "boxmsg.h"
#include "loop.h"
#include "syncd.h"

namespace octo {

// ~/Library/Application Support/octomancer/octomancer-syncd.sock
//
// A different file from src/server.h's, because a different protocol is
// spoken on it. One socket serving two formats would be one socket where the
// first line a client sends decides which program it is talking to.
std::string default_box_socket_path();

class LineServer {
 public:
  using OnOpen = std::function<void(MsgPeer* peer)>;
  using OnLine = std::function<void(MsgPeer* peer, const std::string& line)>;
  using OnClose = std::function<void(MsgPeer* peer)>;

  LineServer(Loop* loop, std::string path);
  ~LineServer();

  LineServer(const LineServer&) = delete;
  LineServer& operator=(const LineServer&) = delete;

  bool start(std::string* err);
  void stop();

  void on_open(OnOpen handler);
  void on_line(OnLine handler);
  void on_close(OnClose handler);

  const std::string& path() const { return path_; }
  size_t clients() const;

  // How much unwritten output a client may accumulate before it is dropped.
  //
  // This is not a tuning knob so much as an answer to a question the
  // announcements raise. A peer that connects and then stops reading -- a
  // suspended UI, a console whose window is scrolled and paused, a serial
  // cable pulled out mid-line -- would otherwise make the daemon hold every
  // announcement it ever produced. Dropping that peer is the only answer that
  // does not eventually consume the process, and on the box there is a quarter
  // of a megabyte to consume.
  void set_max_pending(size_t bytes);

 private:
  class Client;

  void accept_one();
  void drop(Client* client);
  // A client that cannot be written to is marked rather than dropped on the
  // spot, because the call that discovers it is a call from inside the
  // daemon's own loop over its peers. See Client::send.
  void schedule_sweep();
  void sweep();

  Loop* loop_ = nullptr;
  std::string path_;
  int listen_fd_ = -1;
  bool bound_ = false;
  SourceId listen_source_ = kNoSource;
  size_t max_pending_ = 1u << 20;

  OnOpen on_open_;
  OnLine on_line_;
  OnClose on_close_;

  std::vector<std::unique_ptr<Client>> clients_;
  bool sweep_pending_ = false;
  // Checked by the deferred sweep, which is a timer that captures `this`.
  std::shared_ptr<bool> alive_;
};

}  // namespace octo

#endif  // OCTO_BOXSOCK_H
