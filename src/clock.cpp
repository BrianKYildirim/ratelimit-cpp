#include "ratelimit/clock.h"

#include "fast_clock.h"

#if RL_HAVE_TSC
#include <cpuid.h>
#endif

namespace rl {

#if RL_HAVE_TSC
namespace detail {

TscAnchor g_tsc_anchor; // constant-initialized; span == 0 sends the first read to the slow path

namespace {

enum TscState : int { kUnknown, kUsable, kUnusable };
std::atomic<int> g_state{kUnknown};
std::atomic<bool> g_busy{false}; // held while calibrating or re-anchoring

// Start of the interval the tick rate is measured over; guarded by g_busy.
uint64_t g_ref_tsc = 0;
uint64_t g_ref_ns = 0;

constexpr uint64_t kAnchorNs = 500'000'000; // re-anchor to steady_clock this often
constexpr uint64_t kCalibrateNs = 2'000'000; // first rate estimate, refined at every re-anchor

struct Sample {
    uint64_t tsc;
    uint64_t ns;
};

// A steady_clock reading paired with the TSC midway through it, from the
// tightest of a few tries (a preempted try just has a wide bracket).
Sample sample() noexcept {
    Sample best{};
    uint64_t best_gap = ~uint64_t{0};
    for (int i = 0; i < 5; ++i) {
        const uint64_t t0 = __rdtsc();
        const uint64_t ns = steady_now_ns();
        const uint64_t t1 = __rdtsc();
        if (t1 - t0 < best_gap) {
            best_gap = t1 - t0;
            best = {t0 + (t1 - t0) / 2, ns};
        }
    }
    return best;
}

bool invariant_tsc() noexcept {
    unsigned a, b, c, d;
    return __get_cpuid(0x80000007, &a, &b, &c, &d) && (d & (1u << 8)) != 0;
}

// ns per tick between two samples in 32.32 fixed point; 0 unless the implied
// frequency is between 50 MHz and 20 GHz.
uint64_t rate(Sample from, Sample to) noexcept {
    if (to.tsc <= from.tsc || to.ns <= from.ns) return 0;
    const unsigned __int128 m = (static_cast<unsigned __int128>(to.ns - from.ns) << 32) / (to.tsc - from.tsc);
    if (m > static_cast<unsigned __int128>(20) << 32 || m < (uint64_t{1} << 32) / 20) return 0;
    return static_cast<uint64_t>(m);
}

void publish(Sample at, uint64_t mult) noexcept {
    TscAnchor& a = g_tsc_anchor;
    const uint32_t seq = a.seq.load(std::memory_order_relaxed);
    a.seq.store(seq + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    a.base_tsc.store(at.tsc, std::memory_order_relaxed);
    a.base_ns.store(at.ns, std::memory_order_relaxed);
    a.mult.store(mult, std::memory_order_relaxed);
    a.span.store(static_cast<uint64_t>((static_cast<unsigned __int128>(kAnchorNs) << 32) / mult) + kTscSkew,
                 std::memory_order_relaxed);
    a.seq.store(seq + 2, std::memory_order_release);
}

uint64_t calibrate() noexcept {
    if (!invariant_tsc()) {
        g_state.store(kUnusable, std::memory_order_release);
        return steady_now_ns();
    }
    const Sample a = sample();
    Sample b = a;
    // Bounded by ticks too, in case steady_clock does not advance.
    while (b.ns - a.ns < kCalibrateNs && b.tsc - a.tsc < (uint64_t{1} << 32)) b = sample();
    const uint64_t mult = rate(a, b);
    if (mult == 0) {
        g_state.store(kUnusable, std::memory_order_release);
        return b.ns;
    }
    g_ref_tsc = a.tsc;
    g_ref_ns = a.ns;
    publish(b, mult);
    g_state.store(kUsable, std::memory_order_release);
    return b.ns;
}

uint64_t reanchor() noexcept {
    const Sample now = sample();
    uint64_t mult = g_tsc_anchor.mult.load(std::memory_order_relaxed);
    // The rate over the whole interval since the reference sample is the most
    // precise estimate, unless the interval spans a discontinuity (a TSC reset,
    // a suspend that one clock counts and the other does not), which shows up
    // as a rate far from the current one: then measure afresh from here.
    const uint64_t m = rate({g_ref_tsc, g_ref_ns}, now);
    if (m > mult - mult / 64 && m < mult + mult / 64) {
        mult = m;
    } else {
        g_ref_tsc = now.tsc;
        g_ref_ns = now.ns;
    }
    publish(now, mult);
    return now.ns;
}

} // namespace

uint64_t tsc_now_ns_slow(uint64_t) noexcept {
    if (g_state.load(std::memory_order_acquire) == kUnusable) return steady_now_ns();
    // Readers that lose the race use steady_clock directly for this one read.
    if (g_busy.exchange(true, std::memory_order_acquire)) return steady_now_ns();
    const uint64_t now = g_state.load(std::memory_order_relaxed) == kUnknown ? calibrate() : reanchor();
    g_busy.store(false, std::memory_order_release);
    return now;
}

} // namespace detail

bool FastClock::uses_tsc() noexcept {
    (void)detail::fast_now_ns(); // calibrates on first use
    return detail::g_state.load(std::memory_order_acquire) == detail::kUsable;
}
#else
bool FastClock::uses_tsc() noexcept { return false; }
#endif

uint64_t FastClock::now_ns() const noexcept { return detail::fast_now_ns(); }

} // namespace rl
