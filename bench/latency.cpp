// Per-call latency distribution and wall-clock throughput of RateLimiterInProc.
//
// For each scenario and thread count, two phases on a fresh limiter:
//   throughput: all threads start together and call the limiter for a fixed
//     wall time; Mops/s = all calls / wall time, cpu% = process CPU time /
//     (wall time * threads), so oversubscribed runs show below 100%.
//   latency: every call is timed with fenced TSC reads, minus the measured
//     cost of an empty timed region; percentiles are over all threads' calls.
//
// usage: rl_latency [seconds_per_phase=0.5] [max_threads=16]

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <sys/resource.h>
#endif

#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
#include <x86intrin.h>
static inline uint64_t ticks_begin() {
    _mm_lfence();
    const uint64_t t = __rdtsc();
    _mm_lfence();
    return t;
}
static inline uint64_t ticks_end() {
    unsigned aux;
    const uint64_t t = __rdtscp(&aux);
    _mm_lfence();
    return t;
}
#else
static inline uint64_t ticks_begin() {
    return static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
}
static inline uint64_t ticks_end() { return ticks_begin(); }
#endif

#include "ratelimit/rate_limiter.h"

namespace {

using Clock = std::chrono::steady_clock;

double ticks_per_ns() {
    const auto c0 = Clock::now();
    const uint64_t t0 = ticks_begin();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const auto c1 = Clock::now();
    const uint64_t t1 = ticks_end();
    return static_cast<double>(t1 - t0) / std::chrono::duration<double, std::nano>(c1 - c0).count();
}

// 1-tick buckets up to 8192 ticks, then one bucket per power of two.
struct Histogram {
    static constexpr size_t kLinear = 8192;
    std::vector<uint64_t> linear = std::vector<uint64_t>(kLinear);
    uint64_t log2[64] = {};
    uint64_t max = 0, count = 0;

    void add(uint64_t t) {
        if (t < kLinear) ++linear[t];
        else ++log2[63 - __builtin_clzll(t)];
        max = std::max(max, t);
        ++count;
    }
    void merge(const Histogram& o) {
        for (size_t i = 0; i < kLinear; ++i) linear[i] += o.linear[i];
        for (int i = 0; i < 64; ++i) log2[i] += o.log2[i];
        max = std::max(max, o.max);
        count += o.count;
    }
    uint64_t percentile(double p) const {
        const auto want = static_cast<uint64_t>(p * static_cast<double>(count));
        uint64_t seen = 0;
        for (size_t i = 0; i < kLinear; ++i)
            if ((seen += linear[i]) > want) return i;
        for (int i = 0; i < 64; ++i)
            if ((seen += log2[i]) > want) return uint64_t{1} << i; // lower bound of the bucket
        return max;
    }
};

double process_cpu_seconds() {
#if defined(__linux__)
    rusage ru{};
    getrusage(RUSAGE_SELF, &ru);
    return static_cast<double>(ru.ru_utime.tv_sec + ru.ru_stime.tv_sec) +
           static_cast<double>(ru.ru_utime.tv_usec + ru.ru_stime.tv_usec) / 1e6;
#else
    return 0;
#endif
}

std::atomic<uint64_t> g_sink{0}; // keeps every decision observable

constexpr rl::Limit kAllowAll{1u << 30, 1u << 30};
constexpr rl::Limit kMostlyDeny{1, 1};

struct Scenario {
    const char* name;
    rl::Limit limit;
    // Keys for `threads` threads; thread t uses keys [first(t), first(t) + count) starting at offset(t).
    std::function<std::vector<std::string>(int threads)> keys;
    bool disjoint;
    bool try_acquire;
};

struct Fixture {
    std::unique_ptr<rl::RateLimiterInProc> limiter;
    std::vector<std::string> keys;
    rl::Limit limit;
    bool disjoint, try_acquire;
    int threads;

