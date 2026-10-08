# Design: mirrored-tree mode

Status: implemented on the `mac-pool-perf` branch, 2026-10-07 (steps 1-3 of
the plan below, plus the fixes from a design audit); results are in
"Results". The wire format is in `doc/protocol-4.txt`. It follows on from
`doc/perf-findings-two-mac-pool.md` (same two Macs, same Ohmly workload).

## The idea

Today every remote job runs cpp on the host, ships a multi-MB `.ii`, and the
helper compiles the full header text, because a `.ii` cannot use the host's
precompiled header (PCH). In **mirrored-tree mode** the helper has the source
tree, the build tree, the PCH files and the external headers at the same
absolute paths as the host. The client sends only the command and the working
directory. The daemon runs the compiler on the source in that directory, so it
preprocesses locally and loads the PCH. Results come back as today, with a
description of every file and directory the compile used, which the client
checks against its own before it accepts the object.

What it removes per remote job: host cpp time and the local cpp lock, the
helper slot sitting idle while the host preprocesses, the `.ii` transfer, and
the missing-PCH penalty on the helper. What it adds: a sync step
(`distcc --mirror-sync`), and a way to prove the mirror was current.

## Part A: what the PCH is worth on Ohmly (measured on the host)

Host: Mac mini M5 Pro, `/usr/bin/clang++` = Apple clang 21.0.0
(clang-2100.3.33.1), target arm64-apple-darwin27.0.0. Each cell is the minimum
of 3 runs, compiler invoked directly (no ccache, no distcc), outputs written to
a scratch dir. Load average was 1.5–3 on 15 cores during the run. Commands are
`-O3 -DNDEBUG -std=c++20` from `build-dev/compile_commands.json`. The PCH was
current: variant 1 compiled with `-Winvalid-pch` and no PCH error.

| TU (PCH used) | 1: as recorded (PCH) | 2: cpp `-E` | 2: compile `.ii` | 2: total | 3: no PCH | `.ii` | `.o` |
|---|---|---|---|---|---|---|---|
| `pcbnew/pcb_edit_frame.cpp` (pcbnew_kiface) | 5.06 s | 0.43 s | 5.63 s | 6.06 s | 6.07 s | 22.0 MB | 751 KB |
| `eeschema/sch_symbol.cpp` (eeschema_kiface) | 2.73 s | 0.45 s | 3.66 s | 4.10 s | 4.03 s | 17.1 MB | 478 KB |
| `common/eda_draw_frame.cpp` (common) | 2.11 s | 0.28 s | 2.52 s | 2.80 s | 2.58 s | 11.1 MB | 407 KB |

Implied saving per remote job if the helper could use the PCH (variant 2
total minus variant 1): 1.00 s, 1.38 s and 0.69 s of CPU, i.e. 17%, 34% and
25% of today's per-job cost. Of that, the helper's part (compile `.ii` minus
compile with PCH) is 0.57 s, 0.93 s and 0.41 s (10%, 25%, 16%), and the host's
part is the cpp run (0.28–0.45 s).

Other observations:

- Variant 3 (no PCH, one step) costs about the same as variant 2 (cpp +
  compile `.ii`). Splitting cpp from compile costs nothing extra; what is lost
  today is only the PCH.
- At `-O3` the PCH is worth less than one might expect (0.5–1.3 s of a
  2–6 s compile), because optimisation and codegen dominate.
- A fourth variant, the `dev-tools/distcc-clang.sh` spelling
  (`-Xpreprocessor -include-pch ...`) compiled straight from source in the
  build dir, which is what a mirrored helper would run, took 5.35 / 2.78 /
  1.77 s in a separate 3-run set where variant 1 took 5.07 / 2.78 / 1.79 s. Its
  objects have the PCH variant's size (750,672 B; the non-PCH variants give 750,648 B), so
  the wrapper's spelling does load the PCH when compiling from source. Run to
  run noise between sets was about ±15%.
- A `-MD` dependency file for these TUs lists 1660–3316 files. **The `.pch`
  itself is not listed**, in either spelling. Union over the three TUs:
  1129 SDK headers, 31 CLT clang builtins, 820 Homebrew (boost, glm, abseil,
  protobuf), 448 kicad-mac-builder (wx), 729 Ohmly source, 21 generated in
  `build-dev`.
- PCH use across the build: 2023 of 3490 entries in `compile_commands.json`
  (58%) use `-include-pch`; excluding `qa/` (782 TUs, none use a PCH) it is
  2023 of 2708 (75%). Nine further entries generate PCHs (`-emit-pch`).
- A second repeat run was discarded: other builds started on the host (load
  21) and inflated times by up to 2x.

Not measured: anything on the helper. The M6 has no Ohmly tree, and this investigation
did not create files there. All gains below assume the helper's
PCH/non-PCH ratio matches the host's.

## Facts about the environment that shape the design

- **Compilers must match.** Clang refuses a PCH written by a different
  compiler build. On 2026-10-06 the host had clang-2100.3.33.1 and the M6
  clang-2100.3.34.2, and the M6 failed with `error: PCH file '...' built
  from a different branch ((clang-2100.3.33.1)) than the compiler
  ((clang-2100.3.34.2))`. On 2026-10-07 the host got Command Line Tools for
  Xcode 27.0 (clang-2100.3.34.2; both Macs on macOS 27.0.1, 26A434). The
  host's older CLT had no package receipt, so Software Update offered the
  new one only after creating
  `/tmp/.com.apple.dt.CommandLineTools.installondemand.in-progress`. Since
  then a PCH built on the host and copied with `rsync -a` loads on the M6,
  and both Macs produce byte-identical `.o` and `.d` from it.
- **Physical vs logical cwd.** `~/Git/Ohmly` is a real directory, but
  `~/Git/Ohmly/build-dev` is a symlink to
  `/Volumes/ExternalSSD/Developer/Ohmly/build-dev`. `dcc_x_cwd`
  (`src/clirpc.c:95`) sends `getcwd()`, which is the physical path, and the
  M6 has no external drive. That path does not need to exist there. The
  argv distcc receives for an Ohmly compile (captured after ccache 4.13.6
  with `base_dir=/Users/clementhathaway/Git`, with `$PWD` both logical and
  physical) names every input by an absolute logical path: the source file,
  every `-I`, the PCH and its header, generated headers under
  `~/Git/Ohmly/build-dev`. The only relative paths are the outputs (`-o`,
  `-MF`, `-MT`), which the daemon replaces with temp files anyway. ccache
  rewrote nothing to a relative path, because the physical cwd is outside
  `base_dir`. So the client sends the cwd under its logical name (see
  `DISTCC_MIRROR_PATHMAP` below), and on the M6 `~/Git/Ohmly/build-dev` is a
  plain directory.
