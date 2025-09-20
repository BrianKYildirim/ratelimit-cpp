#include <shared_mutex>
#include <unordered_map>
#include <string>
#include <atomic>
#include <cassert>
#include <cstring>
#include <limits>
#include <mutex>

#include "ratelimit/rate_limiter.h"
#include "ratelimit/version.h"

#ifndef RL_FP_SCALE
#define RL_FP_SCALE 4294967296ULL // 2^32
#endif

namespace rl {

static inline uint64_t fnv1a_64(std::string_view s) {
    uint64_t h = 1469598103934665603ULL;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    return h;
}

struct Entry {
    std::atomic<uint64_t> tokens_fp; // Q32.32 fixed-point tokens
    std::atomic<uint64_t> last_ns;   // last refill time
    Entry(uint64_t init_tokens_fp, uint64_t now_ns)
        : tokens_fp(init_tokens_fp), last_ns(now_ns) {}
};

struct Shard {
    std::unordered_map<std::string, Entry*> map;
    mutable std::shared_mutex mtx; // shared for reads, unique for insert/rehash
    size_t approx_size = 0;

    ~Shard() {
        for (auto& kv : map) delete kv.second;
    }
};

struct RateLimiterInProc::Impl {
    size_t shard_mask;
    std::vector<std::unique_ptr<Shard>> shards;
    const Clock* clock;
    SteadyClock default_clock;

    Impl(size_t shard_count, size_t cap_hint_per_shard, const Clock* c)
        : clock(c ? c : &default_clock) {
        if ((shard_count & (shard_count - 1)) != 0) {
            size_t p = 1;
            while (p < shard_count) p <<= 1;
            shard_count = p;
        }
        shard_mask = shard_count - 1;
        shards.reserve(shard_count);
        for (size_t i = 0; i < shard_count; ++i) {
            auto s = std::make_unique<Shard>();
            s->map.reserve(cap_hint_per_shard);
            shards.emplace_back(std::move(s));
        }
    }

    inline Shard& get_shard(std::string_view key) const {
        uint64_t h = fnv1a_64(key);
        return *shards[h & shard_mask];
    }

    static Decision decide(Entry* e, const Limit& limit, uint64_t now_ns) {
        const uint64_t cap_fp = limit.capacity * RL_FP_SCALE;
        const uint64_t rate = limit.refill_per_sec;
        const bool no_refill = (rate == 0);

        while (true) {
            uint64_t last = e->last_ns.load(std::memory_order_acquire);
            uint64_t tokens0 = e->tokens_fp.load(std::memory_order_relaxed);

            uint64_t delta_ns = (now_ns > last) ? (now_ns - last) : 0ULL;

            unsigned __int128 add_fp = 0;
            if (!no_refill && delta_ns != 0) {
                add_fp = (unsigned __int128)delta_ns * (unsigned __int128)rate;
                add_fp = add_fp * (unsigned __int128)RL_FP_SCALE / (unsigned __int128)1000000000ULL;
            }

            uint64_t tokens1;
            if (add_fp > 0) {
                unsigned __int128 sum = (unsigned __int128)tokens0 + add_fp;
                if (sum > std::numeric_limits<uint64_t>::max()) {
                    tokens1 = std::numeric_limits<uint64_t>::max();
                } else {
                    tokens1 = (uint64_t)sum;
                }
            } else {
                tokens1 = tokens0;
            }

            if (tokens1 > cap_fp) tokens1 = cap_fp;

            if (tokens1 >= RL_FP_SCALE) {
                uint64_t tokens2 = tokens1 - RL_FP_SCALE;

                if (delta_ns > 0) {
                    if (!e->last_ns.compare_exchange_strong(
                            last, now_ns, std::memory_order_acq_rel)) {
                        continue;
                    }
                }
                if (!e->tokens_fp.compare_exchange_strong(
                        tokens0, tokens2, std::memory_order_acq_rel)) {
                    continue;
                }
                uint64_t remaining = tokens2 / RL_FP_SCALE;
                return Decision{true, remaining, 0};
            } else {
                if (delta_ns > 0) {
                    if (!e->last_ns.compare_exchange_strong(
                            last, now_ns, std::memory_order_acq_rel)) {
                        continue;
                    }
                    e->tokens_fp.store(tokens1, std::memory_order_release);
                }
                uint64_t reset_ms = 0;
                if (!no_refill) {
                    uint64_t missing_fp = (RL_FP_SCALE > tokens1) ? (RL_FP_SCALE - tokens1) : 0ULL;
                    unsigned __int128 num = (unsigned __int128)missing_fp * (unsigned __int128)1000000000ULL;
                    unsigned __int128 den = (unsigned __int128)rate * (unsigned __int128)RL_FP_SCALE;
                    unsigned __int128 ns = (num + den - 1) / den; // ceil div
                    if (ns > std::numeric_limits<uint64_t>::max()) {
                        reset_ms = std::numeric_limits<uint64_t>::max();
                    } else {
                        reset_ms = (uint64_t)ns / 1000000ULL;
                        if (((uint64_t)ns % 1000000ULL) != 0ULL) reset_ms += 1ULL;
                    }
                }
                uint64_t remaining = tokens1 / RL_FP_SCALE;
                return Decision{false, remaining, reset_ms};
            }
        }
    }
};

RateLimiterInProc::RateLimiterInProc(const Config& cfg, const Clock* clock)
    : impl_(std::make_unique<Impl>(cfg.shards, cfg.capacity_hint_per_shard, clock)) {}

RateLimiterInProc::~RateLimiterInProc() = default;

Decision RateLimiterInProc::allow(std::string_view key, const Limit& limit) noexcept {
    try {
        uint64_t now = impl_->clock->now_ns();
        Shard& shard = impl_->get_shard(key);

        {
            std::shared_lock lk(shard.mtx);
            auto it = shard.map.find(std::string(key));
            if (it != shard.map.end()) {
                Entry* e = it->second;
                return Impl::decide(e, limit, now);
            }
        }
        {
            std::unique_lock lk(shard.mtx);
            auto it = shard.map.find(std::string(key));
            if (it == shard.map.end()) {
                uint64_t init_fp = limit.capacity * RL_FP_SCALE; // start full
                Entry* e = new Entry(init_fp, now);
                auto [ins_it, ok] = shard.map.emplace(std::string(key), e);
                (void)ins_it; (void)ok;
                shard.approx_size = shard.map.size();
                return Impl::decide(e, limit, now);
            } else {
                Entry* e = it->second;
                return Impl::decide(e, limit, now);
            }
        }
    } catch (...) {
        return Decision{false, 0, 0};
    }
}

void RateLimiterInProc::clear() {
    for (auto& s : impl_->shards) {
        std::unique_lock lk(s->mtx);
        for (auto& kv : s->map) delete kv.second;
        s->map.clear();
        s->approx_size = 0;
    }
}

size_t RateLimiterInProc::size() const noexcept {
    size_t sum = 0;
    for (auto& s : impl_->shards) {
        std::shared_lock lk(s->mtx);
        sum += s->approx_size;
    }
    return sum;
}

} // namespace rl
