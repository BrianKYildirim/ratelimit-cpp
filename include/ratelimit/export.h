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
#else
  // Non-Windows, or static build
  #define RL_API
#endif
