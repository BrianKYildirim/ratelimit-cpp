
#pragma once
#include <cstdint>
#include <chrono>
#include "ratelimit/export.h"

namespace rl {

struct Clock {
    virtual ~Clock() = default;
    virtual uint64_t now_ns() const noexcept = 0;
};

struct SteadyClock final : public Clock {
    uint64_t now_ns() const noexcept override {
        using namespace std::chrono;
        return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
    }
};

// steady_clock time read from the CPU timestamp counter where it runs at a
// constant rate (x86-64 with invariant TSC), re-anchored to steady_clock about
// every half second; plain steady_clock elsewhere. A read costs a few
// nanoseconds instead of a clock_gettime call, which is a system call under
// WSL1 and some hypervisors. The rate is measured against steady_clock, first
// over 2 ms (on the first read in the process), then over ever longer
// intervals. This is RateLimiterInProc's default clock.
struct RL_API FastClock final : public Clock {
    uint64_t now_ns() const noexcept override;
    // Whether reads come from the TSC; calibrates the clock if needed.
    static bool uses_tsc() noexcept;
};

} // namespace rl
