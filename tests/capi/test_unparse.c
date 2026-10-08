#include <noisemaker/core.h>

#include <stdio.h>
#include <string.h>

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
        return 1; \
    } \
} while (0)

int main(void) {
    /* parity/programs/curl.dsl at the pinned frontend corpus revision. */
    const char source[] = "search synth\ncurl().write(o0)\nrender(o0)\n";
    nm_compiled *first = NULL, *second = NULL;
    nm_diagnostics *first_errors = NULL, *second_errors = NULL;
    char* regenerated = NULL;

    CHECK(nm_compile(source, strlen(source), &first, &first_errors) == NM_OK);
    CHECK(nm_unparse(first, NULL, &regenerated) == NM_OK);
    CHECK(regenerated != NULL);
    CHECK(nm_compile(regenerated, strlen(regenerated), &second, &second_errors) == NM_OK);
    /* Reformatting changes the source and its source-derived graph ID. */
    const char* first_passes = strstr(nm_compiled_graph_json(first), "\"passes\":");
    const char* second_passes = strstr(nm_compiled_graph_json(second), "\"passes\":");
    CHECK(first_passes != NULL);
    CHECK(second_passes != NULL);
    CHECK(strcmp(first_passes, second_passes) == 0);

    nm_string_free(regenerated);
    nm_compiled_destroy(first);
    nm_compiled_destroy(second);
    nm_diagnostics_destroy(first_errors);
    nm_diagnostics_destroy(second_errors);
    return 0;
}
