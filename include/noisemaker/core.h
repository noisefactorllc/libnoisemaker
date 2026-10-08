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

typedef struct nm_compiled nm_compiled;
typedef struct nm_diagnostics nm_diagnostics;

NM_API uint32_t nm_abi_version(void);
NM_API const char* nm_engine_version(void);
/* Source is UTF-8. Diagnostic positions count UTF-16 code units. */
NM_API nm_status nm_compile(const char* source, size_t length,
                            nm_compiled** out, nm_diagnostics** diagnostics);
NM_API void nm_compiled_destroy(nm_compiled* compiled);
/* Valid until nm_compiled_destroy is called. */
NM_API const char* nm_compiled_graph_json(const nm_compiled* compiled);
NM_API size_t nm_diagnostics_count(const nm_diagnostics* diagnostics);
NM_API const char* nm_diagnostics_json(const nm_diagnostics* diagnostics, size_t index);
NM_API void nm_diagnostics_destroy(nm_diagnostics* diagnostics);
/* Returns a newly allocated UTF-8 DSL string; release with nm_string_free. */
NM_API nm_status nm_unparse(const nm_compiled* compiled,
                            const char* overrides_json, char** out_source);
NM_API void nm_string_free(char* source);
/* The message for the last failed call on this thread. Never NULL.
 * Valid until the next libnoisemaker call on this thread. */
NM_API const char* nm_last_error(void);

#ifdef __cplusplus
}
#endif

#endif
