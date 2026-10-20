
#include <gtest/gtest.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <limits>
#include <map>
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

// The token-bucket semantics written separately from the engine: a std::map
// and 128-bit arithmetic instead of overflow checks. A denial within 1 ms of
// the key's recorded time leaves the key unchanged (unless a lowered capacity
// clamps its balance).
struct ModelLimiter {
    static constexpr uint64_t kUnit = 1'000'000'000;
    static constexpr uint64_t kQuietNs = 1'000'000;
    struct Bucket {
        uint64_t tokens, last;
    };
    std::map<std::string, Bucket> buckets;

    rl::Decision allow(const std::string& key, rl::Limit lim, uint64_t now) {
        using u128 = unsigned __int128;
        const uint64_t cap = std::min<uint64_t>(lim.capacity, std::numeric_limits<uint64_t>::max() / kUnit) * kUnit;
        auto it = buckets.find(key);
        if (it == buckets.end()) it = buckets.emplace(key, Bucket{cap, now}).first;
        Bucket& b = it->second;
        uint64_t tokens = std::min(b.tokens, cap);
        const uint64_t at = std::max(b.last, now);
        if (now > b.last) {
            const u128 add = u128(now - b.last) * lim.refill_per_sec;
            tokens = add >= cap - tokens ? cap : tokens + uint64_t(add);
        }
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
        const u128 per_ms = u128(lim.refill_per_sec) * 1'000'000;
        if (per_ms > std::numeric_limits<uint64_t>::max()) return {false, 0, 1};
        return {false, 0, (kUnit - tokens - 1) / uint64_t(per_ms) + 1};
    }

    size_t evict_idle(uint64_t idle, uint64_t now) {
        return std::erase_if(buckets, [&](const auto& kv) { return now >= kv.second.last && now - kv.second.last >= idle; });
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
            for (int r = 0; r < rounds; ++r)
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
    for (int k = 0; k < live; ++k) ASSERT_EQ(granted[k].load(), cap) << k; // asked 180 times each
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
