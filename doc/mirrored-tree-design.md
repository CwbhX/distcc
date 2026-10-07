# Design: mirrored-tree mode

Status: proposal, 2026-10-06. Nothing here is implemented. It follows on from
`doc/perf-findings-two-mac-pool.md` (same two Macs, same Ohmly workload).

## The idea

Today every remote job runs cpp on the host, ships a multi-MB `.ii`, and the
helper compiles the full header text, because a `.ii` cannot use the host's
precompiled header (PCH). In **mirrored-tree mode** the helper has the source
tree, the build tree, the PCH files and the external headers at the same
absolute paths as the host. The client sends only the command and the working
directory. The daemon runs the compiler on the source in that directory, so it
preprocesses locally and loads the PCH. Results come back as today.

What it removes per remote job: host cpp time and the local cpp lock, the
helper slot sitting idle while the host preprocesses, the `.ii` transfer, and
the missing-PCH penalty on the helper. What it adds: a sync step outside
distcc, and a way to prove the mirror was current.

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

- **Compilers differ today.** Host: clang-2100.3.33.1. M6: clang-2100.3.34.2.
  Clang refuses a PCH written by a different compiler build, so mirrored PCH
  jobs fail on the M6 until both Command Line Tools match. Tested on the M6
  on 2026-10-06: `error: PCH file '...' built from a different branch
  ((clang-2100.3.33.1)) than the compiler ((clang-2100.3.34.2))`.
- **Physical vs logical cwd.** `~/Git/Ohmly/build-dev` is a symlink to
  `/Volumes/ExternalSSD/Developer/Ohmly/build-dev`. `dcc_x_cwd`
  (`src/clirpc.c:95`) sends `getcwd()`, which is the physical path. The M6 has
  no `/Volumes/ExternalSSD`. Commands mix logical absolute paths
  (`-I/Users/.../Git/Ohmly/build-dev/...`) with paths relative to the
  physical cwd (`-o pcbnew/CMakeFiles/...`), and ccache's
  `base_dir=/Users/clementhathaway/Git` may turn absolute paths into relative
  ones. The mirror must therefore reproduce **both** paths: the same symlink
  at `~/Git/Ohmly/build-dev`, and the same physical target. On the M6 that
  most simply means an APFS volume named `ExternalSSD` (it mounts at
  `/Volumes/ExternalSSD`; creating it may need admin rights).
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
  version.
- **ccache runs in depend mode** (`depend_mode = true`): it never runs cpp
  itself and builds its manifest from the `.d` file the compile writes. A
  mirrored job must therefore return the real `.d` to the client's `-MF`
  path. It also means a wrong object compiled from stale helper headers
  would be cached under the key of the host's current headers. The
  staleness check below exists mainly to prevent that.
- Ninja passes `-MD -MT $out -MF $DEP_FILE` (`build-dev/CMakeFiles/rules.ninja`).

## Host-list syntax

`172.31.250.2/12,mirror`

- `,mirror` sets `host->cpp_where = DCC_CPP_MIRROR` (new enum value in
  `src/distcc.h`) and `protover = DCC_VER_4`. Parsed in `dcc_parse_options`
  (`src/hosts.c:239`) next to `,cpp`.
- `,mirror` and `,cpp` together is a hostspec error. `,lzo` with `,mirror` is
  rejected at first (the payload is one `.o`; compression can come later by
  adding a feature word to the version 4 request).
- Optional client filter `DISTCC_MIRROR_ROOTS=/Users/clementhathaway/Git/Ohmly:/Volumes/ExternalSSD/Developer/Ohmly`.
  Jobs whose cwd is outside these roots use the classic path to the same
  host. Without it every job tries mirror first; a miss costs one round trip
  (~0.4 ms on this link) plus a reconnect.
- **No local cpp lock.** For a mirrored job, `dcc_build_somewhere` must skip
  `dcc_lock_local_cpp` (`src/compile.c:777`). The client does only a few
  `stat`s before sending and a few thousand after. This sidesteps findings 1
  and 3 of the perf doc for mirrored jobs; `localslots_cpp` then only matters
  for fallbacks and non-mirror hosts.

## Protocol: a new version 4

Protocol 3 cannot be reused with zero files. Its server side
(`make_temp_dir_and_chdir_for_cpp`, `tweak_arguments_for_server`,
`src/serve.c`) roots every absolute `-I` and the input under a fresh temp
dir and chdirs into `<tmp><cwd>`. Mirror mode needs the opposite: the real
cwd, no rewriting. Version 3 also implies LZO and pump clients depend on its
semantics. Old daemons reject `DIST 4` (`dcc_r_request_header`,
`src/srvrpc.c`), so `,mirror` against an old daemon fails the connection and
the client backs off that host; document that `,mirror` needs a new daemon.

