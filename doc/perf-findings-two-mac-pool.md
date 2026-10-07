# Performance findings: two-Mac distcc pool over Thunderbolt

Notes from an investigation on 2026-10-06. Nothing in this tree has been
changed yet; this file records what was measured and what is worth changing,
so the work can be picked up later.

## Setup that was measured

| | Host (runs the build) | Helper |
|---|---|---|
| Machine | Mac mini, Apple M5 Pro (Mac17,16) | Mac mini, Apple M6 (Mac18,5), `ssh m6` |
| Cores | 15 (5 Super + 10 Performance) | 12 (2 Super + 4 Performance + 6 Efficiency) |
| Address | 172.31.250.1 (`bridge0`, MTU 1500) | 172.31.250.2 |
| Link | Thunderbolt bridge, ~0.4 ms ping | |

- **Installed distcc:** Homebrew distcc 3.4 on both machines
  (`/opt/homebrew/bin/distcc`, built May 2021). All measurements are of that
  binary, not of this tree. This tree is 153 commits past `v3.4`; the code
  cited below is from this tree, and the first step next time is to build it
  and confirm it behaves the same.
- **Helper daemon:** `~/Library/LaunchAgents/com.ohmly.distccd.plist` runs
  `distccd --daemon --no-detach --listen 172.31.250.2 --allow 172.31.250.1/32
  --jobs 12`, at the default nice 5, `ProcessType` Standard.
- **Client config** (`~/.distcc/hosts` on the host, after this session):

  ```text
  172.31.250.2/12
  localhost/15
  --localslots_cpp=40
  ```

- **Main user:** the Ohmly build (`~/Git/Ohmly/dev-tools/ohmly.sh`). It runs
  ccache with `CCACHE_PREFIX=dev-tools/distcc-clang.sh`, takes the job count
  from `distcc -j` (27), and exports `DISTCC_PAUSE_TIME_MSEC=20`.

## Benchmark

150 compiles of one synthetic C++ file (`heavy.cpp` in the appendix: about 1 s
to compile, 0.1 s to preprocess, 4.2 MB preprocessed), run through a thread
pool of `J` concurrent `distcc clang++ -std=c++20 -O2 -c` processes. The
script is in the appendix. Run-to-run noise was about ±10%, and the helper was
running Photos/Spotlight indexing (about 2.5 cores) during all of it, so
helper numbers are somewhat low.

### Each machine alone

| Config | Jobs/s |
|---|---|
| Host only, plain clang, `-j15` | 9.3 |
| Helper only, preprocessed input, 10 slots | 7.8 |
| Helper only, preprocessed input, 12 slots | 8.6 |
| Helper only, preprocessed input, 14–24 slots | 8.2–8.7 (jobs queue past 12) |
| Helper only, from source (host preprocesses), 12 slots | 7.9 |

Sum of the two alone is about 17–18 jobs/s. That is the ceiling.

### Both machines, default 1 s poll (`DISTCC_PAUSE_TIME_MSEC` unset)

| Hosts | J | Total jobs/s | Helper jobs/s | Jobs that slept |
|---|---|---|---|---|
| helper/12, local/15, `--localslots_cpp=15` | 27 | 10.2–11.6 | 2.6–3.5 | 31–32 of 150, 117–138 sleeps |
| helper/12, local/15, `--localslots_cpp=40` | 27 | 14.7 | 7.0–7.1 | 12 of 150, 12 sleeps |
| helper/10, local/15, `--localslots_cpp=15` | 40 | 9.4 | not recorded | not recorded |
| helper/12, local/15, `--localslots_cpp=40` | 40 | 13.8 | 6.6 | 25 of 150, 128 sleeps |

### Both machines, 20 ms poll (what `ohmly.sh` uses)

| Hosts | J | Total jobs/s | Helper jobs/s |
|---|---|---|---|
| helper/10, local/15, `--localslots_cpp=15` | 25 | 12.3–13.5 | 4.9–5.4 |
| helper/12, local/15, `--localslots_cpp=15` | 27 | 12.5–13.2 | 5.9–6.3 |
| helper/12, local/15, `--localslots_cpp=40` | 27 | 13.5–14.2 | 6.8–6.9 |

A later run of the appendix script, all four combinations back to back:

| `--localslots_cpp` | Poll | Total jobs/s | Helper jobs/s | Jobs that slept |
|---|---|---|---|---|
| 15 | 1 s | 10.9 | 3.3 | 45 of 150, 122 sleeps |
| 15 | 20 ms | 15.3 | 6.6 | 86 of 150, 1014 sleeps |
| 40 | 1 s | 14.3 | 6.9 | 15 of 150, 15 sleeps |
| 40 | 20 ms | 15.0 | 7.2 | 38 of 150, 159 sleeps |

