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
// (linearizable per key) under a short per-shard lock.
class RL_API RateLimiterInProc final : public RateLimiter {
public:
    struct Config {
        size_t shards = 128;                   // rounded up to a power of two, clamped to [1, 65536]
        size_t capacity_hint_per_shard = 1024; // keys reserved when a shard is first used
    };

    // Inline delegating ctor so it’s defined (avoids linker errors).
    explicit RateLimiterInProc() : RateLimiterInProc(Config{}, nullptr) {}
    explicit RateLimiterInProc(const Config& cfg, const Clock* clock = nullptr);
    ~RateLimiterInProc();

    Decision allow(std::string_view key, const Limit& limit) noexcept override;
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
