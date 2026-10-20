#include <algorithm>
#include <atomic>
#include <bit>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
#include <immintrin.h>
#endif

#include "ratelimit/rate_limiter.h"
#include "ratelimit/version.h"
#include "fast_clock.h"

// Initial-exec TLS keeps the per-thread reader lookup a single load even when
// this code is built into a shared library.
#if defined(__ELF__) && (defined(__GNUC__) || defined(__clang__))
#define RL_TLS_IE __attribute__((tls_model("initial-exec")))
#else
#define RL_TLS_IE
#endif

// For helpers on the lookup path that GCC otherwise leaves as calls.
#if defined(__GNUC__) || defined(__clang__)
#define RL_INLINE inline __attribute__((always_inline))
#else
#define RL_INLINE inline
#endif

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

// Waits out a critical section of a few dozen instructions held by another
// thread: exponentially longer pauses, so waiters do not keep pulling the
// cache line away from the holder, then yields in case it was preempted.
inline void backoff(unsigned& spins) noexcept {
    if (spins < 24) {
        for (unsigned i = 1u << std::min(spins, 4u); i > 0; --i) cpu_relax();
        ++spins;
    } else {
        std::this_thread::yield();
    }
}

// Prefetches never fault, so `p` may be stale. Deliberately read-intent: a
// write-intent prefetch (PREFETCHW, which __builtin_prefetch(p, 1) becomes
// under -march flags that have it) takes the line away from every other core,
// which cut throughput on a hot over-limit key by two thirds, and measured
// no better elsewhere.
inline void prefetch(uintptr_t p) noexcept {
#if defined(__GNUC__) || defined(__clang__)
    __builtin_prefetch(reinterpret_cast<const void*>(p), 0);
#elif defined(_M_X64) || defined(_M_IX86)
    _mm_prefetch(reinterpret_cast<const char*>(p), _MM_HINT_T0);
#else
    (void)p;
#endif
}

inline uint64_t fmix64(uint64_t h) noexcept {
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    h *= 0xc4ceb9fe1a85ec53ULL;
    return h ^ (h >> 33);
}

inline uint64_t load64(const char* p) noexcept { uint64_t v; std::memcpy(&v, p, 8); return v; }
inline uint64_t load32(const char* p) noexcept { uint32_t v; std::memcpy(&v, p, 4); return v; }

// The 128-bit product of a and b, as (low, high).
inline void mul128(uint64_t& a, uint64_t& b) noexcept {
#if defined(__SIZEOF_INT128__)
    const unsigned __int128 r = static_cast<unsigned __int128>(a) * b;
    a = static_cast<uint64_t>(r);
    b = static_cast<uint64_t>(r >> 64);
#elif defined(_MSC_VER) && defined(_M_X64)
    a = _umul128(a, b, &b);
#else
    const uint64_t ha = a >> 32, hb = b >> 32, la = static_cast<uint32_t>(a), lb = static_cast<uint32_t>(b);
    const uint64_t rh = ha * hb, rm0 = ha * lb, rm1 = hb * la, rl = la * lb, t = rl + (rm0 << 32);
    uint64_t lo = t + (rm1 << 32);
    const uint64_t hi = rh + (rm0 >> 32) + (rm1 >> 32) + (t < rl) + (lo < t);
    a = lo;
    b = hi;
#endif
}

inline uint64_t mum(uint64_t a, uint64_t b) noexcept {
    mul128(a, b);
    return a ^ b;
}

// wyhash (final v4, public domain, github.com/wangyi-fudan/wyhash): 16 bytes
// per 64x64->128-bit multiply, three independent lanes beyond 48 bytes.
constexpr uint64_t kWy[4] = {0x2d358dccaa6c78a5ULL, 0x8bb84b93962eacc9ULL, 0x4b33a62ed433d4a3ULL,
                             0x4d5a2da51de1aa47ULL};

// Applied to a seed once, before it is passed to hash_key.
inline uint64_t prepare_seed(uint64_t seed) noexcept { return seed ^ mum(seed ^ kWy[0], kWy[1]); }

