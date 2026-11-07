# ratelimit-cpp

[![CI](https://github.com/BrianKYildirim/ratelimit-cpp/actions/workflows/ci.yml/badge.svg)](https://github.com/BrianKYildirim/ratelimit-cpp/actions/workflows/ci.yml)

A thread-safe, in-process rate limiter for C++20, with C and Java (JNI) bindings.

**In plain terms:** a rate limiter decides whether a request may go ahead right now, for example "each user may make 10 requests, refilling at 5 per second". Servers use one to protect themselves from floods of traffic, to stop login brute-forcing, or to share capacity fairly between customers. This library makes that decision inside your program, for many different keys (users, IP addresses, API tokens; the benchmarks go up to a million), from many threads at once.

**In technical terms:** a sharded, open-addressing hash table of token buckets with lock-free lookups, a per-entry version lock that doubles as a seqlock, epoch-based memory reclamation, and exact integer refill arithmetic. It also has a read-only inspection API that reports a bucket's real state without affecting any decision.

---

## Contents

- [What it does](#what-it-does)
- [Quick start](#quick-start)
- [How a token bucket works](#how-a-token-bucket-works)
- [API](#api)
- [How it works inside](#how-it-works-inside)
- [Design choices and why](#design-choices-and-why)
- [Testing](#testing)
- [Performance: what I measured and how](#performance-what-i-measured-and-how)
- [Challenges and what I learned](#challenges-and-what-i-learned)
- [Limitations](#limitations)
- [Building, installing and using it from CMake](#building-installing-and-using-it-from-cmake)
- [Project layout](#project-layout)

---

## What it does

- **Per-key token buckets.** Each key gets its own bucket with a capacity and a refill rate, both passed in on every call.
- **Two decision calls.** `allow()` returns the verdict, the tokens left and the time until the next token. `try_acquire()` returns only the verdict and skips the extra arithmetic.
- **Caller-supplied time.** Every call has an overload that takes a timestamp, so a batch of requests can share one clock read, and tests can run on a fake clock.
- **Read-only inspection.** `inspect()` shows a bucket's stored state and what a call would see at a given instant. It takes no token and changes nothing.
- **Idle-key eviction.** `evict_idle()` forgets keys that haven't been used for a while, so memory stays bounded.
- **C API and JNI bridge.** C callers use `ratelimit_c.h`; a small JNI library exposes `allow()` to Java.
- **Packaging.** CMake install rules and `find_package(Ratelimit)` support, tested against the sample project in `consumer/`, plus pkg-config files for the C and C++ libraries.

It is an **in-process** limiter: state lives in one process's memory. It does not coordinate limits across processes or machines; for that you need a shared store such as Redis.

## Quick start

Build and run the tests (Linux or macOS, CMake 3.16+, a C++20 compiler):

```sh
git clone https://github.com/BrianKYildirim/ratelimit-cpp.git
cd ratelimit-cpp
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DRL_BUILD_JNI=OFF
cmake --build build
ctest --test-dir build --output-on-failure
```

JNI is on by default and needs a JDK; `-DRL_BUILD_JNI=OFF` skips it. GoogleTest and Google Benchmark are downloaded by CMake.

Try the command-line example. It reads one key per line and applies a limit of 10 tokens refilling at 5 per second:

```sh
printf "user-1\nuser-1\nuser-1\n" | ./build/rl_cli
```

Use it from C++:

```cpp
#include <ratelimit/rate_limiter.h>
#include <iostream>

int main() {
    rl::RateLimiterInProc limiter;
    const rl::Limit limit{.capacity = 10, .refill_per_sec = 5};

    rl::Decision d = limiter.allow("user:123", limit);
    if (d.allowed) {
        std::cout << "allowed, " << d.remaining << " tokens left\n";
    } else {
        std::cout << "denied, retry in " << d.reset_ms << " ms\n";
    }
}
```

## How a token bucket works

**The idea.** Picture a bucket that holds up to `capacity` tokens and is refilled at `refill_per_sec` tokens per second. Each request takes one token. If the bucket is empty, the request is denied until enough time passes for a token to drip back in. A full bucket allows a short burst; after that, requests are admitted at the refill rate.

**The arithmetic.** Balances are stored in *nanotokens* (one token = 10^9 nanotokens) together with the time of the last update. A call at time `now`:

1. Clamps the stored balance to the capacity (the capacity may have been lowered since the last call).
2. Adds `(now - last) * refill_per_sec` nanotokens, saturating at the capacity. This is a single 64-bit multiply with an overflow check. Because one token per second is exactly one nanotoken per nanosecond, there is no division and no rounding error that builds up over many calls.
3. If at least one whole token is available, takes it and allows the call; otherwise denies it.

A time earlier than the stored one adds nothing, so a clock that steps backwards can't create tokens. New keys start with a full bucket. Capacities above 18,446,744,073 tokens (2^64 / 10^9) are clamped to that value.

## API

### C++

```cpp
rl::RateLimiterInProc limiter;                      // default configuration and clock
rl::RateLimiterInProc limiter2({.shards = 64, .capacity_hint_per_shard = 4096});

rl::Decision d   = limiter.allow(key, limit);       // full decision
bool ok          = limiter.try_acquire(key, limit); // same decision, verdict only

uint64_t now     = limiter.now_ns();                // the limiter's clock
rl::Decision d2  = limiter.allow(key, limit, now);  // at a caller-supplied time
bool ok2         = limiter.try_acquire(key, limit, now);

rl::BucketState s = limiter.inspect(key, limit, now); // read-only, see below

size_t keys      = limiter.size();
size_t evicted   = limiter.evict_idle(10ull * 60 * 1'000'000'000); // idle for 10 minutes
limiter.clear();
```

- **Keys** are `std::string_view`: any bytes, including empty keys and embedded NULs.
- **Time.** Timestamps passed in must come from the limiter's own `now_ns()`, since refill is measured against times stored by earlier calls.
- **Clock.** The constructor takes an optional `const rl::Clock*`, which must outlive the limiter; tests use this to inject a fake clock.
- **Configuration.** `shards` is rounded up to a power of two between 1 and 65536 (default 128). A shard reserves room for `capacity_hint_per_shard` keys (default 1024) the first time a key lands in it, so an unused limiter allocates very little.

`Decision` fields:

| Field       | Meaning                                                                                                   |
| ----------- | --------------------------------------------------------------------------------------------------------- |
| `allowed`   | Whether the call was admitted                                                                             |
| `remaining` | Whole tokens left after the call                                                                          |
| `reset_ms`  | When denied: milliseconds until the next token, rounded up. `0` if a token can never arrive (rate or capacity 0) |

**Eviction** is invisible to callers when `idle_ns` is at least the time an empty bucket needs to refill (`capacity / refill_per_sec` seconds). By then the key's bucket is full again, which is exactly how a new key starts.

### Inspecting a bucket

`inspect()` answers "what does this key's bucket look like right now?" without taking a token, inserting the key, or writing to the key's entry, so no later decision can tell it happened. I added it so that a live visualization can show buckets filling and draining from the library's real state, rather than estimating it outside.

```cpp
const uint64_t now = limiter.now_ns();
rl::BucketState s = limiter.inspect("user:123", limit, now);
rl::Decision d = limiter.allow("user:123", limit, now);   // same instant
// With no other call for this key in between:
//   d.allowed == s.would_allow, and d.remaining == s.tokens - s.would_allow
```

| Field               | Kind               | Meaning                                                                                                   |
| ------------------- | ------------------ | --------------------------------------------------------------------------------------------------------- |
| `tracked`           | Stored             | Whether the limiter holds a bucket for the key. Inspecting never inserts one.                            |
| `stored_nanotokens` | Stored             | The balance the key's last recorded call left. `0` if not tracked.                                       |
| `stored_at_ns`      | Stored             | The time of that balance. `0` if not tracked.                                                            |
| `nanotokens`        | Derived, exact     | Available at `now`: clamped, refilled, saturated. For an untracked key, the full bucket a new key gets.  |
| `tokens`            | Derived, exact     | Whole tokens available *before* a call (`Decision::remaining` counts after it).                          |
| `until_token_ns`    | Derived, exact     | Nanoseconds until a call would be allowed, rounded up; `0` if now; `BucketState::kNever` if never.       |
| `until_full_ns`     | Derived, exact     | Nanoseconds until the bucket is full, rounded up; `0` if full; `kNever` if the rate is 0.                |
| `would_allow`       | Prediction         | Whether a call at `now` would be allowed. Exact, but not a reservation: another thread may get there first. |

The derived values come from the same function `allow()` uses to decide, run on a copy of the stored state, so they can't drift from real decisions. For a denied call, `reset_ms` equals `until_token_ns` divided by 10^6 and rounded up. The two differ only for a token that never arrives, which `reset_ms` reports as 0 and `inspect()` as `kNever`.

A reading of one key is a state the key really had, never a half-written one. Readings of different keys are separate snapshots, not one atomic view. Denials within <!-- claim:rl.design.quiet_denial_ms -->1 ms of a key's stored time aren't written back (see below), so `stored_at_ns` can lag the latest call by up to <!-- claim:rl.design.quiet_denial_ms -->1 ms while the derived values stay exact.

### C

```c
#include <ratelimit/ratelimit_c.h>

void* h = rl_new_inproc(128, 1024);        /* 0 selects the defaults */
rl_limit_t limit = {10, 5};

rl_decision_t d = rl_allow(h, "user:123", limit);
int ok = rl_try_acquire(h, "user:123", limit);

uint64_t now = rl_now_ns(h);               /* pair a reading with a decision */
rl_bucket_state_t s = rl_inspect_at(h, "user:123", limit, now);
rl_decision_t d2 = rl_allow_at(h, "user:123", limit, now);

uint64_t evicted = rl_evict_idle(h, 600ull * 1000000000ull);
rl_free(h);
```

C keys are NUL-terminated strings, so a key ends at its first NUL byte in every C function alike. Keys that contain NUL bytes are only reachable through the C++ API. Functions return zeroed results for a `NULL` handle or key.

### Java (JNI)

`jni/NativeRateLimiter.java` declares `NativeRateLimiter.allow(String key, int capacity, int refillPerSec)`, backed by one process-wide limiter in `libratelimit_jni`. CMake builds the native library only; compile the Java class yourself and put the library on `java.library.path`. No Java test is included.

## How it works inside

```text
allow("user:123", limit)
  │
  ├─ hash the key (wyhash)            bits 32+ pick a shard, low bits a slot
  ├─ prefetch the key's slot, then read the clock while that memory arrives
  ├─ load the shard's table pointer   one atomic word, no lock
  ├─ probe for the key                linear probing, at most 3/4 full
  │     ├─ found:     lock that one slot, refill + consume, unlock
  │     └─ not found: take the shard's mutex and insert (growing the table if needed)
  └─ return the decision
```

**Storage.** Each key lives in one <!-- claim:rl.design.slot_bytes -->64-byte slot, which is one cache line: its hash, a version word, the balance, the last-update time, and the key itself if it fits in <!-- claim:rl.design.inline_key_bytes -->28 bytes (longer keys get one exact-size heap allocation). One slot per cache line means a call normally touches only its own key's line, and two keys never share a line that cores would fight over.

**Concurrency.**
- *Lookups take no lock.* A shard publishes its table as a single word, so a reader always sees either the old table or the new one, both complete.
- *Each decision locks only its own key.* The slot's version word is the lock: even means free, odd means held. Calls for different keys never wait for each other.
- *Inserts and structural changes take the shard's mutex.* A new key usually fills an empty slot in place. Growth and eviction build a new table, seal the old slots so in-flight callers retry under the mutex, and publish the new table. `clear()` seals the slots and publishes an empty table.
- *Old tables are freed safely with epoch-based reclamation.* A reader announces a global epoch while it is inside a table; a replaced table is freed only once every announced epoch has moved past the point where it was retired.

**Over-limit keys.** When one key is flooded, most calls are denials that change nothing, because refill is linear and exact. A denial within <!-- claim:rl.design.quiet_denial_ms -->1 ms of the key's stored time therefore skips the write and is decided from a lock-free snapshot, which uses the version word as a seqlock. The key's cache line can then stay shared by every core instead of bouncing between them. Each thread remembers which slots it recently saw denied this way, and reads first only for those. For an ordinary call, reading before locking would cost an extra cache round trip: the line arrives shared, then the lock has to upgrade it. That's why the snapshot path isn't used for every call.

**Clock.** The default `FastClock` reads the CPU's timestamp counter, re-anchored to `std::chrono::steady_clock` about every half second, and falls back to `steady_clock` where the counter isn't usable. It exists because `clock_gettime` is a system call under WSL1 and some hypervisors. On my laptop under WSL2, where it isn't, a `FastClock` read took <!-- claim:rl.clock.fast.ubuntu-wsl2 -->9.4 ns and a `steady_clock` read took <!-- claim:rl.clock.steady.ubuntu-wsl2 -->15.2 ns (time per read in a Google Benchmark loop, amortized; median of 10 repetitions on Ubuntu under WSL2). MSVC builds don't use the counter, so on native Windows `FastClock` is `steady_clock` (`fastclock_uses_tsc` is false in that run's `meta.json`): a `FastClock` read took <!-- claim:rl.clock.fast.windows-native -->15.8 ns and a `steady_clock` read <!-- claim:rl.clock.steady.windows-native -->15.3 ns, <!-- claim:rl.cmp.clock.windows-native -->no measurable difference (medians of 10 runs each).

## Design choices and why

| Choice | Alternative | Why |
| --- | --- | --- |
| Nanotoken integers | Floating point; Q32.32 fixed point (an earlier version) | Exact refill with one multiply and no drift. Q32.32 overflowed for large capacities; a test (`Core.LargeCapacity`) guards that case. |
| One <!-- claim:rl.design.slot_bytes -->64-byte slot per key, inline keys up to <!-- claim:rl.design.inline_key_bytes -->28 bytes | Separate chaining with pointers; packing several keys per line | A lookup normally touches only its own key's cache line, short keys need no allocation, and neighboring keys never share a line. |
| Lock-free lookup + per-key lock | One lock per shard for everything | Different keys never contend; the shard mutex is only for inserts and rebuilds, which are rare once keys exist. |
| Epoch-based reclamation | Reference counting; never freeing old tables | Readers pay one atomic announcement and no shared counter traffic; memory is still returned. |
| Quiet denials from snapshots | Always lock and write | A flooded key's cache line stays shared across cores, so denials scale instead of serializing. Because refill is linear and exact, skipping the write changes no later verdict; the differential tests check this. |
| `try_acquire()` alongside `allow()` | Only `allow()` | Many callers need only the verdict, and computing `remaining` and `reset_ms` takes extra arithmetic, including a division. |
| `inspect()` reuses the decision function | A separate "peek" formula | One implementation means a reading can't disagree with the next decision. Tests check this against an independent model. |
| Caller-supplied timestamps | Always read the clock | Deterministic tests, batching, and pairing an inspection with a decision at the same instant. |

## Testing

`ctest` runs <!-- claim:rl.tests.ubuntu-wsl2 -->54 tests: 53 GoogleTest cases and a C program that links the shared C library, which checks the C symbols are actually exported.

- **Exact behavior on a fake clock:** bursts, refill, `reset_ms` rounding, fractional rates (a token every <!-- claim-waiver: worked arithmetic for a rate of 3 tokens per second (10^9 / 3 ns), a test input, not a measurement -->333,333,333.3 ns), zero and huge rates and capacities, overflow, clocks going backwards, lowered capacities, empty keys, embedded NULs, keys around the <!-- claim:rl.design.inline_key_bytes -->28-byte inline limit, growth, eviction and `clear()`.
- **Differential tests against a model:** a separate, simple implementation (a `std::map`, with overflow handled by exact division instead of the engine's checks) receives the same random operations as the real limiter, 20,000 per seed across several seeds and table sizes. Every decision, size and eviction count must match. A second differential test sends the same operations to two limiters, one of which is also inspected at random times. Every decision must be identical, and every inspection must match the model and predict the decision made at its instant.
- **Concurrency:** exact grant counts on a contended key; no lost or duplicated refill under load; inserts that grow tables, eviction and `clear()` running alongside traffic; and inspections racing with all of these, checking that only committed states are ever seen.
- **Sanitizers:** the suite runs under AddressSanitizer with UndefinedBehaviorSanitizer (`-DRL_ENABLE_ASAN=ON`) and ThreadSanitizer (`-DRL_ENABLE_TSAN=ON`). CI runs both, plus normal builds on Ubuntu and macOS.

To check that the inspection tests can actually fail, I deliberately broke the implementation three ways: reading without the seqlock check, treating a moved slot as missing, and an off-by-one in the wait arithmetic. Each break was caught by the tests.

## Performance: what I measured and how

There are two benchmark programs:
- `rl_bench` (Google Benchmark) covers one hot key, 100,000 shared keys, 10,000 keys per thread, new-key inserts, inspection, and memory per key, at 1 to 16 threads.
- `rl_latency` prints latency percentiles and wall-clock throughput.

The results below come from one command, which builds the library, runs `ctest`, runs both programs, and writes the raw output, a `meta.json` (hardware, OS, compiler and flags, load, power) and a `summary.json` to `bench/results/<date>-<commit>/<environment>/`:

```sh
python bench/run_bench.py --env ubuntu-wsl2      # or --env windows-native
```

For a quick look without the runner:

```sh
./build/rl_bench --benchmark_filter=Hot --benchmark_repetitions=5
./build/rl_latency 0.5 8        # seconds per phase, max threads
```

For multi-threaded runs, read the `wall_ops/s` counter: all threads' calls divided by wall time. Google Benchmark's built-in rate divides by average per-thread time, which overstates throughput when threads finish at different times.

<!-- bench-methodology:begin -->
**How these numbers were measured.** One session on 2026-11-06, at commit `e3b8ab7`, results in `bench/results/2026-11-06-e3b8ab7/`. Both environments ran on the same 4-core, 8-thread laptop (Intel i7-1185G7), one after the other: Ubuntu 26.04 under WSL2 with GCC 15.2 (`-O3`), and native Windows 11 with MSVC 19.43 (`/O2`), both Release builds.

- **Warm-up:** Google Benchmark runs each benchmark for <!-- claim-waiver: harness setting passed to rl_bench (--benchmark_min_warmup_time), not a measurement -->0.2 s before timing it. In `rl_latency`, the fixture inserts and the throughput phase run before the latency phase; there is no separate warm-up.
- **Timer:** `rl_bench` times with `std::chrono::steady_clock` (Google Benchmark real time) and reports time per iteration, which is an average over many calls, not a per-call latency. `rl_latency` times each call with the CPU's timestamp counter (`rdtsc`/`rdtscp` fenced with `lfence`), calibrated at <!-- claim:rl.timer.ticks_per_ns.ubuntu-wsl2 -->2.995 ticks/ns on Ubuntu and <!-- claim:rl.timer.ticks_per_ns.windows-native -->2.995 ticks/ns on Windows. The median cost of an empty timed region, <!-- claim:rl.timer.overhead_ns.ubuntu-wsl2 -->38.7 ns on Ubuntu and <!-- claim:rl.timer.overhead_ns.windows-native -->41.4 ns on Windows, is subtracted from every sample. Samples at or above 8192 ticks are counted in power-of-two buckets, so a percentile that lands there is the bucket's lower bound (shown with ≥). The resolution of `steady_clock` on Windows isn't recorded in `meta.json`; it matters little for `rl_bench` because each reported time is averaged over many iterations.
- **Repetitions:** `rl_bench` runs every benchmark 10 times, randomly interleaved (`--benchmark_enable_random_interleaving`), so a slow period is spread across benchmarks instead of hitting one. `rl_latency` runs 7 times, <!-- claim-waiver: workload parameter of rl_latency (seconds per phase), not a measurement -->0.5 s per phase, at up to 8 threads. Tables show the median, with the lowest and highest run in parentheses.
- **Outliers:** no samples are dropped. A whole run is marked invalid, kept in `raw/` and left out of the statistics if the laptop is on battery, if the CPU stays busy after waiting, or if a step fails. No run in this session was invalid.
- **Noise control:** AC power, Windows power mode Balanced, other applications closed, one environment at a time. Before each measured step the runner waits until CPU use is below <!-- claim-waiver: threshold setting of the runner's quiet gate (--max-busy-pct), not a measurement -->10%, read from `GetSystemTimes` on Windows and from the WSL VM's `/proc/stat` on Ubuntu (which can't see activity on the Windows host).
<!-- bench-methodology:end -->

**Throughput.** Calls per second (`wall_ops/s`), the median of 10 interleaved repetitions with the lowest and highest in parentheses. Treat them as data points for this machine, not a guarantee.

Ubuntu under WSL2:

<!-- claims-table:begin id=rl.example_run.ubuntu-wsl2 -->
| Scenario | 1 thread | 8 threads |
|---|---:|---:|
| One hot key, every call allowed (`Hot_Allowed`) | 37.9M (35.5–40.3M) | 9.9M (7.0–13.0M) |
| One hot key, almost every call denied (`Hot_Denied`) | 45.5M (41.9–47.2M) | 153.3M (116.7–178.2M) |
| 100,000 keys shared by all threads (`Uniform_Allowed`) | 14.3M (13.3–14.8M) | 55.1M (47.7–64.7M) |
| 10,000 keys per thread (`Disjoint_Allowed`) | 28.3M (23.8–29.2M) | 90.3M (71.3–95.9M) |
| `inspect()` on one hot key (`Inspect_Hot`) | 49.6M (47.3–50.2M) | 161.1M (123.3–191.7M) |
<!-- claims-table:end -->

Native Windows:

<!-- claims-table:begin id=rl.example_run.windows-native -->
| Scenario | 1 thread | 8 threads |
|---|---:|---:|
| One hot key, every call allowed (`Hot_Allowed`) | 17.0M (16.4–17.7M) | 5.7M (4.5–6.6M) |
| One hot key, almost every call denied (`Hot_Denied`) | 15.8M (14.9–16.0M) | 50.9M (35.4–67.7M) |
| 100,000 keys shared by all threads (`Uniform_Allowed`) | 9.1M (8.5–9.4M) | 43.5M (29.2–48.4M) |
| 10,000 keys per thread (`Disjoint_Allowed`) | 14.8M (14.3–15.6M) | 48.3M (34.7–55.3M) |
| `inspect()` on one hot key (`Inspect_Hot`) | 14.6M (14.0–15.3M) | 45.4M (37.0–67.2M) |
<!-- claims-table:end -->

**Per-call latency.** `rl_latency` times every call; p50 and p99 are over all threads' calls in one run, and the table shows the median of 7 runs with the lowest and highest in parentheses.

Ubuntu under WSL2:

<!-- claims-table:begin id=rl.latency.ubuntu-wsl2 -->
| Scenario | Threads | p50 | p99 |
|---|---:|---:|---:|
| `hot_allowed` | 1 | 8.7 ns (6.0–34.1 ns) | 30.7 ns (25.4–100 ns) |
| `hot_allowed` | 8 | 204 ns (196–228 ns) | ≥ 10,940 ns |
| `hot_denied` | 1 | 3.3 ns (0.7–28.7 ns) | 19.4 ns (4.7–38.7 ns) |
| `hot_denied` | 8 | 38.7 ns (34.1–64.1 ns) | 61.4 ns (54.1–88.8 ns) |
<!-- claims-table:end -->

Native Windows:

<!-- claims-table:begin id=rl.latency.windows-native -->
| Scenario | Threads | p50 | p99 |
|---|---:|---:|---:|
| `hot_allowed` | 1 | 28.7 ns (28.0–42.1 ns) | 50.7 ns (40.1–148 ns) |
| `hot_allowed` | 8 | 395 ns (359–512 ns) | ≥ 10,940 ns |
| `hot_denied` | 1 | 34.7 ns (34.1–48.7 ns) | 66.8 ns (48.1–147 ns) |
| `hot_denied` | 8 | 113 ns (68.8–120 ns) | 156 ns (91.5–169 ns) |
<!-- claims-table:end -->

**Windows and Ubuntu.** I state a difference only where the two environments' lowest-to-highest ranges don't overlap. On one hot key at 8 threads, denials were <!-- claim:rl.cmp.hot_denied.t8 -->faster on Ubuntu on WSL2 by ~3.0×, medians of 10 runs each. With 10,000 keys per thread at 1 thread, calls were <!-- claim:rl.cmp.disjoint_allowed.t1 -->faster on Ubuntu on WSL2 by ~1.9×, medians of 10 runs each. For the p50 latency of an allowed call on one hot key at 1 thread, the ranges overlap: <!-- claim:rl.cmp.lat.hot_allowed.t1.p50 -->no measurable difference (medians of 7 runs each). Why Ubuntu is faster is a hypothesis I haven't verified: MSVC `/O2` versus GCC `-O3` code generation, and `FastClock` falling back to `steady_clock` under MSVC. The clock part can't be the whole story: reading the clock is a median <!-- claim:rl.clock_share.windows-native -->0.13 of `try_acquire()` time on Windows and <!-- claim:rl.clock_share.ubuntu-wsl2 -->0.16 on Ubuntu (`clock_share` in `summary.json`), too small a share of a call to explain the gap by itself.

Other findings:

- **Memory:** heap bytes per key, measured by the `MemoryPerKey` benchmark, which counts every allocation through a heap-accounting allocator (a replacement `operator new`). That allocator needs glibc, so memory is measured only on Ubuntu under WSL2; on Windows the benchmark skips itself. On Ubuntu, `rl_bench` is linked with the allocator for every benchmark, but it counts only inside `MemoryPerKey`. Tables double in size, so the cost depends on where the key count falls. The 10 repetitions agree to the digit shown.

  <!-- claims-table:begin id=rl.memory.ubuntu-wsl2 -->
  | Keys | Key type | Bytes per key | Counted by |
  |---:|---|---:|---|
  | 700,000 | short (`key_123`) | 95.9 | heap-accounting allocator |
  | 1,000,000 | short | 134.2 | heap-accounting allocator |
  | 700,000 | 60–65 bytes | 167.9 | heap-accounting allocator |
  | 1,000,000 | 60–65 bytes | 206.2 | heap-accounting allocator |
  <!-- claims-table:end -->

  An empty limiter with the default <!-- claim:rl.design.default_shards -->128 shards allocates <!-- claim:rl.memory.empty.ubuntu-wsl2 -->9.1 KiB (counted by the heap-accounting allocator).
- **Scaling on one hot key behaves as designed.** With all threads hitting one key, denied traffic went up <!-- claim:rl.scaling.hot_denied.ubuntu-wsl2 -->3.5× from 1 to 8 threads on Ubuntu and <!-- claim:rl.scaling.hot_denied.windows-native -->3.3× on Windows, because denials only read the key's cache line. Admitted traffic fell to <!-- claim:rl.scaling.hot_allowed.ubuntu-wsl2 -->0.25× of its 1-thread rate on Ubuntu and <!-- claim:rl.scaling.hot_allowed.windows-native -->0.33× on Windows, because every admitted call must write that same line. Each figure is the median of 10 per-repetition ratios, and every repetition showed the same pattern in both environments.
- **Inspection doesn't slow decisions down.** When adding `inspect()`, I compared the compiled machine code of `allow()` and `try_acquire()` before and after: the instructions were identical. I also counted CPU instructions per call with `perf`, which background load doesn't distort. In every benchmark scenario they matched within normal run-to-run variation.
- **The real cost of inspection is cache-line sharing.** One thread inspecting a hot key in a tight loop made each `allow()` call on that key take <!-- claim:rl.inspect.same_key_cost.ubuntu-wsl2 -->1.7× as long on Ubuntu and <!-- claim:rl.inspect.same_key_cost.windows-native -->1.7× as long on Windows as the same loop on an untracked key (medians of 10 per-repetition ratios). The table compares time per `allow()` call (Google Benchmark time per iteration, amortized, 1 deciding thread) with no inspector. The tight loop on an untracked key made no measurable difference on Ubuntu but slowed calls on Windows. Reading the hot key at 500 or 10,000 reads per second, the range a live view would use, made no measurable difference in either environment.

  <!-- claims-table:begin id=rl.inspect -->
  | Inspector thread | Ubuntu under WSL2 | vs no inspector | Native Windows | vs no inspector |
  |---|---:|---|---:|---|
  | None | 26.4 ns (24.8–28.2 ns) |  | 58.8 ns (56.4–60.9 ns) |  |
  | Back to back on an untracked key | 29.3 ns (25.5–34.9 ns) | no measurable difference (medians of 10 runs each) | 82.6 ns (73.8–91.2 ns) | slower with the inspector by ~1.4×, medians of 10 runs each |
  | Same hot key, 10,000 reads per second | 27.0 ns (24.7–28.7 ns) | no measurable difference (medians of 10 runs each) | 59.0 ns (56.9–63.0 ns) | no measurable difference (medians of 10 runs each) |
  | Same hot key, 500 reads per second | 24.9 ns (24.6–28.2 ns) | no measurable difference (medians of 10 runs each) | 58.0 ns (56.1–66.3 ns) | no measurable difference (medians of 10 runs each) |
  <!-- claims-table:end -->

## Challenges and what I learned

**Measuring performance honestly is harder than making code fast.** My first before/after benchmark runs seemed to show large changes, in both directions. Running the *same* binary twice showed just as much spread, because other programs were sharing the machine. I switched to three methods that hold up under noise: comparing generated assembly, counting instructions with `perf stat`, and interleaving short before/after/before runs so background load affects both sides alike. *Lesson: measure the noise first, and prefer evidence that noise can't fake.*

**A benchmark can change the thing it measures.** Adding an inspection benchmark to a shared helper changed the compiler's inlining decisions, which added about <!-- claim-waiver: historical anecdote from development (an instruction count seen with perf at the time), not a current performance figure -->7 instructions per call to existing, unrelated benchmarks, even though the library itself hadn't changed at all. Keeping the new benchmark code separate restored identical machine code. *Lesson: when numbers move, check the harness too, not just the code under test.*

**Freeing memory while other threads might still read it.** Lookups take no lock, so a table replaced by growth or eviction can't simply be deleted. Epoch-based reclamation solved this, but it meant reasoning carefully about memory ordering, and about the states a slot can be in mid-change (being written, sealed, or moved to a new table).

**Contention on a single hot key.** The natural design, lock the entry and update it, makes every core fight over one cache line when a single key is flooded. Recognizing that most of those calls are denials that change nothing led to the quiet-denial snapshot path. But reading the snapshot first on *every* call costs ordinary calls an extra cache round trip, so it's used only for slots a thread recently saw denied. *Lesson: an optimization for one workload can be a tax on another; measure both.*

**Number representation.** An earlier fixed-point format (Q32.32) overflowed for large capacities. Moving to nanotoken integers made refill exact and the overflow cases explicit, saturating instead of wrapping. *Lesson: pick the representation that makes the arithmetic exact, then test the edges (zero, maximum, overflow) on purpose. A differential test against a simple model is a good way to check that a rewrite like this changed nothing else.*

**Tools have blind spots.** ThreadSanitizer doesn't model the memory fences a seqlock relies on (the compiler even warns about it), so a clean TSan run doesn't prove the snapshot logic is correct. The concurrency tests therefore check the observable property directly: a reader must never see a mix of two states.

**A test that depends on timing will eventually fail.** One eviction test checked that the evictor had removed something while traffic was running. Usually it had. But in <!-- claim-waiver: historical anecdote about a flaky test during development (failure counts from repeated test runs), not a performance figure -->6 to 28 runs out of 1,000 on my machine, depending on the build, the traffic threads finished their fixed number of rounds before the evictor thread got going. The fix was to make the traffic keep going until eviction had happened (with a bound), rather than hope the scheduler cooperates. *Lesson: a concurrency test should wait for the condition it needs, not for a fixed amount of work; run it a few thousand times before trusting it.*

**Smaller lessons.** CMake's default (unset) build type compiles without optimization, so the project now defaults to Release. Building shared libraries with hidden symbol visibility silently hid the C API until it was explicitly exported; the C test now links against the shared library to catch that.

## Limitations

- In-process only; no distributed limits.
- I build and test locally on Linux with GCC (Ubuntu under WSL2) and on Windows with MSVC 2022, where the full suite of <!-- claim:rl.tests.windows-native -->54 tests passes. CI still builds and tests only on Ubuntu and macOS. On MSVC, `FastClock` falls back to `steady_clock`, and the memory-per-key benchmark is skipped because its allocation counter needs glibc.
- The lock-free snapshot read retries while a writer holds the key, with backoff. It doesn't have a hard bound under continuous writes to one key.
- `FastClock` can step backwards when it re-anchors to `steady_clock`. On my machine under heavy load I've seen steps of up to about <!-- claim-waiver: historical observation from development under heavy load, not a benchmark result -->1 ms, which makes the two `FastClock` tests (<!-- claim:rl.design.fastclock_test_tolerance_us -->100 µs tolerance) fail intermittently. A backwards step can't make the limiter grant extra tokens, since an earlier time adds none, but it can delay refill by up to the size of the step.
- The JNI bridge exposes only `allow()`, on one shared limiter, with `int` limits.

## Building, installing and using it from CMake

| Option            | Default | Description                                    |
| ----------------- | ------: | ---------------------------------------------- |
| `RL_BUILD_SHARED` |    `ON` | Build the shared C++ library (`ratelimit`)     |
| `RL_BUILD_TESTS`  |    `ON` | Build the tests                                |
| `RL_BUILD_BENCH`  |    `ON` | Build `rl_bench` and `rl_latency`              |
| `RL_BUILD_JNI`    |    `ON` | Build the JNI library (needs a JDK)            |
| `RL_ENABLE_ASAN`  |   `OFF` | AddressSanitizer + UndefinedBehaviorSanitizer  |
| `RL_ENABLE_TSAN`  |   `OFF` | ThreadSanitizer                                |

Targets: `ratelimit_core` (static C++), `ratelimit` (shared C++), `ratelimit_c` (shared C API), `ratelimit_jni`, `rl_cli`, `rl_tests`, `rl_c_api_test`, `rl_bench`, `rl_latency`.

Install, then build the sample consumer against the install:

```sh
cmake --install build --prefix "$PWD/install"
cmake -S consumer -B consumer-build -DCMAKE_PREFIX_PATH="$PWD/install"
cmake --build consumer-build
```

In your own project:

```cmake
find_package(Ratelimit CONFIG REQUIRED)
target_link_libraries(app PRIVATE Ratelimit::ratelimit_core)   # or Ratelimit::ratelimit
```

Without CMake, use pkg-config. The `.pc` files locate the install from their own directory, so they work with any `--prefix`, and after the install is moved:

```sh
export PKG_CONFIG_PATH="$PWD/install/lib/pkgconfig"
cc  app.c   $(pkg-config --cflags --libs ratelimit_c)   # C API, shared library
c++ app.cpp $(pkg-config --cflags --libs ratelimit)     # C++ API, static library
```

A program linked against the shared `ratelimit_c` needs the library on the loader path at run time (for example `LD_LIBRARY_PATH="$PWD/install/lib"`) unless it's installed system-wide.

## Project layout

```text
include/ratelimit/   public headers: rate_limiter.h, types.h, clock.h, ratelimit_c.h, export.h, version.h
src/                 inproc_rate_limiter.cpp (the limiter), clock.cpp + fast_clock.h (FastClock)
c_api/               C API implementation
jni/                 JNI bridge and its Java class
examples/cli.cpp     rl_cli
tests/               test_core.cpp (GoogleTest), test_c_api.c
bench/               bench_core.cpp (Google Benchmark), latency.cpp
consumer/            sample CMake project using an installed copy
cmake/               package config and pkg-config templates
```

## License

MIT. See [LICENSE](LICENSE).
