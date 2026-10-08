#pragma once

// js_unicode.h -- ECMAScript source character classes, as V8 applies them
// (identifier tables from ICU; see js_unicode.cpp).

namespace nm::js {

// IdentifierStartChar: ID_Start, `$` or `_`.
bool isIdStartCodePoint(char32_t cp);
// IdentifierPartChar: ID_Continue, `$`, ZWNJ or ZWJ (`_` is in ID_Continue).
bool isIdPartCodePoint(char32_t cp);
// WhiteSpace: TAB, VT, FF, ZWNBSP and the Zs category (SP and NBSP among it).
bool isWhiteSpace(char32_t cp);
// LineTerminator: LF, CR, LS, PS.
bool isLineTerminator(char32_t cp);

} // namespace nm::js