// Never returns 0 (marks an empty slot). Low bits pick the slot, bits 32+ the
// shard, so the two are independent.
RL_INLINE uint64_t hash_key(std::string_view s, uint64_t seed) noexcept {
    const char* p = s.data();
    const size_t n = s.size();
    uint64_t a, b;
    if (n <= 16) {
        if (n >= 4) {
            const size_t mid = (n >> 3) << 2;
            a = load32(p) << 32 | load32(p + mid);
            b = load32(p + n - 4) << 32 | load32(p + n - 4 - mid);
        } else if (n > 0) {
            a = uint64_t(uint8_t(p[0])) << 16 | uint64_t(uint8_t(p[n >> 1])) << 8 | uint8_t(p[n - 1]);
            b = 0;
        } else {
            a = b = 0;
        }
    } else {
        size_t i = n;
        if (i > 48) {
            uint64_t s1 = seed, s2 = seed;
            do {
                seed = mum(load64(p) ^ kWy[1], load64(p + 8) ^ seed);
                s1 = mum(load64(p + 16) ^ kWy[2], load64(p + 24) ^ s1);
                s2 = mum(load64(p + 32) ^ kWy[3], load64(p + 40) ^ s2);
                p += 48;
                i -= 48;
            } while (i > 48);
            seed ^= s1 ^ s2;
        }
        for (; i > 16; p += 16, i -= 16) seed = mum(load64(p) ^ kWy[1], load64(p + 8) ^ seed);
        a = load64(p + i - 16); // last 16 bytes, may overlap the previous ones
        b = load64(p + i - 8);
    }
    a ^= kWy[1];
    b ^= seed;
    mul128(a, b);
    const uint64_t h = mum(a ^ kWy[0] ^ n, b ^ kWy[1]);
    return h + (h == 0);
}

// memcmp(a, b, n) == 0, without a library call for keys up to 16 bytes.
inline bool bytes_equal(const char* a, const char* b, size_t n) noexcept {
    if (n >= 8) {
        return load64(a) == load64(b) && load64(a + n - 8) == load64(b + n - 8) &&
               (n <= 16 || std::memcmp(a + 8, b + 8, n - 16) == 0);
    }
    if (n >= 4) return load32(a) == load32(b) && load32(a + n - 4) == load32(b + n - 4);
    return n == 0 || (a[0] == b[0] && a[n / 2] == b[n / 2] && a[n - 1] == b[n - 1]);
}

// ---------- Epoch-based reclamation ----------
//
// Lookups read hash tables without locks, so a table replaced by a resize,
// eviction or clear() is freed only once no reader can still be inside it.
// A reader announces the global epoch while it is inside a table; something
// retired at epoch E is freed once every announced epoch is at least E.

struct alignas(64) Reader {
    std::atomic<uint64_t> epoch{0}; // epoch at entry while reading; 0 otherwise
    std::atomic<bool> taken{false};
    Reader* next = nullptr; // immutable once the reader is published

    // Owning thread only: addresses of slots it recently saw denied without a
    // write (see Impl::decide), direct-mapped. Stale entries are harmless.
    static constexpr size_t kQuietHints = 16;
    alignas(64) uintptr_t quiet[kQuietHints] = {};
    uintptr_t& quiet_hint(uintptr_t slot) noexcept { return quiet[(slot >> 6) % kQuietHints]; }
};

struct Epochs {
    alignas(64) std::atomic<uint64_t> current{1}; // read by every lookup, written only on retire
    alignas(64) std::atomic<Reader*> readers{nullptr};
};

// Never destroyed: readers outlive every thread and limiter.
constinit Epochs g_epochs;
RL_TLS_IE constinit thread_local Reader* t_reader = nullptr;

struct ReaderRelease {
    ~ReaderRelease() {
        if (t_reader) t_reader->taken.store(false, std::memory_order_release);
        t_reader = nullptr;
    }
};

// Claims a free reader record for this thread (records are reused across
// threads); nullptr if one cannot be allocated.
Reader* register_reader() noexcept {
    Reader* r = nullptr;
    for (Reader* p = g_epochs.readers.load(std::memory_order_acquire); p && !r; p = p->next)
        if (!p->taken.load(std::memory_order_relaxed) && !p->taken.exchange(true, std::memory_order_acquire)) r = p;
    if (!r) {
        r = new (std::nothrow) Reader;
        if (!r) return nullptr;
        r->taken.store(true, std::memory_order_relaxed);
        Reader* head = g_epochs.readers.load(std::memory_order_relaxed);
        do r->next = head;
        while (!g_epochs.readers.compare_exchange_weak(head, r, std::memory_order_release, std::memory_order_relaxed));
    }
    static thread_local ReaderRelease release_at_thread_exit;
    (void)release_at_thread_exit;
    t_reader = r;
    return r;
}

