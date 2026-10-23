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

    // Reads `key`'s bucket without changing it: takes no token, inserts no
    // key, and never writes the key's entry, its lock or the per-thread
    // state decisions keep, so every later decision is the same as without
    // the call. The derived fields are what allow(key, limit, now_ns) would
    // compute at that instant, by the same code; see BucketState. `now_ns`
    // must come from now_ns(), as for allow(); the first overload reads it.
    //
    // Consistency: one key's reading is a state the key actually had, as
    // committed by one of its decisions or by its insert, never a partly
    // written one. It is linearizable with that key's decisions: it reflects
    // every decision that happens-before the call and none that starts after
    // it returns. Readings of different keys are separate snapshots, taken at
    // different instants, not one atomic view. would_allow is a prediction
    // for one instant, not a reservation: another thread may take the token
    // first. Denials within 1 ms of a key's stored time are not recorded
    // (they change nothing), so stored_at_ns may lag the key's latest call by
    // up to 1 ms while every derived value stays exact. While the key stays
    // tracked, stored_at_ns never moves backwards: a call stamped earlier
    // adds no tokens and leaves it unchanged.
    //
    // Thread-safe alongside every other member, including clear(),
    // evict_idle() and inserts that grow a table. Costs about as much as a
    // lookup: a hash, a probe and a lock-free read of the key's entry, which
    // waits only while a decision is writing that key. Only while the key's
    // table is being replaced (growth, eviction, clear()), or if the thread
    // cannot register for lock-free reads (out of memory), does it take the
    // key's shard mutex for the lookup instead, as allow() does then. Reading
    // a key that other threads write costs each of them a cache miss on its
    // next write of the key.
    BucketState inspect(std::string_view key, const Limit& limit) const noexcept;
    BucketState inspect(std::string_view key, const Limit& limit, uint64_t now_ns) const noexcept;

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
