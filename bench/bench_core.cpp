
#include <benchmark/benchmark.h>
#include <string>
#include <vector>
#include "ratelimit/rate_limiter.h"

static void BM_UniformKeys(benchmark::State& state) {
    rl::RateLimiterInProc::Config cfg;
    cfg.shards = 128;
    cfg.capacity_hint_per_shard = 1 << 15;
    rl::RateLimiterInProc rl(cfg);

    rl::Limit lim{10, 1000};
    std::vector<std::string> keys;
    for (int i = 0; i < 100000; ++i) keys.emplace_back("key_" + std::to_string(i));

    size_t idx = 0;
    for (auto _ : state) {
        auto& k = keys[idx++ % keys.size()];
        rl.allow(k, lim);
        benchmark::DoNotOptimize(k);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_UniformKeys);

static void BM_HotKey(benchmark::State& state) {
    rl::RateLimiterInProc::Config cfg;
    rl::RateLimiterInProc rl(cfg);
    rl::Limit lim{1000, 100000};
    std::string k = "hot";
    for (auto _ : state) {
        rl.allow(k, lim);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_HotKey);

BENCHMARK_MAIN();
