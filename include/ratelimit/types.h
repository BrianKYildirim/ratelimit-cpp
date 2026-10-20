
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

} // namespace rl
