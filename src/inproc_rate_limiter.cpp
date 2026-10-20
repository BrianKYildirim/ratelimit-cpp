#include <algorithm>
#include <atomic>
#include <bit>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <thread>

#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
#include <immintrin.h>
#endif

#include "ratelimit/rate_limiter.h"
#include "ratelimit/version.h"

namespace rl {
namespace {

// Balances are kept in nanotokens (1 token = 1e9 units), so refilling for
// `dt` nanoseconds at `rate` tokens/s adds exactly `dt * rate` units: one
// 64-bit multiply, no division and no rounding drift between calls.
constexpr uint64_t kUnit = 1'000'000'000ULL;
constexpr uint64_t kMaxCapacity = std::numeric_limits<uint64_t>::max() / kUnit;

inline bool mul_overflow(uint64_t a, uint64_t b, uint64_t* out) noexcept {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_mul_overflow(a, b, out);
#else
    *out = a * b;
    return a != 0 && *out / a != b;
#endif
}

inline void cpu_relax() noexcept {
#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
    _mm_pause();
#elif defined(__aarch64__)
    asm volatile("yield");
#endif
}

inline void prefetch_for_write(const void* p) noexcept {
#if defined(__GNUC__) || defined(__clang__)
    __builtin_prefetch(p, 1);
#elif defined(_M_X64) || defined(_M_IX86)
    _mm_prefetch(static_cast<const char*>(p), _MM_HINT_T0);
#else
    (void)p;
#endif
}

// Critical sections are a few dozen instructions, so a test-and-test-and-set
// spinlock beats a futex mutex; it yields if the holder appears preempted.
class SpinLock {
public:
    void lock() noexcept {
        while (locked_.exchange(true, std::memory_order_acquire)) {
            for (unsigned spins = 0; locked_.load(std::memory_order_relaxed); ++spins) {
                if (spins < 64) cpu_relax();
                else std::this_thread::yield();
            }
        }
    }
    void unlock() noexcept { locked_.store(false, std::memory_order_release); }

private:
    std::atomic<bool> locked_{false};
};

inline uint64_t fmix64(uint64_t h) noexcept {
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    h *= 0xc4ceb9fe1a85ec53ULL;
    return h ^ (h >> 33);
}

inline uint64_t mix(uint64_t h, uint64_t w) noexcept {
    h = (h ^ w) * 0x9e3779b97f4a7c15ULL;
    return h ^ (h >> 32);
}

inline uint64_t load64(const char* p) noexcept { uint64_t v; std::memcpy(&v, p, 8); return v; }
inline uint64_t load32(const char* p) noexcept { uint32_t v; std::memcpy(&v, p, 4); return v; }

// Word-at-a-time hash; never returns 0 (marks an empty slot).
uint64_t hash_key(std::string_view s, uint64_t seed) noexcept {
    const char* p = s.data();
    size_t n = s.size();
    uint64_t h = seed ^ n;
    if (n > 8) {
        for (; n > 8; p += 8, n -= 8) h = mix(h, load64(p));
        h = mix(h, load64(p + n - 8)); // last word, may overlap the previous one
    } else if (n >= 4) {
        h = mix(h, load32(p) << 32 | load32(p + n - 4));
    } else if (n > 0) {
        h = mix(h, uint64_t(uint8_t(p[0])) << 16 | uint64_t(uint8_t(p[n / 2])) << 8 | uint8_t(p[n - 1]));
    }
    h = fmix64(h);
    return h + (h == 0);
}

// Key bytes stored inline when they fit in 20 bytes, else in one exact-size
// heap block whose pointer lives in the inline bytes. 24 bytes vs 32 for
// std::string, which only holds 15 bytes inline.
class Key {
public:
    Key() noexcept = default;
    Key(Key&& o) noexcept { steal(o); }
    Key& operator=(Key&& o) noexcept {
        if (this != &o) {
            release();
            steal(o);
        }
        return *this;
    }
    ~Key() { release(); }

    // Requires an empty key. Throws std::bad_alloc or std::length_error.
    void assign(std::string_view s) {
        if (s.size() > std::numeric_limits<uint32_t>::max()) throw std::length_error("key too long");
        char* dst = raw_;
        if (s.size() > sizeof raw_) {
            dst = new char[s.size()];
            std::memcpy(raw_, &dst, sizeof dst);
        }
        std::memcpy(dst, s.data(), s.size());
        len_ = static_cast<uint32_t>(s.size());
    }

