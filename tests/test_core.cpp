
#include <gtest/gtest.h>
#include <atomic>
#include <limits>
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