inline Reader* this_reader() noexcept {
    Reader* r = t_reader;
    return r ? r : register_reader();
}

// While alive, no table the thread can reach is freed. The exchange orders the
// announcement before the table loads that follow it.
class ReadSection {
public:
    explicit ReadSection(Reader* r) noexcept : r_(r) {
        r_->epoch.exchange(g_epochs.current.load(std::memory_order_seq_cst), std::memory_order_seq_cst);
    }
    ~ReadSection() { r_->epoch.store(0, std::memory_order_release); }
    ReadSection(const ReadSection&) = delete;
    ReadSection& operator=(const ReadSection&) = delete;

private:
    Reader* r_;
};

uint64_t oldest_reader_epoch() noexcept {
    uint64_t oldest = std::numeric_limits<uint64_t>::max();
    for (Reader* r = g_epochs.readers.load(std::memory_order_acquire); r; r = r->next) {
        const uint64_t e = r->epoch.load(std::memory_order_seq_cst);
        if (e != 0 && e < oldest) oldest = e;
    }
    return oldest;
}

// ---------- Hash table ----------

// One slot per cache line: a lookup normally touches only its own key's line,
// and keys in neighboring slots never share (and bounce) a line.
constexpr size_t kInlineKey = 28;
constexpr uint64_t kMoved = ~uint64_t{0}; // ver of a slot copied to a newer table

struct alignas(64) Slot {
    // 0 while empty. Stored last, with release, when the slot is filled, and
    // never changes afterwards; len and bytes are immutable from then on.
    std::atomic<uint64_t> hash{0};
    // Version lock: even = free, odd = held (or kMoved). Writers bump it twice
    // per critical section, so it doubles as a seqlock version.
    std::atomic<uint64_t> ver{0};
    std::atomic<uint64_t> tokens{0};  // nanotokens as of last_ns; guarded by ver
    std::atomic<uint64_t> last_ns{0}; // guarded by ver; read unlocked only by evict_idle's pre-check
    uint32_t len = 0;
    char bytes[kInlineKey]; // the key if len <= kInlineKey, else a pointer to it

    // Fills an empty slot; `heap` owns the key when it does not fit inline.
    void fill(uint64_t h, std::string_view key, char* heap, uint64_t tok, uint64_t last) noexcept {
        len = static_cast<uint32_t>(key.size());
        if (heap) std::memcpy(bytes, &heap, sizeof heap);
        else if (!key.empty()) std::memcpy(bytes, key.data(), key.size());
        tokens.store(tok, std::memory_order_relaxed);
        last_ns.store(last, std::memory_order_relaxed);
        hash.store(h, std::memory_order_release);
    }
    // Fills an empty slot with a sealed slot's entry; a heap key moves along.
    void fill(const Slot& o) noexcept {
        len = o.len;
        std::memcpy(bytes, o.bytes, sizeof bytes);
        tokens.store(o.tokens.load(std::memory_order_relaxed), std::memory_order_relaxed);
        last_ns.store(o.last_ns.load(std::memory_order_relaxed), std::memory_order_relaxed);
        hash.store(o.hash.load(std::memory_order_relaxed), std::memory_order_release);
    }

    char* heap_key() const noexcept {
        if (len <= kInlineKey) return nullptr;
        char* p;
        std::memcpy(&p, bytes, sizeof p);
        return p;
    }
    bool key_equals(std::string_view k) const noexcept {
        if (k.size() != len) return false;
        return bytes_equal(len <= kInlineKey ? bytes : heap_key(), k.data(), len);
    }
};
static_assert(sizeof(Slot) == 64);

// Locks the slot; false if it has moved to a newer table.
inline bool lock_slot(Slot& s) noexcept {
    for (unsigned spins = 0; s.ver.fetch_or(1, std::memory_order_acquire) & 1;) {
        for (uint64_t v; (v = s.ver.load(std::memory_order_relaxed)) & 1; backoff(spins))
            if (v == kMoved) return false;
    }
    // Orders the version bump before the data writes for unlocked readers.
    std::atomic_thread_fence(std::memory_order_release);
    return true;
}

inline void unlock_slot(Slot& s) noexcept {
    s.ver.store(s.ver.load(std::memory_order_relaxed) + 1, std::memory_order_release);
}

