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
    auto isTrimmed = [](QChar c) { return js::isWhiteSpace(c) || js::isLineTerminator(c); };
    qsizetype start = 0;
    qsizetype end = s.size();
    while (start < end && isTrimmed(s.at(start))) ++start;
    while (end > start && isTrimmed(s.at(end - 1))) --end;
    return s.mid(start, end - start);
}

bool isDigit(QChar c) {
    return c >= QLatin1Char('0') && c <= QLatin1Char('9');
}

bool isLetter(QChar c) {
    return (c >= QLatin1Char('a') && c <= QLatin1Char('z')) || (c >= QLatin1Char('A') && c <= QLatin1Char('Z'));
}

bool isHexDigit(QChar c) {
    return isDigit(c) || (c >= QLatin1Char('a') && c <= QLatin1Char('f')) || (c >= QLatin1Char('A') && c <= QLatin1Char('F'));
}

// Bounds-safe char fetch (JS `src[k]` past the end of the string is
// `undefined`, which compares unequal to any real character; here a NUL
// sentinel that never matches any lexer test serves the same purpose).
QChar at(const LangString& src, int k) {
    return (k >= 0 && k < src.length()) ? src.at(k) : QChar(u'\0');
}

const std::unordered_map<LangString, LangString>& keywords() {
    static const std::unordered_map<LangString, LangString> table = {
        {QStringLiteral("let"), TokenType::LET},
        {QStringLiteral("render"), TokenType::RENDER},
        {QStringLiteral("write"), TokenType::WRITE},
        {QStringLiteral("write3d"), TokenType::WRITE3D},
        {QStringLiteral("true"), TokenType::TRUE},
        {QStringLiteral("false"), TokenType::FALSE},
        {QStringLiteral("if"), TokenType::IF},
        {QStringLiteral("elif"), TokenType::ELIF},
        {QStringLiteral("else"), TokenType::ELSE},
        {QStringLiteral("break"), TokenType::BREAK},
        {QStringLiteral("continue"), TokenType::CONTINUE},
        {QStringLiteral("return"), TokenType::RETURN},
        {QStringLiteral("search"), TokenType::SEARCH},
        {QStringLiteral("subchain"), TokenType::SUBCHAIN},
    };
    return table;
}

} // namespace

