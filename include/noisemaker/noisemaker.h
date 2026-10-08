#ifndef NOISEMAKER_NOISEMAKER_H
#define NOISEMAKER_NOISEMAKER_H

#include <noisemaker/core.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct nm_context nm_context;

typedef struct nm_context_desc {
    uint32_t struct_size;
    int allow_fallback_adapter;
} nm_context_desc;

/* desc may be NULL for defaults. */
NM_API nm_status nm_context_create(const nm_context_desc* desc, nm_context** out);
NM_API void nm_context_destroy(nm_context* ctx);
NM_API const char* nm_context_adapter_name(const nm_context* ctx);

#ifdef __cplusplus
}
#endif

#endif
