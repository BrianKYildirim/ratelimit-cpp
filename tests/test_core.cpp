
#include <gtest/gtest.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>
#include "ratelimit/rate_limiter.h"
#include "ratelimit/clock.h"

struct FakeClock : rl::Clock {
    uint64_t t = 0;
    uint64_t now_ns() const noexcept override { return t; }
    void advance_ns(uint64_t ns) { t += ns; }
};

// Thread-safe clock that moves forward by `step` ns on every read.
struct SteppingClock : rl::Clock {
    explicit SteppingClock(uint64_t step) : step(step) {}
    uint64_t step;
    mutable std::atomic<uint64_t> t{0};
    uint64_t now_ns() const noexcept override { return t.fetch_add(step, std::memory_order_relaxed); }
};

constexpr uint64_t kMs = 1'000'000;
constexpr uint64_t kSec = 1'000'000'000;

TEST(Core, InitialFull) {
    FakeClock fc;
    rl::RateLimiterInProc::Config cfg;
    rl::RateLimiterInProc rl(cfg, &fc);
    rl::Limit lim{5, 10}; // 5 capacity, 10 tokens/sec
    auto d = rl.allow("k", lim);
    EXPECT_TRUE(d.allowed);
    EXPECT_EQ(d.remaining, 4);
}

TEST(Core, NoRefillSameTick) {
    FakeClock fc;
    rl::RateLimiterInProc::Config cfg;
    rl::RateLimiterInProc rl(cfg, &fc);
    rl::Limit lim{2, 100}; // 2 cap
    auto d1 = rl.allow("a", lim);
    auto d2 = rl.allow("a", lim);
    auto d3 = rl.allow("a", lim);
    EXPECT_TRUE(d1.allowed);
    EXPECT_TRUE(d2.allowed);
    EXPECT_FALSE(d3.allowed);
    EXPECT_GT(d3.reset_ms, 0u);
}

TEST(Core, RefillOverTime) {
    FakeClock fc;
    rl::RateLimiterInProc::Config cfg;
    rl::RateLimiterInProc rl(cfg, &fc);
    rl::Limit lim{1, 10}; // 10/s
    auto d1 = rl.allow("x", lim);
    EXPECT_TRUE(d1.allowed);
    auto d2 = rl.allow("x", lim);
    EXPECT_FALSE(d2.allowed);
    fc.advance_ns(150000000ULL); // 0.15s -> 1.5 tokens => clamp at 1
    auto d3 = rl.allow("x", lim);
    EXPECT_TRUE(d3.allowed);
}

TEST(Core, KeyIsolation) {
    FakeClock fc;
    rl::RateLimiterInProc::Config cfg;
    rl::RateLimiterInProc rl(cfg, &fc);
    rl::Limit lim{1, 1};
    auto a1 = rl.allow("a", lim);
    auto b1 = rl.allow("b", lim);
    EXPECT_TRUE(a1.allowed);
    EXPECT_TRUE(b1.allowed);
    auto a2 = rl.allow("a", lim);
    auto b2 = rl.allow("b", lim);
    EXPECT_FALSE(a2.allowed);
    EXPECT_FALSE(b2.allowed);
}

TEST(Core, CapacityZero) {
    FakeClock fc;
    rl::RateLimiterInProc::Config cfg;
    rl::RateLimiterInProc rl(cfg, &fc);
    rl::Limit lim{0, 0};
    auto d = rl.allow("k", lim);
    EXPECT_FALSE(d.allowed);
    EXPECT_EQ(d.reset_ms, 0u);
}

TEST(Core, CapacityZeroWithRefillNeverAllows) {
    FakeClock fc;
    rl::RateLimiterInProc rl({}, &fc);
    for (int i = 0; i < 3; ++i, fc.advance_ns(kSec)) {
        auto d = rl.allow("k", {0, 1000});
        EXPECT_FALSE(d.allowed);
        EXPECT_EQ(d.reset_ms, 0u); // a token can never become available
    }
}

TEST(Core, NoRefillNeverRecovers) {
    FakeClock fc;
    rl::RateLimiterInProc rl({}, &fc);
    EXPECT_TRUE(rl.allow("k", {2, 0}).allowed);
    EXPECT_TRUE(rl.allow("k", {2, 0}).allowed);
    fc.advance_ns(1000 * kSec);
    auto d = rl.allow("k", {2, 0});
    EXPECT_FALSE(d.allowed);
    EXPECT_EQ(d.reset_ms, 0u);
}

TEST(Core, ResetMsIsExactAndRoundsUp) {
    FakeClock fc;
    rl::RateLimiterInProc rl({}, &fc);
    rl::Limit lim{1, 10}; // one token per 100 ms
    EXPECT_TRUE(rl.allow("k", lim).allowed);
    EXPECT_EQ(rl.allow("k", lim).reset_ms, 100u);
    fc.advance_ns(30 * kMs);
    EXPECT_EQ(rl.allow("k", lim).reset_ms, 70u);
    fc.advance_ns(1); // 69.999999 ms left -> rounds up
    EXPECT_EQ(rl.allow("k", lim).reset_ms, 70u);
    fc.advance_ns(70 * kMs - 2); // 1 ns left
    EXPECT_EQ(rl.allow("k", lim).reset_ms, 1u);
    fc.advance_ns(1);
    EXPECT_TRUE(rl.allow("k", lim).allowed);

    rl::Limit fast{1, 3'000'000}; // a token every 333.3 ns
    EXPECT_TRUE(rl.allow("f", fast).allowed);
    EXPECT_EQ(rl.allow("f", fast).reset_ms, 1u);

    rl::Limit huge{1, std::numeric_limits<uint64_t>::max()}; // rate * 1e6 overflows
    EXPECT_TRUE(rl.allow("h", huge).allowed);
    auto d = rl.allow("h", huge);
    EXPECT_FALSE(d.allowed);
    EXPECT_EQ(d.reset_ms, 1u);
}

TEST(Core, FractionalRefillAccumulatesWithoutDrift) {
    FakeClock fc;
    rl::RateLimiterInProc rl({}, &fc);
    // A token every 333'333'333.3 ns: not a whole number of ns. Capacity 2 keeps
    // the balance below capacity, so no refill is clipped.
    rl::Limit lim{2, 3};
    EXPECT_TRUE(rl.allow("k", lim).allowed);
    // Many tiny refills must add up exactly.
    int allowed = 0;
    for (int i = 0; i < 3'000; ++i) {
        fc.advance_ns(kMs);
        allowed += rl.allow("k", lim).allowed;
    }
    EXPECT_EQ(allowed, 10); // 1 left over + 3 s at 3 tokens/s
    // The next token arrives exactly 1e9 / 3 ns (rounded up) after an empty bucket.
    FakeClock fc2;
    rl::RateLimiterInProc rl2({}, &fc2);
    const rl::Limit one{1, 3};
    EXPECT_TRUE(rl2.allow("k", one).allowed);
    for (int i = 0; i < 333'333; ++i) {
        fc2.advance_ns(1000);
        ASSERT_FALSE(rl2.allow("k", one).allowed) << i;
    }
    fc2.advance_ns(333);
    EXPECT_FALSE(rl2.allow("k", one).allowed); // 333'333'333 ns * 3 = 999'999'999 nanotokens
    fc2.advance_ns(1);
    EXPECT_TRUE(rl2.allow("k", one).allowed);
}

TEST(Core, BurstThenSteadyRate) {
    FakeClock fc;
    rl::RateLimiterInProc rl({}, &fc);
    rl::Limit lim{10, 100};
    int allowed = 0;
    for (int i = 0; i < 20; ++i) allowed += rl.allow("k", lim).allowed;
    EXPECT_EQ(allowed, 10);
    allowed = 0;
    for (int i = 0; i < 1000; ++i) { // 1 s in 1 ms steps, 2 requests per step
        fc.advance_ns(kMs);
        allowed += rl.allow("k", lim).allowed;
        allowed += rl.allow("k", lim).allowed;
    }
    EXPECT_EQ(allowed, 100);
}

TEST(Core, RemainingCountsDown) {
    FakeClock fc;
    rl::RateLimiterInProc rl({}, &fc);
    for (uint64_t i = 0; i < 5; ++i) EXPECT_EQ(rl.allow("k", {5, 1}).remaining, 4 - i);
    EXPECT_EQ(rl.allow("k", {5, 1}).remaining, 0u);
}

TEST(Core, LargeCapacity) {
    FakeClock fc;
    rl::RateLimiterInProc rl({}, &fc);
    const uint64_t cap = (1ULL << 33) + 5; // overflowed the old Q32.32 representation
    auto d = rl.allow("k", {cap, 1});
    EXPECT_TRUE(d.allowed);
    EXPECT_EQ(d.remaining, cap - 1);
}

