#include <noisemaker/core.hpp>

#include "core/graph/dsl_compiler.h"
#include "core/graph/expander.h"
#include "core/graph/normalize.h"
#include "core/js/js_syntax.h"
#include "core/lang/diagnostics.h"
#include "core/lang/effect_registry.h"
#include "core/lang/lexer.h"
#include "core/lang/parser.h"
#include "core/lang/validator.h"

#include <utility>
#include <vector>

namespace libnoisemaker {

struct Registry::Impl {
    EffectRegistry registry;
};

struct RegistryAccess {
    static const EffectRegistry& of(const Registry& registry) { return registry.impl_->registry; }
};

namespace {

[[noreturn]] void rethrow_syntax(const DslSyntaxError& error) {
    throw SyntaxError(error.what(), error.line(), error.col(), error.diagnostic());
}

}  // namespace

SyntaxError::SyntaxError(const std::string& message, int line, int column, Value diagnostic)
    : std::runtime_error(message), line_(line), column_(column), diagnostic_(std::move(diagnostic)) {}
SyntaxError::~SyntaxError() = default;
int SyntaxError::line() const noexcept { return line_; }
int SyntaxError::column() const noexcept { return column_; }
const Value& SyntaxError::diagnostic() const noexcept { return diagnostic_; }

CompileError::CompileError(const std::string& message, JsString code, Value diagnostics, Value errors)
    : std::runtime_error(message), code_(std::move(code)), diagnostics_(std::move(diagnostics)),
      errors_(std::move(errors)) {}
CompileError::~CompileError() = default;
const JsString& CompileError::code() const noexcept { return code_; }
const Value& CompileError::diagnostics() const noexcept { return diagnostics_; }
const Value& CompileError::errors() const noexcept { return errors_; }

Registry::Registry() : impl_(std::make_unique<Impl>()) {}
Registry::Registry(Registry&&) noexcept = default;
Registry& Registry::operator=(Registry&&) noexcept = default;
Registry::~Registry() = default;

const Registry& Registry::embedded() {
    static const Registry instance = [] {
        Registry registry;
        registry.impl_->registry.loadEmbedded();
        return registry;
    }();
    return instance;
}

Registry Registry::from_definitions(const Value& definitions) {
    Registry registry;
    registry.impl_->registry.loadDefinitions(definitions);
    return registry;
}

bool Registry::has_effect(const JsString& key) const { return impl_->registry.hasEffect(JsText(key)); }
Value Registry::effect(const JsString& key) const {
    return has_effect(key) ? impl_->registry.getEffectValue(key) : Value();
}
Value Registry::summary() const { return impl_->registry.dumpSummaryValue(); }
Value Registry::std_enums() const { return impl_->registry.enums().std().raw(); }
Value Registry::enum_head(const JsString& name) const {
    return impl_->registry.enums().tryGetHead(JsText(name)).raw();
}
bool Registry::enum_is_leaf(const Value& node) { return Enums::isLeaf(JsValue(node)); }

Value lex(const JsString& source) {
    std::vector<Token> tokens;
    try { tokens = libnoisemaker::lexTokens(source); }
    catch (const DslSyntaxError& error) { rethrow_syntax(error); }
    Array out;
    out.reserve(tokens.size());
    for (const Token& token : tokens) {
        Value json = toJson(token);
        // The reference defines position non-enumerably: parse_tokens reads it,
        // json::stringify leaves it out.
        Object& object = json.as_object();
        if (const Value* position = object.find(u"position")) {
            Value hidden = *position;
            object.erase(u"position");
            object.define_hidden(u"position", std::move(hidden));
        }
        out.push_back(std::move(json));
    }
    return Value(std::move(out));
}

Value parse(const JsString& source) {
    try { return libnoisemaker::parse(libnoisemaker::lexTokens(source)); }
    catch (const DslSyntaxError& error) { rethrow_syntax(error); }
}

Value parse_tokens(const Value& tokens, const Value& options) {
    if (!tokens.is_array()) throw std::invalid_argument("parse_tokens: tokens must be an array");
    std::vector<Token> list;
    list.reserve(tokens.as_array().size());
    for (const Value& token : tokens.as_array()) list.push_back(tokenFromJson(token));
    try { return libnoisemaker::parse(list, options); }
    catch (const DslSyntaxError& error) { rethrow_syntax(error); }
}

Value validate(const Value& ast, const Registry& registry) {
    return libnoisemaker::validate(ast, RegistryAccess::of(registry));
}

Value expand(const Value& validated, const Registry& registry, const Value& options) {
    const ExpandResult expanded =
        libnoisemaker::expand(JsObject(validated), RegistryAccess::of(registry), JsObject(options));
    Array passes;
    passes.reserve(expanded.passes.size());
    for (const auto& pass : expanded.passes) passes.push_back(toRawPassJson(pass));
    Object out;
    out.set(u"passes", Value(std::move(passes)));
    out.set(u"errors", expanded.errors.raw());
    out.set(u"programs", expanded.programs.raw());
    out.set(u"textureSpecs", expanded.textureSpecs.raw());
    out.set(u"renderSurface", expanded.renderSurface.raw());
    out.set(u"mediaSteps", expanded.mediaSteps.raw());
    return Value(std::move(out));
}

Value compile_graph(const JsString& source, const Registry& registry, const Value& options) {
    try {
        const Object opts = options.is_object() ? options.as_object() : Object{};
        return Value(compileGraphJson(source, RegistryAccess::of(registry), opts));
    } catch (const CompilationError& error) {
        throw CompileError(error.what(), error.code(), error.diagnostics().raw(), error.errors().raw());
    } catch (const DslSyntaxError& error) {
        rethrow_syntax(error);
    }
}

Value normalize_graph(const Value& graph, const Registry& registry) {
    return normalizeGraphJson(graph, RegistryAccess::of(registry).defineMap().raw());
}

FunctionBodyVerdict check_function_body(const JsString& param, const JsString& body) {
    switch (js::checkFunctionBody(param, body).verdict) {
        case js::Verdict::Valid: return FunctionBodyVerdict::Valid;
        case js::Verdict::Invalid: return FunctionBodyVerdict::Invalid;
        case js::Verdict::Unsupported: break;
    }
    return FunctionBodyVerdict::Unsupported;
}

}  // namespace libnoisemaker
