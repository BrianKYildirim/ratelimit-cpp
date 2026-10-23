// Benchmark suite for RateLimiterInProc.
//
// Every iteration makes exactly one real admission decision: hash the key,
// find its bucket, refill and consume under the key's lock. Unless a benchmark
// name says otherwise, each call also reads the limiter's clock (FastClock).
// Setup (key generation, pre-insertion) is excluded from timing.
//
// Reading results: "Time" is wall time per call on one thread. wall_ops/s is
// the aggregate throughput: calls by all threads / wall time from the end of
// setup until the last thread finishes. (Google Benchmark's own rate
// counters divide by the average per-thread time instead, which overstates
// throughput when threads finish at different times, as they do when there
// are more threads than CPUs.) granted and denied are decision counts summed
// over all threads; every run aborts if the limiter granted more than
// capacity + rate * elapsed per key it was given.
//
// Key access patterns:
//   Hot       one key shared by all threads (maximum contention)
//   Uniform   N pre-inserted keys shared by all threads; each thread walks
//             them in a shuffled order, so keys move between cores
//   Disjoint  each thread owns 10k pre-inserted keys (no sharing), as when
//             connections or tenants are pinned to worker threads
//   NewKeys   every call inserts a previously unseen key
//   Untracked N keys that were never inserted, probed in tables holding
//             N other keys
//
// API variants:
//   (none)        allow(): full Decision with remaining and reset_ms
//   TryAcquire_   try_acquire(): verdict only, same state change as allow()
//   TryAcquireAt_ try_acquire(key, limit, now_ns) with one now_ns() read per
//                 64 calls, as when a batch of requests shares one timestamp
//   Inspect_      inspect(): reads the key's bucket without changing it (no
//                 granted/denied counters)
//   Clock_        one clock read, for reference
//
// BM_Hot_Allowed_Inspected runs allow() on the hot key while extra threads,
// outside the benchmark's own, inspect it (same_key:1) or an untracked key
// (same_key:0, the control); compare its wall_ops/s with BM_Hot_Allowed at
// the same thread count.

#include <benchmark/benchmark.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <new>
#include <random>
#include <string>
#include <thread>
#include <vector>

#if defined(__GLIBC__)
#include <malloc.h>
#endif
#if defined(__linux__)
#include <sys/resource.h>
#endif

#include "ratelimit/rate_limiter.h"

// ---------- Heap accounting (only while g_track is set) ----------
#if defined(__GLIBC__)
static bool g_track = false;
static int64_t g_live_bytes = 0;

