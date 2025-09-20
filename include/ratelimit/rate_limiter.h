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

// In-process sharded implementation
class RL_API RateLimiterInProc final : public RateLimiter {
public:
    struct Config {
        size_t shards = 128;
        size_t capacity_hint_per_shard = 1024;
    };

    // Inline delegating ctor so it’s defined (avoids linker errors).
    explicit RateLimiterInProc() : RateLimiterInProc(Config{}, nullptr) {}
    explicit RateLimiterInProc(const Config& cfg, const Clock* clock = nullptr);
    ~RateLimiterInProc();

    Decision allow(std::string_view key, const Limit& limit) noexcept override;
    void clear() override;
    size_t size() const noexcept override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace rl