// Locks the slot for good, so lookups that reach it fall back to the shard
// mutex, and returns the version it replaced. Requires the shard mutex, so
// the slot cannot already be moved.
inline uint64_t seal_slot(Slot& s) noexcept {
    unsigned spins = 0;
    for (uint64_t v = s.ver.load(std::memory_order_relaxed);;) {
        if (v & 1) {
            backoff(spins);
            v = s.ver.load(std::memory_order_relaxed);
        } else if (s.ver.compare_exchange_weak(v, kMoved, std::memory_order_acquire, std::memory_order_relaxed)) {
            return v;
        }
    }
}

// Linear-probing table of 2^k slots (maximum load factor 3/4). Entries are
// never deleted in place: eviction and growth build a new table. A shard
// publishes its table as one word, the slot array's address (64-byte
// aligned) with k in the low 6 bits, so a lookup can find its slot's address
// before it may safely read the table, and prefetch it.
struct Table {
    Slot* slots = nullptr;
    size_t mask = 0;

    static Table decode(uintptr_t w) noexcept {
        return {reinterpret_cast<Slot*>(w & ~uintptr_t{63}), (size_t{1} << (w & 63)) - 1};
    }
    uintptr_t encode() const noexcept {
        return reinterpret_cast<uintptr_t>(slots) | static_cast<uintptr_t>(std::countr_zero(mask + 1));
    }

    size_t max_keys() const noexcept { return (mask + 1) / 4 * 3; }

    // Throws std::bad_alloc.
    static Table create(size_t capacity) {
        auto* slots = static_cast<Slot*>(::operator new(capacity * sizeof(Slot), std::align_val_t{alignof(Slot)}));
        for (size_t i = 0; i < capacity; ++i) new (&slots[i]) Slot;
        return {slots, capacity - 1};
    }
    void destroy() const noexcept { ::operator delete(slots, std::align_val_t{alignof(Slot)}); }

    Slot* find(uint64_t h, std::string_view key) const noexcept {
        for (size_t i = h & mask;; i = (i + 1) & mask) {
            Slot& s = slots[i];
            const uint64_t sh = s.hash.load(std::memory_order_acquire);
            if (sh == h && s.key_equals(key)) return &s;
            if (sh == 0) return nullptr;
        }
    }

    // The key's slot, or else the empty slot that ends its probe, which is
    // where it would be inserted. Requires the shard mutex.
    Slot* find_or_empty(uint64_t h, std::string_view key) const noexcept {
        for (size_t i = h & mask;; i = (i + 1) & mask) {
            Slot& s = slots[i];
            const uint64_t sh = s.hash.load(std::memory_order_relaxed);
            if (sh == 0 || (sh == h && s.key_equals(key))) return &s;
        }
    }

    // First empty slot in h's probe sequence. Requires the shard mutex (or a
    // table no reader can see yet) and room.
    Slot& claim(uint64_t h) const noexcept {
        size_t i = h & mask;
        while (slots[i].hash.load(std::memory_order_relaxed) != 0) i = (i + 1) & mask;
        return slots[i];
    }

    template <class F>
    void for_each(F f) const {
        for (size_t i = 0; i <= mask; ++i)
            if (slots[i].hash.load(std::memory_order_relaxed) != 0) f(slots[i]);
    }
};

// Room for `n` keys at the maximum load factor.
size_t slots_for(size_t n) { return std::bit_ceil(std::max<size_t>(8, n + n / 3 + 1)); }

// Address of the slot a lookup of `h` starts at, from a possibly stale table
// word: good for prefetching (a replaced table only wastes the prefetch) and
// for comparing, never for dereferencing.
inline uintptr_t home_slot(uintptr_t table_word, uint64_t h) noexcept {
    const uintptr_t base = table_word & ~uintptr_t{63};
    const uintptr_t mask = (uintptr_t{1} << (table_word & 63)) - 1;
    return base + (h & mask) * sizeof(Slot);
}

// Frees replaced tables, and the keys of evicted entries, once no reader can
// reach them.
class Reclaimer {
public:
    Reclaimer() = default;
    Reclaimer(const Reclaimer&) = delete;
    Reclaimer& operator=(const Reclaimer&) = delete;
    ~Reclaimer() { // no readers remain
        for (Retired& r : items_) release(r);
    }

