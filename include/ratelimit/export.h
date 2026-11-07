// include/ratelimit/export.h
#pragma once

// RL_EXPORTS is checked first: ratelimit_c links the static core (which brings
// RL_STATIC) and still has to export its C API.
#if defined(_WIN32)
  // Building *this* DLL should define RL_EXPORTS on its compile command line.
  #if defined(RL_EXPORTS)
    #define RL_API __declspec(dllexport)
  // If you compile the lib as static, callers shouldn't see dllimport/dllexport.
  #elif defined(RL_STATIC)
    #define RL_API
  #else
    #define RL_API __declspec(dllimport)
  #endif
#elif defined(__GNUC__) || defined(__clang__)
  // The shared libraries build with hidden visibility; this exports the API.
  #define RL_API __attribute__((visibility("default")))
#else
  #define RL_API
#endif
