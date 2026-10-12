#pragma once

#include "core/value/js_string.h"

#include <string>

namespace libnoisemaker::js {
JsString numberToString(double value);
// StringToNumber (ECMA-262 7.1.4.1.1), as Number(string) reads it: whitespace
// and line terminators trimmed, an empty string is 0, then a signed decimal
// literal or Infinity, or an unsigned 0x, 0o or 0b integer; NaN otherwise.
double stringToNumber(const JsString& text);

namespace detail {
// The shortest round-trip digits of a finite double above zero, as "d.ddde±x".
std::string shortest_scientific(double magnitude);
}  // namespace detail
}  // namespace libnoisemaker::js

namespace libnoisemaker {
JsString js_number_to_string(double value);
}