    bool operator==(std::string_view s) const noexcept {
        return s.size() == len_ && std::memcmp(data(), s.data(), len_) == 0;
    }

private:
    bool on_heap() const noexcept { return len_ > sizeof raw_; }
    const char* data() const noexcept {
        if (!on_heap()) return raw_;
        const char* p;
        std::memcpy(&p, raw_, sizeof p);
        return p;
    }
    void release() noexcept {
        if (on_heap()) delete[] data();
        len_ = 0;
    }
    void steal(Key& o) noexcept {
        std::memcpy(raw_, o.raw_, sizeof raw_);
        len_ = o.len_;
        o.len_ = 0;
    }

    char raw_[20];
    uint32_t len_ = 0;
};

struct Slot {
    uint64_t hash = 0; // 0 = empty
    uint64_t tokens;   // nanotokens as of last_ns
    uint64_t last_ns;
    Key key;
};
static_assert(sizeof(Slot) == 48);

// Room for `n` keys at a max load factor of 3/4.
size_t slots_for(size_t n) { return std::bit_ceil(std::max<size_t>(8, n + n / 3 + 1)); }

// Linear-probing table guarded by one spinlock. alignas keeps each shard's
// lock and metadata on its own cache line.
struct alignas(64) Shard {
    SpinLock lock;
    size_t size = 0;
    size_t mask = 0; // capacity - 1 while slots != nullptr
    std::unique_ptr<Slot[]> slots;
    // Lock-free copy of (slots, mask), used only to prefetch a key's slot before
    // taking the lock so the slot's cache miss overlaps the lock acquisition.
    // A stale value just prefetches a useless address; prefetches never fault.
    std::atomic<const Slot*> peek_slots{nullptr};
    std::atomic<size_t> peek_mask{0};

    void prefetch(uint64_t h) const noexcept {
        const auto base = reinterpret_cast<uintptr_t>(peek_slots.load(std::memory_order_relaxed));
        const size_t i = h & peek_mask.load(std::memory_order_relaxed);
        prefetch_for_write(reinterpret_cast<const void*>(base + i * sizeof(Slot)));
    }

    void publish() noexcept {
        peek_slots.store(slots.get(), std::memory_order_relaxed);
        peek_mask.store(mask, std::memory_order_relaxed);
    }

    Slot* find(uint64_t h, std::string_view key) noexcept {
        if (!slots) return nullptr;
        for (size_t i = h & mask;; i = (i + 1) & mask) {
            Slot& s = slots[i];
            if (s.hash == h && s.key == key) return &s;
            if (s.hash == 0) return nullptr;
        }
    }

    Slot& insert(uint64_t h, std::string_view key, size_t min_slots) {
        if ((size + 1) * 4 > (mask + 1) * 3) rehash(std::max(min_slots, slots_for(size + 1)));
        size_t i = h & mask;
        while (slots[i].hash != 0) i = (i + 1) & mask;
        Slot& s = slots[i];
        s.key.assign(key); // may throw; the slot stays empty if it does
        s.hash = h;
        ++size;
        return s;
    }

    void rehash(size_t capacity) { rebuild(capacity, [](const Slot&) { return false; }); }

    // Moves every entry not matching `drop` into a fresh table. Strong exception
    // guarantee: nothing changes if the allocation throws.
    template <class Drop>
    void rebuild(size_t capacity, Drop drop) {
        auto fresh = std::make_unique<Slot[]>(capacity);
        const size_t m = capacity - 1;
        for (size_t i = 0; slots && i <= mask; ++i) {
            if (slots[i].hash == 0 || drop(slots[i])) continue;
            size_t j = slots[i].hash & m;
            while (fresh[j].hash != 0) j = (j + 1) & m;
            fresh[j] = std::move(slots[i]);
        }
        slots = std::move(fresh);
        mask = m;
        publish();
    }

