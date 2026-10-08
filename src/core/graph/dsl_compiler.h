#pragma once
#include "core/graph/compat.h"

// Port of the reference runtime compiler. It compiles DSL into expanded
// passes, program sources, resource allocations, texture specs, render
// surface, and media bindings. Graph::fromJson accepts this shape.

#include <stdexcept>

#include "core/graph/graph.h"

namespace nm {

class EffectRegistry;

// The objects compiler.js compileGraph() throws: {code:
// 'ERR_COMPILATION_FAILED', diagnostics} when validate() reports an
// error-severity diagnostic, and {code: 'ERR_EXPANSION_FAILED', errors} when
// expand() returns errors. what() is the text the reference formats for them
// (compiler.js formatError, which recompile() logs; Noisedeck's demo-ui
// formatCompilationError builds the same text for ERR_COMPILATION_FAILED):
// each error-severity diagnostic's message plus " (line L, col C)" when it
// has a location, joined by "; ", or each expand error's message.
class CompilationError : public std::runtime_error {
public:
    CompilationError(const JsText& code, const JsArray& diagnostics, const JsArray& errors);

    // "ERR_COMPILATION_FAILED" or "ERR_EXPANSION_FAILED".
    const JsText& code() const { return code_; }
    // Every validate() diagnostic, warnings included (ERR_COMPILATION_FAILED).
    const JsArray& diagnostics() const { return diagnostics_; }
    // expand()'s errors, [{message[, step]}] (ERR_EXPANSION_FAILED).
    const JsArray& errors() const { return errors_; }

    // compiler.js formatError for the two codes above.
    static JsText format(const JsText& code, const JsArray& diagnostics, const JsArray& errors);

private:
    JsText code_;
    JsArray diagnostics_;
    JsArray errors_;
};

// JS `hashSource` (compiler.js): `hash=((hash<<5)-hash)+charCodeAt(i)`,
// ToInt32 each step (`hash & hash`), then `.toString(36)`. Uses uint32_t
// arithmetic throughout (well-defined wraparound, unlike signed overflow)
// and reinterprets as int32 only at the final base36-conversion step —
// bit-for-bit equivalent to JS's ToInt32 idiom. Godot's `_hash_source` /
// `_to_base36` (orchestrator.gd) independently implement the identical
// algorithm; cross-checked line-for-line.
JsString hashSource(const JsString& source);

// lex -> parse -> validate -> expand -> allocateResources ->
// extractTextureSpecs, returning the reference compiler graph.
// Shader source remains in each program entry. Throws nm::CompilationError mirroring compiler.js's
// ERR_COMPILATION_FAILED (a validate() diagnostic with severity:"error")
// / ERR_EXPANSION_FAILED (expand() returned errors); nm::DslSyntaxError /
// nm::UnsupportedDsl propagate unchanged from the lex/parse/validate/
// expand stages.
Object compileGraphJson(const JsString& source, EffectRegistry& registry,
                        const Object& options = {});

// compileGraphJson() then nm::Graph::fromJson() on the serialized bytes —
// see file header for why this guarantees --dsl/--graph byte-identity.
nm::Graph compileGraph(const JsString& source, EffectRegistry& registry,
                       const Object& options = {});

// Convenience overload using the embedded effect catalog.
nm::Graph compileGraph(const JsString& source, const Object& options = {});

} // namespace nm
