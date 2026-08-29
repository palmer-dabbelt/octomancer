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

## Traps that have already cost hours

**Every GitHub clone fails here, whatever URL you use.** The global git config
rewrites GitHub to SSH:

```
url.ssh://git@github.com.insteadof https://github.com
```

The agent environment has no SSH key, so any tool that clones from GitHub
internally — `west init`, a package manager, a bootstrap script — dies with
`Permission denied (publickey)` and never mentions URLs, so it reads like a
network or auth bug in that tool. Drop the global config for the command:

```
GIT_CONFIG_GLOBAL=/dev/null git clone https://github.com/...
```

The obvious fix — adding the reverse rewrite per command — **does not work**,
and reads as though it should:

```
git -c url."https://github.com/".insteadOf=ssh://git@github.com clone ...   # still fails
```

Both rules are then present, the https→ssh one still fires, and the clone dies
with the same `publickey` error as before. Verified again on 2026-08-29: the
override fails, `GIT_CONFIG_GLOBAL=/dev/null` succeeds.

**Do not install large toolchains without asking first.** A Zephyr SDK, an
embedded cross-compiler, anything measured in gigabytes: ask, even when it is
plainly needed for the task at hand. Write the code that can be written
without it and say what is blocked.

**Never run a bare `./configure` in this checkout.** It is configured with
`--prefix=$HOME/.local`, and a bare re-run silently resets that to
`/usr/local`, so a later `make install` writes to the wrong place. Plain `make`
re-runs `config.status --recheck` and keeps the existing options, so there is
almost never a reason to re-configure at all. `./autogen.sh` is safe on its
own — it only regenerates the build system and does not configure anything,
whatever its closing message suggests. If you genuinely must re-configure, pass
the prefix again:

```
./configure --prefix=$HOME/.local
```

**`make` does not build the tests.** They are `check_PROGRAMS`, so they are
built by `make check`. Running `make && ./tests/test_foo` therefore runs
whatever binary was lying around, which may be from before the change being
tested. This has already produced a confidently wrong conclusion — that a test
had no teeth, when the test was fine and the binary was stale. To run one test
against current sources, use `make check TESTS=tests/test_foo`.

**Do not pipe `make` into `head`.** `head` closes the pipe and `make` dies of
SIGPIPE part-way through, leaving a half-built tree with no error visible. The
next command then fails for reasons that have nothing to do with the code, and
the obvious reading is that the change broke something. Redirect to a file and
grep it instead:

```
make > /tmp/build.log 2>&1; echo "exit=$?"; grep -c warning: /tmp/build.log
```

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

Build: `make && make check`. The tree is already configured; see the trap
above about not re-running `./configure`. From a fresh clone it is
`./autogen.sh && ./configure --prefix=$HOME/.local && make`. No third-party
dependencies. Prose in `doc/` and `README.md` is written to be read by a
person, not generated — match that register.
