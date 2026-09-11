#pragma once

#if defined(_WIN32) || defined(__CYGWIN__)
#if defined(FASTCHUNK_BUILD_SHARED)
#define FASTCHUNK_C_EXPORT __declspec(dllexport)
#elif defined(FASTCHUNK_USE_SHARED)
#define FASTCHUNK_C_EXPORT __declspec(dllimport)
#else
#define FASTCHUNK_C_EXPORT
#endif
#else
#if __GNUC__ >= 4
#define FASTCHUNK_C_EXPORT __attribute__((visibility("default")))
#else
#define FASTCHUNK_C_EXPORT
#endif
#endif
