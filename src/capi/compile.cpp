#include <noisemaker/core.h>

#include "capi/error.h"
#include "core/graph/dsl_compiler.h"
#include "core/graph/normalize.h"
#include "core/edit/unparser.h"
#include "core/lang/diagnostics.h"
#include "core/lang/effect_registry.h"
#include "core/lang/lexer.h"
#include "core/lang/parser.h"
#include "core/lang/validator.h"
#include "core/value/json.h"

#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

struct nm_compiled {
    std::string graph_json;
    std::string normalized_json;
    libnoisemaker::JsString source;
};

struct nm_diagnostics {
    std::vector<std::string> entries;
};

namespace {
libnoisemaker::EffectRegistry& registry() {
    static libnoisemaker::EffectRegistry instance;
    static const bool loaded = (instance.loadEmbedded(), true);
    (void)loaded;
    return instance;
}

libnoisemaker::Value syntax_diagnostic(const libnoisemaker::DslSyntaxError& error) {
    if (error.diagnostic().is_object()) return error.diagnostic();
    libnoisemaker::Object diagnostic;
    diagnostic.set(u"code", libnoisemaker::Value(u"P001"));
    diagnostic.set(u"stage", libnoisemaker::Value(u"parser"));
    diagnostic.set(u"severity", libnoisemaker::Value(u"error"));
    diagnostic.set(u"message", libnoisemaker::Value(error.message()));
    if (error.line() > 0 && error.col() > 0) {
        libnoisemaker::Object location;
        location.set(u"line", libnoisemaker::Value(error.line()));
        location.set(u"column", libnoisemaker::Value(error.col()));
        diagnostic.set(u"location", libnoisemaker::Value(location));
    }
    return libnoisemaker::Value(diagnostic);
}

void append(nm_diagnostics& out, const libnoisemaker::Value& diagnostic,
            const libnoisemaker::JsString& stage, const libnoisemaker::JsString& code) {
    libnoisemaker::Object entry = diagnostic.is_object() ? diagnostic.as_object() : libnoisemaker::Object{};
    if (!entry.has(u"code")) entry.set(u"code", libnoisemaker::Value(code));
    if (!entry.has(u"stage")) entry.set(u"stage", libnoisemaker::Value(stage));
    if (!entry.has(u"severity")) entry.set(u"severity", libnoisemaker::Value(u"error"));
    if (!entry.has(u"message")) entry.set(u"message", diagnostic);
    if (!entry.has(u"location")) entry.set(u"location", libnoisemaker::Value::null());
    if (!entry.has(u"span")) entry.set(u"span", libnoisemaker::Value::null());
    out.entries.push_back(libnoisemaker::json::stringify(libnoisemaker::Value(entry)));
}

void set_error_noexcept(const char* message) noexcept {
    try { libnoisemaker::capi::set_error(message); }
    catch (...) {}
}
}  // namespace

static nm_status compile_impl(const char* source, size_t length,
                              nm_compiled** out, nm_diagnostics** diagnostics) {
    libnoisemaker::capi::clear_error();
    if (out) *out = nullptr;
    if (diagnostics) *diagnostics = nullptr;
    if (!source || !out || !diagnostics) {
        libnoisemaker::capi::set_error("nm_compile: source, out and diagnostics are required");
        return NM_ERR_INVALID_ARGUMENT;
    }
    try {
        const libnoisemaker::JsString text = libnoisemaker::utf8_to_js(std::string_view(source, length));
        const libnoisemaker::Object graph = libnoisemaker::compileGraphJson(text, registry());
        auto compiled = std::make_unique<nm_compiled>();
        compiled->graph_json = libnoisemaker::json::stringify(libnoisemaker::Value(graph));
        compiled->normalized_json = libnoisemaker::json::stringify(
            libnoisemaker::normalizeGraphJson(libnoisemaker::Value(graph), registry().defineMap().raw()));
        compiled->source = text;
        *out = compiled.release();
        return NM_OK;
    } catch (const libnoisemaker::CompilationError& error) {
        auto output = std::make_unique<nm_diagnostics>();
        for (const auto& item : error.diagnostics())
            append(*output, item.raw(), u"compiler", error.code());
        for (const auto& item : error.errors())
            append(*output, item.raw(), u"expander", error.code());
        if (output->entries.empty()) {
            libnoisemaker::Object diagnostic;
            diagnostic.set(u"message", libnoisemaker::Value(libnoisemaker::utf8_to_js(error.what())));
            append(*output, libnoisemaker::Value(diagnostic), u"compiler", error.code());
        }
        libnoisemaker::capi::set_error(error.what());
        *diagnostics = output.release();
        return NM_ERR_COMPILE;
    } catch (const libnoisemaker::DslSyntaxError& error) {
        auto output = std::make_unique<nm_diagnostics>();
        append(*output, syntax_diagnostic(error), u"parser", u"P001");
        libnoisemaker::capi::set_error(error.what());
        *diagnostics = output.release();
        return NM_ERR_COMPILE;
    } catch (const std::exception& error) {
        libnoisemaker::capi::set_error(error.what());
        return NM_ERR_INTERNAL;
    } catch (...) {
        libnoisemaker::capi::set_error("nm_compile: unknown exception");
        return NM_ERR_INTERNAL;
    }
}