    // The range of keys thread t walks.
    void range(int t, size_t& first, size_t& n, size_t& start) const {
        if (disjoint) {
            n = keys.size() / static_cast<size_t>(threads);
            first = n * static_cast<size_t>(t);
            start = 0;
        } else {
            first = 0;
            n = keys.size();
            start = n * static_cast<size_t>(t) / static_cast<size_t>(threads);
        }
    }
    bool call(const std::string& key) const {
        return try_acquire ? limiter->try_acquire(key, limit) : limiter->allow(key, limit).allowed;
    }
};

Fixture make_fixture(const Scenario& s, int threads) {
    Fixture f{std::make_unique<rl::RateLimiterInProc>(), s.keys(threads), s.limit, s.disjoint, s.try_acquire, threads};
    for (auto& k : f.keys) f.limiter->allow(k, f.limit); // insert (and drain a small bucket)
    return f;
}

struct Result {
    double mops, cpu_pct;
    Histogram hist;
};

Result run(const Scenario& s, int threads, double seconds, uint64_t overhead) {
    Result r{};
    { // Throughput.
        Fixture f = make_fixture(s, threads);
        std::atomic<int> ready{0};
        std::atomic<bool> go{false}, stop{false};
        std::vector<uint64_t> calls(static_cast<size_t>(threads));
        std::vector<std::thread> ts;
        for (int t = 0; t < threads; ++t) {
            ts.emplace_back([&, t] {
                size_t first, n, i;
                f.range(t, first, n, i);
                uint64_t c = 0, sink = 0;
                ++ready;
                while (!go.load(std::memory_order_acquire)) {}
                while (!stop.load(std::memory_order_relaxed)) {
                    for (int k = 0; k < 256; ++k) {
                        sink += f.call(f.keys[first + i]);
                        if (++i == n) i = 0;
                    }
                    c += 256;
                }
                calls[static_cast<size_t>(t)] = c;
                g_sink += sink;
            });
        }
        while (ready.load() < threads) std::this_thread::yield();
        const double cpu0 = process_cpu_seconds();
        const auto t0 = Clock::now();
        go.store(true, std::memory_order_release);
        std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
        stop = true;
        for (auto& t : ts) t.join();
        const double wall = std::chrono::duration<double>(Clock::now() - t0).count();
        const double cpu = process_cpu_seconds() - cpu0;
        uint64_t total = 0;
        for (uint64_t c : calls) total += c;
        r.mops = static_cast<double>(total) / wall / 1e6;
        r.cpu_pct = 100.0 * cpu / (wall * threads);
    }
    { // Latency.
        Fixture f = make_fixture(s, threads);
        std::atomic<bool> go{false}, stop{false};
        std::vector<Histogram> hists(static_cast<size_t>(threads));
        std::vector<std::thread> ts;
        for (int t = 0; t < threads; ++t) {
            ts.emplace_back([&, t] {
                size_t first, n, i;
                f.range(t, first, n, i);
                Histogram& h = hists[static_cast<size_t>(t)];
                uint64_t sink = 0;
                while (!go.load(std::memory_order_acquire)) {}
                while (!stop.load(std::memory_order_relaxed)) {
                    for (int k = 0; k < 256; ++k) {
                        const std::string& key = f.keys[first + i];
                        const uint64_t a = ticks_begin();
                        sink += f.call(key);
                        const uint64_t b = ticks_end();
                        h.add(b - a > overhead ? b - a - overhead : 0);
                        if (++i == n) i = 0;
                    }
                }
                g_sink += sink;
            });
        }
        go.store(true, std::memory_order_release);
        std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
        stop = true;
        for (auto& t : ts) t.join();
        for (auto& h : hists) r.hist.merge(h);
    }
    return r;
}

std::vector<std::string> numbered(size_t n) {
    std::vector<std::string> keys;
    keys.reserve(n);
    for (size_t i = 0; i < n; ++i) keys.push_back("key_" + std::to_string(i));
    return keys;
}

} // namespace

int main(int argc, char** argv) {
    const double seconds = argc > 1 ? std::atof(argv[1]) : 0.5;
    const int max_threads = argc > 2 ? std::atoi(argv[2]) : 16;
    const double tpn = ticks_per_ns();

    // Median cost of an empty timed region, subtracted from every sample.
    std::vector<uint64_t> empty(100000);
    for (auto& e : empty) {
        const uint64_t a = ticks_begin();
        e = ticks_end() - a;
    }
    std::nth_element(empty.begin(), empty.begin() + empty.size() / 2, empty.end());
    const uint64_t overhead = empty[empty.size() / 2];

    std::printf("FastClock uses the TSC: %s; %.3f ticks/ns; timing overhead %.1f ns (subtracted)\n",
                rl::FastClock::uses_tsc() ? "yes" : "no", tpn, static_cast<double>(overhead) / tpn);

    const Scenario scenarios[] = {
        {"hot_allowed", kAllowAll, [](int) { return std::vector<std::string>{"hot"}; }, false, false},
        {"hot_denied", kMostlyDeny, [](int) { return std::vector<std::string>{"hot"}; }, false, false},
        {"uniform_100k_allowed", kAllowAll, [](int) { return numbered(100'000); }, false, false},
        {"disjoint_10k_allowed", kAllowAll, [](int t) { return numbered(10'000 * static_cast<size_t>(t)); }, true, false},
        {"disjoint_10k_try_acquire", kAllowAll, [](int t) { return numbered(10'000 * static_cast<size_t>(t)); }, true, true},
    };
    std::printf("%-26s %7s %10s %6s %8s %8s %8s %8s %10s\n", "scenario", "threads", "Mops/s", "cpu%", "p50 ns",
                "p90 ns", "p99 ns", "p99.9 ns", "max ns");
    for (const Scenario& s : scenarios) {
        for (int threads = 1; threads <= max_threads; threads *= 2) {
            const Result r = run(s, threads, seconds, overhead);
            auto ns = [&](uint64_t t) { return static_cast<double>(t) / tpn; };
            std::printf("%-26s %7d %10.1f %6.0f %8.1f %8.1f %8.1f %8.1f %10.0f\n", s.name, threads, r.mops, r.cpu_pct,
                        ns(r.hist.percentile(0.5)), ns(r.hist.percentile(0.9)), ns(r.hist.percentile(0.99)),
                        ns(r.hist.percentile(0.999)), ns(r.hist.max));
        }
    }
}
