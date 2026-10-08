#pragma once

// js_syntax.h -- ECMAScript syntax check for the DSL's arrow-function bodies.
//
// The reference validator compiles every Func (`() => expr`) value with
// `new Function('state', `with(state){ return ${src}; }`)` and reports S001
// when that throws. Nothing ever calls the function, so the only observable
// result is whether V8 accepts the body. This checker answers that question
// for V8 as run by the parity oracle (node 24 / V8 13.6 and node 26 /
// V8 14.6 agree on every construct it accepts).
//
// Unsupported marks input that this checker cannot decide exactly (see
// js_syntax.cpp); callers fail loud on it instead of guessing.

#include "core/value/js_string.h"

namespace nm::js {

enum class Verdict { Valid, Invalid, Unsupported };

struct CheckResult {
    Verdict verdict = Verdict::Invalid;
    // Reason for Invalid or Unsupported (diagnostic text for logs and
    // tests; not a V8 message).
    JsString detail;
};

// Whether V8's `new Function(param, body)` accepts `body`: `param` (one
// simple identifier) is the only formal parameter, and `body` is parsed as a
// sloppy-mode FunctionBody.
CheckResult checkFunctionBody(const JsString& param, const JsString& body);

} // namespace nm::js
