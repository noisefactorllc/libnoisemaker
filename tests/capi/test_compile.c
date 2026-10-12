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
    nm_compiled* compiled = NULL;
    nm_diagnostics* diagnostics = NULL;
    CHECK(nm_compile(source, strlen(source), &compiled, &diagnostics) == NM_OK);
    CHECK(compiled != NULL);
    CHECK(strstr(nm_compiled_graph_json(compiled), "\"passes\"") != NULL);
    CHECK(strstr(nm_compiled_graph_json_normalized(compiled), "\"passType\":\"effect\"") != NULL);
    nm_compiled_destroy(compiled);
    nm_diagnostics_destroy(diagnostics);

    /* synth/noise's type global carries define NOISE_TYPE, so the normalized
     * graph moves it from the pass's uniforms into its defines. */
    compiled = NULL;
    diagnostics = NULL;
    const char noise[] = "search synth\nnoise().write(o0)\nrender(o0)\n";
    CHECK(nm_compile(noise, strlen(noise), &compiled, &diagnostics) == NM_OK);
    const char* normalized = nm_compiled_graph_json_normalized(compiled);
    CHECK(strstr(normalized, "\"func\":\"noise\"") != NULL);
    CHECK(strstr(normalized, "\"progName\":\"noise\"") != NULL);
    CHECK(strstr(normalized, "\"NOISE_TYPE\":10") != NULL);
    CHECK(strstr(normalized, "\"type\":10") == NULL);
    CHECK(strstr(nm_compiled_graph_json(compiled), "\"type\":10") != NULL);
    nm_compiled_destroy(compiled);
    nm_diagnostics_destroy(diagnostics);

    compiled = NULL;
    diagnostics = NULL;
    CHECK(nm_compile("noise(", 6, &compiled, &diagnostics) == NM_ERR_COMPILE);
    CHECK(compiled == NULL);
    CHECK(nm_diagnostics_count(diagnostics) > 0);
    CHECK(strstr(nm_diagnostics_json(diagnostics, 0), "\"code\"") != NULL);
    nm_diagnostics_destroy(diagnostics);

    compiled = NULL;
    diagnostics = NULL;
    const char missing_surface[] = "search synth\n";
    CHECK(nm_compile(missing_surface, strlen(missing_surface), &compiled, &diagnostics) == NM_ERR_COMPILE);
    CHECK(compiled == NULL);
    CHECK(nm_diagnostics_count(diagnostics) > 0);
    const char* expansion = nm_diagnostics_json(diagnostics, 0);
    CHECK(strstr(expansion, "\"code\":\"ERR_EXPANSION_FAILED\"") != NULL);
    CHECK(strstr(expansion, "\"stage\":\"expander\"") != NULL);
    CHECK(strstr(expansion, "\"severity\":\"error\"") != NULL);
    CHECK(strstr(expansion, "\"location\":null") != NULL);
    CHECK(strstr(expansion, "\"span\":null") != NULL);
    nm_diagnostics_destroy(diagnostics);

    CHECK(nm_compile(NULL, 0, &compiled, &diagnostics) == NM_ERR_INVALID_ARGUMENT);
    return 0;
}
