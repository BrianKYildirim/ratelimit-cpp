cmake_minimum_required(VERSION 3.16)
cmake_policy(SET CMP0135 NEW)

project(ratelimit-cpp VERSION 0.1.0 LANGUAGES CXX)

# ---------- Options ----------
option(RL_BUILD_SHARED "Build shared library" ON)
option(RL_BUILD_TESTS  "Build tests" ON)
option(RL_BUILD_BENCH  "Build benchmarks" ON)
option(RL_ENABLE_TSAN  "Enable TSAN in a job (for CI)" OFF)
option(RL_BUILD_JNI    "Build JNI bindings" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ---------- Version / Git info (for version.h) ----------
set(RL_VERSION_MAJOR 0 CACHE STRING "Major version")
set(RL_VERSION_MINOR 1 CACHE STRING "Minor version")
set(RL_VERSION_PATCH 0 CACHE STRING "Patch version")
set(RL_VERSION_STRING "${RL_VERSION_MAJOR}.${RL_VERSION_MINOR}.${RL_VERSION_PATCH}" CACHE STRING "Full version")

find_package(Git QUIET)
if (NOT DEFINED RL_GIT_DESCRIBE)
  if (GIT_FOUND)
    execute_process(
      COMMAND ${GIT_EXECUTABLE} describe --tags --dirty --always
      WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
      OUTPUT_VARIABLE RL_GIT_DESCRIBE
      OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_QUIET
    )
  endif()
  if (RL_GIT_DESCRIBE STREQUAL "")
    set(RL_GIT_DESCRIBE "v${RL_VERSION_STRING}")
  endif()
  set(RL_GIT_DESCRIBE "${RL_GIT_DESCRIBE}" CACHE STRING "Git describe")
endif()

if (NOT DEFINED RL_BUILD_TS)
  string(TIMESTAMP RL_BUILD_TS "%Y%m%d%H%M%S")
  set(RL_BUILD_TS "${RL_BUILD_TS}" CACHE STRING "Build timestamp")
endif()

# ---------- Generated header: version.h ----------
set(RL_GEN_DIR ${CMAKE_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${RL_GEN_DIR}/ratelimit)

configure_file(
  ${CMAKE_SOURCE_DIR}/include/ratelimit/version.h.in
  ${RL_GEN_DIR}/ratelimit/version.h
  @ONLY
)

# ---------- Core library (static) ----------
add_library(ratelimit_core STATIC
  src/inproc_rate_limiter.cpp
)

target_compile_definitions(ratelimit_core PRIVATE RL_FP_SCALE=4294967296ULL) # 2^32

target_include_directories(ratelimit_core PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<BUILD_INTERFACE:${RL_GEN_DIR}>
  $<INSTALL_INTERFACE:include>
)

# IMPORTANT: RL_STATIC is **PRIVATE** to the static core (does not leak to consumers)
target_compile_definitions(ratelimit_core PRIVATE RL_STATIC)

# ---------- Optional shared library (C++) ----------
if (RL_BUILD_SHARED)
  add_library(ratelimit SHARED
    src/inproc_rate_limiter.cpp
  )
  target_compile_definitions(ratelimit PRIVATE RL_FP_SCALE=4294967296ULL RL_EXPORTS)
  target_include_directories(ratelimit PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${RL_GEN_DIR}>
    $<INSTALL_INTERFACE:include>
  )
  set_target_properties(ratelimit PROPERTIES
    OUTPUT_NAME "ratelimit"
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
  )
  # Hide non-API symbols on GCC/Clang
  if (NOT MSVC)
    set_target_properties(ratelimit PROPERTIES
      C_VISIBILITY_PRESET hidden
      CXX_VISIBILITY_PRESET hidden
      VISIBILITY_INLINES_HIDDEN YES
    )
  endif()
endif()

# ---------- C API (shared) ----------
add_library(ratelimit_c SHARED
  c_api/ratelimit_c.cpp
)
target_include_directories(ratelimit_c PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<BUILD_INTERFACE:${RL_GEN_DIR}>
  $<INSTALL_INTERFACE:include>
)
target_link_libraries(ratelimit_c PRIVATE ratelimit_core)
target_compile_definitions(ratelimit_c PRIVATE RL_EXPORTS)

# Hide non-API symbols on GCC/Clang
if (NOT MSVC)
  set_target_properties(ratelimit_c PROPERTIES
    C_VISIBILITY_PRESET hidden
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
  )
endif()

# ---------- JNI (optional) ----------
if (RL_BUILD_JNI)
  find_package(Java REQUIRED)
  find_package(JNI REQUIRED)
  add_library(ratelimit_jni SHARED
    jni/ratelimit_jni.cpp
  )
  target_include_directories(ratelimit_jni PRIVATE ${JNI_INCLUDE_DIRS} include ${RL_GEN_DIR})
  target_link_libraries(ratelimit_jni PRIVATE ratelimit_core)
  # If JNI headers use RL_API, uncomment:
  # target_compile_definitions(ratelimit_jni PRIVATE RL_EXPORTS)
  if (NOT MSVC)
    set_target_properties(ratelimit_jni PROPERTIES
      C_VISIBILITY_PRESET hidden
      CXX_VISIBILITY_PRESET hidden
      VISIBILITY_INLINES_HIDDEN YES
    )
  endif()
endif()

# ---------- Example CLI ----------
add_executable(rl_cli examples/cli.cpp)
target_link_libraries(rl_cli PRIVATE ratelimit_core)

# --- Tests (GoogleTest via FetchContent)
if (RL_BUILD_TESTS)
  include(FetchContent)
  set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  )
  FetchContent_MakeAvailable(googletest)

  enable_testing()
  add_executable(rl_tests tests/test_core.cpp)
  target_link_libraries(rl_tests PRIVATE ratelimit_core GTest::gtest_main)
  include(GoogleTest)
  gtest_discover_tests(rl_tests)
endif()

# ---------- Benchmarks (Google Benchmark via FetchContent) ----------
if (RL_BUILD_BENCH)
  include(FetchContent)
  set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_WERROR OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_WARNINGS OFF CACHE BOOL "" FORCE)

  FetchContent_Declare(
    gbm
    URL https://github.com/google/benchmark/archive/refs/tags/v1.9.0.zip
  )
  FetchContent_MakeAvailable(gbm)

  if (TARGET benchmark)
    if (MSVC)
      target_compile_options(benchmark PRIVATE /wd4061 /wd4062)
    else()
      target_compile_options(benchmark PRIVATE -Wno-error=switch -Wno-switch)
    endif()
  endif()

  add_executable(rl_bench bench/bench_core.cpp)
  target_link_libraries(rl_bench PRIVATE ratelimit_core benchmark::benchmark)