    // Call after `t` has been unpublished. Frees `keys` with it, or every key
    // still in `t` if `with_keys` (entries that moved to a newer table took
    // their keys along, so their slots in `t` must not free them).
    void retire(Table t, std::vector<char*> keys = {}, bool with_keys = false) noexcept {
        const uint64_t epoch = g_epochs.current.fetch_add(1, std::memory_order_seq_cst) + 1;
        std::lock_guard lk(mu_);
        try {
            items_.push_back(Retired{epoch, t, std::move(keys), with_keys});
        } catch (...) {
            return; // leak rather than free too early
        }
        collect();
    }

    // Frees what has become safe; cheap when nothing is pending.
    void poll() noexcept {
        if (pending_.load(std::memory_order_relaxed) == 0) return;
        std::unique_lock lk(mu_, std::try_to_lock);
        if (lk.owns_lock()) collect();
    }

private:
    struct Retired {
        uint64_t epoch;
        Table table;
        std::vector<char*> keys;
        bool with_keys;
    };

    static void release(Retired& r) noexcept {
        for (char* k : r.keys) delete[] k;
        if (r.with_keys) r.table.for_each([](const Slot& s) { delete[] s.heap_key(); });
        r.table.destroy();
    }

    void collect() noexcept {
        const uint64_t oldest = oldest_reader_epoch();
        std::erase_if(items_, [&](Retired& r) {
            if (r.epoch > oldest) return false;
            release(r);
            return true;
        });
        pending_.store(items_.size(), std::memory_order_relaxed);
    }

    std::mutex mu_;
    std::vector<Retired> items_;
    std::atomic<size_t> pending_{0};
};

// ---------- Token bucket ----------

// Result policies: Full computes allow()'s Decision, Admit only the verdict.
struct Full {
    using Result = Decision;
    static Decision allowed(uint64_t tokens) noexcept { return {true, tokens / kUnit, 0}; }
    static Decision denied(uint64_t tokens, uint64_t cap, uint64_t rate) noexcept {
        // Never: no refill, or the bucket cannot hold a whole token.
        if (rate == 0 || cap < kUnit) return {false, 0, 0};
        // ceil(ceil(missing / rate) ns / 1e6) == ceil(missing / (rate * 1e6)); missing >= 1.
        const uint64_t missing = kUnit - tokens;
        uint64_t per_ms;
        if (mul_overflow(rate, 1'000'000ULL, &per_ms)) return {false, 0, 1};
        return {false, 0, (missing - 1) / per_ms + 1};
    }
    static Decision failed() noexcept { return {false, 0, 0}; }
};

struct Admit {
    using Result = bool;
    static bool allowed(uint64_t) noexcept { return true; }
    static bool denied(uint64_t, uint64_t, uint64_t) noexcept { return false; }
    static bool failed() noexcept { return false; }
};

// A denial less than this long after the key's recorded time leaves the key
// unchanged, so over-limit keys can be denied from a snapshot, without
// writing their cache line (see Impl::decide). Refill is linear and exact, so
// skipping the update changes no later verdict or `remaining`; it only means
// evict_idle() may see such a key as last used up to this much earlier, and a
// later call stamped before such a denial may compute reset_ms from that
// earlier time.
constexpr uint64_t kQuietNs = 1'000'000;

// Refills the bucket (tokens as of last) up to `now` and takes one token if
// available; `now` before `last` adds nothing.
template <class Out>
RL_INLINE typename Out::Result consume(uint64_t& tokens_io, uint64_t& last, const Limit& limit, uint64_t now) noexcept {
    const uint64_t cap = std::min(limit.capacity, kMaxCapacity) * kUnit;
    const uint64_t rate = limit.refill_per_sec;
    uint64_t tokens = std::min(tokens_io, cap); // capacity may have been lowered
    uint64_t at = last;
    if (now > last) {
        uint64_t add;
        if (mul_overflow(now - last, rate, &add) || add >= cap - tokens) tokens = cap;
        else tokens += add;
        at = now;
    }
    if (tokens >= kUnit) {
        tokens_io = tokens - kUnit;
        last = at;
        return Out::allowed(tokens_io);
    }
    if (tokens_io > cap || at - last >= kQuietNs) {
        tokens_io = tokens;
        last = at;
    }
    return Out::denied(tokens, cap, rate);
}

