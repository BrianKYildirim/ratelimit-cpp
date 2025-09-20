#include <ratelimit/rate_limiter.h>
#include <iostream>

int main() {
  rl::RateLimiterInProc limiter;
  rl::Limit L{10, 5};
  auto d = limiter.allow("test", L);
  std::cout << (d.allowed ? "allowed" : "denied") << " remaining=" << d.remaining << " reset_ms=" << d.reset_ms << "\n";
  return 0;
}
