# C++ Rate Limiter

A fast, thread-safe, in-process token-bucket rate limiter for C++20 applications.

The library provides a sharded implementation designed for concurrent workloads, precise fixed-point refill calculations, and multiple integration options through C++, C, and optional JNI bindings.

## Features

* C++20 API
* Thread-safe, in-process rate limiting with linearizable per-key decisions
* Sharded open-addressing key storage: one hash and no allocation per lookup
* Token-bucket algorithm
* Exact integer refill arithmetic (no rounding drift)
* Per-key limits
* Configurable capacity and refill rate
* Idle-key eviction to bound memory
* C-compatible API
* Optional Java/JNI bridge
* CMake package installation support
* `find_package(Ratelimit CONFIG REQUIRED)` integration
* Optional `pkg-config` metadata
* GoogleTest unit tests
* Google Benchmark performance tests
* Optional ThreadSanitizer support
* Windows, Linux, and macOS support

## How It Works

Each key receives its own token bucket.

A bucket is configured with:

* `capacity`: Maximum number of tokens the bucket can hold
* `refill_per_sec`: Number of tokens added per second

Each call to `allow()` attempts to consume one token:

* If a token is available, the request is allowed.
* If no token is available, the request is denied.
* The response includes the number of remaining tokens and the estimated time until the next token becomes available.

```text
Request key
    │
    ▼
Hash key to a shard
    │
    ▼
Lock only the selected shard
    │
    ▼
Refill tokens based on elapsed time
    │
    ▼
Consume one token if available
    │
    ▼
Return allow/deny decision
```

## C++ API

### Basic usage

```C++
#include <iostream>
#include <ratelimit/rate_limiter.h>

int main() {
    rl::RateLimiterInProc limiter;

    // Maximum of 10 tokens, refilling at 5 tokens per second.
    rl::Limit limit{
        .capacity = 10,
        .refill_per_sec = 5
    };

    auto decision = limiter.allow("user:123", limit);

    if (decision.allowed) {
        std::cout << "Request allowed\n";
    } else {
        std::cout << "Request denied\n";
    }

    std::cout << "Remaining tokens: " << decision.remaining << '\n';
    std::cout << "Reset time: " << decision.reset_ms << " ms\n";

    return 0;
}
```

### Decision fields

```C++
struct Decision {
    bool allowed;
    uint64_t remaining;
    uint64_t reset_ms;
};
```

| Field       | Description                                                                                                                                            |
| ----------- | ------------------------------------------------------------------------------------------------------------------------------------------------------ |
| `allowed`   | Whether the request was accepted                                                                                                                       |
| `remaining` | Whole tokens remaining in the bucket                                                                                                                   |
| `reset_ms`  | Milliseconds until another token is available when denied, rounded up; `0` if a token can never become available (`refill_per_sec` or `capacity` is 0) |

`capacity` values above 18,446,744,073 (2^64 / 10^9) are clamped to that value.

### Custom configuration

The in-process limiter can be configured with a custom number of shards and an expected capacity per shard.

```C++
rl::RateLimiterInProc::Config config{
    .shards = 128,
    .capacity_hint_per_shard = 1024
};

rl::RateLimiterInProc limiter(config);
```

The default configuration is:

```text
shards: 128
capacity_hint_per_shard: 1024
```

`shards` is rounded up to a power of two (1 to 65536). A shard reserves room for `capacity_hint_per_shard` keys the first time a key lands in it, so an unused limiter allocates almost nothing.

### Clearing stored keys

```C++
limiter.clear();
```

### Inspecting the number of tracked keys

```C++
std::size_t key_count = limiter.size();
```

### Evicting idle keys

Every distinct key is remembered until it is evicted or `clear()` is called. To bound memory when keys are unbounded (IP addresses, user IDs), periodically evict keys that have been idle for a while:

```C++
// Forget keys not seen for 10 minutes; returns how many were removed.
std::size_t evicted = limiter.evict_idle(10ull * 60 * 1'000'000'000);
```

