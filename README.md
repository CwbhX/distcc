# distcc (mac-pool-perf fork)

A distributed C/C++ compiler: distcc spreads the compile jobs of a build
across several machines on a network. Its output is the same as a local
compile's.

This fork adds features for a small pool of Macs on a fast link, such as two
Mac minis joined by a Thunderbolt bridge. It is based on
[distcc/distcc](https://github.com/distcc/distcc) (3.4 plus later upstream
commits). Without the new options it behaves like upstream distcc and talks
to upstream daemons.

- [What's new in this fork](#whats-new-in-this-fork)
- [Results](#results)
- [How the three modes work](#how-the-three-modes-work)
- [Installing](#installing)
- [Using it](#using-it)
- [Configuration reference](#configuration-reference)
- [Troubleshooting](#troubleshooting)
- [Running the tests](#running-the-tests)
- [Further documentation](#further-documentation)

## What's new in this fork

| Feature | What it does |
|---|---|
| **Mirrored-tree mode** (`,mirror`, protocol 4) | A helper that has a copy of the source tree at the same path compiles in that copy. The client sends no source and runs no preprocessor. It accepts the object only after it checks that every file and directory the compile used is identical on both machines. Precompiled headers (PCHs) work remotely. |
| **`distcc --mirror-sync`** | Copies the working tree to the mirror helpers with rsync. Uncommitted edits are included, and no git is involved. Build trees are filtered to sources, headers and PCHs. |
| **Write confinement** | On the helper, each mirrored compile runs in a macOS Seatbelt sandbox. It can write only to its own job directory and has no network access. The daemon refuses to start in mirror mode if confinement doesn't work. |
| **Safe fallback** | If a check fails, the helper refuses, or the compile fails in the mirror, the job is sent the classic way to the same helper. The helper is not marked bad, and the build never uses an object it can't trust. |
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

Install the same build on **every machine**: the client (where you run the
build) and each helper.

### Prerequisites

macOS (Apple silicon), with the Xcode Command Line Tools and Homebrew:

```sh
xcode-select --install
brew install autoconf automake pkgconf popt python-setuptools
```

Debian or Ubuntu (classic and pump mode only; see the note below):

```sh
sudo apt-get install gcc make python3 python3-dev python3-setuptools autoconf pkg-config libpopt-dev
```

> Mirror mode needs a macOS **helper**, because write confinement uses
> Seatbelt. On other systems the daemon refuses `--mirror-root`, and the
> mirror tests are skipped.

### Build and install

```sh
git clone -b mac-pool-perf https://github.com/CwbhX/distcc.git
cd distcc
./autogen.sh
./configure --prefix="$HOME/.local/distcc-mirror" --disable-pump-mode
make LIBS="$(brew --prefix popt)/lib/libpopt.a -liconv"   # links popt statically
make install
```

Notes:

- Linking popt statically means the binaries don't need Homebrew's popt at
  runtime. You can then build once and copy `~/.local/distcc-mirror` to the
  other Mac:

  ```sh
  rsync -a ~/.local/distcc-mirror/ helper:.local/distcc-mirror/
  ```

  On Linux, a plain `make` is enough.
- Leave out `--disable-pump-mode` if you want pump mode.
- A separate prefix lets this build live next to Homebrew's `distcc`
  without replacing it.

The daemon only runs compilers that are in its whitelist directory
(`<prefix>/lib/distcc`). Create the whitelist on each helper:

```sh
mkdir -p ~/.local/distcc-mirror/lib/distcc
for c in cc c++ gcc g++ clang clang++; do
  ln -sf ../../bin/distcc ~/.local/distcc-mirror/lib/distcc/$c
done
```

(`update-distcc-symlinks` does the same for a system-wide install.)

### Making the helper identical (mirror mode)

Mirror mode only accepts a remote object when every file the compile read
is identical on both machines. This includes the toolchain and the
installed headers:

- **Compiler:** the same Command Line Tools or Xcode version on both Macs.
  Check with `clang --version` and `pkgutil --pkg-info=com.apple.pkg.CLTools_Executables`.
  If they differ, every job falls back with "compiler differs" (`MIRR 6`).
- **Homebrew:** the same formulae at the same versions, for every library
  the build includes headers from. `brew install` installs the newest
  version, which may not match the client's. To get the exact versions,
  copy the kegs from the client's Cellar, then `brew link` and `brew pin`
  them on the helper. Upgrade both Macs together.
- **Paths:** each synced tree must be at the same absolute path on both
  machines. If your client's build directory is reached through another
  path, such as a symlink to an external disk, use
  `DISTCC_MIRROR_PATHMAP` (see below).

A mismatch is never a correctness problem: the job falls back to classic
mode. It only costs speed, and the client's log says why.

## Using it

### Classic mode (as in upstream)

```sh
# On each helper
distccd --daemon --allow 192.168.1.0/24 --jobs 12

# On the client
export DISTCC_HOSTS="helper/12 localhost/8"
make -j20 CC="distcc clang" CXX="distcc clang++"
```

### Mirror mode

**1. Start the daemon on the helper** with the roots it may compile in.
Only clients allowed by `--allow` can connect; `--enable-tcp-insecure` is
refused in mirror mode.

```sh
~/.local/distcc-mirror/bin/distccd --daemon \
  --listen 172.31.250.2 --port 3634 --allow 172.31.250.1/32 \
  --jobs 14 \
  --mirror-root "$HOME/Git/MyProject" \
  --log-file ~/Library/Logs/distccd-mirror.log \
  --pid-file ~/.local/distcc-mirror/distccd.pid
```

The daemon starts by checking that it can confine a compiler. If it can't,
it refuses to start. A different port lets it run next to an existing
upstream daemon on 3632.

To keep it running across reboots, use a LaunchAgent
(`~/Library/LaunchAgents/local.distccd-mirror.plist`, then
`launchctl load` it):

```xml
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>Label</key><string>local.distccd-mirror</string>
  <key>ProgramArguments</key>
  <array>
    <string>/Users/YOU/.local/distcc-mirror/bin/distccd</string>
    <string>--daemon</string><string>--no-detach</string>
    <string>--listen</string><string>172.31.250.2</string>
    <string>--port</string><string>3634</string>
    <string>--allow</string><string>172.31.250.1/32</string>
    <string>--jobs</string><string>14</string>
    <string>--mirror-root</string><string>/Users/YOU/Git/MyProject</string>
    <string>--log-file</string><string>/Users/YOU/Library/Logs/distccd-mirror.log</string>
  </array>
  <key>RunAtLoad</key><true/>
  <key>KeepAlive</key><true/>
</dict>
</plist>
```

**2. Configure the client:**

```sh
export PATH="$HOME/.local/distcc-mirror/bin:$PATH"
export DISTCC_HOSTS="172.31.250.2:3634/14,mirror localhost/17"
export DISTCC_MIRROR_ROOTS="$HOME/Git/MyProject"
export DISTCC_MIRROR_EXCLUDE=".git:build-release:tmp"   # not synced, not compared
export DISTCC_MIRROR_SSH=m6                              # ssh destination(s) for the sync
```

**3. Build.** Build the PCHs and generated headers first (the helper
compiles against copies of them), then sync, then run the build:

```sh
ninja -C build <pch targets>
distcc --mirror-sync
ninja -C build -j31
```

For CMake/Ninja trees,
[`contrib/mirror-build.sh`](contrib/mirror-build.sh) does all three:

```sh
contrib/mirror-build.sh build -j31
```

A sync with no changes takes about 3 s. The first copy of a 2 GB tree took
11 s over Thunderbolt.

**With ccache:** set `CCACHE_PREFIX=distcc` (or a wrapper script that runs
distcc). Mirror mode works with ccache's `base_dir`, `depend_mode` and
`-fpch-preprocess`.

**Build directory under another path:** if the build runs in
`/Volumes/SSD/MyProject/build`, but the helper has the tree at
`~/Git/MyProject`, map one to the other:

```sh
export DISTCC_MIRROR_PATHMAP="/Volumes/SSD/MyProject/build=$HOME/Git/MyProject/build"
```

The client checks that both paths are the same directory (same device and
inode). It then sends the logical path. The helper compiles through a clang
VFS overlay, so the `.d` files and PCH paths come out the same as on the
client.

### Choosing slot counts

On the M5 Pro + M6 pair, the best result came from a few more slots than
cores on the helper (14 on 12 cores) and on the client (`localhost/17` on
15 cores). Going past that didn't help. Watch `distccmon-text 1` during a
build to see where jobs are running.

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

## Troubleshooting

- **See what happened to each job:** run with `DISTCC_VERBOSE=1`
  (or set `DISTCC_LOG`). For mirror jobs, the log shows whether the job was
  accepted, or which check failed and why it fell back.
- **Daemon side:** add `--log-level debug` and read the `--log-file`.
- **Every job falls back with a refusal code** (`MIRR`):

  | Code | Meaning | Usual fix |
  |---|---|---|
  | 1 | The cwd or input is not under a `--mirror-root` | Check `--mirror-root` and `DISTCC_MIRROR_ROOTS` |
  | 2 | A file on the check list differs | Run `distcc --mirror-sync` (rebuild PCHs first) |
  | 3 | The argument or compiler is refused | The job uses an unsupported search option (`-iprefix`, `-ivfsoverlay`, ...) |
  | 4 | The cwd or input is missing on the helper | Sync, or fix the path map |
  | 5 | Confinement failed on the helper | Check the daemon log |
  | 6 | The helper's compiler binary differs | Match the CLT/Xcode versions |

- **Some jobs are rejected after compiling:** a header or a search directory
  differs between the machines. Usually that's an installed library (such
  as a Homebrew formula) at another version on the helper. The verbose log
  names the path.
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