Request:

```text
DIST 4
CDIR <physical cwd>         as dcc_x_cwd today
ARGC/ARGV                   argv after dcc_scan_args and client rewrites; original -o, -MF, -MT kept
NCHK <n>                    check list (see Staleness): the input, every PCH operand,
                            every -include operand and its implicit PCH candidates
  CHKN <path> CHKX <0 absent | 1 present> CHKS <size> CHKM <mtime seconds>
                            repeated n times; CHKS and CHKM are 0 when absent
```

The check list is never empty and is complete from the first release: it
must name every PCH the compile can load, because the `.d` does not. A client
that cannot classify every PCH-related option in argv (`-include-pch` and
`-include` in their driver, `-Xclang` and `-Xpreprocessor` spellings) does not
use mirror mode for that job (classic path, or local before step 2).

Response:

```text
DONE 4
MIRR <code>    0 = compiled; nonzero = not attempted, nothing follows:
               1 mirror disabled / cwd not under a root, 2 pre-check mismatch,
               3 argument refused by policy, 4 cwd or input missing,
               5 write confinement could not be established
STAT, SERR, SOUT, DOTO      as protocol 2
DOTD <bytes>   the .d, target already set to the client's output name
DSTA <bytes>   text, one line per .d entry plus each check-list path:
               "<size> <mtime_s> <digest> <path>", or "absent <path>";
               <digest> is "sha256:<hex>" for paths under an installed
               tree and "-" elsewhere (see Staleness)
```

Client side, `dcc_retrieve_results` (`src/clirpc.c:153`) writes the `.o` to a
temp file, not to `output_fname`. It parses `DSTA`, `stat`s each path
locally (following symlinks, since the compiler read the target), and only
on a full match renames the `.o` into place and writes the
`.d` to `deps_fname`. A full match also requires every path of the check list
the client sent to appear in `DSTA`; the client does not rely on the daemon
to echo it. About 3000 `stat`s is a few milliseconds; digests come from a
cache (see Staleness).

## Daemon side

New code path in `dcc_run_job` (`src/serve.c:647`), taken when
`cpp_where == DCC_CPP_MIRROR`:

1. Refuse (`MIRR 1`) unless the daemon was started with one or more
   `--mirror-root DIR` (new option in `src/dopt.c`). `realpath(cwd)` and
   `realpath(input)` must lie under a root; this resolves `..` and symlinks.
2. Run the pre-check `stat`s on the check list. Mismatch: `MIRR 2`, with the
   differing path in the log. This catches the common cases (source edited
   since the sync, PCH rebuilt and not yet synced) before any compile time is
   spent. The early rejection is an optimisation (step 2 of the plan); the
   correctness check is the client's comparison of the same paths in `DSTA`,
   which step 1 already does.
3. Argument policy (`MIRR 3`). The existing checks stay (compiler whitelist,
   `-fplugin=`, `-specs=`). Output-writing options are either rewritten or
   refused:
   - `-o` becomes a daemon temp file (`dcc_set_output`). `-MF` becomes the
     temp `deps_fname`. `-MD` is added if absent, so a `.d` always exists for
     the stat check. `-MT` is kept (client adds `-MT <output>` when it needs a
     `.d` and set no target, as pump mode does at `src/compile.c:843`).
   - Refused: `-save-temps*`, `-ftime-trace*`, `-gsplit-dwarf`,
     `-serialize-diagnostics`, `-fmodules`/`-fmodules-cache-path`,
     `-fcrash-diagnostics-dir`, `-emit-pch`, `-x *-header`. `-Xclang`
     operands are allowlisted (`-include-pch`, `-include`, and anything else
     observed in real builds) rather than denylisted, which also blocks
     `-Xclang -load`.
4. Create a fresh per-job temp dir and establish write confinement for the
   compiler (below). If that fails for any reason, answer `MIRR 5` and do
   not spawn the compiler. `TMPDIR` points at the job dir, and the temp
   `.o`, `.d` and captured stderr/stdout all live in it.
5. `chdir(cwd)`, spawn the compiler under the confinement,
   `chdir(dcc_daemon_wd)` after (the existing restore at `out_cleanup`
   already handles this).
