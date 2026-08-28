#include "agents.h"

#include <errno.h>
#include <mach-o/dyld.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>

namespace octo {

namespace {

std::string g_plist_dir;

std::string home() {
  const char* h = std::getenv("HOME");
  return h && *h ? h : "/tmp";
}

std::string uid_domain() {
  return "gui/" + std::to_string(static_cast<unsigned>(::getuid()));
}

bool exists(const std::string& path) {
  struct stat st;
  return ::stat(path.c_str(), &st) == 0;
}

std::string dirname_of(const std::string& path) {
  const size_t slash = path.rfind('/');
  return slash == std::string::npos ? std::string(".") : path.substr(0, slash);
}

// Where this binary is. Used to find the plists in an uninstalled build tree,
// which is the case every time somebody is working on octomancer rather than
// running it.
std::string executable_dir() {
  uint32_t size = 0;
  _NSGetExecutablePath(nullptr, &size);
  std::string buf(size, '\0');
  if (_NSGetExecutablePath(&buf[0], &size) != 0) return std::string();
  buf.resize(strlen(buf.c_str()));
  return dirname_of(buf);
}

// Run a command and collect what it said. The exit status is what callers act
// on; the output is only ever used to explain a failure to a person, or to
// find a pid.
int run(const std::vector<std::string>& argv, std::string* output) {
  int pipefd[2];
  if (::pipe(pipefd) != 0) return -1;

  const pid_t pid = ::fork();
  if (pid < 0) {
    ::close(pipefd[0]);
    ::close(pipefd[1]);
    return -1;
  }
  if (pid == 0) {
    ::close(pipefd[0]);
    ::dup2(pipefd[1], 1);
    ::dup2(pipefd[1], 2);
    ::close(pipefd[1]);
    std::vector<char*> args;
    args.reserve(argv.size() + 1);
    for (const std::string& a : argv) {
      args.push_back(const_cast<char*>(a.c_str()));
    }
    args.push_back(nullptr);
    ::execv(args[0], args.data());
    ::_exit(127);
  }

  ::close(pipefd[1]);
  std::string out;
  char buf[1024];
  for (;;) {
    const ssize_t n = ::read(pipefd[0], buf, sizeof buf);
    if (n <= 0) break;
    out.append(buf, static_cast<size_t>(n));
  }
  ::close(pipefd[0]);

  int status = 0;
  ::waitpid(pid, &status, 0);
  if (output != nullptr) *output = out;
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

int launchctl(const std::vector<std::string>& args, std::string* output) {
  std::vector<std::string> argv;
  argv.push_back("/bin/launchctl");
  for (const std::string& a : args) argv.push_back(a);
  return run(argv, output);
}

// Pull "key = value" out of what `launchctl print` produces. Its output is a
// nested, unstable, human-facing dump, so only the two fields that have been
// spelled the same way for a decade are read, and a miss is reported as
// "unknown" rather than guessed at.
bool field(const std::string& text, const char* key, std::string* out) {
  const std::string needle = std::string(key) + " = ";
  const size_t at = text.find(needle);
  if (at == std::string::npos) return false;
  const size_t start = at + needle.size();
  const size_t end = text.find('\n', start);
  *out = text.substr(start, end == std::string::npos ? end : end - start);
  // Trim trailing whitespace and any stray punctuation.
  while (!out->empty() && (out->back() == ' ' || out->back() == '\r' ||
                           out->back() == ';')) {
    out->pop_back();
  }
  return true;
}

bool copy_file(const std::string& from, const std::string& to,
               std::string* err) {
  std::ifstream in(from, std::ios::binary);
  if (!in) {
    if (err) *err = "cannot read " + from;
    return false;
  }
  std::ofstream out(to, std::ios::binary | std::ios::trunc);
  if (!out) {
    if (err) *err = "cannot write " + to;
    return false;
  }
  out << in.rdbuf();
  if (!out) {
    if (err) *err = "short write to " + to;
    return false;
  }
  return true;
}

}  // namespace

const char* agent_label(Agent a) {
  return a == Agent::kBench ? "com.dabbelt.octomancerd"
                            : "com.dabbelt.octomancer-sync";
}

const char* agent_program(Agent a) {
  return a == Agent::kBench ? "octomancerd" : "octomancer-sync";
}

const char* agent_description(Agent a) {
  return a == Agent::kBench ? "the bench listener"
                            : "the camera sync daemon";
}

std::string sibling_program_path(const std::string& program) {
  // An uninstalled tree first. Running `./octomancer pair` out of a build
  // directory and having it drive the installed daemon from three days ago
  // is the kind of confusion that costs an afternoon.
  const std::string dir = executable_dir();
  if (!dir.empty()) {
    const std::string beside = dir + "/" + program;
    if (::access(beside.c_str(), X_OK) == 0) return beside;
  }
  const std::string installed = std::string(OCTO_BINDIR) + "/" + program;
  if (::access(installed.c_str(), X_OK) == 0) return installed;
  return program;
}

bool parse_agent_selection(const std::string& name, std::vector<Agent>* out) {
  out->clear();
  if (name.empty() || name == "all" || name == "both") {
    // The listener first, so that when both are started the sync daemon finds
    // a bench already being watched rather than scanning for one itself.
    out->push_back(Agent::kBench);
    out->push_back(Agent::kSync);
    return true;
  }
  if (name == "bench" || name == "octomancerd" || name == "listener") {
    out->push_back(Agent::kBench);
    return true;
  }
  if (name == "sync" || name == "octomancer-sync" || name == "camera") {
    out->push_back(Agent::kSync);
    return true;
  }
  return false;
}

void set_agent_plist_dir(const std::string& dir) { g_plist_dir = dir; }

std::string launch_agents_dir() {
  return home() + "/Library/LaunchAgents";
}

std::string installed_plist_path(Agent a) {
  return launch_agents_dir() + "/" + agent_label(a) + ".plist";
}

std::string agent_plist_source(Agent a) {
  const std::string leaf = std::string(agent_label(a)) + ".plist";

  std::vector<std::string> dirs;
  if (!g_plist_dir.empty()) dirs.push_back(g_plist_dir);
  if (const char* env = std::getenv("OCTOMANCER_AGENT_DIR")) {
    if (*env) dirs.push_back(env);
  }
#ifdef OCTO_PKGDATADIR
  dirs.push_back(OCTO_PKGDATADIR);
#endif
  const std::string exe = executable_dir();
  if (!exe.empty()) {
    dirs.push_back(exe + "/launchd");              // an uninstalled build tree
    dirs.push_back(dirname_of(exe) + "/share/octomancer");
  }

  for (const std::string& dir : dirs) {
    const std::string candidate = dir + "/" + leaf;
    if (exists(candidate)) return candidate;
  }
  return std::string();
}

AgentState agent_state(Agent a) {
  AgentState state;
  state.installed = exists(installed_plist_path(a));

  std::string out;
  const int rc =
      launchctl({"print", uid_domain() + "/" + agent_label(a)}, &out);
  state.loaded = rc == 0;
  if (!state.loaded) return state;

  std::string value;
  if (field(out, "pid", &value)) {
    state.pid = std::atoi(value.c_str());
    state.running = state.pid > 0;
  }
  if (field(out, "last exit code", &value)) {
    state.last_exit = std::atoi(value.c_str());
  }
  return state;
}

bool agent_install(Agent a, std::string* err) {
  const std::string source = agent_plist_source(a);
  if (source.empty()) {
    if (err) {
      *err = std::string("cannot find ") + agent_label(a) +
             ".plist. Run `make install-agent` from the source tree, or set"
             " OCTOMANCER_AGENT_DIR to where the plists are.";
    }
    return false;
  }

  const std::string dir = launch_agents_dir();
  if (::mkdir(dir.c_str(), 0755) != 0 && errno != EEXIST) {
    if (err) *err = "cannot create " + dir + ": " + strerror(errno);
    return false;
  }

  const std::string dest = installed_plist_path(a);
  if (!copy_file(source, dest, err)) return false;

  // Boot it out first: bootstrapping something already loaded is an error, and
  // replacing the plist under a loaded agent leaves launchd running the old
  // one until the next login.
  launchctl({"bootout", uid_domain() + "/" + agent_label(a)}, nullptr);
  std::string out;
  if (launchctl({"bootstrap", uid_domain(), dest}, &out) != 0) {
    if (err) {
      *err = std::string("launchctl could not load ") + agent_label(a);
      if (!out.empty()) *err += ": " + out;
    }
    return false;
  }
  return true;
}

bool agent_uninstall(Agent a, std::string* err) {
  launchctl({"bootout", uid_domain() + "/" + agent_label(a)}, nullptr);
  const std::string dest = installed_plist_path(a);
  if (exists(dest) && ::unlink(dest.c_str()) != 0) {
    if (err) *err = "cannot remove " + dest + ": " + strerror(errno);
    return false;
  }
  return true;
}

bool agent_start(Agent a, std::string* err) {
  const AgentState state = agent_state(a);
  if (state.loaded) {
    // Loaded but not running is an agent launchd is holding off on -- after a
    // crash loop, or with KeepAlive satisfied. kickstart is how it is told to
    // go now.
    std::string out;
    if (launchctl({"kickstart", uid_domain() + "/" + agent_label(a)}, &out) != 0) {
      if (err) {
        *err = std::string("launchctl could not start ") + agent_label(a);
        if (!out.empty()) *err += ": " + out;
      }
      return false;
    }
    return true;
  }
  if (!state.installed) return agent_install(a, err);

  std::string out;
  if (launchctl({"bootstrap", uid_domain(), installed_plist_path(a)}, &out) != 0) {
    if (err) {
      *err = std::string("launchctl could not load ") + agent_label(a);
      if (!out.empty()) *err += ": " + out;
    }
    return false;
  }
  return true;
}

bool agent_stop(Agent a, std::string* err) {
  std::string out;
  const int rc = launchctl({"bootout", uid_domain() + "/" + agent_label(a)}, &out);
  // Already stopped is not a failure. Asking for something that has already
  // happened should not be an error a script has to special-case.
  if (rc != 0 && !agent_state(a).loaded) return true;
  if (rc != 0) {
    if (err) {
      *err = std::string("launchctl could not stop ") + agent_label(a);
      if (!out.empty()) *err += ": " + out;
    }
    return false;
  }
  return true;
}

bool agent_restart(Agent a, std::string* err) {
  const AgentState state = agent_state(a);
  if (state.loaded) {
    // -k kills the running process and starts it again, which is exactly a
    // restart and does not disturb whether it is loaded.
    std::string out;
    if (launchctl({"kickstart", "-k", uid_domain() + "/" + agent_label(a)},
                  &out) != 0) {
      if (err) {
        *err = std::string("launchctl could not restart ") + agent_label(a);
        if (!out.empty()) *err += ": " + out;
      }
      return false;
    }
    return true;
  }
  return agent_start(a, err);
}

}  // namespace octo
