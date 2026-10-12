// The C++ API through the installed headers and exported symbols only: no
// src/ include path, and with NM_BUILD_SHARED a shared noisemaker_core.

#include <noisemaker/core.hpp>

#include <cstdio>
#include <cstdlib>
#include <string>

#define CHECK(cond)                                                                      \
    do {                                                                                 \
        if (!(cond)) {                                                                   \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            std::exit(1);                                                                \
        }                                                                                \
    } while (0)

namespace nm = libnoisemaker;

namespace {

const nm::Value& member(const nm::Value& object, const char16_t* key) {
    static const nm::Value missing;
    if (!object.is_object()) return missing;
    const nm::Value* found = object.as_object().find(key);
    return found ? *found : missing;
}

bool has_string(const nm::Value& object, const char16_t* key, const char16_t* expected) {
    const nm::Value& value = member(object, key);
    return value.is_string() && value.as_string() == expected;
}

const nm::JsString kNoise = u"search synth\nnoise().write(o0)\nrender(o0)\n";

void lex_and_parse() {
    const nm::Value tokens = nm::lex(u"noise().write(o0)");
    CHECK(tokens.is_array() && !tokens.as_array().empty());
    CHECK(has_string(tokens.as_array().back(), u"type", u"EOF"));
    CHECK(member(tokens.as_array().front(), u"position").is_object());
    CHECK(nm::json::stringify(tokens).find("position") == std::string::npos);

    CHECK(nm::json::stringify(nm::parse(kNoise)) == nm::json::stringify(nm::parse_tokens(nm::lex(kNoise))));

    bool thrown = false;
    try {
        nm::parse(u"noise(");
    } catch (const nm::SyntaxError& error) {
        thrown = true;
        CHECK(error.line() == 1);
        CHECK(error.column() > 0);
        CHECK(std::string(error.what()).size() > 0);
    }
    CHECK(thrown);

    // The token positions survive the round trip through JSON values.
    thrown = false;
    try {
        nm::parse_tokens(nm::lex(u"noise("));
    } catch (const nm::SyntaxError& error) {
        thrown = true;
        CHECK(error.line() == 1);
        CHECK(error.column() > 0);
    }
    CHECK(thrown);
}

void stages() {
    const nm::Registry& registry = nm::Registry::embedded();
    const nm::Value validated = nm::validate(nm::parse(kNoise), registry);
    CHECK(validated.is_object());
    const nm::Value expanded = nm::expand(validated, registry);
    CHECK(member(expanded, u"passes").is_array());
    CHECK(member(expanded, u"errors").is_array() && member(expanded, u"errors").as_array().empty());

    const nm::Value graph = nm::compile_graph(kNoise, registry);
    CHECK(member(graph, u"passes").is_array() && !member(graph, u"passes").as_array().empty());
    const nm::Value normalized = nm::normalize_graph(graph, registry);
    for (const nm::Value& pass : member(normalized, u"passes").as_array()) {
        for (const char16_t* key : {u"passType", u"func", u"progName", u"program"}) {
            CHECK(!member(pass, key).is_undefined());
        }
    }
    const nm::Value& first = member(normalized, u"passes").as_array().front();
    CHECK(has_string(first, u"func", u"noise"));
    CHECK(member(member(first, u"defines"), u"NOISE_TYPE").is_number());
    CHECK(member(member(first, u"uniforms"), u"type").is_undefined());
}

void compile_errors() {
    bool thrown = false;
    try {
        nm::compile_graph(u"search synth\n", nm::Registry::embedded());
    } catch (const nm::CompileError& error) {
        thrown = true;
        CHECK(error.code() == u"ERR_EXPANSION_FAILED");
        CHECK(error.errors().is_array() && !error.errors().as_array().empty());
    }
    CHECK(thrown);

    thrown = false;
    try {
        nm::compile_graph(u"search synth\nnoise(bogus: 1).write(o0)\nrender(o0)\n", nm::Registry::embedded());
    } catch (const nm::CompileError& error) {
        thrown = true;
        CHECK(error.code() == u"ERR_COMPILATION_FAILED");
        CHECK(error.diagnostics().is_array() && !error.diagnostics().as_array().empty());
    } catch (const std::exception& error) {
        std::fprintf(stderr, "unexpected exception: %s\n", error.what());
    }
    CHECK(thrown);

    thrown = false;
    try {
        nm::compile_graph(u"noise(", nm::Registry::embedded());
    } catch (const nm::SyntaxError&) {
        thrown = true;
    }
    CHECK(thrown);
}

void registries() {
    const nm::Registry& embedded = nm::Registry::embedded();
    CHECK(&embedded == &nm::Registry::embedded());
    CHECK(embedded.has_effect(u"synth/noise"));
    CHECK(embedded.effect(u"synth/missing").is_undefined());
    const nm::Value summary = embedded.summary();
    const nm::Value& ops = member(summary, u"ops");
    CHECK(ops.is_object() && ops.as_object().size() == 210);
    CHECK(member(embedded.std_enums(), u"oscKind").is_object());
    const nm::Value head = embedded.enum_head(u"oscKind");
    CHECK(head.is_object());
    CHECK(nm::Registry::enum_is_leaf(member(head, u"sine")));
    CHECK(!nm::Registry::enum_is_leaf(head));
    CHECK(embedded.enum_head(u"noSuchEnum").is_undefined());

    nm::Object definitions;
    definitions.set(u"synth/noise", embedded.effect(u"synth/noise"));
    const nm::Registry custom = nm::Registry::from_definitions(nm::Value(definitions));
    CHECK(custom.has_effect(u"synth/noise"));
    CHECK(!custom.has_effect(u"filter/blur"));
    CHECK(nm::json::stringify(nm::compile_graph(kNoise, custom)) ==
          nm::json::stringify(nm::compile_graph(kNoise, embedded)));
}

void values() {
    CHECK(nm::check_function_body(u"state", u"return state.x + 1") == nm::FunctionBodyVerdict::Valid);
    CHECK(nm::check_function_body(u"state", u"return state.x +") == nm::FunctionBodyVerdict::Invalid);

    const nm::Value parsed = nm::json::parse(R"({"b":1,"a":[true,null,"é"]})");
    CHECK(nm::json::stringify(parsed) == R"({"b":1,"a":[true,null,"é"]})");
    CHECK(nm::js_to_utf8(nm::utf8_to_js("é")) == "é");
    bool thrown = false;
    try {
        nm::json::parse("{");
    } catch (const nm::json::ParseError&) {
        thrown = true;
    }
    CHECK(thrown);
}

}  // namespace

int main() {
    lex_and_parse();
    stages();
    compile_errors();
    registries();
    values();
    std::puts("facade: ok");
    return 0;
}
