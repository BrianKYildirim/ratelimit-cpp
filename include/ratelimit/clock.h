
#pragma once
#include <cstdint>
#include <chrono>

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

} // namespace rl
