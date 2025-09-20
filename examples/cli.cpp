
#include <iostream>
#include <string>
#include "ratelimit/rate_limiter.h"

int main(int argc, char** argv) {
    rl::RateLimiterInProc::Config cfg;
    if (argc > 1) cfg.shards = std::stoul(argv[1]);
    if (argc > 2) cfg.capacity_hint_per_shard = std::stoul(argv[2]);
    rl::RateLimiterInProc limiter(cfg);

    rl::Limit lim{10, 5}; // capacity 10, refill 5 tokens/sec

    std::string key;
    std::cout << "Enter keys (Ctrl+D to exit). Using capacity=10, refill=5/s\n";
    while (std::getline(std::cin, key)) {
        auto d = limiter.allow(key, lim);
        std::cout << key << " -> allowed=" << (d.allowed ? "yes" : "no")
                  << " remaining=" << d.remaining
                  << " reset_ms=" << d.reset_ms << "\n";
    }
    return 0;
}
