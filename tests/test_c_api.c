/* Links against the shared C library, so it also checks that the C API is
   exported (it was hidden on GCC/Clang before RL_API set default visibility). */
#include <ratelimit/ratelimit_c.h>
#include <stdio.h>

static int failures = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                          \
        }                                                                        \
    } while (0)

#define MS 1000000ULL
#define SEC 1000000000ULL
#define TOKEN 1000000000ULL /* nanotokens */

static int is_zero(rl_bucket_state_t s) {
    return !s.tracked && !s.would_allow && !s.stored_nanotokens && !s.stored_at_ns && !s.nanotokens && !s.tokens &&
           !s.until_token_ns && !s.until_full_ns;
}

/* Readings and decisions paired at the same instants. */
static void check_inspection(void) {
    void* h = rl_new_inproc(0, 0);
    const rl_limit_t lim = {2, 10}; /* a token per 100 ms */
    const uint64_t now = rl_now_ns(h);
    const uint64_t t = now + 60 * SEC; /* ahead of the clock for the whole test */
    rl_bucket_state_t s;
    rl_decision_t d;
    CHECK(h != NULL && now != 0);

    /* Untracked: a full bucket derived from the limit, nothing stored or inserted. */
    s = rl_inspect_at(h, "k", lim, t);
    CHECK(!s.tracked && s.stored_nanotokens == 0 && s.stored_at_ns == 0);
    CHECK(s.nanotokens == 2 * TOKEN && s.tokens == 2 && s.would_allow && s.until_token_ns == 0 && s.until_full_ns == 0);
    s = rl_inspect_at(h, "k", lim, t);
    CHECK(!s.tracked);

    d = rl_allow_at(h, "k", lim, t);
    CHECK(d.allowed == 1 && d.remaining == 1);
    s = rl_inspect_at(h, "k", lim, t);
    CHECK(s.tracked && s.stored_nanotokens == TOKEN && s.stored_at_ns == t && s.tokens == 1 && s.would_allow);
    CHECK(s.until_token_ns == 0 && s.until_full_ns == 100 * MS);
    CHECK(rl_allow_at(h, "k", lim, t).allowed == 1);

    s = rl_inspect_at(h, "k", lim, t + 30 * MS + 1);
    d = rl_allow_at(h, "k", lim, t + 30 * MS + 1);
    CHECK(s.tracked && s.stored_nanotokens == 0 && s.stored_at_ns == t);
    CHECK(s.nanotokens == 300000010 && s.tokens == 0 && !s.would_allow);
    CHECK(s.until_token_ns == 70 * MS - 1 && s.until_full_ns == 170 * MS - 1);
    CHECK(d.allowed == 0 && d.remaining == 0 && d.reset_ms == (s.until_token_ns + MS - 1) / MS);

    s = rl_inspect_at(h, "k", lim, t + 100 * MS);
    d = rl_allow_at(h, "k", lim, t + 100 * MS);
    CHECK(s.would_allow && s.tokens == 1 && d.allowed == 1 && d.remaining == s.tokens - s.would_allow);

    /* The clock-reading variant sees the same key. */
    s = rl_inspect(h, "k", lim);
    CHECK(s.tracked && s.stored_at_ns == t + 100 * MS);

    /* Never: capacity 0, or no refill once empty. */
    s = rl_inspect_at(h, "zero", (rl_limit_t){0, 5}, t);
    CHECK(!s.tracked && s.tokens == 0 && !s.would_allow && s.until_token_ns == RL_NEVER && s.until_full_ns == 0);
    CHECK(rl_allow_at(h, "dry", (rl_limit_t){1, 0}, t).allowed == 1);
    s = rl_inspect_at(h, "dry", (rl_limit_t){1, 0}, t + 1000 * SEC);
    CHECK(s.tracked && s.stored_at_ns == t && s.until_token_ns == RL_NEVER && s.until_full_ns == RL_NEVER);
    CHECK(rl_allow_at(h, "dry", (rl_limit_t){1, 0}, t + 1000 * SEC).reset_ms == 0); /* recorded */

    /* Keys end at the first NUL, for every function alike. */
    CHECK(rl_inspect_at(h, "k\0ignored", lim, t).tracked);
    CHECK(rl_inspect_at(h, "dry\0x", (rl_limit_t){1, 0}, t).stored_at_ns == t + 1000 * SEC);

    /* NULL arguments. */
    CHECK(rl_now_ns(NULL) == 0);
    CHECK(is_zero(rl_inspect_at(NULL, "k", lim, t)) && is_zero(rl_inspect_at(h, NULL, lim, t)));
    CHECK(is_zero(rl_inspect(NULL, "k", lim)) && is_zero(rl_inspect(h, NULL, lim)));
    d = rl_allow_at(NULL, "k", lim, t);
    CHECK(d.allowed == 0 && d.remaining == 0 && d.reset_ms == 0);
    d = rl_allow_at(h, NULL, lim, t);
    CHECK(d.allowed == 0 && d.remaining == 0 && d.reset_ms == 0);

    /* Eviction forgets a key last used in the past: untracked again. "k" and
       "dry" were used at times the clock has not reached, and inspecting
       "zero" inserted nothing, or it would be evicted too. */
    CHECK(rl_allow_at(h, "old", lim, now - SEC).allowed == 1);
    CHECK(rl_evict_idle(h, 0) == 1);
    CHECK(!rl_inspect_at(h, "old", lim, t).tracked && rl_inspect_at(h, "k", lim, t).tracked);
    rl_free(h);
}

int main(void) {
    void* h = rl_new_inproc(0, 0);
    if (!h) return 1;
    const rl_limit_t lim = {2, 0};
    const rl_decision_t d = rl_allow(h, "k", lim);
    const int ok = d.allowed == 1 && d.remaining == 1 && rl_try_acquire(h, "k", lim) == 1 &&
                   rl_try_acquire(h, "k", lim) == 0 && rl_allow(h, "k", lim).allowed == 0 &&
                   rl_try_acquire(NULL, "k", lim) == 0 && rl_try_acquire(h, NULL, lim) == 0;
    (void)rl_evict_idle(h, 0);
    rl_free(h);
    check_inspection();
    if (!ok || failures) {
        fprintf(stderr, "C API test failed\n");
        return 1;
    }
    printf("C API ok (version %s)\n", rl_version());
    return 0;
}
