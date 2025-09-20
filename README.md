# ratelimit-cpp

A tiny, fast C++20 rate limiter library with an in-process, sharded token-bucket core, clean C++ and C interfaces, and first-class CMake packaging. Ships with examples, unit tests, and an optional Google Benchmark suite.

- OS: Windows (MSYS2/MinGW, MSVC), Linux, macOS
- Toolchain: CMake (≥3.16), Ninja (recommended), C++20
- Packaging: `find_package(Ratelimit CONFIG REQUIRED)` and pkg-config
- Visibility: Exports controlled via `RL_API`, hidden by default on shared builds

---

## Features

- Sharded in-process token-bucket implementation for excellent concurrency
- Fixed-point arithmetic for precise refill and capacity handling
- Header-only public API surface with compact implementation
- Clean separation of concerns (core, C API, JNI optional)
- Unit tests with GoogleTest
- Optional microbenchmarks with Google Benchmark
- CMake install rules + `RatelimitConfig.cmake` for downstream `find_package`
- Optional `ratelimit.pc` for pkg-config consumers

---

## Directory layout

    .
    ├─ include/ratelimit/        # Public headers
    │  ├─ clock.h
    │  ├─ export.h
    │  ├─ rate_limiter.h
    │  ├─ ratelimit_c.h
    │  ├─ types.h
    │  ├─ version.h
    │  └─ version.h.in           # Template -> generated version.h at build time
    ├─ src/
    │  └─ inproc_rate_limiter.cpp
    ├─ c_api/
    │  └─ ratelimit_c.cpp
    ├─ bench/
    │  └─ bench_core.cpp         # Optional (GBM)
    ├─ tests/
    │  └─ test_core.cpp          # GoogleTest
    ├─ examples/
    │  └─ cli.cpp                # Simple CLI smoke test
    ├─ cmake/
    │  ├─ RatelimitConfig.cmake.in
    │  └─ ratelimit.pc.in        # Optional pkg-config template
    ├─ CMakeLists.txt
    ├─ README.md
    ├─ LICENSE
    └─ .gitignore

---

## Build from source (quick start)

1) Configure the build (static core + shared C++ and C DLLs by default):

    `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release`

2) Build:

    `ninja -C build`

3) Run tests (if enabled):

    `ctest --test-dir build --output-on-failure`

4) Install (choose your prefix):

    `cmake --install build --prefix "C:/Users/you/ratelimit-install"`

You’ll find:
- Headers under `<prefix>/include/ratelimit`
- Libraries and import libs under `<prefix>/lib` (Linux/macOS) or `<prefix>/bin` and `<prefix>/lib` (Windows)
- CMake package files under `<prefix>/lib/cmake/Ratelimit`
- Optional `ratelimit.pc` under `<prefix>/lib/pkgconfig`

---

## Using with CMake (as a consumer)

### Option A: Use CMAKE_PREFIX_PATH

    cmake -S consumer -B consumer-build -G Ninja -DCMAKE_PREFIX_PATH="C:/Users/you/ratelimit-install"
    ninja -C consumer-build

`consumer/CMakeLists.txt`:

    cmake_minimum_required(VERSION 3.16)
    project(consumer LANGUAGES CXX)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)

    find_package(Ratelimit CONFIG REQUIRED)

    add_executable(consumer main.cpp)
    # Link against the static core or the shared C++ library:
    target_link_libraries(consumer PRIVATE Ratelimit::ratelimit_core)
    # or: target_link_libraries(consumer PRIVATE Ratelimit::ratelimit)

`consumer/main.cpp`:

    #include <ratelimit/rate_limiter.h>
    #include <iostream>
    int main() {
      rl::RateLimiterInProc limiter;
      rl::Limit lim{10, 5};  // capacity=10, refill=5/s
      auto d = limiter.allow("test-key", lim);
      std::cout << (d.allowed ? "allowed" : "denied")
                << " remaining=" << d.remaining
                << " reset_ms=" << d.reset_ms << "\n";
      return 0;
    }

