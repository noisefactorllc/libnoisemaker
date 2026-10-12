#pragma once

#include "core/value/js_string.h"

#include <string>
#include <string_view>

namespace libnoisemaker::js {
JsString numberToString(double value);
// StringToNumber (ECMA-262 7.1.4.1.1), as Number(string) reads it: whitespace
// and line terminators trimmed, an empty string is 0, then a signed decimal
// literal or Infinity, or an unsigned 0x, 0o or 0b integer; NaN otherwise.
double stringToNumber(const JsString& text);

// WhiteSpace and LineTerminator, the code points parseFloat, parseInt,
// String.prototype.trim and the regular-expression class \s skip.
bool isWhiteSpaceOrLineTerminator(char16_t c);
// parseFloat(text): leading white space skipped, then the longest prefix
// that is a StrDecimalLiteral ([+-] Infinity | digits [. [digits]] [exponent]
// | . digits [exponent]); NaN when no prefix is one. parseFloatPrefix reads
// a token that starts at the literal.
double parseFloat(const JsString& text);
double parseFloatPrefix(std::u16string_view token);

namespace detail {
// The shortest round-trip digits of a finite double above zero, as "d.ddde±x".
std::string shortest_scientific(double magnitude);
}  // namespace detail
}  // namespace libnoisemaker::js

namespace libnoisemaker {
JsString js_number_to_string(double value);
}
