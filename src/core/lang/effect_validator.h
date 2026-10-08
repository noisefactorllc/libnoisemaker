#pragma once

#include "enums.h"

// effect_validator.h -- structural validation of an effect definition against
// the grammar the runtime consumes. Port of the REFERENCE
// shaders/src/runtime/effect-validator.js validateEffectDefinition()
// (upstream commits ba87ffae + 9d3474df: full definition-grammar
// validation contract -- metadata, globals, passes, textures, dimension
// expressions, uniform layouts, enabledBy conditions, ui controls, and
// top-level unknown-field diagnosis, with byte-identical error strings).
//
// Contract (mirrors the reference doc comment): deterministic and
// side-effect-free. Returns a list of error strings; empty for valid input.
// Returns errors for malformed/null/array/non-object containers, never mutates
// the input, never invokes lifecycle hooks, and never sorts runtime globals.
// As in JavaScript, string coercion of an object with a noncallable own
// toString property can throw a TypeError.
// Declaration/schema validation is distinct from shader compilation, GPU
// capability, and runtime behavior.
//
// PORT-SHAPE NOTES:
//   - Plain JSON definitions receive top-level unknown-field diagnosis.
//     The parity harness's tagged Effect instance/subclass wrappers follow
//     the reference's instance paths and skip only that top-level check.
//   - Lifecycle hooks use Value::Function when supplied by a host. Other
//     present values receive the reference's non-function error.
//   - Converter metadata such as `starter` and `jsHooks` is unknown to the
//     reference definition grammar and is rejected here.
//   - builtinMeshes accepts both the reference's map form
//     ({name: "path"}) and the port's generated array form
//     ([{name, path}] -- see effects/render/meshLoader.json), which the
//     converter emits for this port's mesh loader.
//   - Object fields retain declaration order, including the order of
//     per-key diagnostics.
//
// Not wired into EffectRegistry::loadEmbedded(): the reference ships the
// validator as an authoring/test-time surface (only its own test suite and
// the corpus gate call it), and this port mirrors that surface.


#include <string>
#include <vector>

namespace nm {

// Validate a definition. `def` may be any JsValue shape; the error list
// is empty iff the definition satisfies the full grammar.
std::vector<std::string> validateEffectDefinition(const JsValue& def);
std::vector<std::string> validateEffectDefinition(const Value& def);

// Convenience overload for already-extracted definition objects.
std::vector<std::string> validateEffectDefinition(const JsObject& def);

// Injectable std-enum table the validator resolves `member` defaults and
// `enum` paths against (the reference imports std_enums.js directly). The
// table is COPIED, so callers may pass any object, including temporaries;
// the validator's live default (nm::Enums::std()) is used until this is
// called, and the last call's copy is kept.
void setEffectValidatorStdEnums(const JsObject& stdEnums);
void setEffectValidatorStdEnums(const Value& stdEnums);

} // namespace nm