static void* counted_alloc(size_t n, size_t align) {
    void* p = align > alignof(std::max_align_t) ? std::aligned_alloc(align, (n + align - 1) / align * align)
                                                : std::malloc(n ? n : 1);
    if (!p) throw std::bad_alloc();
    if (g_track) g_live_bytes += static_cast<int64_t>(malloc_usable_size(p));
    return p;
}
static void counted_free(void* p) noexcept {
    if (!p) return;
    if (g_track) g_live_bytes -= static_cast<int64_t>(malloc_usable_size(p));
    std::free(p);
}
void* operator new(size_t n) { return counted_alloc(n, 0); }
void* operator new[](size_t n) { return counted_alloc(n, 0); }
void* operator new(size_t n, std::align_val_t a) { return counted_alloc(n, static_cast<size_t>(a)); }
void* operator new[](size_t n, std::align_val_t a) { return counted_alloc(n, static_cast<size_t>(a)); }
void* operator new(size_t n, const std::nothrow_t&) noexcept {
    try { return counted_alloc(n, 0); } catch (...) { return nullptr; }
}
void operator delete(void* p) noexcept { counted_free(p); }
void operator delete[](void* p) noexcept { counted_free(p); }
void operator delete(void* p, size_t) noexcept { counted_free(p); }
void operator delete[](void* p, size_t) noexcept { counted_free(p); }
void operator delete(void* p, std::align_val_t) noexcept { counted_free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { counted_free(p); }
void operator delete(void* p, size_t, std::align_val_t) noexcept { counted_free(p); }
void operator delete[](void* p, size_t, std::align_val_t) noexcept { counted_free(p); }
#endif

// ---------- Shared fixtures ----------
static constexpr rl::Limit kAllowAll{1u << 30, 1u << 30}; // never runs dry
static constexpr rl::Limit kMostlyDeny{1, 1};             // 1 token/s
static constexpr size_t kDisjointKeys = 10'000;           // per thread

static std::string short_key(size_t i) { return "key_" + std::to_string(i); } // SSO-sized
static std::string long_key(size_t i) {                                         // heap-sized
    return "tenant:acme-corp/user:3f2504e0-4f89-11d3-9a0c-0305e82c3301/" + std::to_string(i);
}
static std::string sized_key(size_t i, size_t len) { // exactly `len` bytes, unique for i < 10^min(len,16)
    std::string s = std::to_string(i);
    return s.size() >= len ? s.substr(s.size() - len) : std::string(len - s.size(), 'k') + s;
}

static std::vector<std::string> make_keys(size_t n, bool long_keys, uint64_t seed) {
    std::vector<std::string> keys;
    keys.reserve(n);
    for (size_t i = 0; i < n; ++i) keys.push_back(long_keys ? long_key(i) : short_key(i));
    std::shuffle(keys.begin(), keys.end(), std::mt19937_64(seed));
    return keys;
}

// State of the current run. Setup and Teardown run once per run, outside the
// threads, so the threads only read these.
static std::unique_ptr<rl::RateLimiterInProc> g_rl;
static std::vector<std::string> g_keys;
static rl::Limit g_limit;
static uint64_t g_start_ns;                // limiter time when the timed run began
static std::atomic<uint64_t> g_granted{0}; // grants during the timed run, all threads
static std::chrono::steady_clock::time_point g_wall_start;
static std::atomic<int> g_finished{0};     // threads done with the timed loop

static void begin_run(std::vector<std::string> keys, rl::Limit limit, bool insert) {
    g_rl = std::make_unique<rl::RateLimiterInProc>();
    g_keys = std::move(keys);
    g_limit = limit;
    if (insert) {
        for (auto& k : g_keys) g_rl->allow(k, limit); // insert (and drain a small bucket)
    }
    g_granted = 0;
    g_finished = 0;
    g_start_ns = g_rl->now_ns();
    g_wall_start = std::chrono::steady_clock::now();
}

// No key may be granted more than capacity + rate * elapsed (+1 for rounding).
static void teardown(const benchmark::State&) {
    const double secs = static_cast<double>(g_rl->now_ns() - g_start_ns) / 1e9;
    const double per_key = static_cast<double>(g_limit.capacity) + static_cast<double>(g_limit.refill_per_sec) * secs + 1;
    const double bound = per_key * static_cast<double>(g_keys.size());
    if (static_cast<double>(g_granted.load()) > bound) {
        std::fprintf(stderr, "FATAL: %llu grants exceed the bound %.0f\n",
                     static_cast<unsigned long long>(g_granted.load()), bound);
        std::abort();
    }
    g_rl.reset();
    g_keys = {};
}

static void finish(benchmark::State& state, uint64_t granted) {
    g_granted += granted;
    const auto calls = static_cast<double>(state.iterations());
    // Every thread makes the same number of calls. The last thread to finish
    // reports the aggregate rate; counters are summed, so the others report 0.
    double wall_rate = 0;
    if (g_finished.fetch_add(1) + 1 == state.threads()) {
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_wall_start).count();
        wall_rate = calls * state.threads() / secs;
    }
    state.counters["wall_ops/s"] = wall_rate;
    state.counters["granted"] = static_cast<double>(granted);
    state.counters["denied"] = calls - static_cast<double>(granted);
}

enum class Api { Allow, TryAcquire, TryAcquireAt };

// Calls the limiter on g_keys[first + i] for i = start, start + 1, ... wrapping at n.
template <Api api>
static void run(benchmark::State& state, size_t first, size_t n, size_t start) {
    rl::RateLimiterInProc& limiter = *g_rl;
    const rl::Limit limit = g_limit;
    const std::string* keys = g_keys.data() + first;
    uint64_t granted = 0, now = 0;
    unsigned until_clock = 0;
    size_t i = start;
    for (auto _ : state) {
        bool ok;
        if constexpr (api == Api::Allow) {
            rl::Decision d = limiter.allow(keys[i], limit);
            benchmark::DoNotOptimize(d);
            ok = d.allowed;
        } else if constexpr (api == Api::TryAcquire) {
            ok = limiter.try_acquire(keys[i], limit);
        } else {
            if (until_clock-- == 0) {
                now = limiter.now_ns();
                until_clock = 63;
            }
            ok = limiter.try_acquire(keys[i], limit, now);
        }
        benchmark::DoNotOptimize(ok);
        granted += ok;
        if (++i == n) i = 0;
    }
    finish(state, granted);
}

