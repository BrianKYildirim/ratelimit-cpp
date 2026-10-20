




// Benchmark suite for RateLimiterInProc.
//
// Every benchmark separates setup (key generation, pre-insertion) from the
// timed loop and uses the real SteadyClock.
//
// Reading multi-threaded results: Google Benchmark sums wall time across
// threads, so "Time" is per-thread latency per call and items_per_second would
// be per-thread throughput. `total_ops/s` is the aggregate across all threads.
//
//   Hot_*          one key shared by all threads (worst-case contention)
//   Uniform_*      pre-inserted keys, each thread walks a shuffled order
//   NewKeys_*      every call inserts a previously unseen key
//   MemoryPerKey_* heap bytes per tracked key (glibc only)

#include <benchmark/benchmark.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <new>
#include <random>
#include <string>
#include <vector>

#if defined(__GLIBC__)
#include <malloc.h>
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

static std::string short_key(size_t i) { return "key_" + std::to_string(i); } // SSO-sized
static std::string long_key(size_t i) {                                         // heap-sized
    return "tenant:acme-corp/user:3f2504e0-4f89-11d3-9a0c-0305e82c3301/" + std::to_string(i);
}

static std::vector<std::string> make_keys(size_t n, bool long_keys, uint64_t seed) {
    std::vector<std::string> keys;
    keys.reserve(n);
    for (size_t i = 0; i < n; ++i) keys.push_back(long_keys ? long_key(i) : short_key(i));
    std::shuffle(keys.begin(), keys.end(), std::mt19937_64(seed));
    return keys;
}

static std::unique_ptr<rl::RateLimiterInProc> g_rl;
static std::vector<std::string> g_keys;

// Each thread reports iterations * threads; the sum divided by the summed
// per-thread wall time is total iterations / wall time.
static void report_total_rate(benchmark::State& state) {
    state.counters["total_ops/s"] = benchmark::Counter(
        static_cast<double>(state.iterations()) * state.threads(), benchmark::Counter::kIsRate);
}

static void teardown(const benchmark::State&) {
    g_rl.reset();
    g_keys = {};
}

static void run_keys(benchmark::State& state, const rl::Limit& lim) {
    const size_t n = g_keys.size();
    size_t i = n * static_cast<size_t>(state.thread_index()) / static_cast<size_t>(state.threads());
    uint64_t allowed = 0;
    for (auto _ : state) {
        rl::Decision d = g_rl->allow(g_keys[i], lim);
        benchmark::DoNotOptimize(d);
        allowed += d.allowed;
        if (++i == n) i = 0;
    }
    report_total_rate(state);
    state.counters["allowed%"] = benchmark::Counter(
        100.0 * static_cast<double>(allowed) / static_cast<double>(std::max<int64_t>(state.iterations(), 1)),
        benchmark::Counter::kAvgThreads);
}

// ---------- Hot key ----------
static void setup_hot(const benchmark::State&) {
    g_rl = std::make_unique<rl::RateLimiterInProc>();
    g_keys = {"hot"};
}
static void BM_Hot_Allowed(benchmark::State& state) { run_keys(state, kAllowAll); }
static void BM_Hot_Denied(benchmark::State& state) { run_keys(state, kMostlyDeny); }

// ---------- Uniform existing keys ----------
template <bool LongKeys>
static void setup_uniform(const benchmark::State& state) {
    g_rl = std::make_unique<rl::RateLimiterInProc>();
    g_keys = make_keys(static_cast<size_t>(state.range(0)), LongKeys, 42);
    for (auto& k : g_keys) g_rl->allow(k, kMostlyDeny); // insert + drain the single token
}
static void BM_Uniform_Allowed(benchmark::State& state) { run_keys(state, kAllowAll); }
static void BM_Uniform_Denied(benchmark::State& state) { run_keys(state, kMostlyDeny); }
static void BM_UniformLong_Allowed(benchmark::State& state) { run_keys(state, kAllowAll); }

// ---------- New keys (insertion path) ----------
static constexpr int64_t kNewKeysPerThread = 200000;
static void setup_new(const benchmark::State& state) {
    g_rl = std::make_unique<rl::RateLimiterInProc>();
    g_keys = make_keys(static_cast<size_t>(kNewKeysPerThread * state.threads()), state.range(0) != 0, 7);
}
static void BM_NewKeys(benchmark::State& state) {
    size_t i = static_cast<size_t>(kNewKeysPerThread * state.thread_index());
    for (auto _ : state) benchmark::DoNotOptimize(g_rl->allow(g_keys[i++], kAllowAll));
    report_total_rate(state);
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

BENCHMARK(BM_Hot_Allowed)->Setup(setup_hot)->Teardown(teardown) RL_THREADS;
BENCHMARK(BM_Hot_Denied)->Setup(setup_hot)->Teardown(teardown) RL_THREADS;
BENCHMARK(BM_Uniform_Allowed)->Arg(100000)->Setup(setup_uniform<false>)->Teardown(teardown) RL_THREADS;
BENCHMARK(BM_Uniform_Allowed)->Arg(1000000)->Setup(setup_uniform<false>)->Teardown(teardown)
    ->Threads(1)->Threads(8)->UseRealTime();
BENCHMARK(BM_Uniform_Denied)->Arg(100000)->Setup(setup_uniform<false>)->Teardown(teardown) RL_THREADS;
BENCHMARK(BM_UniformLong_Allowed)->Arg(100000)->Setup(setup_uniform<true>)->Teardown(teardown) RL_THREADS;
BENCHMARK(BM_NewKeys)->ArgName("long")->Arg(0)->Arg(1)->Setup(setup_new)->Teardown(teardown)
    ->Iterations(kNewKeysPerThread) RL_THREADS;
// Open-addressing tables double in size, so bytes/key depends on where the key
// count falls between doublings: 700k is near the densest point, 1M just past a doubling.
BENCHMARK(BM_MemoryPerKey)->ArgNames({"keys", "long"})
    ->Args({700000, 0})->Args({1000000, 0})->Args({700000, 1})->Args({1000000, 1})
    ->Iterations(1)->Unit(benchmark::kMillisecond);

BENCHMARK_MAIN();
