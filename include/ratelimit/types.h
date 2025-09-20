
#pragma once
#include <cstdint>

namespace rl {

struct Limit {
    uint64_t capacity;       // whole tokens
    uint64_t refill_per_sec; // tokens per second
};

struct Decision {
    bool allowed;
    uint64_t remaining; // whole tokens remaining (floor)
    uint64_t reset_ms;  // ms until next token when denied; 0 if never
};

} // namespace rl
