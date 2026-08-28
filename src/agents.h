// Starting and stopping the daemons.
//
// Neither daemon can be asked to start over its own socket, for the obvious
// reason, so this is the one part of octomancer that reaches around the
// sockets and talks to launchd instead. Both the command-line tool and the app
// need it, and doing it twice -- once in C++ and once in Objective-C -- is how
// the two would end up disagreeing about which labels exist.
//
// Everything here shells out to /bin/launchctl. There is a C API for this and
// it has been deprecated twice; the command-line tool is the interface Apple
// actually keeps working.
#ifndef OCTO_AGENTS_H
#define OCTO_AGENTS_H

#include <string>
#include <vector>

namespace octo {

// The two agents, which are two because the one that can write to a camera
// should be stoppable without also stopping the one that only listens.
enum class Agent {
  kBench,  // octomancerd
  kSync,   // octomancer-sync
};

const char* agent_label(Agent a);
const char* agent_program(Agent a);
// What to call it when talking to a person.
const char* agent_description(Agent a);

// Parses "bench", "sync", or "all". Returns false on anything else.
bool parse_agent_selection(const std::string& name, std::vector<Agent>* out);

// Where to find another octomancer program. Beside the running executable
// first, so an uninstalled build tree runs its own binaries rather than
// whatever an earlier `make install` left on the system; then the directory
// this build installs into. Falls back to the bare name and leaves the rest
// to PATH.
//
// This exists because the front door has no radio of its own. `scan` and
// `pair` have to hand the work to the binary that does, and it has to be the
// one from the same build as the tool that asked.
std::string sibling_program_path(const std::string& program);

struct AgentState {
  bool installed = false;  // a plist in ~/Library/LaunchAgents
  bool loaded = false;     // launchd knows about it
  bool running = false;    // ...and it has a process right now
  int pid = 0;
  int last_exit = 0;
};

AgentState agent_state(Agent a);

// Where the shipped plists are looked for, in order: whatever was handed to
// set_agent_plist_dir(), then $OCTOMANCER_AGENT_DIR, then the installed data
// directory, then a `launchd` directory beside the running executable so that
// an uninstalled build tree works too.
//
// The app calls the setter with its own Resources directory: the plists are
// copied into the bundle at build time from the same templates `make
// install-agent` uses, so there is one place their contents are decided.
void set_agent_plist_dir(const std::string& dir);
std::string agent_plist_source(Agent a);
std::string launch_agents_dir();
std::string installed_plist_path(Agent a);

// Copy the plist into place and load it, so it comes back at every login.
bool agent_install(Agent a, std::string* err);
// Unload it and take the plist away again.
bool agent_uninstall(Agent a, std::string* err);

// Load an already-installed agent, or report why it could not be.
bool agent_start(Agent a, std::string* err);
bool agent_stop(Agent a, std::string* err);
// Stop and start in one step, keeping it installed either way.
bool agent_restart(Agent a, std::string* err);

}  // namespace octo

#endif  // OCTO_AGENTS_H