extern "C" nm_status nm_compile(const char* source, size_t length,
                                 nm_compiled** out, nm_diagnostics** diagnostics) {
    try { return compile_impl(source, length, out, diagnostics); }
    catch (...) {
        if (out) *out = nullptr;
        if (diagnostics) *diagnostics = nullptr;
        set_error_noexcept("nm_compile: error handling failed");
        return NM_ERR_INTERNAL;
    }
}

extern "C" void nm_compiled_destroy(nm_compiled* compiled) { delete compiled; }
extern "C" const char* nm_compiled_graph_json(const nm_compiled* compiled) {
    return compiled ? compiled->graph_json.c_str() : "";
}
extern "C" const char* nm_compiled_graph_json_normalized(const nm_compiled* compiled) {
    return compiled ? compiled->normalized_json.c_str() : "";
}
extern "C" size_t nm_diagnostics_count(const nm_diagnostics* diagnostics) {
    return diagnostics ? diagnostics->entries.size() : 0;
}
extern "C" const char* nm_diagnostics_json(const nm_diagnostics* diagnostics, size_t index) {
    return diagnostics && index < diagnostics->entries.size()
        ? diagnostics->entries[index].c_str() : "";
}
extern "C" void nm_diagnostics_destroy(nm_diagnostics* diagnostics) { delete diagnostics; }

static nm_status unparse_impl(const nm_compiled* compiled,
                              const char* overrides_json, char** out_source) {
    libnoisemaker::capi::clear_error();
    if (out_source) *out_source = nullptr;
    if (!compiled || !out_source) {
        libnoisemaker::capi::set_error("nm_unparse: compiled and out_source are required");
        return NM_ERR_INVALID_ARGUMENT;
    }
    try {
        const libnoisemaker::Value overrides = overrides_json
            ? libnoisemaker::json::parse(overrides_json) : libnoisemaker::Value(libnoisemaker::Object{});
        const libnoisemaker::Value validated = libnoisemaker::validate(libnoisemaker::parse(libnoisemaker::lex(compiled->source)), registry());
        const libnoisemaker::JsString source = libnoisemaker::unparse(validated, overrides,
            libnoisemaker::UnparseOptions::from_value(libnoisemaker::Value(), &registry()), registry());
        const std::string utf8 = libnoisemaker::js_to_utf8(source);
        char* result = static_cast<char*>(std::malloc(utf8.size() + 1));
        if (!result) {
            libnoisemaker::capi::set_error("nm_unparse: allocation failed");
            return NM_ERR_OUT_OF_MEMORY;
        }
        std::memcpy(result, utf8.c_str(), utf8.size() + 1);
        *out_source = result;
        return NM_OK;
    } catch (const std::exception& error) {
        libnoisemaker::capi::set_error(error.what());
        return NM_ERR_INTERNAL;
    } catch (...) {
        libnoisemaker::capi::set_error("nm_unparse: unknown exception");
        return NM_ERR_INTERNAL;
    }
}

extern "C" nm_status nm_unparse(const nm_compiled* compiled,
                                  const char* overrides_json, char** out_source) {
    try { return unparse_impl(compiled, overrides_json, out_source); }
    catch (...) {
        if (out_source) *out_source = nullptr;
        set_error_noexcept("nm_unparse: error handling failed");
        return NM_ERR_INTERNAL;
    }
}

extern "C" void nm_string_free(char* source) { std::free(source); }
