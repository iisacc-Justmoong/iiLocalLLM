#pragma once
#if defined(_WIN32)
# if defined(IILOCALLLM_BUILDING_LIBRARY)
#  define IILOCALLLM_EXPORT __declspec(dllexport)
# else
#  define IILOCALLLM_EXPORT __declspec(dllimport)
# endif
#else
# define IILOCALLLM_EXPORT __attribute__((visibility("default")))
#endif