Eviction is invisible to callers as long as `idle_ns` is at least the time an empty bucket needs to refill completely (`capacity / refill_per_sec` seconds): such a key has refilled to capacity, which is exactly how a new key starts. Eviction locks one shard at a time and rebuilds only shards that lose keys.

## C API

The library also provides a C-compatible interface through:

```C
#include <ratelimit/ratelimit_c.h>
```

Example:

```C
#include <ratelimit/ratelimit_c.h>
#include <stdio.h>

int main(void) {
    void* limiter = rl_new_inproc(128, 1024);

    rl_limit_t limit = {
        .capacity = 10,
        .refill_per_sec = 5
    };

    rl_decision_t decision = rl_allow(limiter, "user:123", limit);

    printf("allowed=%s remaining=%llu reset_ms=%llu\n",
           decision.allowed ? "yes" : "no",
           (unsigned long long) decision.remaining,
           (unsigned long long) decision.reset_ms);

    rl_free(limiter);
    return 0;
}
```

The C API exposes:

```C
const char* rl_version(void);

void* rl_new_inproc(
    unsigned shards,
    unsigned capacity_hint_per_shard
);

rl_decision_t rl_allow(
    void* handle,
    const char* key,
    rl_limit_t limit
);

void rl_free(void* handle);

uint64_t rl_evict_idle(void* handle, uint64_t idle_ns);
```

## Requirements

* CMake 3.16 or newer
* A compiler with C++20 support
* Ninja is recommended but not required
* Java and JNI development headers are required when JNI bindings are enabled

Supported environments include:

* Windows with MSVC or MSYS2/MinGW
* Linux
* macOS

## Build From Source

Clone the repository:

```Shell
git clone https://github.com/BrianKYildirim/ratelimit-cpp.git
cd ratelimit-cpp
```

Configure a Release build:

```Shell
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DRL_BUILD_JNI=OFF
```

Build the project:

```Shell
cmake --build build
```

The build produces the following primary targets:

* `ratelimit_core` — static C++ core library
* `ratelimit` — optional shared C++ library
* `ratelimit_c` — shared C-compatible library
* `rl_cli` — command-line example
* `rl_tests` — GoogleTest test executable
* `rl_bench` — optional Google Benchmark executable

> JNI is enabled by default in the current CMake configuration. Use `-DRL_BUILD_JNI=OFF` if Java/JNI is not installed.

## Run the Example CLI

After building:

```Shell
./build/rl_cli
```

The CLI reads keys from standard input and applies a default limit of 10 tokens with a refill rate of 5 tokens per second.

Example:

```text
Enter keys (Ctrl+D to exit). Using capacity=10, refill=5/s
user-1
user-1 -> allowed=yes remaining=9 reset_ms=0
user-1
user-1 -> allowed=yes remaining=8 reset_ms=0
```

You can also pipe input into the CLI:

```Shell
printf "user-1\nuser-1\nuser-1\n" | ./build/rl_cli
```

On Windows, use the generated executable path:

```PowerShell
build\rl_cli.exe
```

## Run Tests

Tests are enabled by default.

Run the test suite through CTest:

```Shell
ctest --test-dir build --output-on-failure
```

The tests cover:

* Initial bucket capacity, bursts, and denial after tokens are exhausted
* Exact token refill over time, including rates that are not a whole number of nanoseconds per token
* Reset-time calculations and rounding
* Zero, very large, and clamped capacities; zero and huge refill rates; overflow
* Clocks going backwards and limits changing between calls
* Isolation between different keys, key lengths, empty keys, and embedded NULs
* Table growth, `clear()`, and `evict_idle()`
* Concurrency: exact grant counts and no lost or duplicated refills under contention
* Deterministic clock-based testing

Run the tests under sanitizers with `-DRL_ENABLE_ASAN=ON` (AddressSanitizer + UndefinedBehaviorSanitizer) or `-DRL_ENABLE_TSAN=ON` (ThreadSanitizer).

GoogleTest is downloaded automatically by CMake through `FetchContent` when tests are enabled.

## Run Benchmarks

Benchmarks are enabled by default.