What the numbers support:

- With the 1 s poll, raising `--localslots_cpp` above the local slot count is
  a large, repeatable gain (about 10–11 to 14–15 jobs/s).
- With the 20 ms poll, the two cpp settings are within run-to-run noise of
  each other. The short poll hides the lock collision by busy-waiting through
  it (1014 sleeps against 159).
- The best configuration reaches about 14–15 jobs/s of a 17–18 ceiling, and
  the helper reaches about 7 jobs/s of the 8.6 it manages alone.

### Per-job overhead

| Measurement | Time |
|---|---|
| Trivial C file, local clang | 22 ms |
| Trivial C file, remote via `/opt/homebrew/bin/distcc` | 30–50 ms |
| Trivial C file, remote via `~/.distcc/bin/distcc` wrapper | 85 ms |
| `heavy.cpp` single job, local | about 900 ms |
| `heavy.cpp` single job, remote (includes 0.1 s local cpp) | about 880 ms |
| `heavy.cpp` single job, local under `nice -n 5` | no change |
| `heavy.cpp` single job, local under `taskpolicy -b` | about 3600 ms |

## Findings

### 1. cpp slots and local compile slots share lock files

- `dcc_make_lock_filename` (`src/lock.c:139`) names every local lock
  `<lockdir>/cpu_localhost_<n>`, whether it comes from a `localhost/N` host
  entry, `--localslots` (`dcc_hostdef_local`), or `--localslots_cpp`
  (`dcc_hostdef_local_cpp`). The comment at `src/lock.c:74` says this is
  deliberate, so that both limits are respected.
- A remote job takes its remote slot first (`src/compile.c:762`), then calls
  `dcc_lock_local_cpp` (`src/compile.c:777`) to get a slot for preprocessing.
- When `--localslots_cpp` is not larger than the `localhost/N` count and all N
  local slots are busy compiling, that second lock fails. The job then sleeps
  in `dcc_lock_pause` while still holding the remote slot, so the helper slot
  is idle for the whole wait.
- Measured effect with the 1 s poll: the helper got 39–45 of 150 jobs instead
  of 72, and total throughput was 10.2–11.6 instead of 14.7 jobs/s.
- Workaround in use: `--localslots_cpp=40`. cpp takes the lowest free index,
  so it uses indices 15 and up when the local compile slots are full. The cost
  is that local load is no longer capped at 15: up to 12 preprocessors can run
  on top of 15 compiles.

### 2. Waiting for a slot is a fixed-interval poll

- `dcc_lock_pause` (`src/where.c:126`) sleeps `DISTCC_PAUSE_TIME_MSEC`,
  default 1000 ms, and `dcc_lock_one` (`src/where.c:167`) then retries every
  slot with non-blocking locks.
- With the default, any build that runs more jobs than there are slots leaves
  freed slots idle for up to a second: `-j40` on a 25-slot pool gave 9.4
  jobs/s against 12.1 at `-j25`. With 10 ms it recovered to 11.5.
- A 10 ms poll at `-j40` produced 8195 sleeps across 150 jobs. That was cheap
  here, but it is busy-waiting.
- `sys_lock` (`src/lock.c:181`) already supports blocking (`F_SETLKW`); the
  slot search just never uses it.

### 3. The remote slot is held during local preprocessing

- Because the remote slot is locked before preprocessing starts, every helper
  slot is idle for the length of each job's local cpp run, even when nothing
  has to wait.
- Likely part of why the helper reaches about 7 jobs/s in the pool against 8.6
  alone on preprocessed input. This was not isolated from host CPU contention
  (15 compiles plus up to 12 preprocessors on 15 cores), so treat it as a
  hypothesis.

### 4. Wrong default target triple on Apple Silicon (3.4)

- Homebrew 3.4 has `arm-apple-darwin27.0.0` baked in, so a bare `clang`
  command gets `-target arm-apple-darwin…` appended (`dcc_add_clang_target`,
  `src/compile.c:549`) and the helper returns 32-bit ARM objects.
- Both wrapper scripts (`~/.distcc/bin/distcc` and Ohmly's
  `dev-tools/distcc-clang.sh`) work around this by running
  `/usr/bin/clang -dumpmachine` on every compile. The generic wrapper costs
  about 35 ms per invocation (85 ms against 51 ms on a trivial file).
- `DISTCC_NO_REWRITE_CROSS=1` (`src/compile.c:727`) skips the rewrite. A bare
  `clang` compile sent to the helper that way produced an arm64 object
  byte-identical to the local one.
- This tree's `configure.ac:484` takes the triple from `$CC -dumpmachine`
  instead of `config.guess`, which should fix it at build time. Not verified:
  the tree was not built. `./config.guess` here still prints
  `arm-apple-darwin27.0.0`.

