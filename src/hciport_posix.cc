// POSIX serial transport. Builds on macOS and on Linux, which is the first
// place this project has code that is not tied to one host's radio API.
#include "hciport.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <string>
#include <vector>

namespace octo {
namespace hci {
namespace {

const char kNoPortMessage[] = "no Bluetooth dongle found";

// The name a USB CDC ACM device gets, per platform. macOS offers each port
// twice: /dev/tty.* blocks on open until carrier detect, which for a dongle
// never arrives, so only the callout device is ever a candidate.
#if defined(__APPLE__)
const char* kPrefixes[] = {"cu.usbmodem", "cu.usbserial", nullptr};
#else
const char* kPrefixes[] = {"ttyACM", "ttyUSB", nullptr};
#endif

bool has_prefix(const std::string& s, const char* prefix) {
  return s.size() > std::strlen(prefix) &&
         s.compare(0, std::strlen(prefix), prefix) == 0;
}

class PosixPort : public Port {
 public:
  PosixPort(int fd, std::string name) : fd_(fd), name_(std::move(name)) {}
  ~PosixPort() override { close(); }

  int read(uint8_t* buf, size_t len, double timeout) override {
    if (fd_ < 0) return -1;
    struct pollfd pfd;
    pfd.fd = fd_;
    pfd.events = POLLIN;
    pfd.revents = 0;
    int ms = timeout < 0 ? -1 : static_cast<int>(timeout * 1000.0);
    int rc = ::poll(&pfd, 1, ms);
    if (rc == 0) return 0;
    if (rc < 0) return errno == EINTR ? 0 : -1;
    // POLLHUP without POLLIN is the dongle having been unplugged. Reporting it
    // as a timeout would leave a scanner spinning forever on a dead port.
    if ((pfd.revents & (POLLERR | POLLNVAL)) != 0) return -1;
    ssize_t n = ::read(fd_, buf, len);
    if (n < 0) {
      if (errno == EINTR || errno == EAGAIN) return 0;
      return -1;
    }
    if (n == 0) return (pfd.revents & POLLHUP) ? -1 : 0;
    return static_cast<int>(n);
  }

  bool write(const uint8_t* data, size_t len) override {
    if (fd_ < 0) return false;
    size_t off = 0;
    while (off < len) {
      ssize_t n = ::write(fd_, data + off, len - off);
      if (n < 0) {
        if (errno == EINTR) continue;
        if (errno == EAGAIN) {
          // The CDC endpoint is momentarily full. Wait for room rather than
          // dropping the rest of a command, which the controller would then
          // read as whatever follows it.
          struct pollfd pfd;
          pfd.fd = fd_;
          pfd.events = POLLOUT;
          pfd.revents = 0;
          if (::poll(&pfd, 1, 1000) > 0) continue;
        }
        return false;
      }
      off += static_cast<size_t>(n);
    }
    return true;
  }

  void close() override {
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
  }

  bool is_open() const override { return fd_ >= 0; }
  std::string name() const override { return name_; }

 private:
  int fd_ = -1;
  std::string name_;
};

int open_raw(const std::string& path, std::string* err) {
  // O_NONBLOCK on open matters even for a callout device: without it, opening
  // a port whose far end is not asserting the modem lines can block
  // indefinitely with no diagnostic at all.
  int fd = ::open(path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
  if (fd < 0) {
    if (err) *err = path + ": " + std::strerror(errno);
    return -1;
  }
  // Back to blocking now that the port is open; reads are gated by poll().
  int flags = ::fcntl(fd, F_GETFL, 0);
  if (flags >= 0) ::fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);

  struct termios tio;
  if (::tcgetattr(fd, &tio) != 0) {
    // A CDC ACM device that is not a tty at all is unusual but not fatal: the
    // USB stack still moves bytes. Only report the failure if raw mode cannot
    // be established on something that claims to be a terminal.
    if (err) *err = path + ": tcgetattr: " + std::strerror(errno);
    ::close(fd);
    return -1;
  }
  ::cfmakeraw(&tio);
  tio.c_cflag |= (CLOCAL | CREAD);
  tio.c_cflag &= ~CRTSCTS;
  // Read returns as soon as any byte is available. HCI packets are short and
  // latency here is timecode accuracy, so waiting to fill a buffer is exactly
  // the wrong trade.
  tio.c_cc[VMIN] = 0;
  tio.c_cc[VTIME] = 0;
  // The baud rate is meaningless over USB CDC -- the endpoint runs at USB
  // speed -- but a sane value has to be set or some drivers refuse the port.
  ::cfsetispeed(&tio, B115200);
  ::cfsetospeed(&tio, B115200);
  if (::tcsetattr(fd, TCSANOW, &tio) != 0) {
    if (err) *err = path + ": tcsetattr: " + std::strerror(errno);
    ::close(fd);
    return -1;
  }
  ::tcflush(fd, TCIOFLUSH);
  return fd;
}

}  // namespace

std::vector<std::string> list_candidate_ports() {
  std::vector<std::string> out;
  DIR* dir = ::opendir("/dev");
  if (!dir) return out;
  struct dirent* e;
  while ((e = ::readdir(dir)) != nullptr) {
    std::string name(e->d_name);
    for (const char** p = kPrefixes; *p; ++p) {
      if (has_prefix(name, *p)) {
        out.push_back("/dev/" + name);
        break;
      }
    }
  }
  ::closedir(dir);
  // Stable order, so "the first candidate" means the same thing across runs
  // and a log naming a port stays meaningful.
  std::sort(out.begin(), out.end());
  return out;
}

std::unique_ptr<Port> open_port(const std::string& device, std::string* err) {
  if (!device.empty()) {
    int fd = open_raw(device, err);
    if (fd < 0) return nullptr;
    return std::unique_ptr<Port>(new PosixPort(fd, device));
  }

  std::vector<std::string> candidates = list_candidate_ports();
  if (candidates.empty()) {
    if (err) *err = kNoPortMessage;
    return nullptr;
  }
  std::string first_error;
  for (const std::string& path : candidates) {
    std::string e;
    int fd = open_raw(path, &e);
    if (fd >= 0) return std::unique_ptr<Port>(new PosixPort(fd, path));
    if (first_error.empty()) first_error = e;
  }
  // Candidates existed but none opened. That is a real failure -- most likely
  // another process holds the port -- and must not be reported as "no dongle",
  // or the caller silently falls back to the other radio and the user never
  // finds out why the dongle did nothing.
  if (err) *err = first_error;
  return nullptr;
}

bool no_port_found(const std::string& err) { return err == kNoPortMessage; }

}  // namespace hci
}  // namespace octo
