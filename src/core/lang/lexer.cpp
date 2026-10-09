#include <unordered_map>
#include "core/lang/lexer.h"

#include "core/lang/diagnostics.h"
#include "core/js/js_unicode.h"
#include "core/lang/tokens.h"


namespace nm {

namespace {

// PARITY-CRITICAL details replicated exactly from the reference
// shaders/src/lang/lexer.js (source of truth), cross-checked against
// td/noisemaker/compiler/lang/lexer.py and
// godot/addons/noisemaker/compiler/lang/lexer.gd, and spot-verified
// against the reference oracle (NM_REFERENCE_ROOT tools/dump-tokens.mjs)
// for every hazard below -- see task report:
//   - 1-based line/col; col counts UTF-16 code units (LangString indexing
//     matches JS `src[i]` for the DSL's ASCII-only identifier surface).
//     Tabs = 1 col. '\n' resets col=1, line++.
//   - Rule ORDER is load-bearing (do not reorder / "clean up"):
//     whitespace/newline, line comment, block comment, o/s ref, vol ref
//     (BEFORE vel -- disambiguated by the 3rd char), geo ref, xyz ref,
//     vel ref, rgba ref (4-char prefix), mesh ref (4-char prefix), hex
//     color literal (length 4/7/9 ONLY), arrow-function FUNC, leading-dot
//     number, single-char punctuation, triple-quoted string (checked
//     BEFORE single/double), single/double-quoted string, number,
//     identifier/keyword, else throw.
//   - HEX gated to total length 4/7/9 (3/6/8 hex digits); any other
//     length falls through every remaining rule to the final
//     "unexpected character" throw (the '#' itself matches nothing else).
//   - String escapes are NOT decoded: the lexeme is the raw
//     inter-delimiter text (backslash kept, next char just skipped over
//     so an escaped delimiter doesn't end the string early).

// JS String.prototype.trim strips WhiteSpace and LineTerminator code
// units; this explicit check preserves the reference behavior.
LangString jsTrim(const LangString& s) {
    auto isTrimmed = [](char16_t c) { return js::isWhiteSpace(c) || js::isLineTerminator(c); };
    std::ptrdiff_t start = 0;
    std::ptrdiff_t end = s.size();
    while (start < end && isTrimmed(s.at(start))) ++start;
    while (end > start && isTrimmed(s.at(end - 1))) --end;
    return s.mid(start, end - start);
}

bool isDigit(char16_t c) {
    return c >= u'0' && c <= u'9';
}

bool isLetter(char16_t c) {
    return (c >= u'a' && c <= u'z') || (c >= u'A' && c <= u'Z');
}

bool isHexDigit(char16_t c) {
    return isDigit(c) || (c >= u'a' && c <= u'f') || (c >= u'A' && c <= u'F');
}

// Bounds-safe char fetch (JS `src[k]` past the end of the string is
// `undefined`, which compares unequal to any real character; here a NUL
// sentinel that never matches any lexer test serves the same purpose).
char16_t at(const LangString& src, int k) {
    return (k >= 0 && k < src.length()) ? src.at(k) : char16_t(u'\0');
}

const std::unordered_map<LangString, LangString>& keywords() {
    static const std::unordered_map<LangString, LangString> table = {
        {LangString(u"let"), TokenType::LET},
        {LangString(u"render"), TokenType::RENDER},
        {LangString(u"write"), TokenType::WRITE},
        {LangString(u"write3d"), TokenType::WRITE3D},
        {LangString(u"true"), TokenType::TRUE},
        {LangString(u"false"), TokenType::FALSE},
        {LangString(u"if"), TokenType::IF},
        {LangString(u"elif"), TokenType::ELIF},
        {LangString(u"else"), TokenType::ELSE},
        {LangString(u"break"), TokenType::BREAK},
        {LangString(u"continue"), TokenType::CONTINUE},
        {LangString(u"return"), TokenType::RETURN},
        {LangString(u"search"), TokenType::SEARCH},
        {LangString(u"subchain"), TokenType::SUBCHAIN},
    };
    return table;
}

} // namespace

std::vector<Token> lex(const JsString& source) {
    const LangString src(source);
    LangVector<Token> tokens;
    const int n = src.length();
    int i = 0;
    int line = 1;
    int col = 1;
    int srcLine = 1;
    int srcCol = 1;
    int anchor = 0;

    auto add = [&](const LangString& type, const LangString& lexeme, int tokLine, int tokCol, int end) {
        for (int offset = anchor; offset < i; ++offset) {
            if (src.at(offset) == u'\n') {
                srcLine++;
                srcCol = 1;
            } else {
                srcCol++;
            }
        }
        const int startLine = srcLine;
        const int startColumn = srcCol;
        for (int offset = i; offset < end; ++offset) {
            if (src.at(offset) == u'\n') {
                srcLine++;
                srcCol = 1;
            } else {
                srcCol++;
            }
        }
        anchor = end;

        Token token;
        token.type = type;
        token.lexeme = lexeme;
        token.line = tokLine;
        token.col = tokCol;
        token.hasLine = (tokLine > 0);
        token.hasCol = (tokCol > 0);
        token.hasPosition = true;
        token.posLine = startLine;
        token.posColumn = startColumn;
        token.posStart = i;
        token.posEnd = end;
        tokens.push_back(token);
    };

    // Only scan source coordinates on failure. Successful tokens and legacy
    // error messages retain their existing position bookkeeping.
    auto fail = [&](const LangString& code, const LangString& message, int start, int end) {
        int errorLine = 1;
        int column = 1;
        for (int offset = 0; offset < start; ++offset) {
            if (src.at(offset) == u'\n') {
                errorLine++;
                column = 1;
            } else {
                column++;
            }
        }
        JsonObject location;
        location.insert(LangString(u"line"), errorLine);
        location.insert(LangString(u"column"), column);
        JsonObject span;
        span.insert(LangString(u"start"), start);
        span.insert(LangString(u"end"), end);
        JsonObject diagnostic;
        diagnostic.insert(LangString(u"code"), code);
        diagnostic.insert(LangString(u"stage"), diagStage(code));
        diagnostic.insert(LangString(u"severity"), diagSeverity(code));
        diagnostic.insert(LangString(u"message"), message);
        diagnostic.insert(LangString(u"location"), location);
        diagnostic.insert(LangString(u"span"), span);
        throw DslSyntaxError(message, errorLine, column, diagnostic.native());
    };

    while (i < n) {
        char16_t ch = src.at(i);

        if (ch == u' ' || ch == u'\t' || ch == u'\r') {
            i++;
            col++;
            continue;
        }
        if (ch == u'\n') {
            i++;
            line++;
            col = 1;
            continue;
        }

        const int startLine = line;
        const int startCol = col;

        // line comment //...
        if (ch == u'/' && at(src, i + 1) == u'/') {
            int j = i + 2;
            while (j < n && src.at(j) != u'\n') j++;
            add(TokenType::COMMENT, src.mid(i, j - i), startLine, startCol, j);
            col += j - i;
            i = j;
            continue;
        }

        // block comment /* ... */
        if (ch == u'/' && at(src, i + 1) == u'*') {
            int j = i + 2;
            int endLine = line;
            int endCol = col + 2;
            while (j < n && !(src.at(j) == u'*' && at(src, j + 1) == u'/')) {
                if (src.at(j) == u'\n') {
                    endLine++;
                    endCol = 1;
                } else {
                    endCol++;
                }
                j++;
            }
            if (j >= n) {
                fail(LangString(u"L003"),
                     LangString(u"Unterminated comment at line %1 col %2").arg(startLine).arg(startCol),
                     i, n);
            }
            j += 2;
            add(TokenType::COMMENT, src.mid(i, j - i), startLine, startCol, j);
            line = endLine;
            col = endCol + 2;
            i = j;
            continue;
        }

        // output or source reference (o/s + digit)
        if ((ch == u'o' || ch == u's') && isDigit(at(src, i + 1))) {
            int j = i + 1;
            while (j < n && isDigit(src.at(j))) j++;
            const LangString lexeme = src.mid(i, j - i);
            const LangString tokenType = (ch == u'o') ? TokenType::OUTPUT_REF : TokenType::SOURCE_REF;
            const bool isMemberSegment = !tokens.isEmpty() && tokens.last().type == TokenType::DOT;
            if (tokenType == TokenType::OUTPUT_REF && !isMemberSegment
                && !(lexeme.length() == 2 && lexeme.at(1) >= u'0' && lexeme.at(1) <= u'7')) {
                fail(LangString(u"L004"),
                     LangString(u"Output surface reference '%1' is out of range; expected o0-o7 at line %2 col %3")
                         .arg(lexeme).arg(startLine).arg(startCol),
                     i, j);
            }
            add(tokenType, lexeme, startLine, startCol, j);
            col += j - i;
            i = j;
            continue;
        }

        // volume reference (vol0-vol7, or more digits) -- tested BEFORE vel
        if (ch == u'v' && at(src, i + 1) == u'o' && at(src, i + 2) == u'l'
            && isDigit(at(src, i + 3))) {
            int j = i + 3;
            while (j < n && isDigit(src.at(j))) j++;
            add(TokenType::VOL_REF, src.mid(i, j - i), startLine, startCol, j);
            col += j - i;
            i = j;
            continue;
        }

        // geometry reference (geo0-geo7)
        if (ch == u'g' && at(src, i + 1) == u'e' && at(src, i + 2) == u'o'
            && isDigit(at(src, i + 3))) {
            int j = i + 3;
            while (j < n && isDigit(src.at(j))) j++;
            add(TokenType::GEO_REF, src.mid(i, j - i), startLine, startCol, j);
            col += j - i;
            i = j;
            continue;
        }

        // xyz reference (xyz0-xyz7) -- agent position surfaces
        if (ch == u'x' && at(src, i + 1) == u'y' && at(src, i + 2) == u'z'
            && isDigit(at(src, i + 3))) {
            int j = i + 3;
            while (j < n && isDigit(src.at(j))) j++;
            add(TokenType::XYZ_REF, src.mid(i, j - i), startLine, startCol, j);
            col += j - i;
            i = j;
            continue;
        }

        // vel reference (vel0-vel7) -- agent velocity surfaces; 'v' is
        // disambiguated from vol by the 3rd character, and this rule MUST
        // come after the vol rule above (rule ORDER is parity behavior).
        if (ch == u'v' && at(src, i + 1) == u'e' && at(src, i + 2) == u'l'
            && isDigit(at(src, i + 3))) {
            int j = i + 3;
            while (j < n && isDigit(src.at(j))) j++;
            add(TokenType::VEL_REF, src.mid(i, j - i), startLine, startCol, j);
            col += j - i;
            i = j;
            continue;
        }

        // rgba reference (rgba0-rgba7) -- agent color surfaces
        if (ch == u'r' && at(src, i + 1) == u'g' && at(src, i + 2) == u'b'
            && at(src, i + 3) == u'a' && isDigit(at(src, i + 4))) {
            int j = i + 4;
            while (j < n && isDigit(src.at(j))) j++;
            add(TokenType::RGBA_REF, src.mid(i, j - i), startLine, startCol, j);
            col += j - i;
            i = j;
            continue;
        }

        // mesh reference (mesh0-mesh7) -- mesh geometry surfaces
        if (ch == u'm' && at(src, i + 1) == u'e' && at(src, i + 2) == u's'
            && at(src, i + 3) == u'h' && isDigit(at(src, i + 4))) {
            int j = i + 4;
            while (j < n && isDigit(src.at(j))) j++;
            add(TokenType::MESH_REF, src.mid(i, j - i), startLine, startCol, j);
            col += j - i;
            i = j;
            continue;
        }

        // html hex color literal -- only length 4/7/9 (3/6/8 hex digits);
        // anything else falls through to the final "unexpected character"
        // throw below (the '#' matches no other rule).
        if (ch == u'#') {
            int j = i + 1;
            while (j < n && isHexDigit(src.at(j))) j++;
            const int len = j - i;
            if (len == 4 || len == 7 || len == 9) {
                add(TokenType::HEX, src.mid(i, len), startLine, startCol, j);
                col += len;
                i = j;
                continue;
            }
        }

        // arrow function expression (() => expr)
        if (ch == u'(' && at(src, i + 1) == u')') {
            int j = i + 2;
            while (j < n && (src.at(j) == u' ' || src.at(j) == u'\t')) j++;
            if (at(src, j) == u'=' && at(src, j + 1) == u'>') {
                j += 2;
                while (j < n && (src.at(j) == u' ' || src.at(j) == u'\t')) j++;
                int depth = 0;
                const int exprStart = j;
                while (j < n) {
                    const char16_t c = src.at(j);
                    if (c == u'(') {
                        depth++;
                    } else if (c == u')') {
                        if (depth == 0) break;
                        depth--;
                    } else if (depth == 0) {
                        if (c == u',' || c == u';' || c == u'\n'
                            || c == u'}') {
                            break;
                        }
                    }
                    j++;
                }
                const LangString expr = jsTrim(src.mid(exprStart, j - exprStart));
                add(TokenType::FUNC, expr, startLine, startCol, j);
                col += j - i;
                i = j;
                continue;
            }
            // else fall through: '(' handled by single-char punctuation below
        }

        if (ch == u'.' && isDigit(at(src, i + 1))) {
            int j = i + 1;
            while (j < n && isDigit(src.at(j))) j++;
            add(TokenType::NUMBER, src.mid(i, j - i), startLine, startCol, j);
            col += j - i;
            i = j;
            continue;
        }
        if (ch == u'.') { add(TokenType::DOT, LangString(u"."), startLine, startCol, i + 1); i++; col++; continue; }
        if (ch == u'(') { add(TokenType::LPAREN, LangString(u"("), startLine, startCol, i + 1); i++; col++; continue; }
        if (ch == u')') { add(TokenType::RPAREN, LangString(u")"), startLine, startCol, i + 1); i++; col++; continue; }
        if (ch == u'{') { add(TokenType::LBRACE, LangString(u"{"), startLine, startCol, i + 1); i++; col++; continue; }
        if (ch == u'}') { add(TokenType::RBRACE, LangString(u"}"), startLine, startCol, i + 1); i++; col++; continue; }
        if (ch == u'[') { add(TokenType::LBRACKET, LangString(u"["), startLine, startCol, i + 1); i++; col++; continue; }
        if (ch == u']') { add(TokenType::RBRACKET, LangString(u"]"), startLine, startCol, i + 1); i++; col++; continue; }
        if (ch == u',') { add(TokenType::COMMA, LangString(u","), startLine, startCol, i + 1); i++; col++; continue; }
        if (ch == u':') { add(TokenType::COLON, LangString(u":"), startLine, startCol, i + 1); i++; col++; continue; }
        if (ch == u'=') { add(TokenType::EQUAL, LangString(u"="), startLine, startCol, i + 1); i++; col++; continue; }
        if (ch == u';') { add(TokenType::SEMICOLON, LangString(u";"), startLine, startCol, i + 1); i++; col++; continue; }
        if (ch == u'+') { add(TokenType::PLUS, LangString(u"+"), startLine, startCol, i + 1); i++; col++; continue; }
        if (ch == u'-') { add(TokenType::MINUS, LangString(u"-"), startLine, startCol, i + 1); i++; col++; continue; }
        if (ch == u'*') { add(TokenType::STAR, LangString(u"*"), startLine, startCol, i + 1); i++; col++; continue; }
        if (ch == u'/') { add(TokenType::SLASH, LangString(u"/"), startLine, startCol, i + 1); i++; col++; continue; }

        // Triple-quoted strings (multi-line) -- must check before single quotes
        if (ch == u'"' && at(src, i + 1) == u'"' && at(src, i + 2) == u'"') {
            int j = i + 3;
            // Find closing """
            while (j < n - 2) {
                if (src.at(j) == u'"' && src.at(j + 1) == u'"' && src.at(j + 2) == u'"') {
                    break;
                }
                if (src.at(j) == u'\n') {
                    line++;
                    col = 0; // will be set correctly after the loop
                }
                j++;
            }
            if (j >= n - 2
                || !(at(src, j) == u'"' && at(src, j + 1) == u'"' && at(src, j + 2) == u'"')) {
                fail(LangString(u"L002"),
                     LangString(u"Unterminated triple-quoted string at line %1 col %2").arg(startLine).arg(startCol),
                     i, n);
            }
            // Extract string content without the triple quotes
            const LangString content = src.mid(i + 3, j - (i + 3));
            add(TokenType::STRING, content, startLine, startCol, j + 3);
            // Update position past closing """
            const auto lines = content.split(u'\n', false);
            if (lines.size() > 1) {
                col = lines.back().length() + 4; // +3 for closing """ +1 for next char
            } else {
                col += j - i + 3;
            }
            i = j + 3;
            continue;
        }

        if (ch == u'"' || ch == u'\'') {
            const char16_t quote = ch;
            int j = i + 1;
            while (j < n && src.at(j) != quote && src.at(j) != u'\n') {
                // Handle escape sequences -- NOT decoded, just skipped over
                // so an escaped delimiter doesn't end the string early.
                if (src.at(j) == u'\\' && j + 1 < n) {
                    j += 2;
                } else {
                    j++;
                }
            }
            if (j >= n || src.at(j) == u'\n') {
                fail(LangString(u"L002"),
                     LangString(u"Unterminated string literal at line %1 col %2").arg(line).arg(col),
                     i, j);
            }
            // Extract string content without quotes
            const LangString content = src.mid(i + 1, j - (i + 1));
            add(TokenType::STRING, content, startLine, startCol, j + 1);
            col += j - i + 1;
            i = j + 1;
            continue;
        }

        if (isDigit(ch)) {
            int j = i;
            while (j < n && isDigit(src.at(j))) j++;
            if (at(src, j) == u'.' && isDigit(at(src, j + 1))) {
                j++;
                while (j < n && isDigit(src.at(j))) j++;
            }
            add(TokenType::NUMBER, src.mid(i, j - i), startLine, startCol, j);
            col += j - i;
            i = j;
            continue;
        }

        if (isLetter(ch) || ch == u'_') {
            int j = i;
            while (j < n && (isLetter(src.at(j)) || isDigit(src.at(j)) || src.at(j) == u'_')) j++;
            const LangString lexeme = src.mid(i, j - i);
            const auto it = keywords().find(lexeme);
            if (it != keywords().end()) {
                add(it->second, lexeme, startLine, startCol, j);
            } else if (lexeme == LangString(u"__proto__")) {
                add(LangString(u"[object Object]"), lexeme, startLine, startCol, j);
                tokens.back().nonStringType = Token::NonStringType::Object;
            } else {
                static const std::unordered_set<JsString> inheritedFunctions = {
                    u"constructor", u"toString", u"valueOf", u"hasOwnProperty", u"isPrototypeOf",
                    u"propertyIsEnumerable", u"toLocaleString", u"__defineGetter__", u"__defineSetter__",
                    u"__lookupGetter__", u"__lookupSetter__"};
                if (inheritedFunctions.contains(lexeme)) {
                    const JsString name = lexeme == LangString(u"constructor") ? JsString(u"Object") : JsString(lexeme);
                    add(LangString(u"function ") + name + u"() { [native code] }", lexeme, startLine, startCol, j);
                    tokens.back().nonStringType = Token::NonStringType::Function;
                } else {
                    add(TokenType::IDENT, lexeme, startLine, startCol, j);
                }
            }
            col += j - i;
            i = j;
            continue;
        }

        fail(LangString(u"L001"),
             LangString(u"Unexpected character '%1' at line %2 col %3").arg(ch).arg(line).arg(col),
             i, i + 1);
    }

    add(TokenType::EOF_, LangString(), line, col, n);

    return std::vector<Token>(tokens.begin(), tokens.end());
}

} // namespace nm