6. Return `DOTO` from the temp `.o`. No `dcc_fix_debug_info`: paths are
   already the client's. Return `DOTD` and build `DSTA` from the `.d`
   (parser in `src/dotd.c`, handling `\ ` escapes and line continuations),
   plus every check-list path, because the PCH does not appear in the `.d`,
   with digests for installed-tree paths.

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
to completion before the build starts and never during it (what the
`ohmly.sh` phases below do; the simplest and the recommended first step), or
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
on both Macs). Only the PCH file's own mtime tells them apart: both PCHs
were 825,820 bytes.

Two checks, in order:

- **Pre-check** (daemon, before compiling): size and mtime of every
  check-list path in a synced tree; installed-tree paths are left to the
  post-check. A handful of `stat`s; fails fast with `MIRR 2`. This is an
  optimisation and can arrive after step 1.
- **Post-check** (client, `DSTA`): the identity of every file the compile
  actually read according to the `.d`, **and of every check-list path**,
  compared on the host before the object is accepted. This covers headers,
  including generated ones, SDK and compiler builtin headers, and the PCHs.
  It is the correctness check and is complete from step 1. A Command Line
  Tools mismatch therefore shows up as a post-check failure, as well as a PCH
  load error.

**File identity** depends on how the file got onto the M6. Every path is
`stat`ed with symlinks followed; size-only is never enough, because a
same-length edit to a header (one changed digit in a constant) would pass it
and the wrong object would be cached by ccache under the host's key.

- **Synced trees** (source tree, `build-dev` subset including the PCHs,
  kicad-mac-builder dest dirs): size and mtime. These are equal on both
  Macs because the sync copied them with `rsync -a`. Only seconds are
  compared: openrsync sets whole-second mtimes on the M6 and drops the
  nanoseconds (measured). This is the cheap path for the files most likely
  to change, and the only one the PCHs need.
- **Installed trees** (default `/Library/Developer/CommandLineTools` and
  `/opt/homebrew`; the same list configured on client and daemon): size and
  SHA-256 of the content. Their mtimes cannot be relied on to agree across
  machines even when the content does (see Facts), and aligning them would
  mean writing into root-owned CLT directories after every update. The
  daemon sends the digest in `DSTA`; the client compares it with its own and
  treats a path under an installed tree without a digest as a mismatch.

Digests are cached on each side, keyed by the resolved file's device,
inode, size, nanosecond mtime and ctime; any write to the file changes its
ctime, so a cache hit can only return the digest of the current content.
Measured on the host: the 6413 Homebrew and CLT headers Ohmly reads total
61 MB and hash in 0.28 s (`shasum -a 256`), and a TU such as
`pcb_edit_frame.cpp` reads about 2200 of them, so an uncached job pays
about 0.1 s on each side and a cached one only the `stat`s (0.05 s for all
6413). The distcc client is one process per job, so its cache has to live
on disk.

The remaining blind spot is in the synced trees: an edit that keeps both
size and mtime, e.g. a tool restoring the mtime after a same-length change.
Ninja's rebuild logic makes the same assumption.

What happens when stale:

| Where detected | Action |
|---|---|
| `MIRR 1/2/4` | Classic path on the same host: reconnect with protocol 1/2, take the local cpp lock, preprocess, send the `.ii`. The helper slot is still held. Do not call `bad_host`; the daemon is healthy. (Step 1: compile locally instead.) |
| `MIRR 3` | Same, and log the refused argument once. |
| `MIRR 5` | Same, and log once that the helper cannot confine mirror jobs. |
| Remote compile failed (e.g. PCH rejected by clang, `-Winvalid-pch`, or a write denied by the confinement) | Existing logic: `dcc_critique_status` then local retry (`src/compile.c:935`). Clang accepting a PCH is not evidence that it is current; that is the post-check's job. |
| Post-check mismatch, including a check-list path missing from `DSTA` | Discard `.o` and `.d`, compile locally (simplest, and the slot was already spent). Log the first differing path. |

**The sync step (outside distcc, in `ohmly.sh`).** The PCH and generated
headers are build outputs, so a sync before the build is not enough:

1. `rsync -a --delete` the source tree to the M6, excluding `.git`,
   `build-*`, `output`, `tmp`.
2. Build the PCHs and generated headers locally first:
   `ninja <the 9 cmake_pch.hxx.pch targets> <generated headers>`. The PCH
   targets are named in `build.ninja`, e.g.
   `common/CMakeFiles/kicommon.dir/cmake_pch.hxx.pch`.
3. `rsync -a -W` the `build-dev` subset that compiles read: `*.pch`,
   `cmake_pch.hxx*`, and generated `*.h/*.hpp/*.inc/*.cpp/*.cc`. `-a` keeps
   the PCH mtimes the post-check compares.