// ---------- Hot key ----------
static void setup_hot_allowed(const benchmark::State&) { begin_run({"hot"}, kAllowAll, false); }
static void setup_hot_denied(const benchmark::State&) { begin_run({"hot"}, kMostlyDeny, false); }

template <Api api>
static void BM_Hot(benchmark::State& state) { run<api>(state, 0, 1, 0); }

// ---------- Uniform shared keys ----------
template <bool LongKeys, bool Deny>
static void setup_uniform(const benchmark::State& state) {
    begin_run(make_keys(static_cast<size_t>(state.range(0)), LongKeys, 42), Deny ? kMostlyDeny : kAllowAll, true);
}

template <Api api>
static void BM_Uniform(benchmark::State& state) {
    const size_t n = g_keys.size();
    run<api>(state, 0, n, n * static_cast<size_t>(state.thread_index()) / static_cast<size_t>(state.threads()));
}

// ---------- Disjoint per-thread keys ----------
// range(0) = key length in bytes, or 0 for short "key_<i>" keys.
static void setup_disjoint(const benchmark::State& state) {
    const size_t n = kDisjointKeys * static_cast<size_t>(state.threads());
    const auto len = static_cast<size_t>(state.range(0));
    std::vector<std::string> keys;
    keys.reserve(n);
    for (size_t i = 0; i < n; ++i) keys.push_back(len ? sized_key(i, len) : short_key(i));
    begin_run(std::move(keys), kAllowAll, true);
}

template <Api api>
static void BM_Disjoint(benchmark::State& state) {
    run<api>(state, kDisjointKeys * static_cast<size_t>(state.thread_index()), kDisjointKeys, 0);
}

// ---------- New keys (insertion path) ----------
static constexpr int64_t kNewKeysPerThread = 200000;
static void setup_new(const benchmark::State& state) {
    begin_run(make_keys(static_cast<size_t>(kNewKeysPerThread * state.threads()), state.range(0) != 0, 7), kAllowAll,
              false);
}
// Minor page faults taken by the calling thread so far (new table memory).
static double thread_page_faults() {
#if defined(__linux__)
    rusage ru{};
    getrusage(RUSAGE_THREAD, &ru);
    return static_cast<double>(ru.ru_minflt);
#else
    return 0;
#endif
}

static void BM_NewKeys(benchmark::State& state) {
    const double faults = thread_page_faults();
    run<Api::Allow>(state, static_cast<size_t>(kNewKeysPerThread * state.thread_index()),
                    static_cast<size_t>(kNewKeysPerThread), 0);
    state.counters["faults/op"] = benchmark::Counter(thread_page_faults() - faults, benchmark::Counter::kAvgIterations);
}

// ---------- Inspection ----------
// Separate from run() and finish(), so the loops above compile exactly as
// before this benchmark existed.

// Inspects g_keys[i] for i = start, start + 1, ... wrapping at n.
static void run_inspect(benchmark::State& state, size_t n, size_t start) {
    const rl::RateLimiterInProc& limiter = *g_rl;
    const rl::Limit limit = g_limit;
    const std::string* keys = g_keys.data();
    size_t i = start;
    for (auto _ : state) {
        rl::BucketState s = limiter.inspect(keys[i], limit);
        benchmark::DoNotOptimize(s);
        if (++i == n) i = 0;
    }
    double wall_rate = 0; // as finish() computes it
    if (g_finished.fetch_add(1) + 1 == state.threads()) {
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_wall_start).count();
        wall_rate = static_cast<double>(state.iterations()) * state.threads() / secs;
    }
    state.counters["wall_ops/s"] = wall_rate;
}

static void BM_Inspect_Hot(benchmark::State& state) { run_inspect(state, 1, 0); }
static void BM_Inspect_Uniform(benchmark::State& state) {
    const size_t n = g_keys.size();
    run_inspect(state, n, n * static_cast<size_t>(state.thread_index()) / static_cast<size_t>(state.threads()));
}