// Whether consume() would deny and change nothing: a denial within kQuietNs
// of `last`, with no capacity clamp to store.
RL_INLINE bool quiet_denial(uint64_t tokens, uint64_t last, const Limit& limit, uint64_t now) noexcept {
    const uint64_t cap = std::min(limit.capacity, kMaxCapacity) * kUnit;
    if (tokens > cap) return false;
    if (now > last) {
        if (now - last >= kQuietNs) return false;
        uint64_t add;
        if (mul_overflow(now - last, limit.refill_per_sec, &add) || add >= cap - tokens) tokens = cap;
        else tokens += add;
    }
    return tokens < kUnit;
}

// Snapshot of a slot's bucket, consistent with some point between its locked
// critical sections.
struct Snapshot {
    uint64_t tokens;
    uint64_t last;
    bool valid; // false if the slot has moved to a newer table
};

// A snapshot torn by a writer is retried rather than turned into a locked
// call, whose write would tear more snapshots.
inline Snapshot snapshot(const Slot& s) noexcept {
    for (unsigned spins = 0;; backoff(spins)) {
        const uint64_t v = s.ver.load(std::memory_order_acquire);
        if (v == kMoved) return {0, 0, false};
        const uint64_t tokens = s.tokens.load(std::memory_order_relaxed);
        const uint64_t last = s.last_ns.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (!(v & 1) && s.ver.load(std::memory_order_relaxed) == v) return {tokens, last, true};
    }
}

// Requires the slot's lock.
template <class Out>
RL_INLINE typename Out::Result consume(Slot& s, const Limit& limit, uint64_t now) noexcept {
    uint64_t tokens = s.tokens.load(std::memory_order_relaxed);
    uint64_t last = s.last_ns.load(std::memory_order_relaxed);
    const auto r = consume<Out>(tokens, last, limit, now);
    s.tokens.store(tokens, std::memory_order_relaxed);
    s.last_ns.store(last, std::memory_order_relaxed);
    return r;
}

// Inserts, resizes and evictions in a shard take its mutex; lookups do not.
struct alignas(64) ShardWriter {
    std::mutex mu;
    std::atomic<size_t> size{0}; // written under mu
};

} // namespace

struct RateLimiterInProc::Impl {
    // Each shard's encoded Table (0 until its first key); read by every lookup.
    std::unique_ptr<std::atomic<uintptr_t>[]> tables;
    std::unique_ptr<ShardWriter[]> writers;
    size_t shard_mask;
    size_t min_slots;   // initial table size, from capacity_hint_per_shard
    uint64_t seed;
    const Clock* clock; // nullptr = FastClock, called without virtual dispatch
    Reclaimer reclaimer;

    Impl(size_t shard_count, size_t cap_hint_per_shard, const Clock* c)
        : clock(c) {
        shard_count = std::bit_ceil(std::clamp<size_t>(shard_count, 1, size_t{1} << 16));
        tables = std::make_unique<std::atomic<uintptr_t>[]>(shard_count);
        writers = std::make_unique<ShardWriter[]>(shard_count);
        shard_mask = shard_count - 1;
        min_slots = slots_for(std::min<size_t>(cap_hint_per_shard, size_t{1} << 30));
        seed = prepare_seed(fmix64(reinterpret_cast<uintptr_t>(this) ^ now()));
    }

    ~Impl() { // no concurrent calls remain
        for (size_t i = 0; i <= shard_mask; ++i) {
            const Table t = table(i);
            if (!t.slots) continue;
            t.for_each([](const Slot& s) { delete[] s.heap_key(); });
            t.destroy();
        }
    }

    uint64_t now() const noexcept { return clock ? clock->now_ns() : detail::fast_now_ns(); }

    size_t shard_of(uint64_t h) const noexcept { return (h >> 32) & shard_mask; }

    // The shard's current table; stable while holding its mutex.
    Table table(size_t shard) const noexcept { return Table::decode(tables[shard].load(std::memory_order_relaxed)); }

    void publish(size_t shard, Table t) noexcept { tables[shard].store(t.encode(), std::memory_order_release); }

