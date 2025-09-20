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

extern "C" RL_API void rl_free(void* handle) {
    try {
        auto* rlptr = static_cast<RateLimiterInProc*>(handle);
        delete rlptr;
    } catch (...) {
    }
}