std::vector<Token> lex(const JsString& source) {
    const LangString src(source);
    QVector<Token> tokens;
    const int n = src.length();
    int i = 0;
    int line = 1;
    int col = 1;
    int srcLine = 1;
    int srcCol = 1;
    int anchor = 0;

    auto add = [&](const LangString& type, const LangString& lexeme, int tokLine, int tokCol, int end) {
        for (int offset = anchor; offset < i; ++offset) {
            if (src.at(offset) == QLatin1Char('\n')) {
                srcLine++;
                srcCol = 1;
            } else {
                srcCol++;
            }
        }
        const int startLine = srcLine;
        const int startColumn = srcCol;
        for (int offset = i; offset < end; ++offset) {
            if (src.at(offset) == QLatin1Char('\n')) {
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
            if (src.at(offset) == QLatin1Char('\n')) {
                errorLine++;
                column = 1;
            } else {
                column++;
            }
        }
        JsonObject location;
        location.insert(QStringLiteral("line"), errorLine);
        location.insert(QStringLiteral("column"), column);
        JsonObject span;
        span.insert(QStringLiteral("start"), start);
        span.insert(QStringLiteral("end"), end);
        JsonObject diagnostic;
        diagnostic.insert(QStringLiteral("code"), code);
        diagnostic.insert(QStringLiteral("stage"), diagStage(code));
        diagnostic.insert(QStringLiteral("severity"), diagSeverity(code));
        diagnostic.insert(QStringLiteral("message"), message);
        diagnostic.insert(QStringLiteral("location"), location);
        diagnostic.insert(QStringLiteral("span"), span);
        throw DslSyntaxError(message, errorLine, column, diagnostic.native());
    };

    while (i < n) {
        QChar ch = src.at(i);

        if (ch == QLatin1Char(' ') || ch == QLatin1Char('\t') || ch == QLatin1Char('\r')) {
            i++;
            col++;
            continue;
        }
        if (ch == QLatin1Char('\n')) {
            i++;
            line++;
            col = 1;
            continue;
        }

        const int startLine = line;
        const int startCol = col;

        // line comment //...
        if (ch == QLatin1Char('/') && at(src, i + 1) == QLatin1Char('/')) {
            int j = i + 2;
            while (j < n && src.at(j) != QLatin1Char('\n')) j++;
            add(TokenType::COMMENT, src.mid(i, j - i), startLine, startCol, j);
            col += j - i;
            i = j;
            continue;
        }

        // block comment /* ... */
        if (ch == QLatin1Char('/') && at(src, i + 1) == QLatin1Char('*')) {
            int j = i + 2;
            int endLine = line;
            int endCol = col + 2;
            while (j < n && !(src.at(j) == QLatin1Char('*') && at(src, j + 1) == QLatin1Char('/'))) {
                if (src.at(j) == QLatin1Char('\n')) {
                    endLine++;
                    endCol = 1;
                } else {
                    endCol++;
                }
                j++;
            }
            if (j >= n) {
                fail(QStringLiteral("L003"),
                     QStringLiteral("Unterminated comment at line %1 col %2").arg(startLine).arg(startCol),
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
        if ((ch == QLatin1Char('o') || ch == QLatin1Char('s')) && isDigit(at(src, i + 1))) {
            int j = i + 1;
            while (j < n && isDigit(src.at(j))) j++;
            const LangString lexeme = src.mid(i, j - i);
            const LangString tokenType = (ch == QLatin1Char('o')) ? TokenType::OUTPUT_REF : TokenType::SOURCE_REF;
            const bool isMemberSegment = !tokens.isEmpty() && tokens.last().type == TokenType::DOT;
            if (tokenType == TokenType::OUTPUT_REF && !isMemberSegment
                && !(lexeme.length() == 2 && lexeme.at(1) >= QLatin1Char('0') && lexeme.at(1) <= QLatin1Char('7'))) {
                fail(QStringLiteral("L004"),
                     QStringLiteral("Output surface reference '%1' is out of range; expected o0-o7 at line %2 col %3")
                         .arg(lexeme).arg(startLine).arg(startCol),
                     i, j);
            }
            add(tokenType, lexeme, startLine, startCol, j);
            col += j - i;
            i = j;
            continue;
        }

        // volume reference (vol0-vol7, or more digits) -- tested BEFORE vel
        if (ch == QLatin1Char('v') && at(src, i + 1) == QLatin1Char('o') && at(src, i + 2) == QLatin1Char('l')
            && isDigit(at(src, i + 3))) {
            int j = i + 3;
            while (j < n && isDigit(src.at(j))) j++;
            add(TokenType::VOL_REF, src.mid(i, j - i), startLine, startCol, j);
            col += j - i;
            i = j;
            continue;
        }

        // geometry reference (geo0-geo7)
        if (ch == QLatin1Char('g') && at(src, i + 1) == QLatin1Char('e') && at(src, i + 2) == QLatin1Char('o')
            && isDigit(at(src, i + 3))) {
            int j = i + 3;
            while (j < n && isDigit(src.at(j))) j++;
            add(TokenType::GEO_REF, src.mid(i, j - i), startLine, startCol, j);
            col += j - i;
            i = j;
            continue;
        }

        // xyz reference (xyz0-xyz7) -- agent position surfaces
        if (ch == QLatin1Char('x') && at(src, i + 1) == QLatin1Char('y') && at(src, i + 2) == QLatin1Char('z')
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
        if (ch == QLatin1Char('v') && at(src, i + 1) == QLatin1Char('e') && at(src, i + 2) == QLatin1Char('l')
            && isDigit(at(src, i + 3))) {
            int j = i + 3;
            while (j < n && isDigit(src.at(j))) j++;
            add(TokenType::VEL_REF, src.mid(i, j - i), startLine, startCol, j);
            col += j - i;
            i = j;
            continue;
        }

        // rgba reference (rgba0-rgba7) -- agent color surfaces
        if (ch == QLatin1Char('r') && at(src, i + 1) == QLatin1Char('g') && at(src, i + 2) == QLatin1Char('b')
            && at(src, i + 3) == QLatin1Char('a') && isDigit(at(src, i + 4))) {
            int j = i + 4;
            while (j < n && isDigit(src.at(j))) j++;
            add(TokenType::RGBA_REF, src.mid(i, j - i), startLine, startCol, j);
            col += j - i;
            i = j;
            continue;
        }

        // mesh reference (mesh0-mesh7) -- mesh geometry surfaces
        if (ch == QLatin1Char('m') && at(src, i + 1) == QLatin1Char('e') && at(src, i + 2) == QLatin1Char('s')
            && at(src, i + 3) == QLatin1Char('h') && isDigit(at(src, i + 4))) {
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
        if (ch == QLatin1Char('#')) {
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
        if (ch == QLatin1Char('(') && at(src, i + 1) == QLatin1Char(')')) {
            int j = i + 2;
            while (j < n && (src.at(j) == QLatin1Char(' ') || src.at(j) == QLatin1Char('\t'))) j++;
            if (at(src, j) == QLatin1Char('=') && at(src, j + 1) == QLatin1Char('>')) {
                j += 2;
                while (j < n && (src.at(j) == QLatin1Char(' ') || src.at(j) == QLatin1Char('\t'))) j++;
                int depth = 0;
                const int exprStart = j;
                while (j < n) {
                    const QChar c = src.at(j);
                    if (c == QLatin1Char('(')) {
                        depth++;
                    } else if (c == QLatin1Char(')')) {
                        if (depth == 0) break;
                        depth--;
                    } else if (depth == 0) {
                        if (c == QLatin1Char(',') || c == QLatin1Char(';') || c == QLatin1Char('\n')
                            || c == QLatin1Char('}')) {
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

        if (ch == QLatin1Char('.') && isDigit(at(src, i + 1))) {
            int j = i + 1;
            while (j < n && isDigit(src.at(j))) j++;
            add(TokenType::NUMBER, src.mid(i, j - i), startLine, startCol, j);
            col += j - i;
            i = j;
            continue;
        }
        if (ch == QLatin1Char('.')) { add(TokenType::DOT, QStringLiteral("."), startLine, startCol, i + 1); i++; col++; continue; }
        if (ch == QLatin1Char('(')) { add(TokenType::LPAREN, QStringLiteral("("), startLine, startCol, i + 1); i++; col++; continue; }
        if (ch == QLatin1Char(')')) { add(TokenType::RPAREN, QStringLiteral(")"), startLine, startCol, i + 1); i++; col++; continue; }
        if (ch == QLatin1Char('{')) { add(TokenType::LBRACE, QStringLiteral("{"), startLine, startCol, i + 1); i++; col++; continue; }
        if (ch == QLatin1Char('}')) { add(TokenType::RBRACE, QStringLiteral("}"), startLine, startCol, i + 1); i++; col++; continue; }
        if (ch == QLatin1Char('[')) { add(TokenType::LBRACKET, QStringLiteral("["), startLine, startCol, i + 1); i++; col++; continue; }
        if (ch == QLatin1Char(']')) { add(TokenType::RBRACKET, QStringLiteral("]"), startLine, startCol, i + 1); i++; col++; continue; }
        if (ch == QLatin1Char(',')) { add(TokenType::COMMA, QStringLiteral(","), startLine, startCol, i + 1); i++; col++; continue; }
        if (ch == QLatin1Char(':')) { add(TokenType::COLON, QStringLiteral(":"), startLine, startCol, i + 1); i++; col++; continue; }
        if (ch == QLatin1Char('=')) { add(TokenType::EQUAL, QStringLiteral("="), startLine, startCol, i + 1); i++; col++; continue; }
        if (ch == QLatin1Char(';')) { add(TokenType::SEMICOLON, QStringLiteral(";"), startLine, startCol, i + 1); i++; col++; continue; }
        if (ch == QLatin1Char('+')) { add(TokenType::PLUS, QStringLiteral("+"), startLine, startCol, i + 1); i++; col++; continue; }
        if (ch == QLatin1Char('-')) { add(TokenType::MINUS, QStringLiteral("-"), startLine, startCol, i + 1); i++; col++; continue; }
        if (ch == QLatin1Char('*')) { add(TokenType::STAR, QStringLiteral("*"), startLine, startCol, i + 1); i++; col++; continue; }
        if (ch == QLatin1Char('/')) { add(TokenType::SLASH, QStringLiteral("/"), startLine, startCol, i + 1); i++; col++; continue; }

        // Triple-quoted strings (multi-line) -- must check before single quotes
        if (ch == QLatin1Char('"') && at(src, i + 1) == QLatin1Char('"') && at(src, i + 2) == QLatin1Char('"')) {
            int j = i + 3;
            // Find closing """
            while (j < n - 2) {
                if (src.at(j) == QLatin1Char('"') && src.at(j + 1) == QLatin1Char('"') && src.at(j + 2) == QLatin1Char('"')) {
                    break;
                }
                if (src.at(j) == QLatin1Char('\n')) {
                    line++;
                    col = 0; // will be set correctly after the loop
                }
                j++;
            }
            if (j >= n - 2
                || !(at(src, j) == QLatin1Char('"') && at(src, j + 1) == QLatin1Char('"') && at(src, j + 2) == QLatin1Char('"'))) {
                fail(QStringLiteral("L002"),
                     QStringLiteral("Unterminated triple-quoted string at line %1 col %2").arg(startLine).arg(startCol),
                     i, n);
            }
            // Extract string content without the triple quotes
            const LangString content = src.mid(i + 3, j - (i + 3));
            add(TokenType::STRING, content, startLine, startCol, j + 3);
            // Update position past closing """
            const auto lines = content.split(QLatin1Char('\n'), false);
            if (lines.size() > 1) {
                col = lines.back().length() + 4; // +3 for closing """ +1 for next char
            } else {
                col += j - i + 3;
            }
            i = j + 3;
            continue;
        }

        if (ch == QLatin1Char('"') || ch == QLatin1Char('\'')) {
            const QChar quote = ch;
            int j = i + 1;
            while (j < n && src.at(j) != quote && src.at(j) != QLatin1Char('\n')) {
                // Handle escape sequences -- NOT decoded, just skipped over
                // so an escaped delimiter doesn't end the string early.
                if (src.at(j) == QLatin1Char('\\') && j + 1 < n) {
                    j += 2;
                } else {
                    j++;
                }
            }
            if (j >= n || src.at(j) == QLatin1Char('\n')) {
                fail(QStringLiteral("L002"),
                     QStringLiteral("Unterminated string literal at line %1 col %2").arg(line).arg(col),
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
            if (at(src, j) == QLatin1Char('.') && isDigit(at(src, j + 1))) {
                j++;
                while (j < n && isDigit(src.at(j))) j++;
            }
            add(TokenType::NUMBER, src.mid(i, j - i), startLine, startCol, j);
            col += j - i;
            i = j;
            continue;
        }

        if (isLetter(ch) || ch == QLatin1Char('_')) {
            int j = i;
            while (j < n && (isLetter(src.at(j)) || isDigit(src.at(j)) || src.at(j) == QLatin1Char('_'))) j++;
            const LangString lexeme = src.mid(i, j - i);
            const auto it = keywords().find(lexeme);
            if (it != keywords().end()) {
                add(it->second, lexeme, startLine, startCol, j);
            } else if (lexeme == QStringLiteral("__proto__")) {
                add(QStringLiteral("[object Object]"), lexeme, startLine, startCol, j);
                tokens.back().nonStringType = Token::NonStringType::Object;
            } else {
                static const std::unordered_set<JsString> inheritedFunctions = {
                    u"constructor", u"toString", u"valueOf", u"hasOwnProperty", u"isPrototypeOf",
                    u"propertyIsEnumerable", u"toLocaleString", u"__defineGetter__", u"__defineSetter__",
                    u"__lookupGetter__", u"__lookupSetter__"};
                if (inheritedFunctions.contains(lexeme)) {
                    const JsString name = lexeme == QStringLiteral("constructor") ? JsString(u"Object") : JsString(lexeme);
                    add(QStringLiteral("function ") + name + u"() { [native code] }", lexeme, startLine, startCol, j);
                    tokens.back().nonStringType = Token::NonStringType::Function;
                } else {
                    add(TokenType::IDENT, lexeme, startLine, startCol, j);
                }
            }
            col += j - i;
            i = j;
            continue;
        }

        fail(QStringLiteral("L001"),
             QStringLiteral("Unexpected character '%1' at line %2 col %3").arg(ch).arg(line).arg(col),
             i, i + 1);
    }

    add(TokenType::EOF_, LangString(), line, col, n);

    return std::vector<Token>(tokens.begin(), tokens.end());
}

} // namespace nm
