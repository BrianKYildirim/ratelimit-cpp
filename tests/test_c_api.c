/* Links against the shared C library, so it also checks that the C API is
   exported (it was hidden on GCC/Clang before RL_API set default visibility). */
#include <ratelimit/ratelimit_c.h>
#include <stdio.h>

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
    if (!ok) {
        fprintf(stderr, "C API test failed\n");
        return 1;
    }
    printf("C API ok (version %s)\n", rl_version());
    return 0;
}
