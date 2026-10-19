#include <noisemaker/core.h>

#include <stdio.h>
#include <string.h>

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
        return 1; \
    } \
} while (0)

/* Whether a diagnostic's JSON holds this code, severity and message. */
static int has_diagnostic(const char* json, const char* code, const char* severity,
                          const char* message) {
    char expected[512];
    snprintf(expected, sizeof expected, "\"code\":\"%s\"", code);
    if (!strstr(json, expected)) return 0;
    snprintf(expected, sizeof expected, "\"severity\":\"%s\"", severity);
    if (!strstr(json, expected)) return 0;
    snprintf(expected, sizeof expected, "\"message\":\"%s\"", message);
    return strstr(json, expected) != NULL;
}

int main(void) {
    /* parity/programs/curl.dsl at the pinned frontend corpus revision. */
    const char source[] = "search synth\ncurl().write(o0)\nrender(o0)\n";
    nm_compiled* compiled = NULL;
    nm_diagnostics* diagnostics = NULL;
    CHECK(nm_compile(source, strlen(source), &compiled, &diagnostics) == NM_OK);
    CHECK(compiled != NULL);
    /* A program with no warnings gives no diagnostics. */
    CHECK(diagnostics == NULL);
    CHECK(nm_diagnostics_count(diagnostics) == 0);
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
    CHECK(diagnostics == NULL);
    const char* normalized = nm_compiled_graph_json_normalized(compiled);
    CHECK(strstr(normalized, "\"func\":\"noise\"") != NULL);
    CHECK(strstr(normalized, "\"progName\":\"noise\"") != NULL);
    CHECK(strstr(normalized, "\"NOISE_TYPE\":10") != NULL);
    CHECK(strstr(normalized, "\"type\":10") == NULL);
    CHECK(strstr(nm_compiled_graph_json(compiled), "\"type\":10") != NULL);
    nm_compiled_destroy(compiled);
    nm_diagnostics_destroy(diagnostics);

    /* The reference compileGraph logs each validator warning of a program that
     * compiles. nm_compile gives them to the caller in *diagnostics. The
     * messages are the reference's own at the lock in parity/reference.json. */
    compiled = NULL;
    diagnostics = NULL;
    const char clamped[] = "search synth\nnoise(scaleX: 99999).write(o0)\nrender(o0)\n";
    CHECK(nm_compile(clamped, strlen(clamped), &compiled, &diagnostics) == NM_OK);
    CHECK(compiled != NULL);
    CHECK(strstr(nm_compiled_graph_json(compiled), "\"passes\"") != NULL);
    CHECK(diagnostics != NULL);
    CHECK(nm_diagnostics_count(diagnostics) == 1);
    CHECK(has_diagnostic(nm_diagnostics_json(diagnostics, 0), "S002", "warning",
        "Argument out of range for 'scaleX' in noise() (got 99999, clamped to 100)"));
    CHECK(strcmp(nm_diagnostics_json(diagnostics, 1), "") == 0);
    nm_compiled_destroy(compiled);
    nm_diagnostics_destroy(diagnostics);

    compiled = NULL;
    diagnostics = NULL;
    const char clamped_twice[] = "search synth\nnoise(scaleX: 99999, scaleY: 99999).write(o0)\nrender(o0)\n";
    CHECK(nm_compile(clamped_twice, strlen(clamped_twice), &compiled, &diagnostics) == NM_OK);
    CHECK(compiled != NULL);
    CHECK(nm_diagnostics_count(diagnostics) == 2);
    CHECK(has_diagnostic(nm_diagnostics_json(diagnostics, 0), "S002", "warning",
        "Argument out of range for 'scaleX' in noise() (got 99999, clamped to 100)"));
    CHECK(has_diagnostic(nm_diagnostics_json(diagnostics, 1), "S002", "warning",
        "Argument out of range for 'scaleY' in noise() (got 99999, clamped to 100)"));
    nm_compiled_destroy(compiled);
    nm_diagnostics_destroy(diagnostics);

    compiled = NULL;
    diagnostics = NULL;
    const char subchain[] = "search synth, filter\nnoise().subchain(bogus: \"x\") {\n  .bloom()\n}.write(o0)\nrender(o0)\n";
    CHECK(nm_compile(subchain, strlen(subchain), &compiled, &diagnostics) == NM_OK);
    CHECK(compiled != NULL);
    CHECK(nm_diagnostics_count(diagnostics) == 1);
    CHECK(has_diagnostic(nm_diagnostics_json(diagnostics, 0), "P008", "warning",
        "Unknown subchain argument 'bogus' at line 2 col 18. Valid keys: name, id. The value is discarded."));
    nm_compiled_destroy(compiled);
    nm_diagnostics_destroy(diagnostics);

    /* A failed compile gives its warnings and its errors, in the reference order. */
    compiled = NULL;
    diagnostics = NULL;
    const char failed[] = "search synth\nnoise(scaleX: 99999, bogus: 1).write(o0)\nrender(o0)\n";
    CHECK(nm_compile(failed, strlen(failed), &compiled, &diagnostics) == NM_ERR_COMPILE);
    CHECK(compiled == NULL);
    CHECK(nm_diagnostics_count(diagnostics) == 2);
    CHECK(has_diagnostic(nm_diagnostics_json(diagnostics, 0), "S002", "warning",
        "Argument out of range for 'scaleX' in noise() (got 99999, clamped to 100)"));
    CHECK(has_diagnostic(nm_diagnostics_json(diagnostics, 1), "S001", "error",
        "Unknown argument 'bogus' for noise()"));
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