    // `now` is a callable, read after the key's slot is prefetched so the
    // clock read overlaps the slot's cache miss.
    template <class Out, class Now>
    typename Out::Result decide(std::string_view key, const Limit& limit, Now now) noexcept {
        const uint64_t h = hash_key(key, seed);
        const size_t shard = shard_of(h);
        prefetch(home_slot(tables[shard].load(std::memory_order_relaxed), h));
        const uint64_t t_now = now();
        if (Reader* r = this_reader()) [[likely]] {
            ReadSection rs(r);
            const Table t = Table::decode(tables[shard].load(std::memory_order_seq_cst));
            if (Slot* s = t.slots ? t.find(h, key) : nullptr) [[likely]] {
                // Over-limit keys: when the verdict is a denial that changes
                // nothing (see kQuietNs), decide from a snapshot without
                // writing, so the key's cache line stays shared by all cores
                // instead of bouncing between them. Reading before locking
                // costs other calls an extra coherence round trip (the line
                // arrives shared, then the lock upgrades it), so each thread
                // reads first only for slots it recently denied that way.
                const auto addr = reinterpret_cast<uintptr_t>(s);
                uintptr_t& hint = r->quiet_hint(addr);
                if (hint == addr) {
                    const Snapshot snap = snapshot(*s);
                    if (snap.valid && quiet_denial(snap.tokens, snap.last, limit, t_now)) {
                        uint64_t tokens = snap.tokens, last = snap.last;
                        return consume<Out>(tokens, last, limit, t_now); // computes the same denial
                    }
                    hint = 0;
                }
                if (lock_slot(*s)) [[likely]] {
                    const uint64_t tokens = s->tokens.load(std::memory_order_relaxed);
                    const uint64_t last = s->last_ns.load(std::memory_order_relaxed);
                    if (quiet_denial(tokens, last, limit, t_now)) hint = addr;
                    const auto res = consume<Out>(*s, limit, t_now);
                    unlock_slot(*s);
                    return res;
                }
            }
        }
        // New key, or a table being replaced.
        return decide_locked<Out>(shard, h, key, limit, t_now);
    }

    template <class Out>
    typename Out::Result decide_locked(size_t shard, uint64_t h, std::string_view key, const Limit& limit,
                                       uint64_t now) noexcept {
        typename Out::Result res;
        {
            std::lock_guard lk(writers[shard].mu);
            const Table t = table(shard);
            Slot* s = t.slots ? t.find_or_empty(h, key) : nullptr;
            if (s && s->hash.load(std::memory_order_relaxed) != 0) {
                lock_slot(*s); // cannot have moved: that takes the mutex
                res = consume<Out>(*s, limit, now);
                unlock_slot(*s);
            } else {
                // New keys start full.
                uint64_t tokens = std::min(limit.capacity, kMaxCapacity) * kUnit;
                uint64_t last = now;
                res = consume<Out>(tokens, last, limit, now);
                try {
                    insert(shard, t, s, h, key, tokens, last);
                } catch (...) {
                    res = Out::failed();
                }
            }
        }
        reclaimer.poll();
        return res;
    }

    // Requires the shard mutex; `t` is the shard's current table and `empty`
    // the empty slot that ended the key's probe in it (or null without a
    // table). Throws std::bad_alloc or std::length_error, leaving the shard
    // unchanged.
    void insert(size_t shard, Table t, Slot* empty, uint64_t h, std::string_view key, uint64_t tokens, uint64_t last) {
        if (key.size() > std::numeric_limits<uint32_t>::max()) throw std::length_error("key too long");
        std::unique_ptr<char[]> heap;
        if (key.size() > kInlineKey) {
            heap.reset(new char[key.size()]);
            std::memcpy(heap.get(), key.data(), key.size());
        }
        const size_t n = writers[shard].size.load(std::memory_order_relaxed) + 1;
        if (!t.slots || n > t.max_keys()) {
            const Table bigger = Table::create(std::max(min_slots, slots_for(n))); // doubles when full
            if (t.slots) {
                t.for_each([&](Slot& s) {
                    seal_slot(s);
                    bigger.claim(s.hash.load(std::memory_order_relaxed)).fill(s);
                });
            }
            publish(shard, bigger);
            if (t.slots) reclaimer.retire(t);
            empty = &bigger.claim(h);
        }
        empty->fill(h, key, heap.release(), tokens, last);
        writers[shard].size.store(n, std::memory_order_relaxed);
    }

    // Requires the shard mutex. Empties the shard.
    void drop_all(size_t shard) noexcept {
        const Table t = table(shard);
        if (!t.slots) return;
        t.for_each(seal_slot);
        publish(shard, Table{});
        writers[shard].size.store(0, std::memory_order_relaxed);
        reclaimer.retire(t, {}, /*with_keys=*/true);
    }