### 5. Things that are not the bottleneck

- **Network.** About 10–25 ms of fixed overhead per remote job, and a 4.2 MB
  preprocessed file transfers with no measurable cost. `tcp_cork_sock`
  (`src/io.c:261`) is a no-op on macOS because `TCP_CORK` is Linux-only, and
  the protocol writes many 12-byte tokens separately, but no delayed-ACK
  stalls showed up. LZO, jumbo frames and `TCP_NOPUSH` are not worth pursuing
  for this link.
- **Daemon priority.** `distccd` defaults to nice 5 (`src/dopt.c:51`). Nice 5
  made no difference locally, and the helper's single-job time beats the
  host's, so it is not confined to efficiency cores.
- **Pump mode.** Preprocessing costs about 0.1 s of host CPU per 1 s job. With
  one helper that is under one host core.
- **Helper job limit.** `--jobs 12` equals its core count, and throughput is
  flat from 12 to 24 slots. Half its cores are efficiency cores, which is why
  per-job time roughly doubles between 1 and 12 concurrent jobs.

## Status of the code changes (2026-10-06, later the same day)

Implemented on the `mac-pool-perf` branch:

- Preprocessor slots lock `cpp_localhost_<n>` (`src/lock.c`), so
  `--localslots_cpp` no longer needs to be oversized. Finding 1.
- `dcc_lock_one()` blocks in the kernel on one busy slot (F_SETLKW with a
  SIGALRM timeout) instead of sleeping; `DISTCC_PAUSE_TIME_MSEC` is now the
  rescan bound and defaults to 100 ms (`src/where.c`). `dcc_lock_host()` maps
  EINTR to `EXIT_BUSY`. Finding 2.
- `configure --disable-pump-mode` skips the include server and `pump`;
  without a setuptools-capable Python, pump mode is left out with a warning
  (error only with an explicit `--enable-pump-mode`). macOS prerequisites
  are in INSTALL. Finding 4 is confirmed fixed by this tree's configure: a
  bare `distcc clang` to the helper returns objects identical to local ones.
- Finding 3 and the mirrored-tree idea are designed, not implemented:
  see `doc/mirrored-tree-design.md`.

Merged-tree benchmark (appendix script, same day, M6 idle):

| Client | Env | `--localslots_cpp` | J | Total jobs/s | Helper jobs/s |
|---|---|---|---|---|---|
| Homebrew 3.4 | `PAUSE=20` | 15 | 27 | 14.9 | 7.2 |
| Homebrew 3.4 | `PAUSE=20` | 40 | 27 | 15.5 | 7.4 |
| Homebrew 3.4 | `PAUSE=20` | 15 | 40 | 13.2 | 5.3 |
| Homebrew 3.4 | `PAUSE=20` | 40 | 40 | 14.2 | 7.1 |
| This branch | none | 15 | 27 | 14.5 | 7.0 |
| This branch | none | 40 | 27 | 14.9 | 7.1 |
| This branch | none | 15 | 40 | 14.6 | 7.0 |
| This branch | none | 40 | 40 | 14.7 | 7.1 |

With no environment tuning and the stock `--localslots_cpp`, the branch
holds the helper at about 7 jobs/s in every configuration, where the 3.4
client needed both `DISTCC_PAUSE_TIME_MSEC=20` and `--localslots_cpp=40`
to get there and still dropped to 5.3 when oversubscribed with the stock
cpp count. `make distcc-maintainer-check` passes on the merged tree.

## Proposed code changes

In rough order of value, as written before the work above was done.

1. **Stop a remote job from holding its remote slot while it waits for a cpp
   slot.** Options to evaluate:
   - Give cpp its own lock name (`cpp_localhost_<n>`), so it never collides
     with local compile slots. Simple, but drops the combined local cap that
     `src/lock.c:74` describes.
   - Keep the shared files, but take the cpp lock non-blocking; on failure
     release the remote slot and restart host selection.
   - Preprocess before choosing a host (also addresses finding 3). This is the
     largest change: `dcc_build_somewhere` (`src/compile.c`) starts cpp after
     host selection and overlaps it with the connect.
2. **Replace the poll in `dcc_lock_pause` with a blocking wait.** For example,
   block with `F_SETLKW` on one slot chosen round-robin or at random, with a
   timeout so a job can still move to a slot that frees elsewhere. A cheaper
   interim step is a much lower default than 1000 ms.
3. **Build and install this tree on both Macs**, confirm the triple is right
   without a wrapper, and re-run the benchmark to see whether anything since
   3.4 changes the numbers above.
4. **Low priority:** `TCP_NOPUSH`/`TCP_NODELAY` plus coalescing the token
   writes on macOS. Expected gain is a few milliseconds per job.

