// Catches the library's exceptions and passes values across its boundary in
// both directions.
#include <noisemaker/core.hpp>

#include <cstdio>

namespace nm = libnoisemaker;

int main() {
    const nm::Registry& registry = nm::Registry::embedded();

    int caught = 0;
    try {
        nm::parse(u"noise(");
    } catch (const nm::SyntaxError& error) {
        caught += error.line() == 1 ? 1 : 0;
    }
    try {
        nm::compile_graph(u"search synth\n", registry);
    } catch (const nm::CompileError& error) {
        caught += error.code() == u"ERR_EXPANSION_FAILED" && error.errors().is_array() ? 1 : 0;
    }
    try {
        nm::json::parse("{");
    } catch (const nm::json::ParseError&) {
        ++caught;
    }
    if (caught != 3) return 1;

    // A value built here goes in; values built in the library come out.
    nm::Object definitions;
    definitions.set(u"synth/noise", registry.effect(u"synth/noise"));
    const nm::Registry custom = nm::Registry::from_definitions(nm::Value(definitions));
    const nm::Value graph = nm::compile_graph(u"search synth\nnoise().write(o0)\nrender(o0)\n", custom);
    const nm::Value normalized = nm::normalize_graph(graph, custom);
    const nm::Value* passes = normalized.as_object().find(u"passes");
    if (!passes || !passes->is_array() || passes->as_array().empty()) return 2;
    std::printf("consumer_cxx: ok, %s\n", nm::json::stringify(*passes->as_array().front().as_object().find(u"func")).c_str());
    return 0;
}
