cmake_minimum_required(VERSION 3.16)
project(consumer LANGUAGES CXX)
find_package(Ratelimit REQUIRED CONFIG)

option(USE_SHARED "Link with the shared C++ library" ON)

add_executable(consumer main.cpp)
if (USE_SHARED)
  target_link_libraries(consumer PRIVATE Ratelimit::ratelimit)
else()
  target_link_libraries(consumer PRIVATE Ratelimit::ratelimit_core)
endif()
