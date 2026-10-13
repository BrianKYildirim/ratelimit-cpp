# C++ Rate Limiter

A fast, thread-safe, in-process token-bucket rate limiter for C++20 applications.

The library provides a sharded implementation designed for concurrent workloads, precise fixed-point refill calculations, and multiple integration options through C++, C, and optional JNI bindings.

## Features

- C++20 API
- Thread-safe, in-process rate limiting
- Sharded key storage to reduce lock contention
- Token-bucket algorithm
- Fixed-point arithmetic for precise refill behavior
- Per-key limits
- Configurable capacity and refill rate
- C-compatible API
- Optional Java/JNI bridge
- CMake package installation support
- `find_package(Ratelimit CONFIG REQUIRED)` integration
- Optional `pkg-config` metadata
- GoogleTest unit tests
- Google Benchmark performance tests
- Optional ThreadSanitizer support
- Windows, Linux, and macOS support

## How It Works

Each key receives its own token bucket.

A bucket is configured with:

- `capacity`: Maximum number of tokens the bucket can hold
- `refill_per_sec`: Number of tokens added per second

Each call to `allow()` attempts to consume one token:

- If a token is available, the request is allowed.
- If no token is available, the request is denied.
- The response includes the number of remaining tokens and the estimated time until the next token becomes available.

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

```cpp
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

```cpp
struct Decision {
    bool allowed;
    uint64_t remaining;
    uint64_t reset_ms;
};
```

| Field | Description |
|---|---|
| `allowed` | Whether the request was accepted |
| `remaining` | Whole tokens remaining in the bucket |
| `reset_ms` | Milliseconds until another token is available when denied |

### Custom configuration

The in-process limiter can be configured with a custom number of shards and an expected capacity per shard.

```cpp
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

### Clearing stored keys

```cpp
limiter.clear();
```

### Inspecting the number of tracked keys

```cpp
std::size_t key_count = limiter.size();
```

## C API

The library also provides a C-compatible interface through:

```c
#include <ratelimit/ratelimit_c.h>
```

Example:

```c
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

```c
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
```

## Requirements

- CMake 3.16 or newer
- A compiler with C++20 support
- Ninja is recommended but not required
- Java and JNI development headers are required when JNI bindings are enabled

Supported environments include:

- Windows with MSVC or MSYS2/MinGW
- Linux
- macOS

## Build From Source

Clone the repository:

```bash
git clone https://github.com/BrianKYildirim/ratelimit-cpp.git
cd ratelimit-cpp
```

Configure a Release build:

```bash
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DRL_BUILD_JNI=OFF
```

Build the project:

```bash
cmake --build build
```

The build produces the following primary targets:

- `ratelimit_core` — static C++ core library
- `ratelimit` — optional shared C++ library
- `ratelimit_c` — shared C-compatible library
- `rl_cli` — command-line example
- `rl_tests` — GoogleTest test executable
- `rl_bench` — optional Google Benchmark executable

> JNI is enabled by default in the current CMake configuration. Use `-DRL_BUILD_JNI=OFF` if Java/JNI is not installed.

## Run the Example CLI

After building:

```bash
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

```bash
printf "user-1\nuser-1\nuser-1\n" | ./build/rl_cli
```

On Windows, use the generated executable path:

```powershell
build\rl_cli.exe
```

## Run Tests

Tests are enabled by default.

Run the test suite through CTest:

```bash
ctest --test-dir build --output-on-failure
```

The tests cover:

- Initial bucket capacity
- Denial after tokens are exhausted
- Token refill over time
- Isolation between different keys
- Zero-capacity behavior
- Reset-time calculations
- Deterministic clock-based testing

GoogleTest is downloaded automatically by CMake through `FetchContent` when tests are enabled.

## Run Benchmarks

Benchmarks are enabled by default.

Build the benchmark target:

```bash
cmake --build build --target rl_bench
```

Run it:

```bash
./build/rl_bench
```

The benchmark suite measures:

- Requests across many uniformly distributed keys
- Repeated requests against a single hot key