    void reset() noexcept {
        slots.reset();
        size = 0;
        mask = 0;
        publish();
    }
};

Decision consume(Slot& e, const Limit& limit, uint64_t now) noexcept {
    const uint64_t cap = std::min(limit.capacity, kMaxCapacity) * kUnit;
    const uint64_t rate = limit.refill_per_sec;
    uint64_t tokens = std::min(e.tokens, cap); // capacity may have been lowered
    if (now > e.last_ns) {
        uint64_t add;
        if (mul_overflow(now - e.last_ns, rate, &add) || add >= cap - tokens) tokens = cap;
        else tokens += add;
        e.last_ns = now;
    }
    if (tokens >= kUnit) {
        e.tokens = tokens - kUnit;
        return Decision{true, e.tokens / kUnit, 0};
    }
    e.tokens = tokens;
    // Never: no refill, or the bucket cannot hold a whole token.
    if (rate == 0 || cap < kUnit) return Decision{false, 0, 0};
    // ceil(ceil(missing / rate) ns / 1e6) == ceil(missing / (rate * 1e6)); missing >= 1.
    const uint64_t missing = kUnit - tokens;
    uint64_t per_ms;
    if (mul_overflow(rate, 1'000'000ULL, &per_ms)) return Decision{false, 0, 1};
    return Decision{false, 0, (missing - 1) / per_ms + 1};
}

} // namespace

struct RateLimiterInProc::Impl {
    std::unique_ptr<Shard[]> shards;
    size_t shard_mask;
    size_t min_slots;   // initial table size, from capacity_hint_per_shard
    uint64_t seed;
    const Clock* clock; // nullptr = SteadyClock, called without virtual dispatch

    Impl(size_t shard_count, size_t cap_hint_per_shard, const Clock* c)
        : clock(c) {
        shard_count = std::bit_ceil(std::clamp<size_t>(shard_count, 1, size_t{1} << 16));
        shards = std::make_unique<Shard[]>(shard_count);
        shard_mask = shard_count - 1;
        min_slots = slots_for(std::min<size_t>(cap_hint_per_shard, size_t{1} << 30));
        seed = fmix64(reinterpret_cast<uintptr_t>(this) ^ now());
    }

    uint64_t now() const noexcept { return clock ? clock->now_ns() : SteadyClock{}.now_ns(); }

    // High hash bits pick the shard, low bits the slot, so the two are independent.
    Shard& shard_for(uint64_t h) const noexcept { return shards[(h >> 32) & shard_mask]; }
};

RateLimiterInProc::RateLimiterInProc(const Config& cfg, const Clock* clock)
    : impl_(std::make_unique<Impl>(cfg.shards, cfg.capacity_hint_per_shard, clock)) {}

RateLimiterInProc::~RateLimiterInProc() = default;

Decision RateLimiterInProc::allow(std::string_view key, const Limit& limit) noexcept {
    Impl& im = *impl_;
    const uint64_t now = im.now();
    const uint64_t h = hash_key(key, im.seed);
    Shard& shard = im.shard_for(h);
    shard.prefetch(h);
    std::lock_guard lk(shard.lock);
    Slot* e = shard.find(h, key);
    if (!e) {
        try {
            e = &shard.insert(h, key, im.min_slots);
        } catch (...) {
            return Decision{false, 0, 0};
        }
        e->tokens = std::min(limit.capacity, kMaxCapacity) * kUnit; // new keys start full
        e->last_ns = now;
    }
    return consume(*e, limit, now);
}

void RateLimiterInProc::clear() {
    for (size_t i = 0; i <= impl_->shard_mask; ++i) {
        Shard& s = impl_->shards[i];
        std::lock_guard lk(s.lock);
        s.reset();
    }
}

size_t RateLimiterInProc::size() const noexcept {
    size_t sum = 0;
    for (size_t i = 0; i <= impl_->shard_mask; ++i) {
        Shard& s = impl_->shards[i];
        std::lock_guard lk(s.lock);
        sum += s.size;
    }
    return sum;
}

size_t RateLimiterInProc::evict_idle(uint64_t idle_ns) {
    const uint64_t now = impl_->now();
    size_t evicted = 0;
    for (size_t i = 0; i <= impl_->shard_mask; ++i) {
        Shard& s = impl_->shards[i];
        std::lock_guard lk(s.lock);
        auto idle = [&](const Slot& e) { return e.hash != 0 && now >= e.last_ns && now - e.last_ns >= idle_ns; };
        size_t n = 0;
        for (size_t j = 0; s.slots && j <= s.mask; ++j) n += idle(s.slots[j]);
        if (n == 0) continue;
        evicted += n;
        if (n == s.size) {
            s.reset();
            continue;
        }
        // Rebuild rather than delete in place so the survivors' probe chains stay valid.
        s.rebuild(std::max(impl_->min_slots, slots_for(s.size - n)), idle);
        s.size -= n;
    }
    return evicted;
}

} // namespace rl
