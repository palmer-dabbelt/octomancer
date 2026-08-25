// The daemon's control socket.
//
// Strictly one command, one reply, close. There is no streaming mode and no
// subscription: a client that wants live data asks again. Over a Unix socket
// that costs microseconds, and it means neither side has to carry connection
// state, which is where socket servers usually go wrong.
#ifndef OCTO_SERVER_H
#define OCTO_SERVER_H

#include <string>
#include <vector>

#include "registry.h"

namespace octo {

// Where the socket lives by default: under the user's Application Support,
// because this is a per-user agent and its socket is not shared.
std::string default_socket_path();

class Server {
 public:
  Server(const Registry& registry, std::string path);
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

  const Registry& registry_;
  std::string path_;
  int listen_fd_ = -1;
  bool bound_ = false;
  std::vector<Client> clients_;
};

}  // namespace octo

#endif  // OCTO_SERVER_H