TEST(Core, CapacityAboveMaxIsClamped) {
    FakeClock fc;
    rl::RateLimiterInProc rl({}, &fc);
    const uint64_t max_cap = std::numeric_limits<uint64_t>::max() / 1'000'000'000ULL;
    auto d = rl.allow("k", {std::numeric_limits<uint64_t>::max(), 1});
    EXPECT_TRUE(d.allowed);
    EXPECT_EQ(d.remaining, max_cap - 1);
    fc.advance_ns(std::numeric_limits<uint64_t>::max() / 2);
    EXPECT_EQ(rl.allow("k", {std::numeric_limits<uint64_t>::max(), 1}).remaining, max_cap - 1);
}

TEST(Core, HugeRateAndIdleDoNotOverflow) {
    FakeClock fc;
    rl::RateLimiterInProc rl({}, &fc);
    const rl::Limit lim{3, std::numeric_limits<uint64_t>::max()};
    for (int i = 0; i < 3; ++i) EXPECT_TRUE(rl.allow("k", lim).allowed);
    EXPECT_FALSE(rl.allow("k", lim).allowed);
    fc.advance_ns(1);
    EXPECT_EQ(rl.allow("k", lim).remaining, 2u); // refilled to capacity, not wrapped
    fc.advance_ns(std::numeric_limits<uint64_t>::max() / 2);
    EXPECT_EQ(rl.allow("k", {3, 1'000'000'000}).remaining, 2u);
}

TEST(Core, ClockGoingBackwardsAddsNoTokens) {
    FakeClock fc;
    fc.t = 10 * kSec;
    rl::RateLimiterInProc rl({}, &fc);
    rl::Limit lim{1, 1};
    EXPECT_TRUE(rl.allow("k", lim).allowed);
    fc.t = 5 * kSec;
    EXPECT_FALSE(rl.allow("k", lim).allowed);
    fc.t = 10 * kSec + kSec - 1; // refill is measured from the latest time seen
    EXPECT_FALSE(rl.allow("k", lim).allowed);
    fc.t = 11 * kSec;
    EXPECT_TRUE(rl.allow("k", lim).allowed);
}

// Denials within 1 ms of a key's recorded time are not recorded (so hot
// over-limit keys can be denied without writes), but a key that keeps being
// denied is still recorded at least every millisecond.
TEST(Core, ContinuouslyDeniedKeyStaysRecent) {
    FakeClock fc;
    rl::RateLimiterInProc rl({}, &fc);
    const rl::Limit lim{1, 0};
    EXPECT_TRUE(rl.allow("k", lim).allowed);
    for (int i = 0; i < 40; ++i) {
        fc.advance_ns(kMs / 4);
        EXPECT_FALSE(rl.allow("k", lim).allowed);
    }
    EXPECT_EQ(rl.evict_idle(2 * kMs), 0u); // last recorded within the last millisecond
    fc.advance_ns(2 * kMs);
    EXPECT_EQ(rl.evict_idle(2 * kMs), 1u);
}

TEST(Core, QuietDenialsDoNotChangeLaterVerdicts) {
    // A token every ~1.5 ms, probed every 0.1 ms: most denials are not
    // recorded, yet no refill is lost or duplicated. Capacity 2 keeps the
    // balance below capacity, so no refill is clipped either.
    FakeClock fc;
    rl::RateLimiterInProc rl({}, &fc);
    const rl::Limit lim{2, 666}; // 1501501.5 ns per token
    EXPECT_TRUE(rl.allow("k", lim).allowed);
    uint64_t grants = 0;
    for (int i = 0; i < 100'000; ++i) {
        fc.advance_ns(100'000);
        grants += rl.allow("k", lim).allowed;
    }
    EXPECT_EQ(grants, 1u + 10 * 666); // 1 left over + 10 s at 666 tokens/s
    // Remaining fraction: exact, so a token arrives exactly when 1e9 nanotokens accumulate.
    rl::RateLimiterInProc rl2({}, &fc);
    EXPECT_TRUE(rl2.allow("k", {1, 3}).allowed);
    const uint64_t start = fc.t;
    while (fc.t - start < 333'333'333 - 50'000) {
        fc.advance_ns(50'000);
        ASSERT_FALSE(rl2.try_acquire("k", {1, 3}));
    }
    fc.t = start + 333'333'333;
    EXPECT_FALSE(rl2.try_acquire("k", {1, 3})); // 999'999'999 nanotokens
    fc.t += 1;
    EXPECT_TRUE(rl2.try_acquire("k", {1, 3}));
}

TEST(Core, LoweringCapacityClampsBalance) {
    FakeClock fc;
    rl::RateLimiterInProc rl({}, &fc);
    EXPECT_EQ(rl.allow("k", {100, 1}).remaining, 99u);
    EXPECT_EQ(rl.allow("k", {5, 1}).remaining, 4u);
    EXPECT_EQ(rl.allow("k", {100, 1}).remaining, 3u); // raising it again does not mint tokens
}

TEST(Core, ZeroShardsConfigIsValid) {
    FakeClock fc;
    rl::RateLimiterInProc rl({0, 0}, &fc);
    EXPECT_TRUE(rl.allow("k", {1, 1}).allowed);
    EXPECT_FALSE(rl.allow("k", {1, 1}).allowed);
    rl::RateLimiterInProc rl3({3, 1}, &fc); // non-power-of-two
    EXPECT_TRUE(rl3.allow("k", {1, 1}).allowed);
}

TEST(Core, KeysWithEmbeddedNulAndEmptyKey) {
    FakeClock fc;
    rl::RateLimiterInProc rl({}, &fc);
    using namespace std::literals;
    EXPECT_TRUE(rl.allow(""sv, {1, 1}).allowed);
    EXPECT_FALSE(rl.allow(""sv, {1, 1}).allowed);
    EXPECT_TRUE(rl.allow("a\0b"sv, {1, 1}).allowed);
    EXPECT_TRUE(rl.allow("a\0c"sv, {1, 1}).allowed);
    EXPECT_TRUE(rl.allow("a"sv, {1, 1}).allowed);
    EXPECT_EQ(rl.size(), 4u);
}

TEST(Core, KeyLengthsAroundInlineLimit) {
    FakeClock fc;
    rl::RateLimiterInProc rl({1, 1}, &fc);
    std::vector<std::string> keys;
    for (size_t len = 0; len <= 64; ++len) {
        keys.push_back(std::string(len, 'a'));
        if (len > 0) keys.push_back(std::string(len - 1, 'a') + 'b'); // differs only in the last byte
        if (len > 1) keys.push_back('b' + std::string(len - 1, 'a'));  // differs only in the first byte
    }
    for (auto& k : keys) ASSERT_TRUE(rl.allow(k, {1, 0}).allowed) << k.size();
    EXPECT_EQ(rl.size(), keys.size());
    for (auto& k : keys) ASSERT_FALSE(rl.allow(k, {1, 0}).allowed) << k.size();
}

TEST(Core, ManyKeysSurviveGrowth) {
    FakeClock fc;
    rl::RateLimiterInProc rl({4, 2}, &fc); // tiny tables force many rehashes
    const rl::Limit lim{1, 1};
    const int n = 50'000;
    std::vector<std::string> keys;
    for (int i = 0; i < n; ++i) keys.push_back("key-of-varying-length-" + std::string(i % 23, 'x') + std::to_string(i));
    for (auto& k : keys) ASSERT_TRUE(rl.allow(k, lim).allowed) << k;
    EXPECT_EQ(rl.size(), static_cast<size_t>(n));
    for (auto& k : keys) ASSERT_FALSE(rl.allow(k, lim).allowed) << k;
    EXPECT_EQ(rl.size(), static_cast<size_t>(n));
}

TEST(Core, ClearForgetsKeys) {
    FakeClock fc;
    rl::RateLimiterInProc rl({}, &fc);
    EXPECT_TRUE(rl.allow("k", {1, 1}).allowed);
    EXPECT_FALSE(rl.allow("k", {1, 1}).allowed);
    rl.clear();
    EXPECT_EQ(rl.size(), 0u);
    EXPECT_TRUE(rl.allow("k", {1, 1}).allowed);
    EXPECT_EQ(rl.size(), 1u);
}

TEST(Core, EvictIdle) {
    FakeClock fc;
    rl::RateLimiterInProc rl({2, 4}, &fc);
    const rl::Limit lim{1, 1};
    for (int i = 0; i < 100; ++i) rl.allow("old" + std::to_string(i), lim);
    fc.advance_ns(5 * kSec);
    for (int i = 0; i < 100; ++i) rl.allow("new" + std::to_string(i), lim);
    EXPECT_EQ(rl.size(), 200u);
    EXPECT_EQ(rl.evict_idle(10 * kSec), 0u);
    EXPECT_EQ(rl.evict_idle(5 * kSec), 100u);
    EXPECT_EQ(rl.size(), 100u);
    for (int i = 0; i < 100; ++i) EXPECT_FALSE(rl.allow("new" + std::to_string(i), lim).allowed);
    EXPECT_TRUE(rl.allow("old0", lim).allowed); // re-created full
    fc.advance_ns(kSec);
    EXPECT_EQ(rl.evict_idle(0), 101u);
    EXPECT_EQ(rl.size(), 0u);
    EXPECT_TRUE(rl.allow("new0", lim).allowed);
}

TEST(Core, DefaultClockWorks) {
    rl::RateLimiterInProc rl;
    EXPECT_TRUE(rl.allow("k", {1, 0}).allowed);
    EXPECT_FALSE(rl.allow("k", {1, 0}).allowed);
}

// ---------- Concurrency ----------

TEST(Concurrency, HotKeyGrantsExactlyCapacity) {
    rl::RateLimiterInProc rl;
    const uint64_t cap = 10'000;
    std::atomic<uint64_t> allowed{0};
    std::vector<std::thread> ts;
    for (int t = 0; t < 8; ++t) {
        ts.emplace_back([&] {
            uint64_t mine = 0;
            for (int i = 0; i < 5'000; ++i) mine += rl.allow("hot", {cap, 0}).allowed;
            allowed += mine;
        });
    }
    for (auto& t : ts) t.join();
    EXPECT_EQ(allowed.load(), cap);
}

TEST(Concurrency, RefillIsNeitherLostNorDuplicated) {
    // Every clock read advances 1 us; demand far exceeds supply, so the bucket
    // stays near empty and grants must track capacity + rate * elapsed time.
    SteppingClock clk(1000);
    rl::RateLimiterInProc rl({}, &clk);
    const rl::Limit lim{50, 10'000}; // 1 token per 100 us = per 100 calls
    std::atomic<uint64_t> allowed{0};
    const int threads = 8, per_thread = 50'000;
    std::vector<std::thread> ts;
    for (int t = 0; t < threads; ++t) {
        ts.emplace_back([&] {
            uint64_t mine = 0;
            for (int i = 0; i < per_thread; ++i) mine += rl.allow("hot", lim).allowed;
            allowed += mine;
        });
    }
    for (auto& t : ts) t.join();
    const uint64_t elapsed_ns = clk.t.load();
    const uint64_t expected = 50 + elapsed_ns / 100'000;
    EXPECT_LE(allowed.load(), expected + 1);
    EXPECT_GE(allowed.load() + threads + 1, expected);
}

TEST(Concurrency, RefillKeepsUpWithEqualDemand) {
    // Each clock read advances 1 us = exactly 1 token of refill, so supply equals
    // demand and the 1000-token buffer never drains unless refills are lost.
    SteppingClock clk(1000);
    rl::RateLimiterInProc rl({}, &clk);
    const rl::Limit lim{1000, 1'000'000};
    const int threads = 8, per_thread = 100'000;
    std::atomic<uint64_t> denied{0};
    std::vector<std::thread> ts;
    for (int t = 0; t < threads; ++t) {
        ts.emplace_back([&] {
            uint64_t mine = 0;
            for (int i = 0; i < per_thread; ++i) mine += !rl.allow("hot", lim).allowed;
            denied += mine;
        });
    }
    for (auto& t : ts) t.join();
    EXPECT_EQ(denied.load(), 0u);
}

TEST(Concurrency, ManyKeysWithConcurrentInsertsAndEviction) {
    rl::RateLimiterInProc rl({8, 1});
    const int keys = 2'000, threads = 8;
    const uint64_t cap = 3;
    std::vector<std::atomic<int>> granted(keys);
    std::atomic<bool> stop{false};
    std::thread evictor([&] {
        while (!stop.load()) {
            rl.evict_idle(std::numeric_limits<uint64_t>::max()); // never matches; exercises the scan
            (void)rl.size();
        }
    });
    std::vector<std::thread> ts;
    for (int t = 0; t < threads; ++t) {
        ts.emplace_back([&, t] {
            for (int round = 0; round < 3; ++round) {
                for (int i = 0; i < keys; ++i) {
                    const int k = (i * 7 + t * 131) % keys;
                    if (rl.allow("k" + std::to_string(k), {cap, 0}).allowed) granted[k]++;
                }
            }
        });
    }
    for (auto& t : ts) t.join();
    stop = true;
    evictor.join();
    EXPECT_EQ(rl.size(), static_cast<size_t>(keys));
    for (int k = 0; k < keys; ++k) ASSERT_EQ(granted[k].load(), static_cast<int>(cap)) << k;
}

// ---------- Fast paths ----------

TEST(Api, TryAcquireMakesTheSameDecisionAsAllow) {
    FakeClock fc;
    rl::RateLimiterInProc a({}, &fc), b({}, &fc);
    const rl::Limit lim{3, 2};
    for (int i = 0; i < 2000; ++i) {
        fc.advance_ns((i * 7919) % 400 * kMs);
        const std::string key = "k" + std::to_string(i % 5);
        ASSERT_EQ(a.allow(key, lim).allowed, b.try_acquire(key, lim)) << i;
    }
}

TEST(Api, CallerSuppliedTimestamps) {
    FakeClock fc;
    fc.t = 50 * kSec;
    rl::RateLimiterInProc rl({}, &fc);
    EXPECT_EQ(rl.now_ns(), 50 * kSec);
    const rl::Limit lim{1, 1};
    const uint64_t t = rl.now_ns();
    EXPECT_TRUE(rl.try_acquire("k", lim, t));
    EXPECT_FALSE(rl.try_acquire("k", lim, t + kSec - 1));
    EXPECT_FALSE(rl.try_acquire("k", lim, t)); // earlier than the last call: no refill
    auto d = rl.allow("k", lim, t + kSec - 1);
    EXPECT_FALSE(d.allowed);
    EXPECT_EQ(d.reset_ms, 1u);
    EXPECT_TRUE(rl.allow("k", lim, t + kSec).allowed);
    // The limiter's own clock is not consulted by these calls.
    EXPECT_FALSE(rl.allow("k", lim).allowed);
}

// ---------- Differential test against an independent model ----------

// The token-bucket semantics written separately from the engine: a std::map,
// and exact comparisons by division instead of the engine's overflow checks.
// A denial within 1 ms of the key's recorded time leaves the key unchanged
// (unless a lowered capacity clamps its balance).
struct ModelLimiter {
    static constexpr uint64_t kUnit = 1'000'000'000;
    static constexpr uint64_t kQuietNs = 1'000'000;
    struct Bucket {
        uint64_t tokens, last;
    };
    std::map<std::string, Bucket> buckets;

    // Whether a * b >= c, exactly: a >= ceil(c / b).
    static bool product_at_least(uint64_t a, uint64_t b, uint64_t c) {
        if (a == 0 || b == 0) return c == 0;
        return a >= c / b + (c % b != 0);
    }
    static uint64_t ceil_div(uint64_t a, uint64_t b) { return a / b + (a % b != 0); }

    // The balance after refilling `tokens` for `dt` ns, saturating at `cap`.
    static uint64_t refilled(uint64_t tokens, uint64_t dt, uint64_t rate, uint64_t cap) {
        return product_at_least(dt, rate, cap - tokens) ? cap : tokens + dt * rate; // dt * rate < cap - tokens
    }

    rl::Decision allow(const std::string& key, rl::Limit lim, uint64_t now) {
        const uint64_t cap = std::min<uint64_t>(lim.capacity, std::numeric_limits<uint64_t>::max() / kUnit) * kUnit;
        auto it = buckets.find(key);
        if (it == buckets.end()) it = buckets.emplace(key, Bucket{cap, now}).first;
        Bucket& b = it->second;
        uint64_t tokens = std::min(b.tokens, cap);
        const uint64_t at = std::max(b.last, now);
        if (now > b.last) tokens = refilled(tokens, now - b.last, lim.refill_per_sec, cap);
        if (tokens >= kUnit) {
            b.tokens = tokens - kUnit;
            b.last = at;
            return {true, b.tokens / kUnit, 0};
        }
        if (b.tokens > cap || at - b.last >= kQuietNs) {
            b.tokens = tokens;
            b.last = at;
        }
        if (lim.refill_per_sec == 0 || cap < kUnit) return {false, 0, 0};
        // Nanotokens per ms overflow 64 bits: a token arrives within 1 ms.
        if (lim.refill_per_sec > std::numeric_limits<uint64_t>::max() / 1'000'000) return {false, 0, 1};
        return {false, 0, ceil_div(kUnit - tokens, lim.refill_per_sec * 1'000'000)};
    }

    size_t evict_idle(uint64_t idle, uint64_t now) {
        return std::erase_if(buckets, [&](const auto& kv) { return now >= kv.second.last && now - kv.second.last >= idle; });
    }

    // What inspect() must report: the bucket allow() would see at `now`, and
    // how many ns of refill (from max(now, last)) reach a token and capacity.
    rl::BucketState inspect(const std::string& key, rl::Limit lim, uint64_t now) const {
        const uint64_t cap = std::min<uint64_t>(lim.capacity, std::numeric_limits<uint64_t>::max() / kUnit) * kUnit;
        const uint64_t rate = lim.refill_per_sec;
        rl::BucketState s{};
        uint64_t tokens = cap, last = now; // a new key starts full
        if (auto it = buckets.find(key); it != buckets.end()) {
            s.tracked = true;
            tokens = s.stored_nanotokens = it->second.tokens;
            last = s.stored_at_ns = it->second.last;
        }
        uint64_t avail = std::min(tokens, cap);
        if (now > last) avail = refilled(avail, now - last, rate, cap);
        auto wait = [&](uint64_t target) -> uint64_t {
            if (avail >= target) return 0;
            if (rate == 0 || target > cap) return rl::BucketState::kNever;
            return ceil_div(target - avail, rate);
        };
        s.nanotokens = avail;
        s.tokens = s.nanotokens / kUnit;
        s.until_token_ns = wait(kUnit);
        s.until_full_ns = wait(cap);
        s.would_allow = s.nanotokens >= kUnit;
        return s;
    }
};

TEST(Differential, MatchesModelOverRandomOperations) {
    using namespace std::literals;
    constexpr uint64_t kMax = std::numeric_limits<uint64_t>::max();
    const std::vector<std::string> keys = {
        "", "a", "b", "key_1", "key_2", "abcdefgh", std::string("a\0b", 3),
        std::string(27, 'x'), std::string(28, 'x'), std::string(29, 'x'), std::string(28, 'y'), std::string(29, 'y'),
        std::string(64, 'z'), std::string(200, 'q'), "tenant:acme/user:42", "tenant:acme/user:43",
    };
    const std::vector<rl::Limit> limits = {
        {0, 0}, {1, 0}, {1, 1}, {2, 3}, {5, 10}, {10, 1000}, {3, kMax}, {kMax, 1}, {1000, 1'000'000},
        {1, 3'000'000}, {kMax / 1'000'000'000, 7}, {7, 0}, {100, 1},
    };
    for (uint64_t seed = 1; seed <= 12; ++seed) {
        std::mt19937_64 rng(seed);
        auto pick = [&](uint64_t n) { return static_cast<size_t>(rng() % n); };
        FakeClock fc;
        fc.t = 1'000'000 * kSec;
        // Tiny shard counts and tables force frequent growth and rebuilds.
        rl::RateLimiterInProc rl({size_t{1} << pick(3), pick(3)}, &fc);
        ModelLimiter model;
        for (int op = 0; op < 20'000; ++op) {
            switch (pick(6)) { // advance the clock
            case 0: case 1: break;
            case 2: fc.t += rng() % 1000; break;
            case 3: fc.t += rng() % (200 * kMs); break;
            case 4: fc.t += rng() % (20 * kSec); break;
            default: fc.t -= std::min<uint64_t>(fc.t, rng() % kSec); break; // backwards
            }
            const std::string& key = keys[pick(keys.size())];
            const rl::Limit lim = limits[pick(limits.size())];
            const unsigned kind = static_cast<unsigned>(pick(100));
            if (kind < 50) {
                const rl::Decision want = model.allow(key, lim, fc.t);
                const rl::Decision got = rl.allow(key, lim);
                ASSERT_EQ(got.allowed, want.allowed) << "seed " << seed << " op " << op;
                ASSERT_EQ(got.remaining, want.remaining) << "seed " << seed << " op " << op;
                ASSERT_EQ(got.reset_ms, want.reset_ms) << "seed " << seed << " op " << op;
            } else if (kind < 75) {
                ASSERT_EQ(rl.try_acquire(key, lim), model.allow(key, lim, fc.t).allowed) << "seed " << seed << " op " << op;
            } else if (kind < 90) {
                const uint64_t at = fc.t + rng() % (2 * kSec) - kSec; // around the clock, either side
                if (kind & 1) {
                    ASSERT_EQ(rl.try_acquire(key, lim, at), model.allow(key, lim, at).allowed) << "seed " << seed;
                } else {
                    const rl::Decision want = model.allow(key, lim, at);
                    const rl::Decision got = rl.allow(key, lim, at);
                    ASSERT_EQ(got.allowed, want.allowed) << "seed " << seed << " op " << op;
                    ASSERT_EQ(got.remaining, want.remaining) << "seed " << seed << " op " << op;
                    ASSERT_EQ(got.reset_ms, want.reset_ms) << "seed " << seed << " op " << op;
                }
            } else if (kind < 96) {
                ASSERT_EQ(rl.size(), model.buckets.size()) << "seed " << seed << " op " << op;
            } else if (kind < 99) {
                const uint64_t idle = rng() % (30 * kSec);
                ASSERT_EQ(rl.evict_idle(idle), model.evict_idle(idle, fc.t)) << "seed " << seed << " op " << op;
            } else {
                rl.clear();
                model.buckets.clear();
            }
        }
        ASSERT_EQ(rl.size(), model.buckets.size());
    }
}

// ---------- Inspection ----------

constexpr uint64_t kToken = ModelLimiter::kUnit; // nanotokens per token
constexpr uint64_t kNever = rl::BucketState::kNever;

static std::string str(const rl::BucketState& s) {
    char buf[256];
    std::snprintf(buf, sizeof buf,
                  "{tracked=%d stored=%llu@%llu nanotokens=%llu tokens=%llu until_token=%llu until_full=%llu allow=%d}",
                  s.tracked, (unsigned long long)s.stored_nanotokens, (unsigned long long)s.stored_at_ns,
                  (unsigned long long)s.nanotokens, (unsigned long long)s.tokens,
                  (unsigned long long)s.until_token_ns, (unsigned long long)s.until_full_ns, s.would_allow);
    return buf;
}

static std::string str(const rl::Decision& d) {
    return "{allowed=" + std::to_string(d.allowed) + " remaining=" + std::to_string(d.remaining) +
           " reset_ms=" + std::to_string(d.reset_ms) + "}";
}

static rl::BucketState stored(uint64_t nanotokens, uint64_t at, uint64_t avail, uint64_t until_token,
                              uint64_t until_full) {
    return {true, nanotokens, at, avail, avail / kToken, until_token, until_full, avail >= kToken};
}

static rl::BucketState untracked(uint64_t avail, uint64_t until_token) {
    return {false, 0, 0, avail, avail / kToken, until_token, 0, avail >= kToken};
}

TEST(Inspect, UntrackedKeyIsNotInsertedAndChangesNothing) {
    FakeClock fc;
    fc.t = 7 * kSec;
    rl::RateLimiterInProc empty({}, &fc); // no shard has a table yet
    EXPECT_EQ(str(empty.inspect("k", {3, 10})), str(untracked(3 * kToken, 0))); // a new key starts full
    EXPECT_EQ(empty.size(), 0u);

    rl::RateLimiterInProc rl({}, &fc), twin({}, &fc);
    const rl::Limit lim{3, 10};
    EXPECT_TRUE(rl.allow("other", lim).allowed);
    EXPECT_TRUE(twin.allow("other", lim).allowed);
    for (int i = 0; i < 3; ++i) EXPECT_EQ(str(rl.inspect("k", lim)), str(untracked(3 * kToken, 0)));
    EXPECT_EQ(str(rl.inspect("zero", {0, 5})), str(untracked(0, kNever))); // never a token, already full
    EXPECT_EQ(rl.size(), 1u);
    for (int i = 0; i < 6; ++i, fc.advance_ns(40 * kMs)) {
        ASSERT_EQ(str(rl.allow("k", lim)), str(twin.allow("k", lim))) << i;
        ASSERT_EQ(rl.try_acquire("zero", {0, 5}), twin.try_acquire("zero", {0, 5})) << i;
    }
    EXPECT_EQ(rl.size(), twin.size());
}

TEST(Inspect, ReportsStoredStateAndExactRefill) {
    FakeClock fc;
    fc.t = 100 * kSec;
    rl::RateLimiterInProc rl({}, &fc);
    const rl::Limit lim{5, 10}; // a token per 100 ms
    for (int i = 0; i < 5; ++i) ASSERT_TRUE(rl.allow("k", lim).allowed);
    const uint64_t t0 = fc.t;
    EXPECT_EQ(str(rl.inspect("k", lim)), str(stored(0, t0, 0, 100 * kMs, 500 * kMs)));
    fc.advance_ns(150 * kMs); // 1.5 tokens of refill, not yet stored
    const rl::BucketState s = rl.inspect("k", lim);
    EXPECT_EQ(str(s), str(stored(0, t0, 1'500'000'000, 0, 350 * kMs)));
    EXPECT_EQ(str(rl.inspect("k", lim, fc.t)), str(s));
    EXPECT_EQ(rl.allow("k", lim).remaining, 0u); // stores what the inspection derived, minus a token
    EXPECT_EQ(str(rl.inspect("k", lim)), str(stored(500'000'000, fc.t, 500'000'000, 50 * kMs, 450 * kMs)));

    // A token every 333'333'333.3 ns: not a whole number of ns.
    const rl::Limit third{1, 3};
    ASSERT_TRUE(rl.allow("f", third).allowed);
    const uint64_t t1 = fc.t;
    EXPECT_EQ(str(rl.inspect("f", third)), str(stored(0, t1, 0, 333'333'334, 333'333'334)));
    EXPECT_EQ(str(rl.inspect("f", third, t1 + 333'333'333)), str(stored(0, t1, 999'999'999, 1, 1)));
    EXPECT_EQ(str(rl.inspect("f", third, t1 + 333'333'334)), str(stored(0, t1, kToken, 0, 0))); // clamped
    EXPECT_FALSE(rl.try_acquire("f", third, t1 + 333'333'333));
    EXPECT_TRUE(rl.try_acquire("f", third, t1 + 333'333'334));
}

// until_token_ns and until_full_ns are the first nanosecond at which a call
// is allowed or the bucket is full, checked against real decisions.
TEST(Inspect, WaitsAreTheFirstNanosecond) {
    std::mt19937_64 rng(99);
    const uint64_t rates[] = {1, 3, 7, 10, 666, 1'000'000, 3'000'000, 999'999'999, 1'000'000'007, 123'456'789'012};
    const uint64_t t0 = 1000 * kSec;
    for (int c = 0; c < 300; ++c) {
        const rl::Limit lim{1 + rng() % 20, rates[rng() % std::size(rates)]};
        const uint64_t drained = 1 + rng() % lim.capacity, at = t0 + rng() % (2 * kSec);
        FakeClock fc;
        // A limiter whose key had `drained` tokens taken at t0.
        auto make = [&] {
            auto r = std::make_unique<rl::RateLimiterInProc>(rl::RateLimiterInProc::Config{1, 1}, &fc);
            for (uint64_t i = 0; i < drained; ++i) r->allow("k", lim, t0);
            return r;
        };
        const rl::BucketState s = make()->inspect("k", lim, at);
        ASSERT_TRUE(s.tracked);
        ASSERT_NE(s.until_token_ns, kNever);
        ASSERT_NE(s.until_full_ns, kNever);
        const std::string where = "case " + std::to_string(c) + " " + str(s);
        if (s.until_token_ns != 0) {
            ASSERT_FALSE(make()->try_acquire("k", lim, at + s.until_token_ns - 1)) << where;
            ASSERT_TRUE(make()->try_acquire("k", lim, at + s.until_token_ns)) << where;
        }
        if (s.until_full_ns != 0) {
            auto r = make();
            ASSERT_LT(r->inspect("k", lim, at + s.until_full_ns - 1).nanotokens, lim.capacity * kToken) << where;
            ASSERT_EQ(r->inspect("k", lim, at + s.until_full_ns).nanotokens, lim.capacity * kToken) << where;
            ASSERT_EQ(r->allow("k", lim, at + s.until_full_ns).remaining, lim.capacity - 1) << where;
        }
    }
}

// The same rounding as Decision::reset_ms: ceil(until_token_ns / 1 ms).
TEST(Inspect, WaitMatchesResetMsRounding) {
    FakeClock fc;
    rl::RateLimiterInProc rl({}, &fc);
    auto ms = [](const rl::BucketState& s) { return (s.until_token_ns - 1) / kMs + 1; };
    const rl::Limit lim{1, 10}; // one token per 100 ms
    EXPECT_TRUE(rl.allow("k", lim).allowed);
    EXPECT_EQ(rl.inspect("k", lim).until_token_ns, 100 * kMs);
    fc.advance_ns(30 * kMs + 1); // 69.999999 ms left
    EXPECT_EQ(rl.inspect("k", lim).until_token_ns, 70 * kMs - 1);
    EXPECT_EQ(ms(rl.inspect("k", lim)), 70u);
    EXPECT_EQ(rl.allow("k", lim).reset_ms, 70u);

    const rl::Limit fast{1, 3'000'000}; // a token every 333.3 ns
    EXPECT_TRUE(rl.allow("f", fast).allowed);
    EXPECT_EQ(rl.inspect("f", fast).until_token_ns, 334u);
    EXPECT_EQ(rl.allow("f", fast).reset_ms, ms(rl.inspect("f", fast)));

    const rl::Limit huge{1, std::numeric_limits<uint64_t>::max()}; // rate * 1e6 overflows
    EXPECT_TRUE(rl.allow("h", huge).allowed);
    EXPECT_EQ(rl.inspect("h", huge).until_token_ns, 1u);
    EXPECT_EQ(rl.allow("h", huge).reset_ms, 1u);

    // Never: reset_ms reports 0, inspect() kNever.
    EXPECT_FALSE(rl.allow("z", {0, 5}).allowed);
    EXPECT_EQ(rl.inspect("z", {0, 5}).until_token_ns, kNever);
    EXPECT_TRUE(rl.allow("n", {1, 0}).allowed);
    EXPECT_EQ(rl.allow("n", {1, 0}).reset_ms, 0u);
    EXPECT_EQ(rl.inspect("n", {1, 0}).until_token_ns, kNever);
}

TEST(Inspect, EdgeLimits) {
    FakeClock fc;
    fc.t = 50 * kSec;
    rl::RateLimiterInProc rl({}, &fc);
    constexpr uint64_t kMax = std::numeric_limits<uint64_t>::max();
    // Capacity 0: tracked once a call inserts it, never a token.
    EXPECT_FALSE(rl.allow("z", {0, 5}).allowed);
    fc.advance_ns(kSec);
    EXPECT_EQ(str(rl.inspect("z", {0, 5})), str(stored(0, fc.t - kSec, 0, kNever, 0)));
    // Rate 0: never refills.
    EXPECT_TRUE(rl.allow("r", {2, 0}).allowed);
    fc.advance_ns(1000 * kSec);
    EXPECT_EQ(str(rl.inspect("r", {2, 0})), str(stored(kToken, fc.t - 1000 * kSec, kToken, 0, kNever)));
    EXPECT_TRUE(rl.allow("r", {2, 0}).allowed);
    EXPECT_EQ(str(rl.inspect("r", {2, 0})), str(stored(0, fc.t, 0, kNever, kNever)));
    // A huge rate: refill saturates at capacity instead of overflowing.
    const rl::Limit huge{3, kMax};
    for (int i = 0; i < 3; ++i) EXPECT_TRUE(rl.allow("h", huge).allowed);
    const uint64_t th = fc.t;
    EXPECT_EQ(str(rl.inspect("h", huge)), str(stored(0, th, 0, 1, 1)));
    EXPECT_EQ(str(rl.inspect("h", huge, th + 1)), str(stored(0, th, 3 * kToken, 0, 0)));
    EXPECT_EQ(str(rl.inspect("h", huge, kMax)), str(stored(0, th, 3 * kToken, 0, 0)));
    // A capacity above the clamp.
    const uint64_t max_cap = kMax / kToken;
    EXPECT_EQ(str(rl.inspect("c", {kMax, 1})), str(untracked(max_cap * kToken, 0)));
    EXPECT_EQ(rl.allow("c", {kMax, 1}).remaining, max_cap - 1);
    EXPECT_EQ(str(rl.inspect("c", {kMax, 1})), str(stored((max_cap - 1) * kToken, fc.t, (max_cap - 1) * kToken, 0, kSec)));
    EXPECT_EQ(str(rl.inspect("c", {kMax, 1}, kMax)), str(stored((max_cap - 1) * kToken, fc.t, max_cap * kToken, 0, 0)));
    // A lowered capacity: the stored balance is above it until a call clamps it.
    EXPECT_EQ(rl.allow("l", {100, 1}).remaining, 99u);
    EXPECT_EQ(str(rl.inspect("l", {5, 1})), str(stored(99 * kToken, fc.t, 5 * kToken, 0, 0)));
    EXPECT_EQ(str(rl.inspect("l", {100, 1})), str(stored(99 * kToken, fc.t, 99 * kToken, 0, kSec)));
    EXPECT_EQ(rl.allow("l", {5, 1}).remaining, 4u);
    EXPECT_EQ(str(rl.inspect("l", {100, 1})), str(stored(4 * kToken, fc.t, 4 * kToken, 0, 96 * kSec)));
}

TEST(Inspect, TimeBeforeStoredTimeAddsNothing) {
    FakeClock fc;
    fc.t = 10 * kSec;
    rl::RateLimiterInProc rl({}, &fc);
    const rl::Limit lim{1, 1};
    EXPECT_TRUE(rl.allow("k", lim).allowed);
    // Waits count from the stored time: the token arrives at 11 s.
    EXPECT_EQ(str(rl.inspect("k", lim, 5 * kSec)), str(stored(0, 10 * kSec, 0, kSec, kSec)));
    EXPECT_EQ(rl.allow("k", lim, 5 * kSec).reset_ms, 1000u);
    EXPECT_EQ(rl.inspect("k", lim, 5 * kSec).stored_at_ns, 10 * kSec); // not moved back
    EXPECT_FALSE(rl.try_acquire("k", lim, 11 * kSec - 1));
    EXPECT_TRUE(rl.try_acquire("k", lim, 11 * kSec));
}

TEST(Inspect, QuietDenialsKeepValuesExact) {
    FakeClock fc;
    fc.t = 10 * kSec;
    rl::RateLimiterInProc rl({}, &fc);
    const rl::Limit lim{1, 1}; // a nanotoken per ns
    EXPECT_TRUE(rl.allow("k", lim).allowed);
    const uint64_t t0 = fc.t;
    fc.advance_ns(kMs / 2);
    EXPECT_FALSE(rl.allow("k", lim).allowed); // within 1 ms: not recorded
    EXPECT_EQ(str(rl.inspect("k", lim)), str(stored(0, t0, kMs / 2, kSec - kMs / 2, kSec - kMs / 2)));
    fc.advance_ns(kMs);
    EXPECT_FALSE(rl.allow("k", lim).allowed); // 1.5 ms after the stored time: recorded
    EXPECT_EQ(str(rl.inspect("k", lim)), str(stored(3 * kMs / 2, fc.t, 3 * kMs / 2, kSec - 3 * kMs / 2, kSec - 3 * kMs / 2)));
}

TEST(Inspect, LongAndEmbeddedNulKeys) {
    FakeClock fc;
    rl::RateLimiterInProc rl({}, &fc);
    using namespace std::literals;
    const std::vector<std::string> keys = {std::string(200, 'q'), std::string(29, 'x'), std::string(28, 'x'),
                                           std::string("a\0b"sv), std::string("a\0c"sv), "a", ""};
    const rl::Limit lim{10, 0};
    for (size_t i = 0; i < keys.size(); ++i)
        for (size_t n = 0; n <= i; ++n) ASSERT_TRUE(rl.allow(keys[i], lim).allowed);
    for (size_t i = 0; i < keys.size(); ++i) {
        const uint64_t left = (9 - i) * kToken;
        EXPECT_EQ(str(rl.inspect(keys[i], lim)), str(stored(left, 0, left, 0, kNever))) << i;
    }
    EXPECT_FALSE(rl.inspect("a\0d"sv, lim).tracked);
    EXPECT_FALSE(rl.inspect(std::string(199, 'q') + 'r', lim).tracked);
    EXPECT_FALSE(rl.inspect(std::string(30, 'x'), lim).tracked);
    EXPECT_EQ(rl.size(), keys.size());
}

TEST(Inspect, ClearEvictionAndGrowth) {
    FakeClock fc;
    rl::RateLimiterInProc rl({1, 1}, &fc); // one tiny shard: many rehashes
    const rl::Limit lim{2, 0};
    for (int i = 0; i < 5000; ++i) {
        ASSERT_TRUE(rl.allow("k" + std::to_string(i), lim).allowed);
        for (int j : {0, i / 2, i})
            ASSERT_EQ(str(rl.inspect("k" + std::to_string(j), lim)), str(stored(kToken, 0, kToken, 0, kNever))) << i;
        ASSERT_FALSE(rl.inspect("k" + std::to_string(i + 1), lim).tracked);
    }
    EXPECT_EQ(rl.size(), 5000u);
    fc.advance_ns(10 * kSec);
    ASSERT_TRUE(rl.allow("fresh", lim).allowed);
    EXPECT_EQ(rl.evict_idle(5 * kSec), 5000u);
    EXPECT_FALSE(rl.inspect("k0", lim).tracked);
    EXPECT_FALSE(rl.inspect("k4999", lim).tracked);
    EXPECT_EQ(str(rl.inspect("fresh", lim)), str(stored(kToken, 10 * kSec, kToken, 0, kNever)));
    rl.clear();
    EXPECT_FALSE(rl.inspect("fresh", lim).tracked);
    EXPECT_EQ(rl.size(), 0u);
}

// Random operations sent to two limiters, one of which also gets inspections
// of any key at arbitrary times: every decision must be the same in both, and
// match the model, and every inspection must match the model.
TEST(Differential, InspectionsChangeNoDecisionAndMatchModel) {
    constexpr uint64_t kMax = std::numeric_limits<uint64_t>::max();
    const std::vector<std::string> keys = {
        "", "a", "b", "key_1", std::string("a\0b", 3), std::string("a\0c", 3), std::string(28, 'x'),
        std::string(29, 'x'), std::string(64, 'z'), std::string(200, 'q'), "tenant:acme/user:42",
    };
    const std::vector<rl::Limit> limits = {
        {0, 0}, {1, 0}, {1, 1}, {2, 3}, {5, 10}, {10, 1000}, {3, kMax}, {kMax, 1}, {1000, 1'000'000},
        {1, 3'000'000}, {kMax / 1'000'000'000, 7}, {7, 0}, {100, 1}, {4, 666}, {2, 999'999'999},
    };
    for (uint64_t seed = 1; seed <= 8; ++seed) {
        std::mt19937_64 rng(seed);
        auto pick = [&](uint64_t n) { return static_cast<size_t>(rng() % n); };
        FakeClock fc;
        fc.t = 1'000'000 * kSec;
        const rl::RateLimiterInProc::Config cfg{size_t{1} << pick(3), pick(3)};
        rl::RateLimiterInProc plain(cfg, &fc), inspected(cfg, &fc);
        ModelLimiter model;
        auto near = [&] { return fc.t + rng() % (2 * kSec) - kSec; };
        auto any_time = [&]() -> uint64_t {
            switch (pick(5)) {
            case 0: return fc.t;
            case 1: return near();
            case 2: return fc.t + rng() % (60 * kSec);
            case 3: return pick(2) ? 0 : kMax;
            default: return rng();
            }
        };
        for (int op = 0; op < 20'000; ++op) {
            switch (pick(6)) { // advance the clock
            case 0: case 1: break;
            case 2: fc.t += rng() % 1000; break;
            case 3: fc.t += rng() % (200 * kMs); break;
            case 4: fc.t += rng() % (20 * kSec); break;
            default: fc.t -= std::min<uint64_t>(fc.t, rng() % kSec); break; // backwards
            }
            for (size_t n = pick(3); n > 0; --n) {
                const std::string& k = keys[pick(keys.size())];
                const rl::Limit l = limits[pick(limits.size())];
                const uint64_t at = any_time();
                ASSERT_EQ(str(inspected.inspect(k, l, at)), str(model.inspect(k, l, at))) << "seed " << seed << " op " << op;
                if (pick(4) == 0)
                    ASSERT_EQ(str(inspected.inspect(k, l)), str(model.inspect(k, l, fc.t))) << "seed " << seed << " op " << op;
            }
            const std::string& key = keys[pick(keys.size())];
            const rl::Limit lim = limits[pick(limits.size())];
            const unsigned kind = static_cast<unsigned>(pick(100));
            if (kind < 45) {
                // An inspection at the decision's instant predicts it.
                const bool own_clock = kind < 30;
                const uint64_t at = own_clock ? fc.t : near();
                const rl::BucketState s = inspected.inspect(key, lim, at);
                const rl::Decision want = model.allow(key, lim, at);
                const rl::Decision a = own_clock ? plain.allow(key, lim) : plain.allow(key, lim, at);
                const rl::Decision b = own_clock ? inspected.allow(key, lim) : inspected.allow(key, lim, at);
                ASSERT_EQ(str(a), str(want)) << "seed " << seed << " op " << op;
                ASSERT_EQ(str(b), str(want)) << "seed " << seed << " op " << op;
                ASSERT_EQ(s.would_allow, b.allowed) << "seed " << seed << " op " << op;
                ASSERT_EQ(b.remaining, s.tokens - s.would_allow) << "seed " << seed << " op " << op;
                const uint64_t reset = b.allowed || s.until_token_ns == kNever ? 0 : (s.until_token_ns - 1) / kMs + 1;
                ASSERT_EQ(b.reset_ms, reset) << "seed " << seed << " op " << op;
            } else if (kind < 80) {
                const bool own_clock = kind < 62;
                const uint64_t at = own_clock ? fc.t : near();
                const bool want = model.allow(key, lim, at).allowed;
                const bool predicted = inspected.inspect(key, lim, at).would_allow;
                ASSERT_EQ(own_clock ? plain.try_acquire(key, lim) : plain.try_acquire(key, lim, at), want);
                ASSERT_EQ(own_clock ? inspected.try_acquire(key, lim) : inspected.try_acquire(key, lim, at), want);
                ASSERT_EQ(predicted, want) << "seed " << seed << " op " << op;
            } else if (kind < 92) {
                ASSERT_EQ(plain.size(), model.buckets.size()) << "seed " << seed << " op " << op;
                ASSERT_EQ(inspected.size(), model.buckets.size()) << "seed " << seed << " op " << op;
            } else if (kind < 98) {
                const uint64_t idle = rng() % (30 * kSec);
                const size_t want = model.evict_idle(idle, fc.t);
                ASSERT_EQ(plain.evict_idle(idle), want) << "seed " << seed << " op " << op;
                ASSERT_EQ(inspected.evict_idle(idle), want) << "seed " << seed << " op " << op;
            } else {
                plain.clear();
                inspected.clear();
                model.buckets.clear();
            }
        }
        ASSERT_EQ(inspected.size(), model.buckets.size());
    }
}

// ---------- Concurrency: structural changes under load ----------

// Thread-safe clock the test sets explicitly.
struct SharedClock : rl::Clock {
    std::atomic<uint64_t> t{0};
    uint64_t now_ns() const noexcept override { return t.load(std::memory_order_relaxed); }
};

TEST(Concurrency, TryAcquireHotKeyGrantsExactlyCapacity) {
    rl::RateLimiterInProc rl;
    const uint64_t cap = 20'000;
    std::atomic<uint64_t> allowed{0};
    std::vector<std::thread> ts;
    for (int t = 0; t < 8; ++t) {
        ts.emplace_back([&] {
            uint64_t mine = 0;
            for (int i = 0; i < 10'000; ++i) mine += rl.try_acquire("hot", {cap, 0});
            allowed += mine;
        });
    }
    for (auto& t : ts) t.join();
    EXPECT_EQ(allowed.load(), cap);
}

// Counted keys must grant exactly their capacity while other threads insert
// enough keys to make every shard's table grow several times.
TEST(Concurrency, GrowthLosesNoUpdates) {
    rl::RateLimiterInProc rl({4, 1});
    const int counted = 64, threads = 8, inserts_per_thread = 20'000;
    const uint64_t cap = 500;
    std::vector<std::atomic<uint64_t>> granted(counted);
    std::vector<std::thread> ts;
    for (int t = 0; t < threads; ++t) {
        ts.emplace_back([&, t] {
            for (int i = 0; i < inserts_per_thread; ++i) {
                (void)rl.try_acquire("fill-" + std::to_string(t) + "-" + std::to_string(i), {1, 0});
                const int k = (i * 13 + t) % counted;
                if (rl.try_acquire("counted-" + std::to_string(k), {cap, 0})) granted[k]++;
            }
        });
    }
    for (auto& t : ts) t.join();
    EXPECT_EQ(rl.size(), static_cast<size_t>(counted + threads * inserts_per_thread));
    // Each counted key was asked threads * inserts / counted = 2500 > cap times.
    for (int k = 0; k < counted; ++k) ASSERT_EQ(granted[k].load(), cap) << k;
}

// Live keys (always used at the current time) must never be evicted or lose
// updates while an evictor keeps rebuilding tables to drop stale keys that
// other threads keep inserting with an old timestamp.
TEST(Concurrency, EvictionDuringTrafficKeepsLiveKeys) {
    SharedClock clk;
    clk.t = 100 * kSec;
    rl::RateLimiterInProc rl({4, 1}, &clk);
    const int live = 200, threads = 6, rounds = 30;
    const uint64_t cap = 40;
    std::vector<std::atomic<uint64_t>> granted(live);
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> evicted{0};
    std::thread evictor([&] {
        while (!stop.load()) evicted += rl.evict_idle(50 * kSec);
    });
    std::thread stale([&] { // keys last seen at t = 0, so always evictable
        for (int i = 0; !stop.load(); ++i) (void)rl.allow("stale-" + std::to_string(i % 5000), {1, 0}, 0);
    });
    std::vector<std::thread> ts;
    for (int t = 0; t < threads; ++t) {
        ts.emplace_back([&, t] {
            // Past `rounds`, keep going until the evictor has evicted, so the
            // traffic overlaps eviction however the threads are scheduled.
            for (int r = 0; r < rounds || (evicted.load() == 0 && r < 100'000); ++r)
                for (int i = 0; i < live; ++i) {
                    const int k = (i + t * 31) % live;
                    if (rl.try_acquire("live-" + std::to_string(k), {cap, 0})) granted[k]++;
                }
        });
    }
    for (auto& t : ts) t.join();
    stop = true;
    evictor.join();
    stale.join();
    EXPECT_GT(evicted.load(), 0u);
    for (int k = 0; k < live; ++k) ASSERT_EQ(granted[k].load(), cap) << k; // asked at least 180 times each
    rl.evict_idle(50 * kSec);
    EXPECT_EQ(rl.size(), static_cast<size_t>(live));
}

TEST(Concurrency, ClearDuringTraffic) {
    rl::RateLimiterInProc rl({8, 2});
    std::atomic<bool> stop{false};
    std::thread clearer([&] {
        while (!stop.load()) {
            rl.clear();
            std::this_thread::yield();
        }
    });
    std::vector<std::thread> ts;
    std::atomic<uint64_t> calls{0};
    for (int t = 0; t < 6; ++t) {
        ts.emplace_back([&, t] {
            std::string long_prefix(40, 'L'); // heap-allocated keys too
            for (int i = 0; i < 30'000; ++i) {
                const std::string key = (i & 1 ? long_prefix : std::string()) + std::to_string((i * 7 + t) % 3000);
                (void)rl.allow(key, {2, 1});
                ++calls;
            }
        });
    }
    for (auto& t : ts) t.join();
    stop = true;
    clearer.join();
    EXPECT_EQ(calls.load(), 6u * 30'000u);
    rl.clear();
    EXPECT_EQ(rl.size(), 0u);
    EXPECT_TRUE(rl.allow("after", {1, 0}).allowed);
    EXPECT_FALSE(rl.allow("after", {1, 0}).allowed);
}

// Many short-lived threads, each using several limiters, then the limiters go away.
TEST(Concurrency, ShortLivedThreadsAndLimiters) {
    for (int round = 0; round < 20; ++round) {
        auto a = std::make_unique<rl::RateLimiterInProc>(rl::RateLimiterInProc::Config{2, 1});
        rl::RateLimiterInProc b({2, 1});
        std::vector<std::thread> ts;
        for (int t = 0; t < 8; ++t) {
            ts.emplace_back([&, t] {
                for (int i = 0; i < 300; ++i) {
                    (void)a->allow("k" + std::to_string(i + t * 1000), {1, 1});
                    (void)b.try_acquire("k" + std::to_string(i), {1, 1});
                }
            });
        }
        for (auto& t : ts) t.join();
        EXPECT_EQ(a->size(), 8u * 300u);
        EXPECT_EQ(b.size(), 300u);
        a.reset();
    }
}

// ---------- Concurrency: inspection under load ----------

// Failures seen by worker threads, checked after they join.
struct Failures {
    std::atomic<int> count{0};
    std::mutex mu;
    std::string first;
    void add(const std::string& what) {
        if (count++ == 0) {
            std::lock_guard lk(mu);
            first = what;
        }
    }
};

// A balance that only goes down (rate 0) as one thread sees it: tracked from
// its first sighting on, whole tokens, within capacity, never higher than
// before. Returns false if it was not tracked.
static bool check_draining(const rl::BucketState& s, uint64_t cap, uint64_t& seen, Failures& f,
                           const std::string& key) {
    if (!s.tracked) {
        if (seen != kNever) f.add(key + " untracked after being seen " + str(s));
        return false;
    }
    if (s.stored_nanotokens % kToken != 0 || s.stored_nanotokens > cap * kToken || s.stored_nanotokens > seen ||
        s.nanotokens != s.stored_nanotokens || s.tokens > cap)
        f.add(key + " after " + std::to_string(seen) + ": " + str(s));
    seen = s.stored_nanotokens;
    return true;
}

// One writer stamps call k with time t0 + k * step, so every committed state
// is (tokens = cap - k, last = t0 + k * step): a torn read would mix two.
// Then several writers: inspections see only whole tokens, going down, and
// the grants are still exactly the capacity.
TEST(Concurrency, InspectionSeesOnlyCommittedStates) {
    rl::RateLimiterInProc rl;
    const uint64_t cap = 1'000'000, t0 = 1'000'000'000, step = 3;
    const uint64_t calls = 200'000;
    const rl::Limit lim{cap, 0};
    std::atomic<bool> done{false};
    Failures f;
    std::vector<std::thread> inspectors;
    for (int t = 0; t < 3; ++t) {
        inspectors.emplace_back([&] {
            uint64_t prev_k = 0, reads = 0;
            for (bool last_round = false; !last_round;) {
                last_round = done.load();
                const rl::BucketState s = rl.inspect("hot", lim, t0);
                if (!s.tracked) {
                    if (prev_k) f.add("untracked after k=" + std::to_string(prev_k));
                    continue;
                }
                const uint64_t k = cap - s.stored_nanotokens / kToken;
                if (s.stored_nanotokens % kToken || k < prev_k || k < 1 || k > calls || s.stored_at_ns != t0 + k * step ||
                    s.nanotokens != s.stored_nanotokens)
                    f.add("after k=" + std::to_string(prev_k) + ": " + str(s));
                prev_k = k;
                ++reads;
            }
            if (prev_k != calls) f.add("last read k=" + std::to_string(prev_k));
            if (reads == 0) f.add("no reads");
        });
    }
    for (uint64_t k = 1; k <= calls; ++k)
        if (!rl.allow("hot", lim, t0 + k * step).allowed) f.add("denied call " + std::to_string(k));
    done = true;
    for (auto& t : inspectors) t.join();
    EXPECT_EQ(f.count.load(), 0) << f.first;

    const uint64_t cap2 = 20'000;
    std::atomic<uint64_t> allowed{0};
    std::atomic<int> writing{4};
    std::vector<std::thread> ts;
    for (int t = 0; t < 4; ++t) {
        ts.emplace_back([&, t] {
            uint64_t mine = 0;
            for (int i = 0; i < 10'000; ++i)
                mine += t % 2 ? rl.try_acquire("hot2", {cap2, 0}) : rl.allow("hot2", {cap2, 0}).allowed;
            allowed += mine;
            --writing;
        });
    }
    for (int t = 0; t < 3; ++t) {
        ts.emplace_back([&] {
            uint64_t seen = kNever;
            while (writing.load() > 0) check_draining(rl.inspect("hot2", {cap2, 0}), cap2, seen, f, "hot2");
            check_draining(rl.inspect("hot2", {cap2, 0}), cap2, seen, f, "hot2");
            if (seen != 0) f.add("final balance " + std::to_string(seen));
        });
    }
    for (auto& t : ts) t.join();
    EXPECT_EQ(f.count.load(), 0) << f.first;
    EXPECT_EQ(allowed.load(), cap2);
    EXPECT_EQ(rl.size(), 2u);
}

// Counted keys keep granting exactly their capacity, and stay visible to
// inspections, while other threads insert enough keys to make every table grow.
TEST(Concurrency, InspectionDuringGrowth) {
    rl::RateLimiterInProc rl({4, 1});
    const int counted = 64, threads = 6, inserts_per_thread = 10'000;
    const uint64_t cap = 300;
    std::vector<std::atomic<uint64_t>> granted(counted);
    std::atomic<int> writing{threads};
    Failures f;
    std::vector<std::thread> ts;
    for (int t = 0; t < threads; ++t) {
        ts.emplace_back([&, t] {
            for (int i = 0; i < inserts_per_thread; ++i) {
                (void)rl.try_acquire("fill-" + std::to_string(t) + "-" + std::to_string(i), {1, 0});
                const int k = (i * 13 + t) % counted;
                if (rl.try_acquire("counted-" + std::to_string(k), {cap, 0})) granted[k]++;
            }
            --writing;
        });
    }
    for (int t = 0; t < 2; ++t) {
        ts.emplace_back([&] {
            std::vector<uint64_t> seen(counted, kNever);
            uint64_t tracked_reads = 0;
            while (writing.load() > 0) {
                for (int k = 0; k < counted; ++k) {
                    const std::string key = "counted-" + std::to_string(k);
                    tracked_reads += check_draining(rl.inspect(key, {cap, 0}), cap, seen[k], f, key);
                    if (rl.inspect("absent-" + std::to_string(k), {cap, 0}).tracked) f.add("absent key tracked");
                }
            }
            if (tracked_reads == 0) f.add("no tracked reads");
        });
    }
    for (auto& t : ts) t.join();
    EXPECT_EQ(f.count.load(), 0) << f.first;
    // Inspections inserted nothing.
    EXPECT_EQ(rl.size(), static_cast<size_t>(counted + threads * inserts_per_thread));
    for (int k = 0; k < counted; ++k) {
        ASSERT_EQ(granted[k].load(), cap) << k; // asked 937 or 938 times each
        ASSERT_EQ(rl.inspect("counted-" + std::to_string(k), {cap, 0}).stored_nanotokens, 0u) << k;
    }
}

// Live keys stay tracked through the rebuilds of a busy evictor; stale keys
// come and go, but are only ever seen as stored.
TEST(Concurrency, InspectionDuringEviction) {
    SharedClock clk;
    clk.t = 100 * kSec;
    rl::RateLimiterInProc rl({4, 1}, &clk);
    const int live = 100, threads = 4, rounds = 150;
    const uint64_t cap = 40;
    std::vector<std::atomic<uint64_t>> granted(live);
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> evicted{0};
    Failures f;
    std::thread evictor([&] {
        while (!stop.load()) evicted += rl.evict_idle(50 * kSec);
    });
    std::thread stale([&] { // keys last seen at t = 0, so always evictable
        for (int i = 0; !stop.load(); ++i) (void)rl.allow("stale-" + std::to_string(i % 2000), {1, 0}, 0);
    });
    std::vector<std::thread> inspectors;
    for (int t = 0; t < 2; ++t) {
        inspectors.emplace_back([&] {
            std::vector<uint64_t> seen(live, kNever);
            for (int i = 0; !stop.load(); ++i) {
                const std::string key = "live-" + std::to_string(i % live);
                check_draining(rl.inspect(key, {cap, 0}), cap, seen[i % live], f, key);
                const rl::BucketState s = rl.inspect("stale-" + std::to_string(i % 2000), {1, 0});
                if (s.tracked && (s.stored_nanotokens != 0 || s.stored_at_ns != 0 || s.nanotokens != 0))
                    f.add("stale " + str(s));
            }
        });
    }
    std::vector<std::thread> ts;
    for (int t = 0; t < threads; ++t) {
        ts.emplace_back([&, t] {
            // As in EvictionDuringTrafficKeepsLiveKeys: run until eviction has happened.
            for (int r = 0; r < rounds || (evicted.load() == 0 && r < 100'000); ++r)
                for (int i = 0; i < live; ++i) {
                    const int k = (i + t * 31) % live;
                    if (rl.try_acquire("live-" + std::to_string(k), {cap, 0})) granted[k]++;
                }
        });
    }
    for (auto& t : ts) t.join();
    stop = true;
    evictor.join();
    stale.join();
    for (auto& t : inspectors) t.join();
    EXPECT_EQ(f.count.load(), 0) << f.first;
    EXPECT_GT(evicted.load(), 0u);
    for (int k = 0; k < live; ++k) ASSERT_EQ(granted[k].load(), cap) << k; // asked at least 600 times each
}

// Inspections racing with clear() see either no bucket or a valid one.
TEST(Concurrency, InspectionDuringClear) {
    rl::RateLimiterInProc rl({8, 2});
    const rl::Limit lim{2, 1};
    std::atomic<bool> stop{false};
    Failures f;
    std::thread clearer([&] {
        while (!stop.load()) {
            rl.clear();
            std::this_thread::yield();
        }
    });
    const std::string long_prefix(40, 'L'); // heap-allocated keys too
    auto key = [&](int i) { return (i & 1 ? long_prefix : std::string()) + std::to_string(i % 3000); };
    std::vector<std::thread> inspectors;
    for (int t = 0; t < 2; ++t) {
        inspectors.emplace_back([&, t] {
            for (int i = t; !stop.load(); i += 7) {
                const rl::BucketState s = rl.inspect(key(i), lim);
                if (s.stored_nanotokens > 2 * kToken || s.nanotokens > 2 * kToken || s.tokens > 2 ||
                    s.until_full_ns > 2 * kSec || (!s.tracked && (s.stored_nanotokens || s.stored_at_ns)))
                    f.add(str(s));
            }
        });
    }
    std::vector<std::thread> ts;
    for (int t = 0; t < 4; ++t) {
        ts.emplace_back([&, t] {
            for (int i = 0; i < 30'000; ++i) (void)rl.allow(key(i * 7 + t), lim);
        });
    }
    for (auto& t : ts) t.join();
    stop = true;
    clearer.join();
    for (auto& t : inspectors) t.join();
    EXPECT_EQ(f.count.load(), 0) << f.first;
    rl.clear();
    for (int i = 0; i < 6000; ++i) ASSERT_FALSE(rl.inspect(key(i), lim).tracked) << i;
    EXPECT_EQ(rl.size(), 0u);
}

// ---------- FastClock ----------

TEST(FastClock, TracksSteadyClock) {
    rl::FastClock fast;
    rl::SteadyClock steady;
    std::printf("FastClock uses the TSC: %s\n", rl::FastClock::uses_tsc() ? "yes" : "no");
    const uint64_t f0 = fast.now_ns(), s0 = steady.now_ns();
    uint64_t prev = f0, worst_step_back = 0;
    // Long enough to cross a few re-anchors.
    for (auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(1300);
         std::chrono::steady_clock::now() < end;) {
        for (int i = 0; i < 1000; ++i) {
            const uint64_t t = fast.now_ns();
            if (t < prev) worst_step_back = std::max(worst_step_back, prev - t);
            prev = t;
        }
    }
    const uint64_t f1 = fast.now_ns(), s1 = steady.now_ns();
    const double ratio = static_cast<double>(f1 - f0) / static_cast<double>(s1 - s0);
    EXPECT_NEAR(ratio, 1.0, 1e-3);
    EXPECT_LT(f1 > s1 ? f1 - s1 : s1 - f1, 2 * kMs); // same time base as steady_clock
    EXPECT_LT(worst_step_back, 100'000u);            // re-anchoring may step back a few microseconds
}

TEST(FastClock, ConcurrentReadersSeeConsistentTime) {
    rl::FastClock fast;
    std::atomic<uint64_t> worst{0};
    std::vector<std::thread> ts;
    for (int t = 0; t < 8; ++t) {
        ts.emplace_back([&] {
            uint64_t prev = fast.now_ns(), mine = 0;
            for (auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(1100);
                 std::chrono::steady_clock::now() < end;) {
                for (int i = 0; i < 1000; ++i) {
                    const uint64_t t = fast.now_ns();
                    if (t < prev) mine = std::max(mine, prev - t);
                    prev = t;
                }
            }
            uint64_t w = worst.load();
            while (mine > w && !worst.compare_exchange_weak(w, mine)) {}
        });
    }
    for (auto& t : ts) t.join();
    EXPECT_LT(worst.load(), 100'000u);
}
