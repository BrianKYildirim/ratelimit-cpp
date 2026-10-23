
#pragma once
#include <cstdint>

namespace rl {

struct Limit {
    uint64_t capacity;       // whole tokens; values above 18'446'744'073 (2^64 / 1e9) are clamped
    uint64_t refill_per_sec; // tokens per second
};

struct Decision {
    bool allowed;
    uint64_t remaining; // whole tokens remaining (floor)
    uint64_t reset_ms;  // ms until next token when denied (rounded up); 0 if never (rate or capacity 0)
};

// A key's bucket as RateLimiterInProc::inspect() read it, for a call at a
// given time `now_ns`. Balances are in nanotokens (1 token = 1'000'000'000);
// times are in the limiter's clock domain (RateLimiterInProc::now_ns()).
struct BucketState {
    static constexpr uint64_t kNever = ~uint64_t{0}; // an until_* wait that never ends

    // Stored state, as the key's last recorded call left it. All zero when
    // `tracked` is false: nothing is invented for a key the limiter does not hold.
    bool tracked;               // the limiter holds a bucket for the key
    uint64_t stored_nanotokens; // balance as of stored_at_ns; above the capacity if the capacity was lowered since
    uint64_t stored_at_ns;      // time of that balance: the latest time a recorded call used for this key

    // Derived exactly as allow(key, limit, now_ns) computes it, from the stored
    // state, or, when `tracked` is false, from the Limit alone: the full bucket
    // a new key starts with. Waits count from max(now_ns, stored_at_ns), since
    // refill is never computed backwards, and assume no other call for the key.
    uint64_t nanotokens;     // available: stored balance clamped to the capacity, plus refill
    uint64_t tokens;         // whole tokens available before a call (floor); Decision::remaining
                             // counts after it, so remaining == tokens - would_allow
    uint64_t until_token_ns; // ns until a whole token is available, rounded up (the first ns
                             // at which it is); 0 if one is; kNever if rate or capacity is 0.
                             // When denied, Decision::reset_ms == ceil(until_token_ns / 1e6),
                             // except that reset_ms reports kNever as 0
    uint64_t until_full_ns;  // ns until the bucket holds its capacity, rounded up; 0 if it
                             // does (always, for capacity 0); kNever if rate is 0
    bool would_allow;        // whether a call at now_ns would be allowed: a prediction, not a reservation
};

} // namespace rl