- **External headers.** ninja's deps log for `build-dev` lists 6984 distinct
  external headers: 2783 from `MacOSX26.5.sdk`, 34 clang builtins, 3596 from
  17 Homebrew formulae (abseil 20260107.1, boost 1.90.0_1, cairo 1.18.4,
  fontconfig, freetype, glm, gmp, harfbuzz 14.5.0, libgit2 1.9.6,
  libqalculate 5.12.0, mpfr, nng 1.12.0, opencascade 7.9.3, pixman,
  protobuf 35.1, unixodbc, zstd), and 571 from
  `~/Github/kicad-mac-builder/build/{wxwidgets,python,ngspice}-dest`.
  Commands also name `-I/opt/homebrew/Cellar/opencascade/7.9.3/include`
  directly. Current Homebrew would install newer versions of nine of these
  formulae, so on 2026-10-06 the M6 got the host's exact kegs instead
  (copied with `rsync -a`, then `brew link` and `brew pin`), plus a copy of
  the kicad-mac-builder dest dirs; it already had `MacOSX26.5.sdk`. All 6984
  headers then had identical content (MD5) on both Macs.
- **Installed trees do not share mtimes.** Even with identical content, 34
  clang builtin headers differ in mtime (different CLT builds), and 12 paths
  in the `.d` are symlinks (`/opt/homebrew/include/zstd.h`, SDK
  `pthread.h`) whose own mtimes differ because `brew link` or the installer
  created them at a different time; their targets match. Before the kegs
  were copied, all 3401 common `MacOSX27.0.sdk` headers differed in mtime
  and 5 differed in content although both Macs report the same SDK
  version. After the CLT update on 2026-10-07 all 6984 match in size, mtime
  (symlinks followed) and content. Installed trees are still checked by
  digest, since the next update on either Mac can break that again.
- **ccache runs in depend mode** (`depend_mode = true`): it never runs cpp
  itself and builds its manifest from the `.d` file the compile writes. A
  mirrored job must therefore return the real `.d` to the client's `-MF`
  path. It also means a wrong object compiled from stale helper headers
  would be cached under the key of the host's current headers. The
  staleness check below exists mainly to prevent that.
- Ninja passes `-MD -MT $out -MF $DEP_FILE` (`build-dev/CMakeFiles/rules.ninja`).

## Host-list syntax and configuration

`172.31.250.2:3634/12,mirror`

- `,mirror` sets `host->cpp_where = DCC_CPP_MIRROR` and `protover =
  DCC_VER_4` (`src/hosts.c`). `,mirror` with `,cpp` is a hostspec error.
  `,lzo` may be combined: mirrored jobs are never compressed (the payload is
  one `.o`), but a job that falls back to the classic path uses it.
- **No local cpp lock.** A mirrored job never calls `dcc_lock_local_cpp`; the
  client only `stat`s files. This sidesteps findings 1 and 3 of the perf doc
  for mirrored jobs; `localslots_cpp` only matters for fallbacks and
  non-mirror hosts.

Client environment (all optional except where noted):

