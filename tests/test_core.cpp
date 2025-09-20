
#include <gtest/gtest.h>
#include "ratelimit/rate_limiter.h"
#include "ratelimit/clock.h"

struct FakeClock : rl::Clock {
    uint64_t t = 0;
    uint64_t now_ns() const noexcept override { return t; }
    void advance_ns(uint64_t ns) { t += ns; }
};

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