// A tracked hot key (inserted, unlike BM_Hot's).
static void setup_hot_tracked(const benchmark::State&) { begin_run({"hot"}, kAllowAll, true); }

// N keys inserted, then N others (one byte longer) probed instead.
static void setup_untracked(const benchmark::State& state) {
    setup_uniform<false, false>(state);
    for (auto& k : g_keys) k += 'x';
}

// Threads, outside the benchmark's, that inspect while the benchmark threads
// call allow() on the hot key: range(0) threads, each sleeping range(1) ns
// between inspections as a frame loop would (0 = back to back), reading the
// hot key if range(2) is 1, or else an untracked key, which costs the same
// work without sharing the writers' cache line.
static std::vector<std::thread> g_inspectors;
static std::atomic<bool> g_stop_inspectors{false};
static std::atomic<uint64_t> g_inspections{0};

// BM_Hot_Allowed's loop, plus the inspectors' rate over thread 0's run.
static void BM_Hot_Allowed_Inspected(benchmark::State& state) {
    BM_Hot<Api::Allow>(state);
    if (state.thread_index() == 0) {
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_wall_start).count();
        state.counters["inspect_ops/s"] = static_cast<double>(g_inspections.load()) / secs;
    }
}

static void setup_hot_inspected(const benchmark::State& state) {
    begin_run({"hot"}, kAllowAll, true);
    g_inspections = 0;
    g_stop_inspectors = false;
    const auto pace = std::chrono::nanoseconds(state.range(1));
    const std::string key = state.range(2) ? g_keys[0] : std::string("elsewhere");
    for (int64_t t = 0; t < state.range(0); ++t) {
        g_inspectors.emplace_back([pace, key] {
            const rl::RateLimiterInProc& limiter = *g_rl;
            const uint64_t batch = pace.count() ? 1 : 256; // counted in batches when back to back
            auto next = std::chrono::steady_clock::now();
            for (uint64_t n = 1; !g_stop_inspectors.load(std::memory_order_relaxed); ++n) {
                if (pace.count()) std::this_thread::sleep_until(next += pace);
                rl::BucketState s = limiter.inspect(key, g_limit);
                benchmark::DoNotOptimize(s);
                if (n % batch == 0) g_inspections.fetch_add(batch, std::memory_order_relaxed);
            }
        });
    }
}

static void teardown_hot_inspected(const benchmark::State& state) {
    g_stop_inspectors = true;
    for (auto& t : g_inspectors) t.join();
    g_inspectors.clear();
    teardown(state);
}

// ---------- Clock reads ----------
static void BM_Clock_Fast(benchmark::State& state) {
    rl::FastClock clock;
    for (auto _ : state) benchmark::DoNotOptimize(clock.now_ns());
}
static void BM_Clock_Steady(benchmark::State& state) {
    rl::SteadyClock clock;
    for (auto _ : state) benchmark::DoNotOptimize(clock.now_ns());
}

// ---------- Memory ----------
static void BM_MemoryPerKey(benchmark::State& state) {
#if defined(__GLIBC__)
    const size_t n = static_cast<size_t>(state.range(0));
    const auto keys = make_keys(n, state.range(1) != 0, 1);
    for (auto _ : state) {
        g_live_bytes = 0;
        g_track = true;
        {
            rl::RateLimiterInProc limiter;
            state.counters["empty_KiB"] = static_cast<double>(g_live_bytes) / 1024.0;
            const int64_t before = g_live_bytes;
            for (auto& k : keys) benchmark::DoNotOptimize(limiter.allow(k, kAllowAll));
            state.counters["bytes/key"] = static_cast<double>(g_live_bytes - before) / static_cast<double>(n);
        }
        g_track = false;
    }
#else
    state.SkipWithError("heap accounting requires glibc");
#endif
}

#define RL_THREADS ->Threads(1)->Threads(2)->Threads(4)->Threads(8)->Threads(16)->UseRealTime()
#define RL_RUN(setup) ->Setup(setup)->Teardown(teardown)

