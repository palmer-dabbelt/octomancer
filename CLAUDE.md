# Working agreements

## Git

**Never push. Palmer does all the pushing.** This is not a permissions problem
to be worked around — it is how he wants the repository handled. Do not push to
any branch, do not offer to, and do not treat a successful-looking push command
as something to try.

Merging to `main` locally *is* wanted. Commit the work on its branch, then
fast-forward `main` to it. `main` is normally checked out in the top-level
directory while agent work happens in a worktree under `.claude/worktrees/`, so
the merge has to be run against the main checkout:

```
git -C <repo-root> merge --ff-only <branch>
```

Leave `origin` alone afterwards.

## This checkout is for the AI

Nobody is editing these files by hand, so there is nothing to be isolated from.
`.claude/settings.json` sets `worktree.bgIsolation` to `"none"` for that
reason: work directly in the checkout and do not create worktrees.

That setting applies to background sessions when they start. A session already
bound to a worktree stays in it regardless -- resuming returns a session to its
worktree -- so if you find yourself under `.claude/worktrees/`, leave with the
`ExitWorktree` tool rather than assuming the setting is wrong.

## Two traps that have already cost hours

**Every GitHub clone fails here, whatever URL you use.** The global git config
rewrites GitHub to SSH:

```
url.ssh://git@github.com.insteadof https://github.com
```

The agent environment has no SSH key, so any tool that clones from GitHub
internally — `west init`, a package manager, a bootstrap script — dies with
`Permission denied (publickey)` and never mentions URLs, so it reads like a
network or auth bug in that tool. Override per-command when it comes up:

```
git -c url."https://github.com/".insteadOf=ssh://git@github.com clone ...
```

**Do not install large toolchains without asking first.** A Zephyr SDK, an
embedded cross-compiler, anything measured in gigabytes: ask, even when it is
plainly needed for the task at hand. Write the code that can be written
without it and say what is blocked.

## The shape of this codebase

The most important thing to preserve: **the decision-making is kept free of any
radio, on purpose**, so `make check` can exercise it on a machine with no
hardware present. `src/camsync.*` decides whether to touch a clock,
`src/bmd.*` and `src/tentacle.*` are pure byte arithmetic, and `src/hci.*`,
`src/att.*`, `src/crypto.*` and `src/smp.*` are a complete Bluetooth host with
no I/O in them. `src/camera.h` and `src/scanner.h` are where the radio starts.

New logic goes on the testable side of that seam and gets a test. Only glue
should be untestable. When something genuinely cannot be verified without
hardware, say so in the commit and in `doc/` rather than letting it look
checked — `doc/dongle-notes.md` has the format: a table of what is pinned to a
published vector, and a list of what is not.

Build: `./autogen.sh && ./configure && make && make check`. No third-party
dependencies. Prose in `doc/` and `README.md` is written to be read by a
person, not generated — match that register.
