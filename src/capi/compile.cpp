#include <noisemaker/core.h>

#include "capi/error.h"
#include "core/graph/dsl_compiler.h"
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
    nm::JsString source;
};

struct nm_diagnostics {
    std::vector<std::string> entries;
};

namespace {
nm::EffectRegistry& registry() {
    static nm::EffectRegistry instance;
    static const bool loaded = (instance.loadEmbedded(), true);
    (void)loaded;
    return instance;
}

nm::Value syntax_diagnostic(const nm::DslSyntaxError& error) {
    if (error.diagnostic().is_object()) return error.diagnostic();
    nm::Object diagnostic;
    diagnostic.set(u"code", nm::Value(u"P001"));
    diagnostic.set(u"stage", nm::Value(u"parser"));
    diagnostic.set(u"severity", nm::Value(u"error"));
    diagnostic.set(u"message", nm::Value(error.message()));
    if (error.line() > 0 && error.col() > 0) {
        nm::Object location;
        location.set(u"line", nm::Value(error.line()));
        location.set(u"column", nm::Value(error.col()));
        diagnostic.set(u"location", nm::Value(location));
    }
    return nm::Value(diagnostic);
}

void append(nm_diagnostics& out, const nm::Value& diagnostic,
            const nm::JsString& stage, const nm::JsString& code) {
    nm::Object entry = diagnostic.is_object() ? diagnostic.as_object() : nm::Object{};
    if (!entry.has(u"code")) entry.set(u"code", nm::Value(code));
    if (!entry.has(u"stage")) entry.set(u"stage", nm::Value(stage));
    if (!entry.has(u"severity")) entry.set(u"severity", nm::Value(u"error"));
    if (!entry.has(u"message")) entry.set(u"message", diagnostic);
    if (!entry.has(u"location")) entry.set(u"location", nm::Value::null());
    if (!entry.has(u"span")) entry.set(u"span", nm::Value::null());
    out.entries.push_back(nm::json::stringify(nm::Value(entry)));
}

void set_error_noexcept(const char* message) noexcept {
    try { nm::capi::set_error(message); }
    catch (...) {}
}
}  // namespace

static nm_status compile_impl(const char* source, size_t length,
                              nm_compiled** out, nm_diagnostics** diagnostics) {
    nm::capi::clear_error();
    if (out) *out = nullptr;
    if (diagnostics) *diagnostics = nullptr;
    if (!source || !out || !diagnostics) {
        nm::capi::set_error("nm_compile: source, out and diagnostics are required");
        return NM_ERR_INVALID_ARGUMENT;
    }
    try {
        const nm::JsString text = nm::utf8_to_js(std::string_view(source, length));
        const nm::Object graph = nm::compileGraphJson(text, registry());
        auto compiled = std::make_unique<nm_compiled>();
        compiled->graph_json = nm::json::stringify(nm::Value(graph));
        compiled->source = text;
        *out = compiled.release();
        return NM_OK;
    } catch (const nm::CompilationError& error) {
        auto output = std::make_unique<nm_diagnostics>();
        for (const auto& item : error.diagnostics())
            append(*output, item.raw(), u"compiler", error.code());
        for (const auto& item : error.errors())
            append(*output, item.raw(), u"expander", error.code());
        if (output->entries.empty()) {
            nm::Object diagnostic;
            diagnostic.set(u"message", nm::Value(nm::utf8_to_js(error.what())));
            append(*output, nm::Value(diagnostic), u"compiler", error.code());
        }
        nm::capi::set_error(error.what());
        *diagnostics = output.release();
        return NM_ERR_COMPILE;
    } catch (const nm::DslSyntaxError& error) {
        auto output = std::make_unique<nm_diagnostics>();
        append(*output, syntax_diagnostic(error), u"parser", u"P001");
        nm::capi::set_error(error.what());
        *diagnostics = output.release();
        return NM_ERR_COMPILE;
    } catch (const std::exception& error) {
        nm::capi::set_error(error.what());
        return NM_ERR_INTERNAL;
    } catch (...) {
        nm::capi::set_error("nm_compile: unknown exception");
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
    nm::capi::clear_error();
    if (out_source) *out_source = nullptr;
    if (!compiled || !out_source) {
        nm::capi::set_error("nm_unparse: compiled and out_source are required");
        return NM_ERR_INVALID_ARGUMENT;
    }
    try {
        const nm::Value overrides = overrides_json
            ? nm::json::parse(overrides_json) : nm::Value(nm::Object{});
        const nm::Value validated = nm::validate(nm::parse(nm::lex(compiled->source)), registry());
        const nm::JsString source = nm::unparse(validated, overrides,
            nm::UnparseOptions::from_value(nm::Value(), &registry()), registry());
        const std::string utf8 = nm::js_to_utf8(source);
        char* result = static_cast<char*>(std::malloc(utf8.size() + 1));
        if (!result) {
            nm::capi::set_error("nm_unparse: allocation failed");
            return NM_ERR_OUT_OF_MEMORY;
        }
        std::memcpy(result, utf8.c_str(), utf8.size() + 1);
        *out_source = result;
        return NM_OK;
    } catch (const std::exception& error) {
        nm::capi::set_error(error.what());
        return NM_ERR_INTERNAL;
    } catch (...) {
        nm::capi::set_error("nm_unparse: unknown exception");
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
