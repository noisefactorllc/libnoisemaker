#pragma once

#include "enums.h"

// validator.h -- semantic analysis: AST -> flattened plans. Port of the
// REFERENCE shaders/src/lang/validator.js (source of truth), cross-checked
// against td/noisemaker/compiler/lang/validator.py (registry-passed-in
// class shape; UnsupportedDsl fail-loud points) and
// godot/addons/noisemaker/compiler/lang/validator.gd (full argument-
// resolution fidelity). Control flow (if/elif/else, break/continue/return)
// compiles to the reference's Branch/Break/Continue/Return plans, and Func
// (arrow-function) values are checked with nm::js::checkFunctionBody, the
// V8 syntax check the reference gets from `new Function`. See validator.cpp
// for the PARITY-CRITICAL details.
//
// Produces {plans, diagnostics, render, vars, searchNamespaces} where each
// plan is {chain:[step], write, write3d, final, states} and each step is
// {op, args, from, temp[, builtin]}. Diagnostics are COLLECTED (never
// thrown), matching the reference exactly, EXCEPT the family's shared
// UnsupportedDsl contract (see below), which fails loud immediately.


#include <stdexcept>

namespace nm {

class EffectRegistry;

// DSL input this AOT frontend cannot compile exactly. Never silently
// wrong -- mirrors the sibling ports' NotImplementedException /
// UnsupportedDsl (TD's validator.py). Thrown for a Func (arrow-function)
// body that nm::js::checkFunctionBody cannot decide (nesting deeper than it
// follows); see js_syntax.h.
class UnsupportedDsl : public std::runtime_error {
public:
    explicit UnsupportedDsl(const JsText& message) : std::runtime_error(message.toStdString()) {}
};

// Validates `ast` (as produced by nm::parse) against `registry`'s loaded
// ops/enums/aliases/starters, producing the flattened, argument-resolved
// plan structure the (future) Expander consumes. Throws nm::UnsupportedDsl
// at the family's fail-loud points (see above); all other semantic issues
// are collected into the returned object's `diagnostics` array, never
// thrown (matches the reference -- validate() has exactly one throw site
// of its own, "Missing required search directive", which nm::parse
// already guarantees can never reach here).
Value validate(const Value& ast, EffectRegistry& registry);
JsObject validate(const JsObject& ast, EffectRegistry& registry);

} // namespace nm
