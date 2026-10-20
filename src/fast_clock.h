// Internal: the inline read path of rl::FastClock, shared by the limiter so its
// default clock is read without a virtual call.
#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>

#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
#include <x86intrin.h>
#define RL_HAVE_TSC 1
#else
#define RL_HAVE_TSC 0
#endif

namespace rl::detail {

inline uint64_t steady_now_ns() noexcept {
    using namespace std::chrono;
    return static_cast<uint64_t>(duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count());
}

#if RL_HAVE_TSC
// Maps TSC ticks to steady_clock nanoseconds: ns = base_ns + (tsc - base_tsc) * mult / 2^32.
// Re-anchored to a fresh steady_clock sample every ~0.5 s, so a TSC discontinuity
// (suspend, migration) is corrected within that time. Published as a seqlock so
// readers never block or write shared memory.
struct alignas(64) TscAnchor {
    std::atomic<uint32_t> seq{0}; // odd while an update is in progress
    std::atomic<uint64_t> base_tsc{0};
    std::atomic<uint64_t> base_ns{0};
    std::atomic<uint64_t> mult{0}; // ns per tick, 32.32 fixed point
    std::atomic<uint64_t> span{0}; // a read converts iff tsc - base_tsc + kTscSkew < span; 0 until calibrated
};

// TSC readings up to this many ticks (~0.35 ms) behind the anchor, e.g. from a
// core whose counter trails the one that took the anchor, still convert.
inline constexpr uint64_t kTscSkew = uint64_t{1} << 20;

extern TscAnchor g_tsc_anchor;
uint64_t tsc_now_ns_slow(uint64_t tsc) noexcept;

inline uint64_t fast_now_ns() noexcept {
    const TscAnchor& a = g_tsc_anchor;
    const uint32_t seq = a.seq.load(std::memory_order_acquire);
    const uint64_t base_tsc = a.base_tsc.load(std::memory_order_relaxed);
    const uint64_t base_ns = a.base_ns.load(std::memory_order_relaxed);
    const uint64_t mult = a.mult.load(std::memory_order_relaxed);
    const uint64_t span = a.span.load(std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_acquire);
    const uint64_t tsc = __rdtsc();
    const auto d = static_cast<int64_t>(tsc - base_tsc);
    const bool stable = ((seq & 1) | (a.seq.load(std::memory_order_relaxed) ^ seq)) == 0;
    if (static_cast<uint64_t>(d) + kTscSkew < span && stable) [[likely]]
        return base_ns + static_cast<uint64_t>(static_cast<int64_t>((static_cast<__int128>(d) * mult) >> 32));
    return tsc_now_ns_slow(tsc);
}
#else
inline uint64_t fast_now_ns() noexcept { return steady_now_ns(); }
#endif

} // namespace rl::detail
