// include/ratelimit/export.h
#pragma once

// If you compile the lib as static, callers shouldn't see dllimport/dllexport.
#if defined(_WIN32) && !defined(RL_STATIC)
  // Building *this* DLL should define RL_EXPORTS on its compile command line.
  #if defined(RL_EXPORTS)
    #define RL_API __declspec(dllexport)
  #else
    #define RL_API __declspec(dllimport)
  #endif
#elif defined(__GNUC__) || defined(__clang__)
  // The shared libraries build with hidden visibility; this exports the API.
  #define RL_API __attribute__((visibility("default")))
#else
  #define RL_API
#endif