Build the benchmark target:

```Shell
cmake --build build --target rl_bench
```

Run it:

```Shell
./build/rl_bench
```

The benchmark suite measures, at 1 to 16 threads:

* `Hot_Allowed` / `Hot_Denied`: one key shared by all threads (worst-case contention)
* `Uniform_Allowed` / `Uniform_Denied`: 100k or 1M pre-inserted keys
* `UniformLong_Allowed`: 100k pre-inserted keys longer than 64 bytes
* `NewKeys`: every call inserts a previously unseen key
* `MemoryPerKey`: heap bytes per tracked key (glibc only)

Setup (key generation, pre-insertion) is excluded from timing, and the real `SteadyClock` is used. For multi-threaded runs, read the `total_ops/s` counter: Google Benchmark sums wall time across threads, so its `Time` column is per-thread latency per call.

Benchmark results depend on the compiler, CPU, operating system, build configuration, and workload. Use Release builds when comparing performance.

## CMake Options

The project supports the following configuration options:

| Option            | Default | Description                                          |
| ----------------- | ------: | ---------------------------------------------------- |
| `RL_BUILD_SHARED` |    `ON` | Build the shared C++ library                         |
| `RL_BUILD_TESTS`  |    `ON` | Build GoogleTest tests                               |
| `RL_BUILD_BENCH`  |    `ON` | Build Google Benchmark tests                         |
| `RL_BUILD_JNI`    |    `ON` | Build Java/JNI bindings                              |
| `RL_ENABLE_TSAN`  |   `OFF` | Enable ThreadSanitizer flags where supported         |
| `RL_ENABLE_ASAN`  |   `OFF` | Enable AddressSanitizer + UndefinedBehaviorSanitizer |

If `CMAKE_BUILD_TYPE` is not set, single-configuration generators default to `Release`.

Example minimal build:

```Shell
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DRL_BUILD_SHARED=OFF \
  -DRL_BUILD_TESTS=OFF \
  -DRL_BUILD_BENCH=OFF \
  -DRL_BUILD_JNI=OFF
```

Example development build with tests and benchmarks:

```Shell
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DRL_BUILD_TESTS=ON \
  -DRL_BUILD_BENCH=ON \
  -DRL_BUILD_JNI=OFF
```

## Install the Library

Install the headers and libraries to a custom prefix:

```Shell
cmake --install build --prefix ./install
```

The installation contains:

```text
install/
├── include/ratelimit/       # Public headers
├── lib/                     # Libraries and CMake package files
├── bin/                     # Runtime DLLs on Windows
└── lib/pkgconfig/           # pkg-config metadata
```

## Use From Another CMake Project

After installing the library, point `CMAKE_PREFIX_PATH` at the installation directory:

```Shell
cmake -S consumer -B consumer-build -G Ninja \
  -DCMAKE_PREFIX_PATH="$PWD/install"

cmake --build consumer-build
```

A consuming project's `CMakeLists.txt` can use:

```CMake
cmake_minimum_required(VERSION 3.16)

project(example LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Ratelimit CONFIG REQUIRED)

add_executable(example main.cpp)

target_link_libraries(example
    PRIVATE Ratelimit::ratelimit_core
)
```

For the shared C++ library:

```CMake
target_link_libraries(example
    PRIVATE Ratelimit::ratelimit
)
```

The repository includes a sample consumer project in `consumer/`.

## Thread Safety

`RateLimiterInProc` is designed for concurrent access.

The implementation:

* Splits key storage across multiple shards
* Makes each decision (lookup, refill, consume) atomically under a short per-shard lock, so decisions for a key are linearizable and no refill or consumption is ever lost
* Reads the clock and hashes the key before taking the lock, and prefetches the key's slot so its cache miss overlaps lock acquisition
* Keeps operations for unrelated keys independent
* Supports concurrent calls to `allow()`, `size()`, `clear()`, and `evict_idle()`