Benchmark results depend on the compiler, CPU, operating system, build configuration, and workload. Use Release builds when comparing performance.

## CMake Options

The project supports the following configuration options:

| Option | Default | Description |
|---|---:|---|
| `RL_BUILD_SHARED` | `ON` | Build the shared C++ library |
| `RL_BUILD_TESTS` | `ON` | Build GoogleTest tests |
| `RL_BUILD_BENCH` | `ON` | Build Google Benchmark tests |
| `RL_BUILD_JNI` | `ON` | Build Java/JNI bindings |
| `RL_ENABLE_TSAN` | `OFF` | Enable ThreadSanitizer flags where supported |

Example minimal build:

```bash
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DRL_BUILD_SHARED=OFF \
  -DRL_BUILD_TESTS=OFF \
  -DRL_BUILD_BENCH=OFF \
  -DRL_BUILD_JNI=OFF
```

Example development build with tests and benchmarks:

```bash
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DRL_BUILD_TESTS=ON \
  -DRL_BUILD_BENCH=ON \
  -DRL_BUILD_JNI=OFF
```

## Install the Library

Install the headers and libraries to a custom prefix:

```bash
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

```bash
cmake -S consumer -B consumer-build -G Ninja \
  -DCMAKE_PREFIX_PATH="$PWD/install"

cmake --build consumer-build
```

A consuming project's `CMakeLists.txt` can use:

```cmake
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

```cmake
target_link_libraries(example
    PRIVATE Ratelimit::ratelimit
)
```

The repository includes a sample consumer project in `consumer/`.

## Thread Safety

`RateLimiterInProc` is designed for concurrent access.

The implementation:

- Splits key storage across multiple shards
- Uses locks at the shard level
- Uses shared and exclusive locking where appropriate
- Keeps operations for unrelated keys independent
- Supports concurrent calls to `allow()`

The number of shards can be adjusted through `RateLimiterInProc::Config`.

More shards may reduce contention for workloads with many concurrent keys, while fewer shards may reduce overhead for smaller workloads.

## Design Details

### Sharded storage

Keys are distributed across shards using a hash function. Each shard maintains its own bucket map and synchronization primitive.

This reduces contention compared with protecting the entire key space with one global lock.

### Token-bucket rate limiting

Each key has a bucket with:

- A maximum capacity
- A current token count
- A refill rate
- A timestamp used to calculate elapsed time

Requests consume one token when allowed. Tokens accumulate over time up to the configured capacity.

### Fixed-point arithmetic

The implementation uses fixed-point arithmetic internally to represent fractional token values while avoiding floating-point rounding problems during refill calculations.

### Injectable clock

The limiter accepts an optional clock abstraction, making time-dependent behavior deterministic and easy to test.

```cpp
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

- API request throttling
- Per-user request limits
- Per-IP connection limits
- Login attempt protection
- Background job throttling
- In-process service protection
- Resource consumption limits
- C and C++ server applications
- Java applications through the optional JNI bridge

## Scope and Limitations

This is an **in-process** rate limiter.

That means:

- State exists only inside the current process.
- Multiple processes do not share buckets.
- Multiple machines do not share limits.
- Restarting the process clears all in-memory buckets.
- It is not a distributed replacement for Redis or another shared datastore.

For distributed rate limiting across multiple service instances, use a shared external store or coordinate limits at the gateway layer.

## Package Targets

After installation, downstream CMake projects can link against:

```cmake
Ratelimit::ratelimit_core
Ratelimit::ratelimit
```

The C library is provided through the `ratelimit_c` target.

The project also installs a `ratelimit.pc` file for pkg-config-based workflows:

```bash
export PKG_CONFIG_PATH="$PWD/install/lib/pkgconfig:$PKG_CONFIG_PATH"
pkg-config --cflags --libs ratelimit
```

## License

This project is released under the MIT License. See [`LICENSE`](LICENSE) for details.

---

Built with C++20, CMake, GoogleTest, and Google Benchmark.
