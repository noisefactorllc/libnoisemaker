#include <noisemaker/core.h>

#include <stdio.h>
#include <string.h>

int main(void) {
    const char source[] = "search synth\nnoise().write(o0)\nrender(o0)\n";
    nm_compiled* compiled = NULL;
    nm_diagnostics* diagnostics = NULL;
    if (nm_compile(source, strlen(source), &compiled, &diagnostics) != NM_OK) return 1;
    if (!strstr(nm_compiled_graph_json_normalized(compiled), "\"NOISE_TYPE\":10")) return 2;
    nm_compiled_destroy(compiled);
    nm_diagnostics_destroy(diagnostics);
    printf("consumer_c: ok, engine %s\n", nm_engine_version());
    return 0;
}