4. `rsync -a` the kicad-mac-builder dest dirs. Homebrew kegs and the CLT
   are not synced: they are installed on the M6 at the host's versions
   (exact kegs copied and pinned, see Facts) and checked by digest, so a
   version drift costs speed, not correctness.
5. Run the build with `,mirror` active.

Sizes and times from the host (estimates where marked):

| Item | Size | Transfer |
|---|---|---|
| Source tree, no `.git`/build dirs | 1.42 GB (137 MB of it is code files) | first copy: ~10–60 s (estimate); unchanged re-sync: file walk ~3.6 s (local `rsync -an` dry run) |
| 9 PCH files | 658 MB | 3.9 s for all nine (measured: streamed over `ssh m6` to `wc -c`, ~170 MB/s) |
| Generated code in `build-dev` | 165 files, 46 MB | < 1 s (estimate) |
| kicad-mac-builder dest dirs | 238 MB whole (copied once on 2026-10-06), ~10 MB of headers | < 1 s when unchanged (estimate) |
| Homebrew include trees (boost 179 MB, opencascade 43 MB, ...) | 273 MB via `/opt/homebrew/include` | done once: the 16 kegs Ohmly reads were copied from the host (~660 MB with libraries) and pinned; redo for any formula upgraded on the host |

A dry run to the M6 itself was not done: openrsync might create the
destination directory even with `-n`, and creating files there was out of
scope. Any header change that rebuilds a PCH costs one 40–113 MB copy
(~0.3–0.7 s at the measured rate).

## ccache and `dev-tools/distcc-clang.sh`

- ccache stays in front. `CCACHE_PREFIX` runs distcc only on a miss, and in
  depend mode ccache reads the `.d` that mirror mode returns. Because cwd and
  paths are identical, `base_dir`-relative arguments resolve the same way on
  the M6.
- The `-Xpreprocessor` PCH rewrite in `distcc-clang.sh` becomes unnecessary
  for mirrored jobs but is harmless: measured above, the rewritten spelling
  still loads the PCH when compiling from source. It stays needed for
  fallback jobs that take the `.ii` path, so leave the wrapper as is.
  `-emit-pch` already bypasses distcc.
- The wrapper's `-target` insertion is unaffected. Once this tree is
  installed (configure takes the triple from `$CC -dumpmachine`) it can go.

## Failure modes

- **Compiler mismatch** (true today). Every PCH job fails remotely and
  retries locally: a slow pool. Mitigation: match Command Line Tools first.
  A later `CVER` token (cached `clang --version` per compiler path) would let
  the daemon answer `MIRR` instead of wasting a compile.
- **Sync forgotten or racing with edits.** Pre-check and post-check send the
  job to classic or local. Correct output, lost speed.
- **Mirror silently diverges** in a file the compile reads but `.d` does not
  list. The PCH is the known case and is covered by the check list, which
  the post-check includes from step 1; `#embed`/`.incbin` data files are
  another case and remain a residual risk, same class as ccache depend mode
  itself.
- **Installed trees drift** (a `brew upgrade` or CLT update on one Mac):
  every job that reads a changed header fails the digest comparison and
  compiles locally until the versions match again. Correct output, lost
  speed. Differing mtimes alone cost nothing.
- **Write confinement unavailable** (no `sandbox-exec`, profile rejected, a
  platform without an equivalent): the daemon refuses to start with
  `--mirror-root`, or answers `MIRR 5` per job. Mirror jobs never run
  unconfined.
- **Old daemon with `,mirror`**: connection fails, host backed off. Document
  it; do not auto-downgrade.
- **Disk on the M6**: about 2.4 GB for source, build subset and external
  headers; it has 135 GB free.

## Implementation plan (smallest shippable step first)

0. **Manual proof, no code.** Match CLT versions; set up the mirror for one
   TU; run its exact command over `ssh m6` in the same cwd; `cmp` the `.o`
   with the host's; time with and without PCH on the M6; check whether any
   PCH is chained to another. Settles path identity, PCH compatibility and
   the real M6 saving. Done on 2026-10-06: external-header parity (all 6984
   identical in content, mtimes not), openrsync mtime precision,
   `sandbox-exec` confinement of a real compile (writes to the mirror,
   `$HOME` and `/tmp`, `-Wp,-MD,<mirror>`, `-save-temps=cwd` and `-o
   <mirror>` all denied; a malformed profile makes `sandbox-exec` exit 65
   without running anything), and the PCH version error. Still open: CLT
   versions, the `ExternalSSD` volume and source tree on the M6, and the
   one-TU proof itself.
