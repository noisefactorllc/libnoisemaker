#include <noisemaker/noisemaker.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); return 1; } } while (0)

int main(void) {
    CHECK(nm_abi_version() == NM_ABI_VERSION);

    CHECK(nm_context_create(NULL, NULL) == NM_ERR_INVALID_ARGUMENT);
    CHECK(nm_last_error() != NULL && strlen(nm_last_error()) > 0);

    nm_context_desc desc;
    memset(&desc, 0, sizeof desc);
    desc.struct_size = sizeof desc;
    desc.allow_fallback_adapter = getenv("NM_ALLOW_FALLBACK_ADAPTER") != NULL;

    nm_context* ctx = NULL;
    nm_status status = nm_context_create(&desc, &ctx);
    if (status != NM_OK) fprintf(stderr, "nm_context_create: %d %s\n", (int)status, nm_last_error());
    CHECK(status == NM_OK);
    CHECK(ctx != NULL);
    CHECK(nm_context_adapter_name(ctx) != NULL && strlen(nm_context_adapter_name(ctx)) > 0);
    nm_context_destroy(ctx);
    nm_context_destroy(NULL);

    desc.struct_size = 1;
    CHECK(nm_context_create(&desc, &ctx) == NM_ERR_INVALID_ARGUMENT);

    printf("test_context: ok\n");
    return 0;
}