endif()

# ---------- Install / export ----------
include(GNUInstallDirs)

# Install headers (exclude template input) + generated version.h
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        PATTERN "ratelimit/version.h.in" EXCLUDE)
install(FILES ${RL_GEN_DIR}/ratelimit/version.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/ratelimit)

# Collect targets to export; conditionally include 'ratelimit' and 'ratelimit_jni'
set(RL_INSTALL_TARGETS ratelimit_core ratelimit_c rl_cli)
if (RL_BUILD_SHARED)
  list(APPEND RL_INSTALL_TARGETS ratelimit)
endif()
if (RL_BUILD_JNI)
  list(APPEND RL_INSTALL_TARGETS ratelimit_jni)
endif()

install(TARGETS ${RL_INSTALL_TARGETS}
  EXPORT ratelimitTargets
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

# Export for build-tree usage and install the export for find_package
export(EXPORT ratelimitTargets
  FILE "${CMAKE_CURRENT_BINARY_DIR}/RatelimitTargets.cmake"
  NAMESPACE Ratelimit::
)

# ---------- CMake package config (for find_package) ----------
include(CMakePackageConfigHelpers)

write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/RatelimitConfigVersion.cmake"
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion
)

configure_file(
  cmake/RatelimitConfig.cmake.in
  "${CMAKE_CURRENT_BINARY_DIR}/RatelimitConfig.cmake"
  @ONLY
)

configure_file(
  cmake/ratelimit.pc.in
  "${CMAKE_CURRENT_BINARY_DIR}/ratelimit.pc"
  @ONLY
)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/ratelimit.pc"
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)

install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/RatelimitConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/RatelimitConfigVersion.cmake"
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Ratelimit
)

install(EXPORT ratelimitTargets
  NAMESPACE Ratelimit::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Ratelimit
)

# ---------- CI helper flag ----------
if (RL_ENABLE_TSAN)
  add_compile_options(-fsanitize=thread)
  add_link_options(-fsanitize=thread)
endif()
