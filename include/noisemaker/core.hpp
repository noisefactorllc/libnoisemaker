#ifndef NOISEMAKER_CORE_HPP
#define NOISEMAKER_CORE_HPP

// The C++ API of libnoisemaker's DSL compiler. Each stage returns the
// reference engine's own output of that stage as JSON values, in the
// reference's member order. Other failures throw std::runtime_error.

#include <noisemaker/core.h>
#include <noisemaker/value.hpp>

#include <memory>
#include <stdexcept>
#include <string>

#if defined(_MSC_VER)
#  pragma warning(push)
#  pragma warning(disable : 4251 4275)
#endif

namespace libnoisemaker {

// A DSL syntax error from the lexer or parser.
class NM_CXX_API SyntaxError : public std::runtime_error {
 public:
    SyntaxError(const std::string& message, int line, int column, Value diagnostic);
    ~SyntaxError() override;
    int line() const noexcept;                 // 1-based; -1 when the error has no location
    int column() const noexcept;               // UTF-16 code units, 1-based; -1 when none
    const Value& diagnostic() const noexcept;  // the reference's diagnostic record, or undefined

 private:
    int line_;
    int column_;
    Value diagnostic_;
};

// compileGraph's ERR_COMPILATION_FAILED (diagnostics) or ERR_EXPANSION_FAILED (errors).
class NM_CXX_API CompileError : public std::runtime_error {
 public:
    CompileError(const std::string& message, JsString code, Value diagnostics, Value errors);
    ~CompileError() override;
    const JsString& code() const noexcept;
    const Value& diagnostics() const noexcept;  // validate()'s diagnostics, warnings included
    const Value& errors() const noexcept;       // expand()'s errors: [{message[, step]}]

 private:
    JsString code_;
    Value diagnostics_;
    Value errors_;
};

// An effect registry: the embedded catalog, or definitions a caller supplies.
// The compiler stages only read a registry.
class NM_CXX_API Registry {
 public:
    static const Registry& embedded();                          // built once, on first use
    static Registry from_definitions(const Value& definitions); // {"<ns>/<func>": definition, ...}
    Registry(Registry&&) noexcept;
    Registry& operator=(Registry&&) noexcept;
    ~Registry();

    bool has_effect(const JsString& key) const;
    Value effect(const JsString& key) const;      // the definition as JSON; undefined when absent
    Value summary() const;                        // the registry dump the registry gate compares
    Value std_enums() const;                      // the standard enum tree
    Value enum_head(const JsString& name) const;  // Enums::tryGetHead; undefined when absent
    static bool enum_is_leaf(const Value& node);  // Enums::isLeaf

 private:
    friend struct RegistryAccess;
    struct Impl;
    Registry();
    std::unique_ptr<Impl> impl_;
};

enum class FunctionBodyVerdict { Valid, Invalid, Unsupported };

// Tokens carry their non-enumerable `position` member, which parse() reads.
NM_CXX_API Value lex(const JsString& source);                                  // throws SyntaxError
NM_CXX_API Value parse(const JsString& source);                                // lex, then parse; throws SyntaxError
NM_CXX_API Value parse_tokens(const Value& tokens, const Value& options = Value());  // throws SyntaxError
NM_CXX_API Value validate(const Value& ast, const Registry& registry);
// {passes, errors, programs, textureSpecs, renderSurface, mediaSteps}
NM_CXX_API Value expand(const Value& validated, const Registry& registry, const Value& options = Value());
// The reference compiler graph; throws CompileError or SyntaxError.
NM_CXX_API Value compile_graph(const JsString& source, const Registry& registry, const Value& options = Value());
// compile_graph's output in the normalized shape a host executor loads.
NM_CXX_API Value normalize_graph(const Value& graph, const Registry& registry);
// Whether V8's `new Function(param, body)` accepts `body`.
NM_CXX_API FunctionBodyVerdict check_function_body(const JsString& param, const JsString& body);

}  // namespace libnoisemaker

#if defined(_MSC_VER)
#  pragma warning(pop)
#endif

#endif