### Option B: Use Ratelimit_DIR

    cmake -S consumer -B consumer-build -G Ninja -DRatelimit_DIR="C:/Users/you/ratelimit-install/lib/cmake/Ratelimit"
    ninja -C consumer-build

---

## Using from C (C API)

`c_consumer.c`:

    #include <ratelimit/ratelimit_c.h>
    #include <stdio.h>

    int main() {
      printf("%s\n", rl_version());
      return 0;
    }

Compile and link against the installed C library (on Windows, add the install `bin` to PATH to run).

---

## Example CLI

After building:

    ./build/rl_cli
    Enter keys (Ctrl+D to exit). Using capacity=10, refill=5/s
    foo
    foo -> allowed=yes remaining=9 reset_ms=0

You can also pipe input:

    yes foo | head -n 15 | ./build/rl_cli

---

## Benchmarks (optional)

Enable at configure time:

    cmake -S . -B build -G Ninja -DRL_BUILD_BENCH=ON -DCMAKE_BUILD_TYPE=Release
    ninja -C build rl_bench
    ./build/rl_bench

Example output (will vary by CPU):

    BM_UniformKeys        260 ns        items_per_second=3.8M/s
    BM_HotKey             135 ns        items_per_second=7.4M/s

---

## Design notes

- Token bucket using Q32.32 fixed-point counters for precise refill math
- Sharded hash map: key space split by FNV-1a hash; reduces lock contention
- Read path first tries shared lock; insert uses unique lock
- Clock abstraction for deterministic tests (mockable clock)
- Overflow-safe arithmetic and careful atomics

---

## Symbol visibility and ABI

- On Windows, public API functions/classes are decorated with `__declspec(dllexport/dllimport)` via `RL_API` macro in headers.
- On Unix-like platforms, shared libraries are compiled with `-fvisibility=hidden -fvisibility-inlines-hidden` and public symbols explicitly exported with `RL_API`.
- The static core (`ratelimit_core`) defines `RL_STATIC` privately, so headers compiled within that target use non-dll semantics without leaking to downstream consumers.

---

## Configuration switches

Common CMake options:

- `RL_BUILD_SHARED` (ON|OFF) — Build shared C++ library `ratelimit` in addition to static core
- `RL_BUILD_TESTS` (ON|OFF) — Build and run GoogleTest unit tests
- `RL_BUILD_BENCH` (ON|OFF) — Build Google Benchmark suite
- `RL_BUILD_JNI` (ON|OFF) — Build JNI bridge (requires Java/JNI)
- `RL_ENABLE_TSAN` (ON|OFF) — Build with ThreadSanitizer (where supported)

Example:

    cmake -S . -B build -G Ninja -DRL_BUILD_SHARED=ON -DRL_BUILD_TESTS=ON -DRL_BUILD_BENCH=OFF

---

## pkg-config

If you enabled pkg-config template, install produces `ratelimit.pc`.

Example:

    export PKG_CONFIG_PATH="/your/prefix/lib/pkgconfig:$PKG_CONFIG_PATH"
    pkg-config --cflags --libs ratelimit --define-prefix

---

## FAQ

**Q: Which target should I link?**  
Use `Ratelimit::ratelimit_core` for the static core or `Ratelimit::ratelimit` for the shared C++ library. The C API is in `libratelimit_c` (shared).

**Q: Windows linking notes?**  
Make sure the `bin` folder containing DLLs is on `PATH` at runtime. When using MSYS2/MinGW, you’ll see `libratelimit.dll` and import libs in `lib`.

**Q: Thread safety?**  
Per-key operations are safe; the structure uses sharded maps and shared/unique locks internally. Your own key strings should remain valid for the duration of each call.

**Q: License?**  
See the LICENSE file.

---

## License

This project is released under the terms of the license in the LICENSE file.

---

## Acknowledgments

- GoogleTest, Google Benchmark
- The broader C++ community for patterns on symbol visibility and CMake packaging