## Open questions

- With `--localslots_cpp=40`, the 1 s poll and `J` equal to the slot count,
  12–15 jobs still slept once in every run. Not investigated. Probably the
  initial burst of 27 simultaneous starts, but that is a guess.
- One trivial remote compile took 7.1 s. It did not recur in 80 further runs
  and was never explained.
- The benchmark is one synthetic file. Ohmly's translation units are much
  longer and go through ccache and a PCH-rewriting wrapper, and no real Ohmly
  build was timed before and after the hosts change.
- Whether `distccd --jobs 14` on the helper (the daemon's own default would be
  cores + 2, `src/dparent.c:121`) gains anything was not tested; client-side
  slot counts above 12 only queued.

## Appendix: reproducing the benchmark

`heavy.cpp`:

```cpp
#include <algorithm>
#include <map>
#include <regex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>
#include <iostream>
#include <functional>
#include <memory>
#include <tuple>
#include <variant>
template<int N> struct Fib { static constexpr unsigned long v = Fib<N-1>::v + Fib<N-2>::v; };
template<> struct Fib<0> { static constexpr unsigned long v = 0; };
template<> struct Fib<1> { static constexpr unsigned long v = 1; };
#define R(n) std::string r##n(const std::string& s){ std::regex re("(a|b)*c" #n "[0-9]+(x|y|z){2,5}"); std::smatch m; std::map<std::string,std::vector<std::variant<int,std::string,double>>> mm; mm[s].push_back(n); std::ostringstream o; o << std::regex_search(s,m,re) << Fib<80>::v << mm.size(); std::vector<std::tuple<int,std::string>> v; v.emplace_back(n,s); std::sort(v.begin(),v.end()); return o.str(); }
R(1) R(2) R(3) R(4) R(5) R(6) R(7) R(8) R(9) R(10) R(11) R(12) R(13) R(14) R(15) R(16) R(17) R(18) R(19) R(20)
R(21) R(22) R(23) R(24) R(25) R(26) R(27) R(28) R(29) R(30)
```

`combo.py` (run with `/usr/bin/python3` from the directory holding
`heavy.cpp`; it writes `out<N>.o` files there):

```python
import subprocess, time, os, re
from concurrent.futures import ThreadPoolExecutor

DISTCC = "/opt/homebrew/bin/distcc"
HELPER = "172.31.250.2"

def run(env, i):
    cmd = [DISTCC, "clang++", "-std=c++20", "-O2", "-c", "heavy.cpp", "-o", f"out{i}.o"]
    start = time.perf_counter()
    r = subprocess.run(cmd, capture_output=True, env=env)
    elapsed = time.perf_counter() - start
    log = r.stderr.decode(errors="replace")
    host = "remote" if re.search(r"compiled on 172\.31\.250\.2", log) else "local"
    return elapsed, host, r.returncode, log

def bench(label, jobs, total, hosts, **extra):
    # DISTCC_VERBOSE is what lets us tell hosts apart and count poll sleeps.
    env = dict(os.environ, DISTCC_NO_REWRITE_CROSS="1", DISTCC_HOSTS=hosts,
               DISTCC_VERBOSE="1", **extra)
    start = time.perf_counter()
    with ThreadPoolExecutor(jobs) as pool:
        results = list(pool.map(lambda i: run(env, i), range(total)))
    wall = time.perf_counter() - start
    line = f"{label:44s} J={jobs:2d} {total / wall:5.1f} jobs/s |"
    for host in ("remote", "local"):
        times = sorted(r[0] for r in results if r[1] == host)
        if times:
            line += (f" {host}: {len(times):3d} jobs ({len(times) / wall:4.1f}/s)"
                     f" med {times[len(times) // 2] * 1000:4.0f}ms |")
    slept = sum(1 for r in results if "nothing available, sleeping" in r[3])
    sleeps = sum(r[3].count("nothing available, sleeping") for r in results)
    failed = sum(1 for r in results if r[2])
    print(line + f" slept {slept}/{total} ({sleeps} sleeps)"
          + (f" FAILS={failed}" if failed else ""), flush=True)

if __name__ == "__main__":
    for cpp in (15, 40):
        hosts = f"{HELPER}/12 localhost/15 --localslots_cpp={cpp}"
        bench(f"cpp={cpp}, 1 s poll", 27, 150, hosts)
        bench(f"cpp={cpp}, 20 ms poll", 27, 150, hosts, DISTCC_PAUSE_TIME_MSEC="20")
```

This is a tidied copy of the script that produced the earlier tables; it was
run once as written and produced the four-row table above. `DISTCC_HOSTS`
overrides `~/.distcc/hosts`, so the benchmark does not depend on the file's
current contents.
