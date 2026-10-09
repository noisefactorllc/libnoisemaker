#pragma once
#include "core/lang/compat.h"
#include <cmath>
namespace nm {
namespace TokenType {
// literals / identifiers
inline const JsString NUMBER = LangString(u"NUMBER");
inline const JsString STRING = LangString(u"STRING");
inline const JsString HEX = LangString(u"HEX");
inline const JsString FUNC = LangString(u"FUNC");
inline const JsString IDENT = LangString(u"IDENT");

// surface refs
inline const JsString OUTPUT_REF = LangString(u"OUTPUT_REF");
inline const JsString SOURCE_REF = LangString(u"SOURCE_REF");
inline const JsString VOL_REF = LangString(u"VOL_REF");
inline const JsString GEO_REF = LangString(u"GEO_REF");
inline const JsString XYZ_REF = LangString(u"XYZ_REF");
inline const JsString VEL_REF = LangString(u"VEL_REF");
inline const JsString RGBA_REF = LangString(u"RGBA_REF");
inline const JsString MESH_REF = LangString(u"MESH_REF");

// punctuation
inline const JsString DOT = LangString(u"DOT");
inline const JsString LPAREN = LangString(u"LPAREN");
inline const JsString RPAREN = LangString(u"RPAREN");
inline const JsString LBRACE = LangString(u"LBRACE");
inline const JsString RBRACE = LangString(u"RBRACE");
inline const JsString LBRACKET = LangString(u"LBRACKET");
inline const JsString RBRACKET = LangString(u"RBRACKET");
inline const JsString COMMA = LangString(u"COMMA");
inline const JsString COLON = LangString(u"COLON");
inline const JsString EQUAL = LangString(u"EQUAL");
inline const JsString SEMICOLON = LangString(u"SEMICOLON");
inline const JsString PLUS = LangString(u"PLUS");
inline const JsString MINUS = LangString(u"MINUS");
inline const JsString STAR = LangString(u"STAR");
inline const JsString SLASH = LangString(u"SLASH");

// keywords (RESERVED_KEYWORDS — reference/01 §1.3)
inline const JsString LET = LangString(u"LET");
inline const JsString RENDER = LangString(u"RENDER");
inline const JsString WRITE = LangString(u"WRITE");
inline const JsString WRITE3D = LangString(u"WRITE3D");
inline const JsString TRUE = LangString(u"TRUE");
inline const JsString FALSE = LangString(u"FALSE");
inline const JsString IF = LangString(u"IF");
inline const JsString ELIF = LangString(u"ELIF");
inline const JsString ELSE = LangString(u"ELSE");
inline const JsString BREAK = LangString(u"BREAK");
inline const JsString CONTINUE = LangString(u"CONTINUE");
inline const JsString RETURN = LangString(u"RETURN");
inline const JsString SEARCH = LangString(u"SEARCH");
inline const JsString SUBCHAIN = LangString(u"SUBCHAIN");

// trivia / end
inline const JsString COMMENT = LangString(u"COMMENT");
inline const JsString EOF_ = LangString(u"EOF"); // identifier dodges the <cstdio> EOF macro
} // namespace TokenType

struct Token {
    enum class NonStringType { None, Function, Object };
    NonStringType nonStringType = NonStringType::None;
    JsString type;
    JsString lexeme;
    int line = 0, col = 0;
    bool hasLine = false, hasCol = false;
    JsString rawLine, rawCol;
    bool hasPosition = false;
    int posLine = 0, posColumn = 0, posStart = -1, posEnd = -1;
};

inline Value toJson(const Token& t) {
    Object o;
    if (t.nonStringType == Token::NonStringType::Object) o.set(u"type", Value(Object{}));
    else if (t.nonStringType == Token::NonStringType::None) o.set(u"type", Value(t.type));
    o.set(u"lexeme", Value(JsString(t.lexeme)));
    o.set(u"line", Value(t.line));
    o.set(u"col", Value(t.col));
    if (t.hasPosition) {
        Object p;
        p.set(u"line", Value(t.posLine));
        p.set(u"column", Value(t.posColumn));
        p.set(u"start", Value(t.posStart));
        p.set(u"end", Value(t.posEnd));
        o.set(u"position", Value(p));
    }
    return Value(o);
}
inline Token tokenFromJson(const Value& value) {
    Token t;
    if (!value.is_object()) return t;
    const Object& o = value.as_object();
    auto field = [&](const char16_t* key) -> Value { const Value* v = o.find(key); return v ? *v : Value(); };
    Value type = field(u"type"), lexeme = field(u"lexeme");
    if (type.is_string()) t.type = type.as_string();
    else if (type.is_object()) { t.nonStringType = Token::NonStringType::Object; t.type = u"[object Object]"; }
    if (lexeme.is_string()) t.lexeme = lexeme.as_string();
    if (type.is_undefined() && !t.lexeme.empty()) {
        static const std::unordered_set<JsString> inherited = {
            u"constructor", u"toString", u"valueOf", u"hasOwnProperty", u"isPrototypeOf",
            u"propertyIsEnumerable", u"toLocaleString", u"__defineGetter__", u"__defineSetter__",
            u"__lookupGetter__", u"__lookupSetter__"};
        if (inherited.contains(t.lexeme)) {
            t.nonStringType = Token::NonStringType::Function;
            t.type = u"function " + (t.lexeme == u"constructor" ? JsString(u"Object") : t.lexeme)
                     + u"() { [native code] }";
        }
    }
    auto coord = [&](const char16_t* key, int& target, bool& has, JsString& raw) {
        Value v = field(key);
        if (v.is_number()) {
            double n = v.as_number();
            raw = LangString::number(n);
            if (std::isfinite(n) && n == std::floor(n) && n >= -2147483648.0 && n <= 2147483647.0) {
                target = static_cast<int>(n); has = target > 0;
                raw = LangString::number(target);
            }
        } else if (v.is_string()) raw = LangString(v.as_string());
        else raw = v.is_null() ? LangString(u"null") : LangString(u"undefined");
    };
    coord(u"line", t.line, t.hasLine, t.rawLine);
    coord(u"col", t.col, t.hasCol, t.rawCol);
    Value pos = field(u"position");
    if (pos.is_object()) {
        const Object& p = pos.as_object();
        auto num = [&](const char16_t* key, int& out) {
            const Value* v = p.find(key);
            if (!v || !v->is_number()) return false;
            double n = v->as_number();
            if (!std::isfinite(n) || n != std::floor(n) || n < -2147483648.0 || n > 2147483647.0) return false;
            out = static_cast<int>(n); return true;
        };
        t.hasPosition = num(u"line", t.posLine) && num(u"column", t.posColumn)
            && num(u"start", t.posStart) && num(u"end", t.posEnd)
            && t.posLine > 0 && t.posColumn > 0 && t.posStart >= 0 && t.posEnd >= t.posStart;
    }
    return t;
}
} // namespace nm
