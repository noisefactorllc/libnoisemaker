#ifndef NOISEMAKER_CORE_H
#define NOISEMAKER_CORE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(NM_SHARED)
#  if defined(_WIN32)
#    if defined(NM_BUILDING)
#      define NM_API __declspec(dllexport)
#    else
#      define NM_API __declspec(dllimport)
#    endif
#  else
#    define NM_API __attribute__((visibility("default")))
#  endif
#else
#  define NM_API
#endif

#define NM_ABI_VERSION 1u

typedef enum nm_status {
    NM_OK = 0,
    NM_ERR_INVALID_ARGUMENT = 1,
    NM_ERR_NO_ADAPTER = 2,
    NM_ERR_DEVICE_FAILED = 3,
    NM_ERR_DEVICE_LOST = 4,
    NM_ERR_GPU_VALIDATION = 5,
    NM_ERR_OUT_OF_MEMORY = 6,
    NM_ERR_COMPILE = 7,
    NM_ERR_INTERNAL = 8
} nm_status;

NM_API uint32_t nm_abi_version(void);
NM_API const char* nm_engine_version(void);
/* The message for the last failed call on this thread. Never NULL.
 * Valid until the next libnoisemaker call on this thread. */
NM_API const char* nm_last_error(void);

#ifdef __cplusplus
}
#endif

#endif
