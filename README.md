# distcc (mac-pool-perf fork)

A distributed C/C++ compiler: distcc spreads the compile jobs of a build
across several machines on a network. Its output is the same as a local
compile's.

This fork adds features for a small pool of Macs on a fast link, such as two
Mac minis joined by a Thunderbolt bridge. It is based on
[distcc/distcc](https://github.com/distcc/distcc) (3.4 plus later upstream
commits). Without the new options it behaves like upstream distcc and talks
to upstream daemons.

- [Quick start](#quick-start)
- [What's new in this fork](#whats-new-in-this-fork)
- [Results](#results)
- [How the three modes work](#how-the-three-modes-work)
- [Installing](#installing)
- [Setting up a project](#setting-up-a-project)
- [Building](#building)
- [distcc-mirror commands](#distcc-mirror-commands)
- [Doing it by hand](#doing-it-by-hand)
- [Configuration reference](#configuration-reference)
- [Troubleshooting](#troubleshooting)
- [Running the tests](#running-the-tests)
- [Further documentation](#further-documentation)

## Quick start

Two Macs: the **client** runs your builds, and the **helper** (here the ssh
host `m6`) lends its cores. On the client:

```sh
git clone -b mac-pool-perf https://github.com/CwbhX/distcc.git && cd distcc
brew install autoconf automake pkgconf popt
contrib/distcc-mirror install --helper m6        # build; install here and on m6
export PATH="$HOME/.local/distcc-mirror/bin:$PATH"

cd ~/src/MyProject                               # a CMake + Ninja project
distcc-mirror init --helper m6 --build build     # writes ~/.config/distcc-mirror/myproject.conf
distcc-mirror helper install                     # starts the daemon on m6 (LaunchAgent)
distcc-mirror doctor                             # checks that both Macs match
distcc-mirror build                              # PCHs, sync to m6, ninja
```

Every step works out the details itself (addresses, slot counts, ccache,
build directories behind symlinks), and `doctor` tells you what to fix.

## What's new in this fork

| Feature | What it does |
|---|---|
| **Mirrored-tree mode** (`,mirror`, protocol 4) | A helper that has a copy of the source tree at the same path compiles in that copy. The client sends no source and runs no preprocessor. It accepts the object only after it checks that every file and directory the compile used is identical on both machines. Precompiled headers (PCHs) work remotely. |
| **`distcc-mirror`** | One command for the whole setup: install on both Macs, write a project config, run the helper daemon as a LaunchAgent, check that the machines match, sync, build, and test-compile against local results. |
| **`distcc --mirror-sync`** | Copies the working tree to the mirror helpers with rsync. Uncommitted edits are included, and no git is involved. Build trees are filtered to sources, headers and PCHs. |
| **Write confinement** | On the helper, each mirrored compile runs in a macOS Seatbelt sandbox. It can write only to its own job directory and has no network access. The daemon refuses to start in mirror mode if confinement doesn't work. |
| **Safe fallback** | If a check fails, the helper refuses, or the compile fails in the mirror, the job is sent the classic way to the same helper. The helper is not marked bad, and the build never uses an object it can't trust. |
| **CMake PCHs in classic mode** | CMake's `-Xclang -include-pch` flags are no longer sent with preprocessed source, where they made every remote compile fail and back the helper off. No wrapper script is needed. |
| **Faster slot scheduling** | When every slot is busy, a job now waits in the kernel on a busy slot instead of sleeping for a second and polling. A freed slot is used right away. |
| **Separate preprocessor locks** | Local preprocessor slots (`--localslots_cpp`) have their own locks, so a job holding a remote slot isn't stuck waiting behind local compiles. |
| **PCH generation stays local** | Jobs that build a PCH (`-x c++-header`, `-emit-pch`, ...) run on the client automatically. |
| **Optional pump mode** | `./configure --disable-pump-mode` now really skips the Python include server, so you can build without setuptools. |

## Results

Full rebuild of a 3481-TU C++ project (CMake + Ninja + ccache with a cold
cache). The host is an M5 Pro Mac mini (15 cores) and the helper an M6 Mac
mini (12 cores), joined by a Thunderbolt bridge.

| Configuration | Time | vs local |
|---|---|---|
| Local only, 15 jobs | 472 s | 1.00x |
| Upstream distcc 3.4, tuned (`--localslots_cpp=40`, `DISTCC_PAUSE_TIME_MSEC=20`) | 320 s | 1.48x |
| This fork, classic mode, M6/14 + localhost/17 | 290 s | 1.63x |
| **This fork, mirror mode, M6/14 + localhost/17** | **241 s** | **1.96x** |

In mirror mode the pool runs at about the two machines' combined capacity:
a mirrored job costs the host almost nothing. All objects were
byte-identical to a local build, except one file that embeds the build time
(that file was compiled locally). The full numbers and method are in
[doc/mirrored-tree-design.md](doc/mirrored-tree-design.md#results-2026-10-07).

## How the three modes work

| Mode | Host spec | Where the preprocessor runs | What the helper needs |
|---|---|---|---|
| Classic | `host/N` | Client | Just the compiler |
| Pump | `host/N,cpp,lzo` | Helper (the client sends the headers) | Same system headers |
| **Mirror** | `host:port/N,mirror` | Helper, in its own copy of the tree | The same compiler, the tree at the same path, and identical installed headers (CLT/Xcode, Homebrew) |

Classic mode has the fewest requirements. Mirror mode is the fastest when
both machines can be kept identical, which is easy for two Macs on one desk.
A job that can't be mirrored (its cwd is outside the synced roots, it uses
an unsupported option, or a file differs) falls back to classic mode.

## Installing

### Prerequisites

On every Mac (Apple silicon): the Xcode Command Line Tools, and on the
client Homebrew with the build tools:

```sh
xcode-select --install
brew install autoconf automake pkgconf popt     # client only
```

The helper needs no Homebrew for distcc itself (popt is linked in
statically), only for the libraries your project includes.

Mirror mode needs a macOS **helper**, because write confinement uses
Seatbelt. On Linux, classic and pump mode build as usual (`sudo apt-get
install gcc make python3 python3-dev python3-setuptools autoconf pkg-config
libpopt-dev`, then the manual steps below).

### Install

From the source tree, on the client:

```sh
contrib/distcc-mirror install --helper m6
```

This runs `autogen.sh` and `configure` if needed, builds with popt linked
statically, and installs into `~/.local/distcc-mirror`. That includes
`distcc`, `distccd`, the `distcc-mirror` tool and the compiler whitelist
the daemon requires. It then copies the install to each `--helper` over
ssh. Run it again after pulling changes; once a project is set up, it
copies to that project's helpers and restarts their daemons. Add
`~/.local/distcc-mirror/bin` to your `PATH`.

Options: `--prefix DIR`, `--pump` (also build pump mode),
`--no-build` (only copy what is built), `--local-only`.

<details>
<summary>Building and installing by hand</summary>

```sh
./autogen.sh
./configure --prefix="$HOME/.local/distcc-mirror" --disable-pump-mode
make LIBS="$(brew --prefix popt)/lib/libpopt.a -liconv"   # popt linked statically
make install
mkdir -p ~/.local/distcc-mirror/lib/distcc                # compiler whitelist
for c in cc c++ gcc g++ clang clang++; do
  ln -sf ../../bin/distcc ~/.local/distcc-mirror/lib/distcc/$c
done
rsync -a ~/.local/distcc-mirror/ m6:.local/distcc-mirror/  # same build on the helper
```

On Linux a plain `make` is enough. Leave out `--disable-pump-mode` to get
pump mode.

</details>

## Setting up a project

### 1. Write the config

From inside the project:

```sh
distcc-mirror init --helper m6 --build build
```

This writes `~/.config/distcc-mirror/<project>.conf`. Nothing is written
into the project itself. `init` works out:

- the root (the git work tree);
- the helper's address, from your ssh config;
- slot counts: cores + 2 on each machine;
- this Mac's address as the helper sees it;
- other build directories to leave out of the sync;
- whether the build uses ccache. If it does, `CCACHE_PREFIX` is set to
  distcc. If the build has no compiler launcher at all, `init` prints the
  `cmake` line that adds one.

Useful options:

| Option | Meaning |
|---|---|
| `--helper SSH[=ADDR][/SLOTS]` | A helper (repeatable). For example, `m6=172.31.250.2/14`. |
| `--extra DIRS` | More trees to sync whole, such as dependencies you built locally. |
| `--exclude NAMES` | More top-level names not to sync, such as `output:tmp`. |
| `--local-jobs N`, `--port N` | Slots on this Mac; the daemon port (default 3634). |
| `--set KEY=VALUE` | Any other variable to export to the build. |

The config is plain `KEY=VALUE`, so edit it freely. With several projects,
the commands pick the config whose root contains the current directory, or
take `-c NAME`. Here is an example:

```sh
ROOTS=~/Git/MyProject
BUILD_DIR=~/Git/MyProject/build-dev     # may be a symlink to another disk
EXCLUDE=.git:build-release
EXTRA=~/deps/wxwidgets-dest
HELPERS="m6=172.31.250.2/14"
LOCAL_JOBS=17
PORT=3634
CLIENT_ADDR=172.31.250.1
PREFIX=~/.local/distcc-mirror
CCACHE_PREFIX=~/.local/distcc-mirror/bin/distcc
```

### 2. Start the helper daemon

```sh
distcc-mirror helper install
```

This installs a LaunchAgent on each helper, so the daemon survives
reboots. It is configured with `--allow` for this Mac only, the project
roots as `--mirror-root`s, and its own log file. If several project
configs name the same helper, one daemon serves all their roots. A daemon
you started by hand on the same port is replaced. Manage it with
`distcc-mirror helper status|log|restart|stop|start|uninstall`.

### 3. Check that both Macs match

```sh
distcc-mirror doctor
```

`doctor` checks this Mac and each helper, and prints the fix for every
problem it finds:

- the same distcc build and compiler (Command Line Tools) on both;
- the same SDK and architecture;
- the roots present on the helper;
- the daemon running and reachable;
- ccache wired to distcc;
- Homebrew formulae and `/opt/homebrew/include` the same on both.

Mirror mode only accepts a remote object when every file the compile read
is identical on both machines, so these matter. A mismatch is never a
correctness problem, though: the jobs fall back and only speed suffers.

**Homebrew:** `brew install` on the helper installs the newest versions,
which may not match yours. Use this instead:

```sh
distcc-mirror brew-parity           # what differs
distcc-mirror brew-parity --apply   # copy the missing kegs at your versions, link and pin them
```

`--apply --different` also replaces kegs that are at other versions on the
helper. Afterwards, upgrade both Macs together.

**Compiler:** install the same Command Line Tools (or Xcode) version on
both. Otherwise every job falls back with "compiler differs" (`MIRR 6`).

## Building

```sh
distcc-mirror build                 # PCHs, sync, ninja -j<all slots>
distcc-mirror build -- my_target    # arguments after -- go to ninja
distcc-mirror test -n 10            # compile 10 random files on the helper and compare with local compiles
```

`build` first builds the precompiled headers locally, because the helper
compiles against copies of them. Then it syncs the tree (about 3 s when
little changed, 11 s for a first copy of 2 GB over Thunderbolt), and runs
ninja with as many jobs as all the slots together. It accepts `--no-sync`,
`--no-pch` and `-C DIR`. For Make projects it runs `make` instead (there is
no PCH step).

For other build commands, use the same environment:

```sh
distcc-mirror sync && distcc-mirror run -- make -j31
eval "$(distcc-mirror env)"         # or put it in your shell
distcc-mirror shell                 # a subshell with it
```

`test` is the quickest way to see whether mirror mode works for a project.
For each file it shows whether it was mirrored, rejected or refused, and
why. It also shows whether the object is byte-identical to a local
compile. Nothing is written into the project.

## distcc-mirror commands

| Command | What it does |
|---|---|
| `install [--helper SSH]` | Build distcc from this source tree and install it here and on the helpers |
| `init --helper SSH [--build DIR]` | Write a config for the project in the current directory |
| `helper install\|status\|log\|restart\|stop\|start\|uninstall` | Manage the daemon on the helpers |
| `doctor` | Check this Mac and the helpers, with fixes |
| `brew-parity [--apply]` | Compare Homebrew with the helpers; copy missing kegs |
| `sync` | Copy the tree to the helpers (`distcc --mirror-sync`) |
| `build [-- NINJA ARGS]` | PCHs, sync, ninja |
| `test [-n N] [--match TEXT]` | Compile a few files on the helper; compare with local compiles |
| `show` | Print the config and the environment it gives |
| `env`, `run CMD`, `shell` | Use that environment for anything else |

`distcc-mirror COMMAND --help` lists every option.

## Doing it by hand

`distcc-mirror` only writes a LaunchAgent and runs the commands below.
Here they are, if you want to see or script them yourself.

<details>
<summary>Helper daemon, client environment, sync and build</summary>

On the helper (`--enable-tcp-insecure` is refused in mirror mode; a
different port lets it run next to an upstream daemon on 3632):

```sh
~/.local/distcc-mirror/bin/distccd --daemon \
  --listen 172.31.250.2 --port 3634 --allow 172.31.250.1/32 \
  --jobs 14 --mirror-root "$HOME/Git/MyProject" \
  --log-file ~/Library/Logs/distccd-mirror.log
```

The daemon first checks that it can confine a compiler, and refuses to
start if it can't. Add `--no-detach` when running it under launchd.

On the client:

```sh
export PATH="$HOME/.local/distcc-mirror/bin:$PATH"
export DISTCC_HOSTS="172.31.250.2:3634/14,mirror localhost/17"
export DISTCC_MIRROR_ROOTS="$HOME/Git/MyProject"
export DISTCC_MIRROR_EXCLUDE=".git:build-release"
export DISTCC_MIRROR_PATHMAP="/Volumes/SSD/MyProject/build=$HOME/Git/MyProject/build"
export DISTCC_MIRROR_SSH=m6
export CCACHE_PREFIX=distcc          # if the build uses ccache

ninja -C build <pch targets>         # the helper compiles against copies of them
distcc --mirror-sync
ninja -C build -j31
```

[`contrib/mirror-build.sh`](contrib/mirror-build.sh) does the last three
steps for a CMake/Ninja tree.

`DISTCC_MIRROR_PATHMAP` marks the build directory as a build tree, so only
its sources, headers and PCHs are synced and compared. It also maps the
build directory when it physically lives elsewhere, such as through a
symlink to another disk. The client checks that both paths are the same
directory (same device and inode), then sends the logical path. The helper
compiles through a clang VFS overlay, so the `.d` files and PCH paths come
out the same as on the client.

</details>

### Classic mode (as in upstream)

```sh
distccd --daemon --allow 192.168.1.0/24 --jobs 12        # on each helper
export DISTCC_HOSTS="helper/12 localhost/8"              # on the client
make -j20 CC="distcc clang" CXX="distcc clang++"
```

### Choosing slot counts

On the M5 Pro + M6 pair, the best result came from a few more slots than
cores on the helper (14 on 12 cores) and on the client (`localhost/17` on
15 cores). Going past that didn't help. `init` picks cores + 2. Watch
`distccmon-text 1` during a build to see where jobs are running.

## Configuration reference

### Host spec options

| Option | Meaning |
|---|---|
| `,mirror` | Use mirrored-tree mode (protocol 4) with this host. It needs this fork's daemon: an upstream daemon rejects the connection. It can't be combined with `,cpp`. `,lzo` still applies to jobs that fall back. |

### Client environment

| Variable | Meaning |
|---|---|
| `DISTCC_MIRROR_ROOTS` | Colon-separated synced source roots. Jobs whose cwd is outside them use classic mode. This is also what `--mirror-sync` copies. |
| `DISTCC_MIRROR_EXCLUDE` | Globs left out at the top of each root, both by the sync and by the directory comparison. |
| `DISTCC_MIRROR_PATHMAP` | `physical=logical` prefix pairs, colon-separated. The logical sides are treated as build trees: only sources, headers and PCHs there are synced and compared. |
| `DISTCC_MIRROR_EXTRA` | More trees to sync whole, such as locally built dependency prefixes. |
| `DISTCC_MIRROR_INSTALLED` | Installed trees, compared with a more tolerant rule for unrelated entries. Default `/Library/Developer/CommandLineTools:/Applications/Xcode.app:/opt/homebrew`. |
| `DISTCC_MIRROR_SSH` | Sync destinations. The default is every `,mirror` host's address. |
| `DISTCC_MIRROR_RSH` | ssh command for rsync (e.g. `ssh -p 2222`). |
| `DISTCC_PAUSE_TIME_MSEC` | How long a job waits on one busy slot before it rescans the others (default 100 ms). It no longer needs tuning. |

### Daemon options

| Option | Meaning |
|---|---|
| `--mirror-root DIR` | Allow mirrored compiles whose cwd and input resolve under `DIR`. It can be repeated. Without it, mirror requests are refused and the jobs fall back. |
| `--mirror-installed LIST` | Installed trees to use when a client sends no list. |

### Commands

| Command | Meaning |
|---|---|
| `distcc --mirror-sync [HOST...]` | Copy the roots, build trees and extra trees to the given hosts, or to `DISTCC_MIRROR_SSH`. Files deleted locally are deleted on the helper too. |
| `distcc-mirror ...` | The setup tool; see [distcc-mirror commands](#distcc-mirror-commands). |

## Troubleshooting

- **Start with** `distcc-mirror doctor`, then `distcc-mirror test -n 10`.
  `test` shows, per file, whether it was mirrored and why not.
- **See what happened to each job in a build:** run with `DISTCC_VERBOSE=1`
  (or set `DISTCC_LOG`). For mirror jobs, the log shows whether the job was
  accepted, or which check failed and why it fell back.
- **Helper daemon:** `distcc-mirror helper status` and
  `distcc-mirror helper log`.
- **Daemon side:** add `--log-level debug` and read the `--log-file`.
- **Every job falls back with a refusal code** (`MIRR`):

  | Code | Meaning | Usual fix |
  |---|---|---|
  | 1 | The cwd or input is not under a `--mirror-root` | Check `--mirror-root` and `DISTCC_MIRROR_ROOTS` |
  | 2 | A file on the check list differs | `distcc-mirror sync` (rebuild PCHs first; `distcc-mirror build` does both) |
  | 3 | The argument or compiler is refused | The job uses an unsupported search option (`-iprefix`, `-ivfsoverlay`, ...) |
  | 4 | The cwd or input is missing on the helper | Sync, or fix the path map |
  | 5 | Confinement failed on the helper | Check the daemon log |
  | 6 | The helper's compiler binary differs | Match the CLT/Xcode versions |

- **Some jobs are rejected after compiling:** a header or a search directory
  differs between the machines. Usually that's an installed library (such
  as a Homebrew formula) at another version on the helper. The verbose log
  and `distcc-mirror test` name the path; `distcc-mirror brew-parity` fixes
  Homebrew differences.
- **Caches:** `$DISTCC_DIR/mirror-digests` (file digests) and
  `$DISTCC_DIR/mirror-searchpath` (the compiler's search paths) can be
  deleted at any time.

## Running the tests

```sh
make check                                    # the whole suite
make single-test TESTNAME=Mirror_Case         # one case (class names are in test/testdistcc.py)
```

The suite runs a daemon on localhost. It covers mirror mode with about 30
cases: stale PCHs and headers, same-second edits, shadowing headers
(including nested spellings, `CPATH` and duplicate search dirs), path
mapping, compiler mismatch, confinement and the fallbacks. The cases that
compile in the mirror need write confinement, so they run on macOS and are
skipped elsewhere.

## Further documentation

- [doc/mirrored-tree-design.md](doc/mirrored-tree-design.md): design,
  the staleness rules, results and the M6 setup.
- [doc/protocol-4.txt](doc/protocol-4.txt): the wire protocol for mirror
  mode.
- [doc/perf-findings-two-mac-pool.md](doc/perf-findings-two-mac-pool.md):
  why upstream distcc left the helper idle, and the scheduling fixes.
- [INSTALL](INSTALL), [README.pump](README.pump), `man distcc`,
  `man distccd`: upstream documentation for classic and pump mode.

## About distcc

distcc was written by Martin Pool. Pump mode was added by Fergus Henderson,
Nils Klarlund, Manos Renieris and Craig Silverstein (Google). distcc is a
front-end to gcc, clang or another compiler of your choice, and every
compiler option works as normal. It's designed for parallel builds
(`make -j`, Ninja): sending a job across the network costs the client few
cycles. It has been used with hundreds of helpers and dozens of
simultaneous compiles.

- Upstream: <https://github.com/distcc/distcc>, <https://distcc.github.io/>
- This fork: <https://github.com/CwbhX/distcc> (branch `mac-pool-perf`)

## Licence

GNU General Public Licence v2 or later. See [COPYING](COPYING).
