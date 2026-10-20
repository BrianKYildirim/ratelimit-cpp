#pragma once
#include "ratelimit/export.h"
#include <string_view>
#include <memory>
#include "types.h"
#include "clock.h"

namespace rl {

class RateLimiter {
public:
    virtual ~RateLimiter() = default;
    virtual Decision allow(std::string_view key, const Limit& limit) noexcept = 0;
    virtual void clear() = 0;
    virtual size_t size() const noexcept = 0;
};

// In-process sharded implementation. Each key's decision is made atomically
// (linearizable per key) under a short lock on that key's entry; lookups take
// no shared lock, so operations on different keys do not contend.
class RL_API RateLimiterInProc final : public RateLimiter {
public:
    struct Config {
        size_t shards = 128;                   // rounded up to a power of two, clamped to [1, 65536]
        size_t capacity_hint_per_shard = 1024; // keys reserved when a shard is first used
    };

    // Inline delegating ctor so it’s defined (avoids linker errors).
    explicit RateLimiterInProc() : RateLimiterInProc(Config{}, nullptr) {}
    // `clock` defaults to FastClock and must outlive the limiter.
    explicit RateLimiterInProc(const Config& cfg, const Clock* clock = nullptr);
    ~RateLimiterInProc();

    Decision allow(std::string_view key, const Limit& limit) noexcept override;

    // The same decision and state change as allow(), without computing
    // Decision::remaining and Decision::reset_ms.
    bool try_acquire(std::string_view key, const Limit& limit) noexcept;

    // As above, at a caller-supplied time instead of reading the clock, e.g.
    // one now_ns() reading shared by a batch of requests. `now_ns` must come
    // from this limiter's now_ns(): refills are measured against times stored
    // from earlier calls, and evict_idle() compares those times to its clock.
    // A time earlier than a key's last call adds no tokens to it.
    Decision allow(std::string_view key, const Limit& limit, uint64_t now_ns) noexcept;
    bool try_acquire(std::string_view key, const Limit& limit, uint64_t now_ns) noexcept;

    // The limiter's clock.
    uint64_t now_ns() const noexcept;

    void clear() override;
    size_t size() const noexcept override;

    // Forgets keys not seen for at least `idle_ns` and returns how many were
    // removed. Eviction is invisible to callers when idle_ns is at least the
    // time to refill an empty bucket (capacity / refill_per_sec seconds),
    // because a key that has refilled completely behaves like a new key.
    size_t evict_idle(uint64_t idle_ns);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace rl