| Variable | Meaning |
|---|---|
| `DISTCC_MIRROR_PATHMAP` | `physical=logical` prefix pairs, colon-separated. The physical cwd is sent under its logical name, after checking that both are the same directory (device and inode); otherwise the job takes the classic path. `$PWD` is not used: it is wrong whenever ninja runs with `-C`. The logical sides are also the build trees. |
| `DISTCC_MIRROR_ROOTS` | Synced source roots. Jobs whose (mapped) cwd is outside them take the classic path. Also what `--mirror-sync` copies. |
| `DISTCC_MIRROR_EXCLUDE` | Globs left out at the top of each root (e.g. `.git:build-release:output:tmp`), both by the sync and by directory identity. |
| `DISTCC_MIRROR_EXTRA` | More trees to sync whole (Ohmly: the kicad-mac-builder dest dirs). |
| `DISTCC_MIRROR_INSTALLED` | Installed-tree prefixes; default `/Library/Developer/CommandLineTools:/Applications/Xcode.app:/opt/homebrew`. Sent to the daemon, which uses the same list. |
| `DISTCC_MIRROR_SSH`, `DISTCC_MIRROR_RSH` | Sync destinations (else every `,mirror` host's address) and the ssh command for rsync. |

Daemon options: `--mirror-root DIR` (repeatable; the cwd and the input must
resolve under one) and `--mirror-installed LIST` (default when a client
sends no list).

## Protocol: a new version 4

Protocol 3 cannot be reused with zero files. Its server side
(`make_temp_dir_and_chdir_for_cpp`, `tweak_arguments_for_server`,
`src/serve.c`) roots every absolute `-I` and the input under a fresh temp
dir and chdirs into `<tmp><cwd>`. Mirror mode needs the opposite: the real
cwd, no rewriting. Version 3 also implies LZO and pump clients depend on its
semantics. Old daemons reject `DIST 4` (`dcc_r_request_header`,
`src/srvrpc.c`), so `,mirror` against an old daemon fails the connection and
the client backs off that host; document that `,mirror` needs a new daemon.

The request carries the cwd, argv, the check list, the compiler identity
(`CVER`), the environment that changes what the compiler reads (`ENVS`)
and the rules for directory identity (`RULE`). The response starts with
`MIRR` (0, or a refusal code followed by a reason, `MIRM`), then, as in
protocol 2, `STAT`, `SERR`, `SOUT`, `DOTO`, and finally `DOTD` (the `.d`)
and `DSTA` (the identity of every file and directory the compile used).
`doc/protocol-4.txt` has every token.

The check list is never empty: the input, and every PCH the compile can
load, because the `.d` does not list it. A client that cannot classify a
PCH- or module-related option in argv does not use mirror mode for that job.

The client writes the `.o` and `.d` next to their destinations and renames
them into place only after the check passes, and holds the compiler's
stdout back until then.

## Daemon side

`dcc_run_job` (`src/serve.c`) hands protocol 4 to `dcc_mirror_serve`
(`src/mirror_serve.c`):

1. Read the whole request before answering anything, so that an early
   refusal never leaves unread bytes behind (closing then would reset the
   connection and the client would back off a healthy host).
2. Refuse (`MIRR 1`) unless the daemon has a `--mirror-root` and the cwd
   and the input resolve (`realpath`) under one; `MIRR 4` if either is
   missing.
3. Argument policy (`MIRR 3`): the classic checks (compiler whitelist or
   `DISTCC_CMDLIST`, `-fplugin=`, `-specs=`), plus refused output and
   module options (`-save-temps*`, `-ftime-trace*`, `-gsplit-dwarf`,
   `-serialize-diagnostics`, `-fmodules*`, `-fcrash-diagnostics*`,
   `-emit-pch`, `-x *-header`, `-MJ`, `-fprofile*`, ...). `-Xclang` and
   `-Xpreprocessor` operands are allowlisted (`-include-pch`, `-include`,
   `-imacros`), which also blocks `-Xclang -load`.
4. Apply the client's environment list, then compare the compiler binary's
   SHA-256 with the client's (`MIRR 6`).
5. Pre-check (`MIRR 2`): synced check-list files must have the client's
   size and mtime. This catches the common stale cases (source edited since
   the sync, PCH rebuilt and not synced) before any compile time is spent.
6. Make a fresh job directory, point `-o` and `-MF` into it (an existing
   `-MF` is replaced; `-MD` is added, and `-MMD` becomes `-MD` so that
   system headers are listed and checked too), set `TMPDIR` to it, and run
   the compiler there under write confinement (`MIRR 5` if that fails).
7. While still in the cwd, describe every file of the `.d` and of the check
   list and every directory that took part in the include search (see
   Staleness), then answer. The job directory is removed recursively.

**Never write into the mirror: OS-enforced write confinement.** Outputs go
to the per-job temp dir and are shipped back, so the host's build tree stays
authoritative and the mirror stays a read-only copy. Argument filtering alone
cannot guarantee that: the driver has more output-producing options than
`-o`/`-MF` (listing, index and dependency outputs, forwarded `-Wa,`/`-Wp,`
arguments, plugins, future flags). The guarantee therefore comes from the
OS, and it is required from the first shippable release:

- The compiler and every process it spawns can create, modify, rename or
  delete files only inside the per-job temp dir (plus `/dev/null`). Reads
  are not restricted. On macOS the mechanism is a `sandbox-exec` profile
  (or `sandbox_init` in the child before `exec`) that denies `file-write*`
  except under the job dir.
- An equivalent OS-enforced mechanism may replace it, but only if it gives
  the same guarantee. A separate restricted user does not qualify on its
  own, because that user can still write to `/tmp`, its home directory and
  anything else it owns; the argument allowlist does not qualify at all. It
  stays as defence in depth, not as the boundary.
- When started with `--mirror-root`, the daemon checks at startup that it
  can establish the confinement (a probe child must fail to write outside
  its job dir and succeed inside it) and refuses to start if not. Per job,
  any failure to set it up is `MIRR 5`: mirror jobs never run unconfined.
  A platform without a qualifying mechanism has no mirror mode.
- Implementation (`src/confine.c`): `sandbox_init()` with a Seatbelt
  profile (`deny file-write*`, allow under the job dir and `/dev/null`,
  `deny network*`) in the forked child, after closing every descriptor
  above 2 (the audit showed Seatbelt still allows `write()` on an inherited
  descriptor, such as the client's socket). A close-on-exec pipe tells the
  daemon whether the profile was applied.

**Security, honestly stated.** Today a client can only make the daemon
compile a `.ii` it sent. In mirror mode an allowed client can make the
compiler read any file the daemon user can read, and error messages can echo
its content back. Writes are confined to the per-job temp dir by the
OS-enforced confinement above, which is part of the first shippable step
rather than optional hardening. The read side is acceptable here: same user
on both machines, a point-to-point link, and `--allow 172.31.250.1/32`.
Mirror mode must be off unless `--mirror-root` is given, and should refuse to
start alongside `--enable-tcp-insecure`.

## Staleness

**The sync must not race the build.** A compile can read a header before a
sync replaces it and a size/mtime check afterwards sees only the final,
matching metadata, so the checks below cannot catch a sync that runs while
jobs are in flight; second-resolution mtimes also miss a same-size edit
within one second. The design therefore requires one of: (a) the sync runs
to completion before the build starts and never during it (what running
`distcc --mirror-sync` before `ninja` does; the simplest and the recommended
first step), or
(b) each sync publishes a new immutable generation of the mirror (new
directory, atomically renamed into place, old generations kept until their
jobs finish) and every job is pinned to the generation it started on. The
checks below then guard against the mirror being behind, not against it
changing underneath a job.

**The check list.** The client builds it from argv: the input file, every
`-include-pch` operand, every `-include` operand, and for each `-include H`
the files the driver would load implicitly in its place (`H.pch`, `H.gch`),
recorded as present or absent. A PCH that is absent on the host but present
on the mirror is a mismatch like any other. If step 0 finds that any of the
nine PCHs is chained to another PCH, the referenced PCH joins the list too.

PCHs need this list because nothing else covers them. The `.d` does not
list the `.pch`, and clang accepting a PCH says nothing about whether it is
the host's current one: two PCHs built from the same, unchanged header that
uses `__TIME__` differ in content and give different program output, yet
clang accepts either one with `-Winvalid-pch`, and the `.d` files and the
metadata of every path they list are identical (reproduced with Apple clang
on both Macs). Only the PCH file itself tells them apart: both PCHs were
825,820 bytes, so its content must be compared.

Two checks, in order:

- **Pre-check** (daemon, before compiling): size and mtime of every
  check-list path in a synced tree; installed-tree paths are left to the
  post-check. A handful of `stat`s; fails fast with `MIRR 2`. An
  optimisation, not the correctness check.
- **Post-check** (client, `DSTA`): the identity of every file the compile
  actually read according to the `.d`, **and of every check-list path**,
  compared on the host before the object is accepted. This covers headers,
  including generated ones, SDK and compiler builtin headers, and the PCHs.
  It is the correctness check and is complete from step 1. A Command Line
  Tools mismatch therefore shows up as a post-check failure, as well as a PCH
  load error.

**File identity** is size and SHA-256 of the content, for every file,
`stat`ed with symlinks followed. Metadata is not enough anywhere: size-only
misses a same-length edit (one changed digit in a constant), and size plus
whole-second mtime misses the same edit made within the second (openrsync
also drops the nanoseconds on the M6, and installed trees need not share
mtimes at all; see Facts). A review reproduced exactly that: local result
1, helper result 2, accepted. A wrong object would be cached by ccache
under the host's key.

Digests are cached on each side, keyed by the resolved file's device,
inode, size, nanosecond mtime and ctime; any write to the file changes its
ctime, so a cache hit can only return the digest of the current content.
A cache hit costs only the `stat` the check makes anyway; a miss hashes
the file once (the 6413 Homebrew and CLT headers Ohmly reads total 61 MB
and hash in 0.28 s; a changed PCH, 40-113 MB, is hashed once after it is
rebuilt). The distcc client is one process per job, so its cache has to
live on disk.

**Include-search shadowing.** Comparing the files a compile read is not
enough. If the client has a header somewhere the search reaches before the
place the helper found its copy (a header added after the sync, a file
filtered out of it, a Homebrew package only the client has), the client's
own compile would read a different file although every listed file
matches. Two checks cover this (`src/mirror_search.c`):

- **Shadow candidates.** Both sides take the include search path from the
  compiler itself: `<compiler> <the job's search and target flags> -x
  <lang> -E -v -`, run in the job's cwd with its environment (`CPATH`,
  `C_INCLUDE_PATH` and the language variants included), reports the
  `"..."` and `<...>` lists in the order the compiler will use, after its
  own rules for duplicates (with `-Ilate -Iearly -isystem late` it drops the
  user `late` and searches `early` first), missing directories and its
  implicit directories. A model of those rules got the duplicate case
  wrong, so none is kept. The answer is cached in `$DISTCC_DIR`, keyed by
  the compiler binary's digest, the cwd, the probe and the environment,
  and each entry records what the order depends on: the identity (device
  and inode, or absent) of every explicit and environment search
  directory and of every directory the report lists or names as ignored
  (missing or duplicate). An entry is used only while all of them are
  unchanged, so a retargeted symlink (which changes the compiler's
  duplicate elimination) or a compiler-provided directory that appears
  later forces a new probe; an entry is not recorded if an explicit
  directory changed while the compiler was being asked. On the daemon the probe runs exactly
  like the compile (confined to the job directory, with the overlay, other
  descriptors closed), since it runs the client's compiler command. Joined
  (`-isystemdir`), separate and `=` spellings are all recognized, and
  options the mirror does not model (`-iprefix`, `-iwithprefix*`,
  `-iwithsysroot`, `-I-`, `-ivfsoverlay`, `-index-header-map`, header
  maps, ...) keep the job off the mirror. For each file read (except the input), spelled S
  relative to the search directory it lies in, the candidates are S in
  every earlier search directory and, when S has a directory part (`a/b.h`),
  S in the directory of every file read (a `"..."` include looks in the
  includer's directory first). Each side lists the candidates that exist
  there but were not read (a candidate that was read is an `#include_next`
  chain, as libc++'s `stdint.h` does, not a shadow). The client accepts
  only if each of its candidates is also on the helper with the same
  content: when the trees match, both see the same harmless duplicates; a
  file only the client has is exactly what would make its own compile read
  something else. On Ohmly a host-only version of this rule (reject any
  existing candidate) would have rejected 2.6% of TUs for duplicates such
  as `include/ohmly_ai/...` and `common/ohmly_ai/...`; comparing with the
  helper's list rejects none of them.
- **Directory entries.** `DSTA` also describes every search directory, the
  directory of every file read and the cwd by the SHA-256 of its sorted
  entries (`<kind><name>`, a symlink's kind being its target's), ignoring
  what the sync deliberately leaves out: the top-level excludes of each
  root, and in build trees every file that is not a source, header or PCH
  (so objects written during the build do not count). This covers a
  single-component spelling in an includer's directory. Synced directories
  must match; installed ones (`/opt/homebrew/include` on the M6 has 45
  entries, the host's 180) may differ in names that are not a component of
  any path the compile read, or that only matter at a later search
  position. Directory hashes are cached like file digests.

While the M6 lacked Homebrew's `fmt`, 19 Ohmly TUs (0.5%) failed the
candidate check: Ohmly's own `fmt` is found first, but `/opt/homebrew/include`
is also the directory of headers those TUs read, so a `"fmt/base.h"` from
one of them would find Homebrew's copy on the host. After the host's
remaining Homebrew kegs were copied to the M6 (see the M6 setup appendix),
those TUs compile in the mirror, byte-identical to local compiles.

**Compiler identity and environment.** The `.d` does not list the
compiler, and ccache keys on the host's compiler, so a helper with another
clang would put its objects under the host's key. The client sends the
SHA-256 of the binary its argv[0] runs (on macOS `/usr/bin` stubs are
resolved through `DEVELOPER_DIR` or the xcode-select link); a mismatch is
`MIRR 6`. Variables that change what the compiler reads (`CPATH`,
`C_INCLUDE_PATH`, `CPLUS_INCLUDE_PATH`, `OBJC_INCLUDE_PATH`, `SDKROOT`,
`DEVELOPER_DIR`, `MACOSX_DEPLOYMENT_TARGET`, `CCC_OVERRIDE_OPTIONS`, ...)
are forwarded and set exactly for the compiler.

The remaining blind spots: files read but not listed in the `.d`
(`.incbin`, possibly `#embed`); and a symlink in an installed directory
whose target appears or disappears without the directory itself changing.
Jobs whose dependency output does not go to a file (`-MF -`,
`DEPENDENCIES_OUTPUT`, `SUNPRO_DEPENDENCIES`) take the classic path.

What happens when stale:

| Where detected | Action |
|---|---|
| Job not mirrorable (outside the roots, untracked PCH option, bad path map) | Classic path to the same host. |
| `MIRR` 1-6 | Classic path to the same host (protocol 1/2: take the local cpp lock, preprocess, send the `.ii`); the host slot is still held. The reason (`MIRM`) is logged. The host is not marked bad. |
| Compile failed in the mirror | Classic path to the same host, since the mirror may be at fault (a file missing there). A real error fails there too and is then retried locally as usual. |
| Check mismatch, or a required file or directory missing from `DSTA` | Discard `.o`, `.d` and stdout; classic path to the same host. Log the first differing path. |
| Connection or protocol error | As for any host: back off and pick another. |

An old daemon rejects `DIST 4` and closes the connection, so `,mirror`
needs a new daemon on the helper.

**Getting files onto the helper: `distcc --mirror-sync`.** No git is
involved: the sync copies the working tree as it is on disk, uncommitted
edits included (`src/mirror_sync.c`). The build script runs it between
building the PCHs and the rest:

```sh
ninja <PCH targets> <generated headers>   # build outputs the compiles read
distcc --mirror-sync                      # copy them and the sources
ninja                                     # the build, with ,mirror active
```

For each destination (arguments, `DISTCC_MIRROR_SSH`, or every `,mirror`
host) it runs `ssh mkdir -p` for the roots, then `rsync -a -W --delete`:

1. **Sources:** each `DISTCC_MIRROR_ROOTS` root, whole, except the
   `DISTCC_MIRROR_EXCLUDE` names at its top and any build tree inside it.
   Whole, not filtered by extension, because directory identity compares
   entries: a filtered sync would leave the helper's directories different
   (or, worse, equal in name while files the filter dropped are missing).
2. **Build trees** (logical sides of `DISTCC_MIRROR_PATHMAP`): every
   directory, and only sources, headers and PCHs (the same selection
   directory identity uses).
3. **Extra trees** (`DISTCC_MIRROR_EXTRA`), whole.

`-a` keeps the mtimes the checks compare; `-W` skips delta computation,
which costs more than sending a changed PCH over this link. The first sync
of Ohmly took 11.2 s (2.0 GB, of which 675 MB build tree); see Results for
a re-sync.

Homebrew kegs and the CLT are not synced: they are installed on the M6 at
the host's versions (exact kegs copied and pinned, see Facts) and checked by
digest, so a version drift costs speed, not correctness.

PCHs and generated headers are build outputs, so they have to exist before
the sync; distcc cannot know a project's targets, which is why the first
`ninja` line stays in the build script. If it is skipped, nothing breaks:
jobs whose PCH is missing or old on the helper fail the check and compile
locally. A later step could let the client push a missing or stale PCH
itself when the daemon reports it (40–113 MB, 0.3–0.7 s), but that writes
into the mirror during a build, so the daemon would also have to record
each check-list file's inode before and after the compile and reject the
job if it changed.

## ccache and `dev-tools/distcc-clang.sh`

- ccache stays in front. `CCACHE_PREFIX` runs distcc only on a miss, and in
  depend mode ccache reads the `.d` that mirror mode returns. Because cwd and
  paths are identical, `base_dir`-relative arguments resolve the same way on
  the M6.
- The `-Xpreprocessor` PCH rewrite in `distcc-clang.sh` becomes unnecessary
  for mirrored jobs but is harmless: measured above, the rewritten spelling
  still loads the PCH when compiling from source. Fallback jobs that take
  the `.ii` path no longer need it either: distcc now strips CMake's
  `-Xclang` PCH pairs from the remote command itself (see Using it with
  Ohmly). `-emit-pch` already bypasses distcc.
- The wrapper's `-target` insertion is unaffected. Once this tree is
  installed (configure takes the triple from `$CC -dumpmachine`) it can go.

## Failure modes

- **Compiler mismatch** (the state until 2026-10-07): every job is refused
  with `MIRR 6` and takes the classic path. Mitigation: match Command Line
  Tools.
- **Sync forgotten or racing with edits.** The checks send the job to the
  classic path. Correct output, lost speed.
- **Mirror silently diverges** in a file the compile reads but `.d` does not
  list. The PCH is the known case and is covered by the check list, which
  the post-check includes from step 1; `#embed`/`.incbin` data files are
  another case and remain a residual risk, same class as ccache depend mode
  itself.
- **Installed trees drift** (a `brew upgrade` or CLT update on one Mac):
  every job that reads a changed header fails the digest comparison and
  takes the classic path until the versions match again. Correct output,
  lost speed. Differing mtimes alone cost nothing.
- **Write confinement unavailable** (no `sandbox-exec`, profile rejected, a
  platform without an equivalent): the daemon refuses to start with
  `--mirror-root`, or answers `MIRR 5` per job. Mirror jobs never run
  unconfined.
- **Old daemon with `,mirror`**: connection fails, host backed off.
- **Disk on the M6**: about 3.3 GB (2.0 GB mirror, the copied Homebrew kegs
  and kicad-mac-builder); it has 135 GB free.
- **Spotlight on the helper** indexes the mirror after each sync. Excluding
  `~/Git` in Spotlight's privacy settings on the M6 needs the user (admin).

## Implementation plan and status

0. **Manual proof.** Done 2026-10-06/07: external-header parity, openrsync
   mtime precision, `sandbox-exec` confinement of a real compile, the PCH
   version error and, after matching CLT, a host-built PCH loading on the
   M6 with byte-identical output, and the argv of a real Ohmly compile.
1. **Protocol 4 with the complete check, confinement and fallback.** Done:
   `src/mirror.c` (check list, file identity, `.d` parser, DSTA format),
   `src/mirror_ident.c` (directory identity, required sets, compiler
   identity, environment list), `src/mirror_digest.c` (persistent digest
   cache), `src/sha256.c`, `src/mirror_client.c` and `src/remote.c`
   (`dcc_compile_mirror`), `src/compile.c` (mirror branch in
   `dcc_build_somewhere`), `src/mirror_serve.c`, `src/confine.c`,
   `src/dopt.c`/`src/daemon.c` (`--mirror-root`, `--mirror-installed`,
   startup probe), `doc/protocol-4.txt`, 17 test cases.
2. **Pre-check and classic-path fallback.** Done (folded into step 1): `MIRR
   2` from the check list, and every refusal or mismatch goes the classic
   way to the same host without marking it bad.
3. **`distcc --mirror-sync`.** Done. Ohmly's `ohmly.sh` is not changed by
   this work; see Results for how the benchmark drove it.
4. Later: an order-aware shadowing check for installed directories (to stop
   the `fmt` false rejections), pushing a missing PCH on demand, a distccmon
   phase for mirrored jobs.

`src/lock.c` and `src/where.c` need no change for mirror mode; it simply
never calls `dcc_lock_local_cpp`.

## Acceptance checks

Each is a case in `test/testdistcc.py` (localhost; the daemon describes a
file or directory differently through `DISTCC_TESTING_MIRROR_*` hooks, so
staleness can be produced on one machine). The cases that compile in the
mirror run only where write confinement is available (`h_mirror confine`,
macOS today) and are skipped elsewhere, as on the Linux CI job; the helper
and startup-refusal cases run everywhere. "Rejected" means the remote
object is not used, the job goes the classic way, and the result is still
right.

| Check | Test case |
|---|---|
| Mirrored compile; `.d` identical to a local compile's | `Mirror_Case` |
| Stale PCH (same size, other mtime) | `MirrorStalePch_Case` |
| Same-size header change | `MirrorStaleHeader_Case` |
| A file or a search directory missing from `DSTA` | `MirrorOmittedFile_Case`, `MirrorOmittedDir_Case` |
| Shadowing: a synced search directory differs | `MirrorShadow_Case` |
| Same size and mtime, other content (same-second edit) | `MirrorSameSecond_Case` |
| Nested shadowing (`<sub/val.h>`, `early/sub` on both sides) with `-I`, joined `-isystem`, `CPATH`, and `-Ilate -Iearly -isystem late` (compiler's duplicate rule) | `MirrorNestedShadow_Case`, `MirrorJoinedShadow_Case`, `MirrorCpathShadow_Case`, `MirrorDedupOrderShadow_Case` |
| The cached search path is not reused after a search directory is retargeted, or a compiler-provided one appears | `MirrorSearchCacheAlias_Case`, `MirrorSearchCacheImplicit_Case` |
| The same duplicate on both sides is accepted | `MirrorDuplicateHarmless_Case` |
| `-MF -` keeps dependencies on stdout (classic path) | `MirrorDepsStdout_Case` |
| Unsupported search option (`-iprefix`) is not mirrored | `MirrorRefusedOption_Case` |
| Installed directory lacks an unrelated entry (accepted) / an entry a path was spelled through (rejected) | `MirrorInstalledHarmless_Case`, `MirrorInstalledShadow_Case` |
| Helper compiler differs (`MIRR 6`) | `MirrorCompiler_Case` |
| Confinement unavailable per job (`MIRR 5`) | `MirrorNoConfine_Case` |
| cwd outside every root (`MIRR 1`) | `MirrorOutsideRoot_Case` |
| Forwarded environment (`CPATH`) | `MirrorEnv_Case` |
| Path map used / refused when it names another directory | `MirrorPathmap_Case`, `MirrorBadPathmap_Case` |
| Compiler writes in the mirror and next to it are denied, for the compile and for the daemon's search-path probe | `MirrorConfined_Case` |
| `--mirror-root` refuses to start with `--enable-tcp-insecure` or a failing probe | `MirrorStartRefused_Case` |
| SHA-256, `.d` escapes, DSTA parsing | `MirrorHelper_Case` |

On the real pair: 40 Ohmly TUs compiled in the mirror gave objects and `.d`
files byte-identical to local compiles; Results has the full set.

## Results (2026-10-07)

Benchmark: every Ohmly TU in `compile_commands.json` except the nine
`-emit-pch` ones (3481), run as ninja runs them (through ccache with the
user's ccache config but a fresh, empty cache directory, `-MD -MT -MF`,
cwd `build-dev`), with objects written outside the Ohmly tree, which is only
read. Driver: `bench2.py` in the session scratchpad (not kept); it is a
thread pool over the commands. Host M5 Pro (15 cores, 48 GB), helper M6
(12 cores, 16 GB), both clang-2100.3.34.2, Thunderbolt bridge. Mirror runs
after one `distcc --mirror-sync`.

| Configuration | Time | TUs/s | TUs on the M6 | vs local |
|---|---|---|---|---|
| Local only, 15 jobs | 472.0 s | 7.38 | 0 | 1.00x |
| Homebrew distcc 3.4, M6/12 + localhost/15, `--localslots_cpp=40`, `DISTCC_PAUSE_TIME_MSEC=20`, 27 jobs (the existing setup) | 319.5 s | 10.90 | 1457 | 1.48x |
| This branch, classic, M6/12 + localhost/15, 27 jobs | 316.3 s | 11.01 | 1457 | 1.49x |
| This branch, classic, M6/14 + localhost/17, 31 jobs | 290.0 s | 12.00 | 1429 | 1.63x |
| **Mirror**, M6/12 + localhost/15, 27 jobs | 256.2 s | 13.59 | 1606 | 1.84x |
| **Mirror**, M6/14 + localhost/15, 29 jobs | 245.0 s | 14.21 | 1657 | 1.93x |
| **Mirror**, M6/16 + localhost/15, 31 jobs | 244.2 s | 14.25 | 1642 | 1.93x |
| **Mirror**, M6/14 + localhost/17, 31 jobs | 241.4 s | 14.42 | 1679 | 1.96x |

- Mirror mode is 24% faster than the existing setup and 17% faster than the
  classic path with the same tuned slot counts. The design estimated +10-20%.
- The pool is now at the two machines' combined capacity: in the 12+15 run
  the host compiled 1875 TUs at 7.32/s against 7.38/s when compiling alone,
  so a mirrored job costs the host almost nothing, and the M6 added 6.3/s.
  The M6 takes 14 slots on 12 cores (load ~17, at least 66% of memory free,
  no swapping at 16 slots); more does not help.
- Every mirrored job was accepted (one conservative rejection in one run,
  see below). All 3481 objects of the 12+15 mirror run are byte-identical
  to the local run except `common/build_version.cpp`, which was compiled
  locally and embeds the build time; all `.d` files are identical apart
  from the output name. A real `ninja` rebuild of 60 PCH TUs through the
  mirror pool gave 60 of 60 objects identical to the local ones.
- `distcc --mirror-sync`: first copy 11.2 s (2.0 GB), unchanged re-sync
  3.0-3.1 s.

The first full runs found three problems that the localhost tests could
not, all fixed:

1. ccache's `base_dir` turns paths into `../../../../../Users/...`; the
   directory classification compared those unnormalized, so the source
   root's excludes did not apply and 233 jobs were rejected.
2. ccache adds `-fpch-preprocess`; the conservative "untracked PCH option"
   rule kept 685 jobs from trying the mirror at all.
3. A PCH records its inputs under the client's physical cwd
   (`/Volumes/ExternalSSD/.../build-dev/../../../../../Users/...`), which
   the M6 cannot open: 697 PCH TUs failed in the mirror and were redone the
   classic way. Fixed with the per-job clang VFS overlay (see Daemon side).

The remaining rejection: a TU for which `/opt/homebrew/include` (where the
M6 lacks Homebrew's `fmt`) comes before the directory its `fmt` headers
could have been found through, so the helper's result is rightly not
trusted.

After a review (content identity for every file, shadow candidates
including nested spellings, `CPATH` and joined options), a rerun with the
same slot counts produced objects byte-identical to the local run for all
3481 TUs except `build_version.cpp`, with 19 conservative rejections (the
`fmt` case above). Its time (294 s) is not comparable: the host was in use
during that run.

## Using it with Ohmly

Nothing in Ohmly was changed. Everything below is driven by
`contrib/distcc-mirror` (installed as `~/.local/distcc-mirror/bin/distcc-mirror`)
and its config `~/.config/distcc-mirror/ohmly.conf`:

```sh
ROOTS=~/Git/Ohmly
BUILD_DIR=~/Git/Ohmly/build-dev            # a symlink to /Volumes/ExternalSSD/Developer/Ohmly/build-dev
EXCLUDE=.git:build-release:output:tmp
EXTRA=~/Github/kicad-mac-builder/build/wxwidgets-dest:~/Github/kicad-mac-builder/build/python-dest:~/Github/kicad-mac-builder/build/ngspice-dest
HELPERS="m6=172.31.250.2/14"
LOCAL_JOBS=17
PORT=3634
CLIENT_ADDR=172.31.250.1
PREFIX=~/.local/distcc-mirror
CCACHE_PREFIX=~/.local/distcc-mirror/bin/distcc
```

It was written by `distcc-mirror init --helper m6=172.31.250.2/14 --build
build-dev --local-jobs 17 --exclude output:tmp --extra ...`. The
environment it gives is the one the benchmark set by hand, with
`DISTCC_MIRROR_PATHMAP` derived from the `BUILD_DIR` symlink. A build is

```sh
cd ~/Git/Ohmly && distcc-mirror build        # PCHs, sync, ninja -j31
```

`distcc-mirror helper install` replaces the hand-started M6 daemon (port
3634, `--jobs 14`, `--mirror-root ~/Git/Ohmly`) with a LaunchAgent
(`local.distcc-mirror.distccd.3634`), next to the Homebrew one on 3632.
`distcc-mirror doctor` checks both Macs, and `distcc-mirror test` compiles
random Ohmly TUs on the M6 and compares them with local compiles: 6 of 6
mirrored and byte-identical, and 4 of 4 PCH TUs through ccache with
`CCACHE_PREFIX` pointing at distcc directly.

`dev-tools/distcc-clang.sh` is no longer needed with this tree:

- its target triple fix was for Homebrew 3.4's `arm-apple-darwin` (this
  build uses `arm64-apple-darwin27.0.0`, and Ohmly's absolute
  `/usr/bin/clang++` is not rewritten anyway);
- distcc now drops CMake's `-Xclang -include-pch -Xclang <pch> -Xclang
  -include -Xclang <header>` from the remote command of a classic job
  itself (`dcc_strip_local_args`). The local `-E` has already expanded the
  PCH, so the remote compile gives an object identical to a direct
  compile. Before, the remote compile failed, was redone locally, and the
  helper was backed off for a minute;
- distcc keeps PCH generation local by itself (`-x *-header`,
  `-emit-pch`).

Dropping it also saves its per-job `/usr/bin/clang -dumpmachine` (about
35 ms of host CPU). `dev-tools/ohmly.sh` cannot be used for this unchanged:
it checks that `~/.distcc/hosts` names `172.31.250.2` without options, and
it does not run the sync.

## Expected gain (estimates)

Per helper slot, a PCH job today occupies cpp (slot idle) + `.ii` transfer +
compile from `.ii`. Mirrored it occupies compile with PCH + `.o` transfer.
From Part A: (0.43 + 5.63) / 5.06 = 1.20x, (0.45 + 3.66) / 2.73 = 1.50x,
(0.28 + 2.52) / 2.11 = 1.33x more jobs per slot. That ignores transfer, which
for 11–22 MB at the measured 170 MB/s+ is another 0.05–0.13 s today.

- Helper: with 75% of non-`qa` TUs on a PCH, about +15–38% helper capacity
  on a full Ohmly rebuild.
- Host: gets back 0.28–0.45 s of cpp per remote job, 8–16% of that TU's
  compile time with PCH.
- Pool: the helper delivered ~45% of jobs in the perf benchmark (about 7 of
  15 jobs/s), so 0.45 x (15–38%) = +7–17% from the helper, plus the host's
  freed cpp: roughly +10–20% pool throughput on full rebuilds. Small builds (< 50 files) stay local in
  `ohmly.sh` and gain nothing.

This rests on host-measured ratios, assumes the M6 behaves alike, and ignores
the sync time (seconds per build). Step 0 should confirm it before any C is
written. For comparison, fixing perf findings 1 and 3 (cpp lock and slot
held during cpp) recovers only the slot-idle part (0.28–0.45 s per job), not
the PCH part.

## Appendix: Part A commands

Harness: `scratchpad/w4/pchbench.py` (session scratch, not kept), run as
`/usr/bin/python3 -I pchbench.py <out> 3 pcbnew/pcb_edit_frame.cpp
eeschema/sch_symbol.cpp common/eda_draw_frame.cpp`. For each TU it takes the
`compile_commands.json` entry that uses `-include-pch` and runs
`subprocess.run(argv, cwd=...)` three times per variant:

- **1**: `cd /Users/clementhathaway/Git/Ohmly/build-dev && <command>`, with
  only the `-o` operand replaced by `<out>/<tu>.v1.o`.
- **2a (cpp)**: same cwd. Each `-Xclang -include-pch -Xclang P -Xclang
  -include -Xclang H` becomes `-Xpreprocessor -include-pch -Xpreprocessor P
  -Xpreprocessor -include -Xpreprocessor H` (the `distcc-clang.sh` rule).
  `-c` becomes `-E`, and `-o` becomes `<out>/<tu>.ii`.
- **2b (compile)**: `cd <out>`; argv 2a passed through a Python copy of
  `dcc_strip_local_args` (`src/strip.c`). `-E` back to `-c`, `-o
  <out>/<tu>.v2.o`, and the input replaced by the `.ii`. Exact command for
  `common/eda_draw_frame.cpp` (`<out>` abbreviated):

  ```text
  /usr/bin/clang++ -fstandalone-debug -Wall -Wsuggest-override -Winconsistent-missing-override
  -Werror=vla -Wimplicit-fallthrough -Werror=return-type -Wshadow -Wsign-compare
  -Wmissing-field-initializers -Wempty-body -Wreorder -Wmismatched-tags -Wpessimizing-move
  -Wredundant-move -Wno-psabi -O3 -std=c++20 -arch arm64
  -isysroot /Library/Developer/CommandLineTools/SDKs/MacOSX26.5.sdk -mmacosx-version-min=26.5
  -fPIC -fvisibility=hidden -fvisibility-inlines-hidden -ftrivial-auto-var-init=zero
  -Winvalid-pch -o <out>/common_eda_draw_frame.cpp.v2.o -c <out>/common_eda_draw_frame.cpp.ii
  ```

- **3**: same cwd as 1. `-Xclang -include-pch -Xclang P` dropped, and
  `-Xclang -include -Xclang H` replaced by `-include <out>/<tu>.pchhdr.hxx`,
  a byte copy of H. The copy is used so the driver cannot find `H.pch` next
  to it and load the PCH anyway. The header uses only `<...>` includes, so
  the copy resolves identically.
- **4 and the `.d` counts**: the 2a argv with `-E` back to `-c`, `-o
  <out>/<tu>.v4.o`, plus `-MD -MT x.o -MF <out>/<tu>.v4.d`, run in the build
  dir. Variant 1 was rerun the same way with `-MD` for comparison.

Link throughput: `find build-dev -name '*.pch' -exec cat {} + | ssh m6 'wc
-c'` (658,549,964 bytes, 3.89 s). Tree walk: `rsync -an --stats` of `Ohmly/`
(with the excludes above) to an empty local scratch dir, 3.65 s.

## Appendix: M6 setup and checks (2026-10-06 and 2026-10-07)

What was changed on the M6, over `ssh m6`, to bring it to header parity with
the host. A copy of this list lives on the M6 in `~/M6-SETUP-NOTES.md`. The
running `distccd` (Homebrew distcc 3.4, LaunchAgent `com.ohmly.distccd`) was
not touched.

- **Homebrew kegs:** the 16 formulae Ohmly's compiles read, copied from the
  host's Cellar at the host's exact versions with `rsync -a
  /opt/homebrew/Cellar/<name>/<version>`, then `brew link <name>` and
  `brew pin <name>` (zstd 1.5.7_1 was already present at the same version
  and was pinned too). `brew install` was not used because it would have
  installed newer versions of nine of them. Dependencies of these kegs were
  not installed, so `brew doctor` may complain; that does not affect
  compiling. A formula upgraded on the host has to be copied again.
- **kicad-mac-builder:** `~/Github/kicad-mac-builder/build/{wxwidgets,python,ngspice}-dest`
  copied with `rsync -a` to the same paths (238 MB).
- **SDK:** nothing to do; the M6 already had `MacOSX26.5.sdk`.
- **Command Line Tools:** the host was updated to Command Line Tools for
  Xcode 27.0 (clang-2100.3.34.2, the M6's version) on 2026-10-07; see Facts
  for why Software Update did not offer it at first. This invalidated the
  host's existing PCHs, which the next build regenerates.
- **All Homebrew formulae (2026-10-07):** the other 87 formulae installed
  on the host were copied the same way (kegs carry Homebrew's install
  receipts, so `brew list` on the M6 shows the same 115 formulae), the 81
  linked on the host linked with `brew link`, and all pinned.
  `/opt/homebrew/include` is then identical on both Macs except `openssl`,
  linked by hand on the host and left keg-only on the M6. Five formulae the
  M6 already had in newer versions were left alone. `brew install` was not
  used because it installs newer versions than the host's, and mirrored
  compiles need identical headers; upgrade both Macs together instead.
- **Not needed:** a `/Volumes/ExternalSSD` volume (the client sends the
  logical cwd, see Facts) and git on the M6 (`distcc --mirror-sync` copies
  the working tree).
- **Not done yet:** copying the Ohmly sources and `build-dev` outputs; that
  waits for `distcc --mirror-sync`.

Checks run, all in `~/mirror-test` on both Macs (deleted afterwards) or
read-only:

| Check | Result |
|---|---|
| openrsync mtime precision | whole seconds kept, nanoseconds dropped on the M6 |
| PCH rebuilt from an unchanged `__TIME__` header | same size (825,820 B), different mtime; clang accepts both, output differs, `.d` identical |
| Same-size header edit | caught by mtime |
| `sandbox-exec` profile allowing writes only in the job dir | real compile succeeds; writes to the mirror, `$HOME`, `/tmp`, `-Wp,-MD,<mirror>`, `-save-temps=cwd` and `-o <mirror>` denied; malformed profile exits 65 without running anything |
| Host PCH on the M6, before the CLT update | rejected: "built from a different branch" |
| Host PCH on the M6, after the CLT update | accepted; `.o` and `.d` byte-identical to the host's |
| 6984 external headers from ninja's deps log | after the keg copy and CLT update: identical size, mtime (symlinks followed) and content on both Macs |
| SHA-256 of the 6413 Homebrew and CLT headers | 61 MB, 0.28 s on the host |
| argv of an Ohmly compile after ccache | all inputs absolute logical paths; only `-o`, `-MF`, `-MT` relative; no `/Volumes` path |