// Full API: allow().
BENCHMARK(BM_Hot<Api::Allow>)->Name("BM_Hot_Allowed") RL_RUN(setup_hot_allowed) RL_THREADS;
BENCHMARK(BM_Hot<Api::Allow>)->Name("BM_Hot_Denied") RL_RUN(setup_hot_denied) RL_THREADS;
BENCHMARK(BM_Uniform<Api::Allow>)->Name("BM_Uniform_Allowed")->Arg(100000) RL_RUN((setup_uniform<false, false>)) RL_THREADS;
BENCHMARK(BM_Uniform<Api::Allow>)->Name("BM_Uniform_Allowed")->Arg(1000000) RL_RUN((setup_uniform<false, false>))
    ->Threads(1)->Threads(8)->UseRealTime();
BENCHMARK(BM_Uniform<Api::Allow>)->Name("BM_Uniform_Denied")->Arg(100000) RL_RUN((setup_uniform<false, true>)) RL_THREADS;
BENCHMARK(BM_Uniform<Api::Allow>)->Name("BM_UniformLong_Allowed")->Arg(100000) RL_RUN((setup_uniform<true, false>)) RL_THREADS;
BENCHMARK(BM_Disjoint<Api::Allow>)->Name("BM_Disjoint_Allowed")->Arg(0) RL_RUN(setup_disjoint) RL_THREADS;
BENCHMARK(BM_Disjoint<Api::Allow>)->Name("BM_KeyLength")->ArgName("bytes")->Arg(8)->Arg(16)->Arg(24)->Arg(32)->Arg(64)->Arg(128)
    RL_RUN(setup_disjoint)->Threads(1)->Threads(8)->UseRealTime();
BENCHMARK(BM_NewKeys)->ArgName("long")->Arg(0)->Arg(1) RL_RUN(setup_new)->Iterations(kNewKeysPerThread) RL_THREADS;

// Fast paths.
BENCHMARK(BM_Hot<Api::TryAcquire>)->Name("BM_TryAcquire_Hot_Allowed") RL_RUN(setup_hot_allowed) RL_THREADS;
BENCHMARK(BM_Hot<Api::TryAcquire>)->Name("BM_TryAcquire_Hot_Denied") RL_RUN(setup_hot_denied) RL_THREADS;
BENCHMARK(BM_Uniform<Api::TryAcquire>)->Name("BM_TryAcquire_Uniform_Allowed")->Arg(100000)
    RL_RUN((setup_uniform<false, false>)) RL_THREADS;
BENCHMARK(BM_Disjoint<Api::TryAcquire>)->Name("BM_TryAcquire_Disjoint_Allowed")->Arg(0) RL_RUN(setup_disjoint) RL_THREADS;
BENCHMARK(BM_Disjoint<Api::TryAcquireAt>)->Name("BM_TryAcquireAt_Disjoint_Allowed")->Arg(0) RL_RUN(setup_disjoint) RL_THREADS;

// Inspection: the read path alone, then its cost to writers of the same key.
BENCHMARK(BM_Inspect_Hot) RL_RUN(setup_hot_tracked) RL_THREADS;
BENCHMARK(BM_Inspect_Uniform)->Arg(100000) RL_RUN((setup_uniform<false, false>)) RL_THREADS;
BENCHMARK(BM_Inspect_Uniform)->Name("BM_Inspect_Untracked")->Arg(100000) RL_RUN(setup_untracked) RL_THREADS;
// 2'000'000 ns: about the rate of a 60 fps view of 8 keys.
BENCHMARK(BM_Hot_Allowed_Inspected)->ArgNames({"inspectors", "pace_ns", "same_key"})
    ->Args({1, 0, 1})->Args({1, 0, 0})->Args({4, 0, 1})->Args({4, 0, 0})->Args({1, 100'000, 1})->Args({1, 2'000'000, 1})
    ->Setup(setup_hot_inspected)->Teardown(teardown_hot_inspected)->Threads(1)->Threads(4)->UseRealTime();

BENCHMARK(BM_Clock_Fast);
BENCHMARK(BM_Clock_Steady);

// Open-addressing tables double in size, so bytes/key depends on where the key
// count falls between doublings.
BENCHMARK(BM_MemoryPerKey)->ArgNames({"keys", "long"})
    ->Args({700000, 0})->Args({1000000, 0})->Args({700000, 1})->Args({1000000, 1})
    ->Iterations(1)->Unit(benchmark::kMillisecond);

BENCHMARK_MAIN();
