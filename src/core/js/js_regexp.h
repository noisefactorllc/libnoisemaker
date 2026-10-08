#pragma once

// js_regexp.h -- early-error validation of a regular-expression literal, as
// V8 performs it while parsing (see js_regexp.cpp).

#include "core/value/js_string.h"

namespace nm::js {

enum class RegexCheck { Valid, Invalid, Undecidable };

// Validates the flags and pattern of the literal `/pattern/flags` as V8 does.
// Undecidable marks groups nested deeper than the validator follows. For
// Invalid and Undecidable, *detail receives a reason (diagnostic text, not a
// V8 message).
RegexCheck validateRegExpLiteral(const JsString& pattern, const JsString& flags, JsString* detail);

} // namespace nm::js
