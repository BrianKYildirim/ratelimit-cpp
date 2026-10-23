#include "ratelimit/ratelimit_c.h"
#include "ratelimit/rate_limiter.h"
#include "ratelimit/version.h"

using rl::RateLimiterInProc;

extern "C" RL_API const char* rl_version(void) {
    return RL_VERSION;
}

extern "C" RL_API void* rl_new_inproc(unsigned shards, unsigned capacity_hint_per_shard) {
    try {
        RateLimiterInProc::Config cfg;
        cfg.shards = shards ? shards : 128;
        cfg.capacity_hint_per_shard = capacity_hint_per_shard ? capacity_hint_per_shard : 1024;
        auto* p = new RateLimiterInProc(cfg, nullptr);
        return static_cast<void*>(p);
    } catch (...) {
        return nullptr;
    }
}

extern "C" RL_API rl_decision_t rl_allow(void* handle, const char* key, rl_limit_t limit) {
    rl_decision_t out{};
    if (!handle || !key) {
        out.allowed = 0;
        out.remaining = 0;
        out.reset_ms = 0;
        return out;
    }
    try {
        auto* rlptr = static_cast<RateLimiterInProc*>(handle);
        rl::Limit lim{};
        lim.capacity = limit.capacity;
        lim.refill_per_sec = limit.refill_per_sec;

        rl::Decision d = rlptr->allow(key, lim);
        out.allowed = d.allowed ? 1 : 0;
        out.remaining = d.remaining;
        out.reset_ms = d.reset_ms;
        return out;
    } catch (...) {
        out.allowed = 0;
        out.remaining = 0;
        out.reset_ms = 0;
        return out;
    }
}

extern "C" RL_API int rl_try_acquire(void* handle, const char* key, rl_limit_t limit) {
    if (!handle || !key) return 0;
    return static_cast<RateLimiterInProc*>(handle)->try_acquire(key, rl::Limit{limit.capacity, limit.refill_per_sec}) ? 1 : 0;
}

extern "C" RL_API uint64_t rl_now_ns(void* handle) {
    if (!handle) return 0;
    return static_cast<RateLimiterInProc*>(handle)->now_ns();
}

extern "C" RL_API rl_decision_t rl_allow_at(void* handle, const char* key, rl_limit_t limit, uint64_t now_ns) {
    if (!handle || !key) return rl_decision_t{};
    const rl::Decision d =
        static_cast<RateLimiterInProc*>(handle)->allow(key, rl::Limit{limit.capacity, limit.refill_per_sec}, now_ns);
    return rl_decision_t{static_cast<uint8_t>(d.allowed ? 1 : 0), d.remaining, d.reset_ms};
}

static rl_bucket_state_t to_c(const rl::BucketState& s) {
    rl_bucket_state_t out{};
    out.tracked = s.tracked ? 1 : 0;
    out.would_allow = s.would_allow ? 1 : 0;
    out.stored_nanotokens = s.stored_nanotokens;
    out.stored_at_ns = s.stored_at_ns;
    out.nanotokens = s.nanotokens;
    out.tokens = s.tokens;
    out.until_token_ns = s.until_token_ns;
    out.until_full_ns = s.until_full_ns;
    return out;
}

extern "C" RL_API rl_bucket_state_t rl_inspect_at(void* handle, const char* key, rl_limit_t limit, uint64_t now_ns) {
    if (!handle || !key) return rl_bucket_state_t{};
    return to_c(static_cast<RateLimiterInProc*>(handle)->inspect(key, rl::Limit{limit.capacity, limit.refill_per_sec}, now_ns));
}

extern "C" RL_API rl_bucket_state_t rl_inspect(void* handle, const char* key, rl_limit_t limit) {
    if (!handle || !key) return rl_bucket_state_t{};
    return to_c(static_cast<RateLimiterInProc*>(handle)->inspect(key, rl::Limit{limit.capacity, limit.refill_per_sec}));
}

extern "C" RL_API uint64_t rl_evict_idle(void* handle, uint64_t idle_ns) {
    if (!handle) return 0;
    try {
        return static_cast<RateLimiterInProc*>(handle)->evict_idle(idle_ns);
    } catch (...) {
        return 0;
    }
}

extern "C" RL_API void rl_free(void* handle) {
    try {
        auto* rlptr = static_cast<RateLimiterInProc*>(handle);
        delete rlptr;
    } catch (...) {
    }
}