    // Requires the shard mutex. Replaces the shard's table with one holding
    // only the entries `keep` accepts, deciding with every slot sealed.
    // Returns how many entries were dropped. Throws std::bad_alloc, leaving
    // the shard unchanged.
    template <class Keep>
    size_t rebuild(size_t shard, Keep keep) {
        const Table t = table(shard);
        if (!t.slots) return 0;
        ShardWriter& w = writers[shard];
        const size_t size = w.size.load(std::memory_order_relaxed);
        std::vector<Slot*> live;
        std::vector<char*> dropped_keys;
        std::vector<uint64_t> versions; // to unseal if allocating the new table fails
        live.reserve(size); // allocate before anything changes
        dropped_keys.reserve(size);
        versions.reserve(size);
        t.for_each([&](Slot& s) {
            versions.push_back(seal_slot(s));
            if (keep(s)) live.push_back(&s);
            else if (char* k = s.heap_key()) dropped_keys.push_back(k);
        });
        if (live.empty()) {
            publish(shard, Table{});
            w.size.store(0, std::memory_order_relaxed);
            reclaimer.retire(t, {}, /*with_keys=*/true);
            return size;
        }
        Table fresh;
        try {
            fresh = Table::create(std::max(min_slots, slots_for(live.size())));
        } catch (...) {
            // Keep the old table: unseal it, with versions newer than any a
            // snapshot reader may have seen.
            size_t i = 0;
            t.for_each([&](Slot& s) { s.ver.store(versions[i++] + 2, std::memory_order_release); });
            throw;
        }
        for (const Slot* s : live) fresh.claim(s->hash.load(std::memory_order_relaxed)).fill(*s);
        publish(shard, fresh);
        w.size.store(live.size(), std::memory_order_relaxed);
        reclaimer.retire(t, std::move(dropped_keys));
        return size - live.size();
    }
};

RateLimiterInProc::RateLimiterInProc(const Config& cfg, const Clock* clock)
    : impl_(std::make_unique<Impl>(cfg.shards, cfg.capacity_hint_per_shard, clock)) {}

RateLimiterInProc::~RateLimiterInProc() = default;

Decision RateLimiterInProc::allow(std::string_view key, const Limit& limit) noexcept {
    return impl_->decide<Full>(key, limit, [im = impl_.get()] { return im->now(); });
}

Decision RateLimiterInProc::allow(std::string_view key, const Limit& limit, uint64_t now_ns) noexcept {
    return impl_->decide<Full>(key, limit, [now_ns] { return now_ns; });
}

bool RateLimiterInProc::try_acquire(std::string_view key, const Limit& limit) noexcept {
    return impl_->decide<Admit>(key, limit, [im = impl_.get()] { return im->now(); });
}

bool RateLimiterInProc::try_acquire(std::string_view key, const Limit& limit, uint64_t now_ns) noexcept {
    return impl_->decide<Admit>(key, limit, [now_ns] { return now_ns; });
}

uint64_t RateLimiterInProc::now_ns() const noexcept { return impl_->now(); }

void RateLimiterInProc::clear() {
    for (size_t i = 0; i <= impl_->shard_mask; ++i) {
        std::lock_guard lk(impl_->writers[i].mu);
        impl_->drop_all(i);
    }
    impl_->reclaimer.poll();
}

size_t RateLimiterInProc::size() const noexcept {
    size_t sum = 0;
    for (size_t i = 0; i <= impl_->shard_mask; ++i) sum += impl_->writers[i].size.load(std::memory_order_relaxed);
    return sum;
}

size_t RateLimiterInProc::evict_idle(uint64_t idle_ns) {
    const uint64_t now = impl_->now();
    auto idle = [&](const Slot& s) {
        const uint64_t last = s.last_ns.load(std::memory_order_relaxed);
        return now >= last && now - last >= idle_ns;
    };
    size_t evicted = 0;
    for (size_t i = 0; i <= impl_->shard_mask; ++i) {
        std::lock_guard lk(impl_->writers[i].mu);
        const Table t = impl_->table(i);
        // Unlocked pre-check, so shards with nothing to evict are left alone;
        // the decision itself is made with every slot sealed.
        bool any = false;
        if (t.slots) t.for_each([&](const Slot& s) { any = any || idle(s); });
        if (any) evicted += impl_->rebuild(i, [&](const Slot& s) { return !idle(s); });
    }
    impl_->reclaimer.poll();
    return evicted;
}

} // namespace rl
