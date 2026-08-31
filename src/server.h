// The daemon's control socket.
//
// Strictly one command, one reply, close. There is no streaming mode and no
// subscription: a client that wants live data asks again. Over a Unix socket
// that costs microseconds, and it means neither side has to carry connection
// state, which is where socket servers usually go wrong.
#ifndef OCTO_SERVER_H
#define OCTO_SERVER_H

#include <functional>
#include <string>
#include <vector>

#include "registry.h"

namespace octo {

// Where the socket lives by default: under the user's Application Support,
// because this is a per-user agent and its socket is not shared.
std::string default_socket_path();

// One request line in, one whole reply out. What a daemon does with the line
// is the daemon's business; this file only moves bytes.
using Handler = std::function<std::string(const std::string&)>;

// The handler octomancerd has always had: status, json, ping, over a registry.
//
// Not const any more, and the reason is worth stating: `forget` throws a
// device away, which is the first thing a client has ever been able to ask
// this daemon to *do* rather than to report. Everything else here is still a
// read.
Handler registry_handler(Registry& registry);

// Where a snapshot comes from, when it is not simply the registry's own.
//
// octomancerd assembles one: the registry's, plus whatever rows a dongle on
// the end of a cable has contributed. Passing the assembly in rather than
// teaching the registry about dongles keeps the registry what it is -- the
// record of what *this* radio heard -- and keeps the joining-up in the one
// program that knows a dongle exists.
using SnapshotSource = std::function<Snapshot()>;

// The same handler, over a snapshot somebody else assembles. `forget` still
// goes to the registry: a row from another radio is not ours to throw away,
// and it will be gone from the next answer that radio gives anyway.
Handler registry_handler(Registry& registry, SnapshotSource snapshot);

class Server {
 public:
  explicit Server(Handler handler, std::string path);
  // The tentacle daemon's shorthand for the above.
  Server(Registry& registry, std::string path);
  ~Server();

  Server(const Server&) = delete;
  Server& operator=(const Server&) = delete;

  bool start(std::string* err);

  // Accept and service whatever is ready, waiting at most timeout_ms.
  void serve(int timeout_ms);

  void shutdown();

  const std::string& path() const { return path_; }

 private:
  struct Client {
    int fd = -1;
    std::string in;
    std::string out;
    bool replied = false;
    double deadline = 0.0;
  };

  std::string handle(const std::string& command) const;
  void drop(size_t index);

  Handler handler_;
  std::string path_;
  int listen_fd_ = -1;
  bool bound_ = false;
  std::vector<Client> clients_;
};

}  // namespace octo

#endif  // OCTO_SERVER_H