1. **Mirror protocol with complete post-check, write confinement and local
   fallback.**
   `src/distcc.h` (`DCC_CPP_MIRROR`, `DCC_VER_4`); `src/hosts.c`
   (`dcc_parse_options`, both protover/feature mappings); `src/compile.c`
   (`dcc_build_somewhere`: skip the cpp lock, build server argv with `-MT`
   as for pump); `src/remote.c` (`dcc_send_header`/`dcc_compile_remote`:
   version 4 branch, no `DOTI`); `src/clirpc.c` (`dcc_retrieve_results`:
   `MIRR`, `DOTD`, `DSTA`, temp-then-rename); `src/srvrpc.c` (accept 4
   only with `--mirror-root`); `src/serve.c` (`dcc_run_job` mirror branch,
   per-job temp dir, spawning the compiler under the confinement, `MIRR 5`);
   `src/dopt.c` (`--mirror-root`, startup confinement probe); new
   `src/mirror.c` (argv policy, check list, `.d` parsing, stat lists,
   installed-tree digests and their on-disk cache);
   `doc/protocol-4.txt`; localhost cases in `test/testdistcc.py` for the
   acceptance checks below that can run on one machine. The check list is
   complete in this step: input, every PCH operand, every `-include` and
   its implicit PCH candidates, all verified by the client in `DSTA`.
   Write confinement is OS-enforced in this step, and the daemon refuses
   mirror jobs when it cannot establish it. Any `MIRR != 0` or check
   failure means local compile.
2. **Daemon pre-check and classic-path fallback**: early `MIRR 2` from the
   check list, and the classic path on the same host for `MIRR != 0`
   (reuse the pump-mode pattern of switching `host->cpp_where` and
   `protover` mid-job, `src/compile.c:806`). Add `DISTCC_MIRROR_ROOTS`.
   These save time; they do not add correctness, which step 1 already has.
3. **`ohmly.sh` sync phases** (outside this repo) and a re-run of the pool
   benchmark on a real Ohmly rebuild.
4. Later: `CVER` compiler identity, LZO, a distccmon phase for mirrored
   jobs.

`src/lock.c` and `src/where.c` need no change for mirror mode; it simply
never calls `dcc_lock_local_cpp`.

## Acceptance checks for step 1

Step 1 ships only when each of these passes with the host as client and the M6
as mirror. Staleness cannot be produced on localhost, where client and daemon
see the same files, so the stale-PCH and same-size checks need the two
machines; the `DSTA`-omission variant and the confinement checks also run as
localhost cases in `test/testdistcc.py`. "Rejected" means the remote object is
discarded, the job compiles locally, the final `.o` and `.d` are
byte-identical to a plain local compile, and the ccache entry written for the
job holds that local object.

- **Stale PCH.** A header containing `__TIME__`, a PCH built from it and a
  TU using it via `-include-pch`. Sync, wait more than a second, rebuild the
  PCH on the host without touching the header, do not sync. The mirror job
  is rejected on the PCH's path, although clang accepts the old PCH and the
  `.d` and its metadata are unchanged. Variants: the PCH named through
  `-Xclang -include-pch`, `-Xpreprocessor -include-pch` and the driver
  option; an implicit `H.pch` present on the mirror but absent on the host;
  a daemon (test build) that omits a check-list path from `DSTA`. All are
  rejected.
- **Same-size header change.** Change one digit of a constant in a header
  the TU includes directly (not through the PCH), so its size is unchanged,
  more than a second after the sync, and do not sync. The job is rejected
  on that header's path. Repeat for a header under `/opt/homebrew`, edited
  only on the M6 and then given back its old mtime (`touch -r`): rejected
  on the digest. Restore the original content, leaving a different mtime:
  accepted, since installed trees are compared by content.
- **Write outside the job temp dir.** A test compiler (allowed through the
  daemon's compiler whitelist) that tries to create or modify a file in the
  mirror root, in the cwd, in `$HOME` and in `/tmp`, then writes its `.o`
  normally. Every outside write fails with a permission error, none of the
  files exists afterwards, and a listing of the mirror (paths, sizes,
  mtimes) is identical before and after. A real compile with an
  output-writing option that slipped past the argument policy (test build
  with the policy off, e.g. `-Wp,-MD,<mirror path>`) fails remotely and is
  retried locally, and the mirror is unchanged.
- **Confinement unavailable.** With confinement setup forced to fail (test
  hook, or the `sandbox-exec` binary unavailable), `distccd --mirror-root`
  refuses to start; with the failure injected per job, the daemon answers
  `MIRR 5`, spawns no compiler, and the client compiles locally.

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
