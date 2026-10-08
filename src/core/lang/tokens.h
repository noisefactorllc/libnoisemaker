#pragma once
#include "core/lang/compat.h"
#include <cmath>
namespace nm {
namespace TokenType {
// literals / identifiers
inline const JsString NUMBER = QStringLiteral("NUMBER");
inline const JsString STRING = QStringLiteral("STRING");
inline const JsString HEX = QStringLiteral("HEX");
inline const JsString FUNC = QStringLiteral("FUNC");
inline const JsString IDENT = QStringLiteral("IDENT");

// surface refs
inline const JsString OUTPUT_REF = QStringLiteral("OUTPUT_REF");
inline const JsString SOURCE_REF = QStringLiteral("SOURCE_REF");
inline const JsString VOL_REF = QStringLiteral("VOL_REF");
inline const JsString GEO_REF = QStringLiteral("GEO_REF");
inline const JsString XYZ_REF = QStringLiteral("XYZ_REF");
inline const JsString VEL_REF = QStringLiteral("VEL_REF");
inline const JsString RGBA_REF = QStringLiteral("RGBA_REF");
inline const JsString MESH_REF = QStringLiteral("MESH_REF");

// punctuation
inline const JsString DOT = QStringLiteral("DOT");
inline const JsString LPAREN = QStringLiteral("LPAREN");
inline const JsString RPAREN = QStringLiteral("RPAREN");
inline const JsString LBRACE = QStringLiteral("LBRACE");
inline const JsString RBRACE = QStringLiteral("RBRACE");
inline const JsString LBRACKET = QStringLiteral("LBRACKET");
inline const JsString RBRACKET = QStringLiteral("RBRACKET");
inline const JsString COMMA = QStringLiteral("COMMA");
inline const JsString COLON = QStringLiteral("COLON");
inline const JsString EQUAL = QStringLiteral("EQUAL");
inline const JsString SEMICOLON = QStringLiteral("SEMICOLON");
inline const JsString PLUS = QStringLiteral("PLUS");
inline const JsString MINUS = QStringLiteral("MINUS");
inline const JsString STAR = QStringLiteral("STAR");
inline const JsString SLASH = QStringLiteral("SLASH");

// keywords (RESERVED_KEYWORDS — reference/01 §1.3)
inline const JsString LET = QStringLiteral("LET");
inline const JsString RENDER = QStringLiteral("RENDER");
inline const JsString WRITE = QStringLiteral("WRITE");
inline const JsString WRITE3D = QStringLiteral("WRITE3D");
inline const JsString TRUE = QStringLiteral("TRUE");
inline const JsString FALSE = QStringLiteral("FALSE");
inline const JsString IF = QStringLiteral("IF");
inline const JsString ELIF = QStringLiteral("ELIF");
inline const JsString ELSE = QStringLiteral("ELSE");
inline const JsString BREAK = QStringLiteral("BREAK");
inline const JsString CONTINUE = QStringLiteral("CONTINUE");
inline const JsString RETURN = QStringLiteral("RETURN");
inline const JsString SEARCH = QStringLiteral("SEARCH");
inline const JsString SUBCHAIN = QStringLiteral("SUBCHAIN");

// trivia / end
inline const JsString COMMENT = QStringLiteral("COMMENT");
inline const JsString EOF_ = QStringLiteral("EOF"); // identifier dodges the <cstdio> EOF macro
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
        else raw = v.is_null() ? QStringLiteral("null") : QStringLiteral("undefined");
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