The per-shard lock is a test-and-test-and-set spinlock that yields to the scheduler if it spins for long. Critical sections are a few dozen instructions. In benchmarks the spinlock was as fast as or faster than `std::mutex` at every thread count tested (1 to 16 threads on 8 hardware threads), and up to 2.7× faster on a contended hot key.

The number of shards can be adjusted through `RateLimiterInProc::Config`.

More shards may reduce contention for workloads with many concurrent keys, while fewer shards may reduce overhead for smaller workloads.

## Design Details

### Sharded storage

Each key is hashed once. The high bits of the hash pick a shard and the low bits a slot within it. Each shard is a linear-probing hash table (maximum load factor 3/4) with its own lock, so contention stays low compared with one global lock.

Each 48-byte slot stores the full hash, the bucket state, and the key. Keys up to 20 bytes are stored inline; longer keys use one exact-size heap block. Lookups never allocate. Inserting a key allocates only if the key is longer than 20 bytes or the table has to grow.

### Token-bucket rate limiting

Each key has a bucket with:

* A maximum capacity
* A current token count
* A refill rate
* A timestamp used to calculate elapsed time

Requests consume one token when allowed. Tokens accumulate over time up to the configured capacity.

### Exact integer arithmetic

Balances are kept in nanotokens (10^-9 tokens). Refilling for `dt` nanoseconds at `refill_per_sec` tokens per second then adds exactly `dt * refill_per_sec` nanotokens. That is one 64-bit multiply with an overflow check: no division, no floating point, and no rounding error accumulating across calls. Long idle periods and huge rates saturate at capacity instead of overflowing.

### Injectable clock

The limiter accepts an optional clock abstraction, making time-dependent behavior deterministic and easy to test.

```C++
class FakeClock : public rl::Clock {
public:
    uint64_t time_ns = 0;

    uint64_t now_ns() const noexcept override {
        return time_ns;
    }
};
```

This allows tests to advance time without sleeping.

## Directory Structure

```text
ratelimit-cpp/
├── include/ratelimit/
│   ├── clock.h              # Clock abstraction
│   ├── export.h             # Symbol visibility macros
│   ├── rate_limiter.h       # Public C++ API
│   ├── ratelimit_c.h        # Public C API
│   ├── types.h              # Limits and decisions
│   └── version.h.in         # Generated version header template
├── src/
│   └── inproc_rate_limiter.cpp
├── c_api/
│   └── ratelimit_c.cpp
├── jni/
│   └── ratelimit_jni.cpp
├── examples/
│   └── cli.cpp
├── tests/
│   └── test_core.cpp
├── bench/
│   └── bench_core.cpp
├── consumer/
│   ├── CMakeLists.txt
│   └── main.cpp
├── cmake/
│   ├── RatelimitConfig.cmake.in
│   └── ratelimit.pc.in
├── CMakeLists.txt
├── LICENSE
└── README.md
```

## Use Cases

This library can be used for:

* API request throttling
* Per-user request limits
* Per-IP connection limits
* Login attempt protection
* Background job throttling
* In-process service protection
* Resource consumption limits
* C and C++ server applications
* Java applications through the optional JNI bridge

## Scope and Limitations

This is an **in-process** rate limiter.

That means:

* State exists only inside the current process.
* Multiple processes do not share buckets.
* Multiple machines do not share limits.
* Restarting the process clears all in-memory buckets.
* It is not a distributed replacement for Redis or another shared datastore.

For distributed rate limiting across multiple service instances, use a shared external store or coordinate limits at the gateway layer.

## Package Targets

After installation, downstream CMake projects can link against:

```CMake
Ratelimit::ratelimit_core
Ratelimit::ratelimit
```

The C library is provided through the `ratelimit_c` target.

The project also installs a `ratelimit.pc` file for pkg-config-based workflows:

```Shell
export PKG_CONFIG_PATH="$PWD/install/lib/pkgconfig:$PKG_CONFIG_PATH"
pkg-config --cflags --libs ratelimit
```

## License

This project is released under the MIT License. See [`LICENSE`](LICENSE) for details.

***

Built with C++20, CMake, GoogleTest, and Google Benchmark.
