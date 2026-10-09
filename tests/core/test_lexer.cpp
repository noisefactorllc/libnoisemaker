// Unit tests for nm::lex (core/lang/lexer.{h,cpp}).
// Plain assert-style checks, no test framework dependency (matches
// test_graph_load.cpp / test_shader_assembly.cpp convention).
//
// Covers the T8 brief's named lexer hazards -- rule-ORDER-dependent
// disambiguation, ported from shaders/src/lang/lexer.js. Every fixture
// below was cross-checked against the reference oracle
// (NM_REFERENCE_ROOT tools/dump-tokens.mjs) before being hard-coded here,
// not derived from reading the JS alone -- see task report "hazards
// encountered" for the one case (token column immediately after a block
// comment) where a hand-traced prediction was wrong and the oracle catch
// caught it:
//   - output/source refs o0/s1 vs plain identifiers ('o'/'s' must be
//     immediately followed by a digit)
//   - vol BEFORE vel (3rd-char disambiguation) + geo/xyz (3-char prefix)
//     and rgba/mesh (4-char prefix) refs
//   - hex color literals gated to length 4/7/9 (3/6/8 hex digits) --
//     anything else falls through every rule to the final "unexpected
//     character" throw
//   - arrow-function FUNC token (() => expr), including nested-paren
//     depth tracking and the un-consumed depth-0 terminator
//   - triple-quoted (multi-line) strings vs single/double-quoted strings
//     (backslash escapes NOT decoded -- raw lexeme)
//   - 1-based line/col, including the triple-quote and block-comment
//     multi-line column-fixup paths
//
// RED (before lexer.cpp has a real definition): this binary fails to LINK
// ("undefined symbols: lexJson(...)"). GREEN: all checks below print PASS
// and the process exits 0.

#include "core/lang/diagnostics.h"
#include "core/lang/lexer.h"
#include "core/lang/tokens.h"


#include <cstdio>

using namespace nm;
namespace {
JsonArray lexJson(const JsString& src) {
    JsonArray out;
    for (const auto& token : lex(src)) out.append(toJson(token));
    return out;
}


int g_failures = 0;

void check(bool condition, const char* description) {
    if (condition) {
        std::printf("PASS: %s\n", description);
    } else {
        std::printf("FAIL: %s\n", description);
        ++g_failures;
    }
}

struct Tok {
    LangString type;
    LangString lexeme;
    int line;
    int col;
};

Tok at(const JsonArray& toks, int i) {
    const JsonObject o = toks.at(i).toObject();
    return Tok{o.value(LangString(u"type")).toString(), o.value(LangString(u"lexeme")).toString(),
               o.value(LangString(u"line")).toInt(), o.value(LangString(u"col")).toInt()};
}

bool isTok(const Tok& t, const LangString& type, const LangString& lexeme, int line, int col) {
    return t.type == type && t.lexeme == lexeme && t.line == line && t.col == col;
}

} // namespace

int main() {
    // --- output/source refs vs identifiers -------------------------------
    {
        const JsonArray toks = lexJson(LangString(u"o0 o7 s3 output0"));
        check(toks.size() == 5, "o0 o7 s3 output0 -> 4 tokens + EOF");
        check(isTok(at(toks, 0), nm::TokenType::OUTPUT_REF, LangString(u"o0"), 1, 1), "o0 -> OUTPUT_REF");
        check(isTok(at(toks, 1), nm::TokenType::OUTPUT_REF, LangString(u"o7"), 1, 4),
              "o7 -> OUTPUT_REF (upper boundary)");
        check(isTok(at(toks, 2), nm::TokenType::SOURCE_REF, LangString(u"s3"), 1, 7), "s3 -> SOURCE_REF");
        check(isTok(at(toks, 3), nm::TokenType::IDENT, LangString(u"output0"), 1, 10),
              "output0 -> IDENT ('o' ref rule needs 'o' immediately followed by a digit)");
    }

    // --- output surface range enforcement (o0-o7, member segments, other families)
    {
        for (const LangString& bad : {LangString(u"o8"), LangString(u"o12"), LangString(u"o99")}) {
            bool threw = false;
            try {
                lexJson(bad);
            } catch (const nm::DslSyntaxError& err) {
                threw = true;
                const LangString expectedMsg = LangString(u"Output surface reference '%1' is out of range; expected o0-o7 at line 1 col 1").arg(bad);
                check(LangString::fromUtf8(err.what()) == expectedMsg, "error message matches reference format");
            }
            check(threw, LangString(u"%1 throws DslSyntaxError").arg(bad).toStdString().c_str());
        }

        // member segments foo.o8 and foo.o99 are allowed
        const JsonArray memberToks = lexJson(LangString(u"foo.o0 foo.o7 foo.o8 foo.o99"));
        check(memberToks.size() == 13, "foo.o0 foo.o7 foo.o8 foo.o99 -> 12 tokens + EOF");
        check(isTok(at(memberToks, 2), nm::TokenType::OUTPUT_REF, LangString(u"o0"), 1, 5), "foo.o0 member segment");
        check(isTok(at(memberToks, 5), nm::TokenType::OUTPUT_REF, LangString(u"o7"), 1, 12), "foo.o7 member segment");
        check(isTok(at(memberToks, 8), nm::TokenType::OUTPUT_REF, LangString(u"o8"), 1, 19), "foo.o8 member segment");
        check(isTok(at(memberToks, 11), nm::TokenType::OUTPUT_REF, LangString(u"o99"), 1, 26), "foo.o99 member segment");

        // other surface reference families preserve multi-digit numbers
        const JsonArray otherToks = lexJson(LangString(u"s99 vol99 geo99 xyz99 vel99 rgba99 mesh99"));
        check(isTok(at(otherToks, 0), nm::TokenType::SOURCE_REF, LangString(u"s99"), 1, 1), "s99 -> SOURCE_REF");
        check(isTok(at(otherToks, 1), nm::TokenType::VOL_REF, LangString(u"vol99"), 1, 5), "vol99 -> VOL_REF");
        check(isTok(at(otherToks, 2), nm::TokenType::GEO_REF, LangString(u"geo99"), 1, 11), "geo99 -> GEO_REF");
        check(isTok(at(otherToks, 3), nm::TokenType::XYZ_REF, LangString(u"xyz99"), 1, 17), "xyz99 -> XYZ_REF");
        check(isTok(at(otherToks, 4), nm::TokenType::VEL_REF, LangString(u"vel99"), 1, 23), "vel99 -> VEL_REF");
        check(isTok(at(otherToks, 5), nm::TokenType::RGBA_REF, LangString(u"rgba99"), 1, 29), "rgba99 -> RGBA_REF");
        check(isTok(at(otherToks, 6), nm::TokenType::MESH_REF, LangString(u"mesh99"), 1, 36), "mesh99 -> MESH_REF");
    }

    // --- vol BEFORE vel (3rd-char disambiguation) + geo/xyz/rgba/mesh ----
    {
        const JsonArray toks = lexJson(LangString(u"vol0 vel1 geo2 xyz3 rgba4 mesh5 vola velvel"));
        check(isTok(at(toks, 0), nm::TokenType::VOL_REF, LangString(u"vol0"), 1, 1), "vol0 -> VOL_REF");
        check(isTok(at(toks, 1), nm::TokenType::VEL_REF, LangString(u"vel1"), 1, 6),
              "vel1 -> VEL_REF (disambiguated from vol by 3rd char)");
        check(isTok(at(toks, 2), nm::TokenType::GEO_REF, LangString(u"geo2"), 1, 11), "geo2 -> GEO_REF");
        check(isTok(at(toks, 3), nm::TokenType::XYZ_REF, LangString(u"xyz3"), 1, 16), "xyz3 -> XYZ_REF");
        check(isTok(at(toks, 4), nm::TokenType::RGBA_REF, LangString(u"rgba4"), 1, 21),
              "rgba4 -> RGBA_REF (4-char prefix)");
        check(isTok(at(toks, 5), nm::TokenType::MESH_REF, LangString(u"mesh5"), 1, 27),
              "mesh5 -> MESH_REF (4-char prefix)");
        check(isTok(at(toks, 6), nm::TokenType::IDENT, LangString(u"vola"), 1, 33),
              "vola -> IDENT (vol ref needs a digit right after 'vol')");
        check(isTok(at(toks, 7), nm::TokenType::IDENT, LangString(u"velvel"), 1, 38),
              "velvel -> IDENT (vel ref needs a digit right after 'vel')");
    }

    // --- hex color literals: only length 4/7/9 (3/6/8 hex digits) --------
    {
        const JsonArray toks = lexJson(LangString(u"#fff #ffffff #ffffffff"));
        check(toks.size() == 4, "3 hex literals -> 3 tokens + EOF");
        check(isTok(at(toks, 0), nm::TokenType::HEX, LangString(u"#fff"), 1, 1), "#fff (3 digits) -> HEX");
        check(isTok(at(toks, 1), nm::TokenType::HEX, LangString(u"#ffffff"), 1, 6), "#ffffff (6 digits) -> HEX");
        check(isTok(at(toks, 2), nm::TokenType::HEX, LangString(u"#ffffffff"), 1, 14),
              "#ffffffff (8 digits) -> HEX");
    }
    {
        // Invalid hex lengths (not 4/7/9) fall through every rule and hit
        // the lexer's final "unexpected character" throw (verified against
        // the reference oracle: '#ff' and '#fffff' both throw there).
        bool threw = false;
        try {
            lexJson(LangString(u"#ff"));
        } catch (const nm::DslSyntaxError&) {
            threw = true;
        }
        check(threw, "#ff (2 digits, invalid length) throws DslSyntaxError");
    }
    {
        bool threw = false;
        try {
            lexJson(LangString(u"#fffff"));
        } catch (const nm::DslSyntaxError&) {
            threw = true;
        }
        check(threw, "#fffff (5 digits, invalid length) throws DslSyntaxError");
    }

    // --- arrow-function FUNC token ----------------------------------------
    {
        const JsonArray toks = lexJson(LangString(u"() => sin(time)"));
        check(toks.size() == 2, "() => sin(time) -> 1 FUNC token + EOF");
        check(isTok(at(toks, 0), nm::TokenType::FUNC, LangString(u"sin(time)"), 1, 1),
              "() => sin(time) -> FUNC with trimmed body");
    }
    {
        // Nested parens inside the arrow body are depth-tracked; the scan
        // stops at the first depth-0 terminator (',' here) WITHOUT
        // consuming it, so it lexes as its own COMMA token afterward.
        const JsonArray toks = lexJson(LangString(u"osc(() => a + (b), 0, 1)"));
        check(isTok(at(toks, 0), nm::TokenType::IDENT, LangString(u"osc"), 1, 1), "osc( -> IDENT");
        check(isTok(at(toks, 1), nm::TokenType::LPAREN, LangString(u"("), 1, 4), "osc( -> LPAREN");
        check(isTok(at(toks, 2), nm::TokenType::FUNC, LangString(u"a + (b)"), 1, 5),
              "nested-paren arrow body stops at depth-0 comma");
        check(isTok(at(toks, 3), nm::TokenType::COMMA, LangString(u","), 1, 18),
              "terminator comma re-lexed as its own token");
    }
    {
        // '(' not followed by ')' is not an arrow-function candidate at
        // all -- ordinary LPAREN/RPAREN punctuation.
        const JsonArray toks = lexJson(LangString(u"()"));
        check(isTok(at(toks, 0), nm::TokenType::LPAREN, LangString(u"("), 1, 1), "'()' without '=>' -> LPAREN");
        check(isTok(at(toks, 1), nm::TokenType::RPAREN, LangString(u")"), 1, 2), "'()' without '=>' -> RPAREN");
    }

    // --- triple-quoted (multi-line) strings vs single/double quotes ------
    {
        const JsonArray toks = lexJson(LangString(u"\"\"\"triple\nline2\"\"\""));
        check(toks.size() == 2, "triple-quoted string -> 1 STRING token + EOF");
        check(at(toks, 0).type == nm::TokenType::STRING, "triple-quote -> STRING");
        check(at(toks, 0).lexeme == LangString(u"triple\nline2"),
              "triple-quote content keeps the embedded newline, drops the delimiters");
        check(at(toks, 0).line == 1 && at(toks, 0).col == 1, "triple-quote token starts at 1-based line 1 col 1");
        check(isTok(at(toks, 1), nm::TokenType::EOF_, LangString(u""), 2, 9),
              "EOF after multi-line triple-quote lands via the col=len(lastLine)+4 fixup");
    }
    {
        // Real trailing '\n' after the closing """ advances line/col via
        // the ordinary newline rule on top of the triple-quote's own
        // internal line tracking.
        const JsonArray toks = lexJson(LangString(u"\"\"\"triple\nline2\"\"\"\n// c"));
        check(toks.size() == 3, "triple-quote then comment -> 2 tokens + EOF");
        check(isTok(at(toks, 1), nm::TokenType::COMMENT, LangString(u"// c"), 3, 1),
              "comment after multi-line triple-quote lands on line 3 col 1");
    }
    {
        const JsonArray toks = lexJson(LangString(u"\"double\" 'single' \"esca\\\"ped\""));
        check(isTok(at(toks, 0), nm::TokenType::STRING, LangString(u"double"), 1, 1), "double-quoted string");
        check(isTok(at(toks, 1), nm::TokenType::STRING, LangString(u"single"), 1, 10), "single-quoted string");
        check(at(toks, 2).type == nm::TokenType::STRING, "escaped-quote string -> STRING");
        check(at(toks, 2).lexeme == LangString(u"esca\\\"ped"),
              "backslash-escape sequence is NOT decoded -- raw lexeme keeps the backslash");
    }
    {
        bool threw = false;
        try {
            lexJson(LangString(u"\"unterminated"));
        } catch (const nm::DslSyntaxError&) {
            threw = true;
        }
        check(threw, "unterminated single-quoted string throws DslSyntaxError");
    }
    {
        bool threw = false;
        try {
            lexJson(LangString(u"\"\"\"unterminated"));
        } catch (const nm::DslSyntaxError&) {
            threw = true;
        }
        check(threw, "unterminated triple-quoted string throws DslSyntaxError");
    }

    // --- 1-based line/col across newlines and comments --------------------
    {
        const JsonArray toks = lexJson(LangString(u"o0\no1\n  o2"));
        check(isTok(at(toks, 0), nm::TokenType::OUTPUT_REF, LangString(u"o0"), 1, 1), "line 1 col 1");
        check(isTok(at(toks, 1), nm::TokenType::OUTPUT_REF, LangString(u"o1"), 2, 1), "line 2 col 1 (after \\n)");
        check(isTok(at(toks, 2), nm::TokenType::OUTPUT_REF, LangString(u"o2"), 3, 3),
              "line 3 col 3 (after 2-space indent)");
        check(isTok(at(toks, 3), nm::TokenType::EOF_, LangString(u""), 3, 5),
              "EOF at 1-based line/col of end of input");
    }
    {
        const JsonArray toks = lexJson(LangString(u"// line\no0"));
        check(isTok(at(toks, 0), nm::TokenType::COMMENT, LangString(u"// line"), 1, 1), "line comment token");
        check(isTok(at(toks, 1), nm::TokenType::OUTPUT_REF, LangString(u"o0"), 2, 1),
              "token after line comment resumes at next line col 1");
    }
    {
        const JsonArray toks = lexJson(LangString(u"/* block\ncomment */o0"));
        check(isTok(at(toks, 0), nm::TokenType::COMMENT, LangString(u"/* block\ncomment */"), 1, 1),
              "block comment spans lines, single token");
        // Empirically verified against the reference oracle: "comment */o0"
        // is 12 chars before 'o' (c-o-m-m-e-n-t-space-*-/), so 'o0' starts
        // at col 11 -- NOT col 12, which a naive hand-trace first predicted
        // (see task report "hazards encountered").
        check(isTok(at(toks, 1), nm::TokenType::OUTPUT_REF, LangString(u"o0"), 2, 11),
              "token immediately after block comment tracks col correctly");
    }
    {
        bool threw = false;
        try {
            lexJson(LangString(u"/* unterminated"));
        } catch (const nm::DslSyntaxError&) {
            threw = true;
        }
        check(threw, "unterminated block comment throws DslSyntaxError");
    }

    // --- numbers: leading-dot, trailing-dot-without-digit -----------------
    {
        const JsonArray toks = lexJson(LangString(u".5 5. 5.5 007"));
        check(isTok(at(toks, 0), nm::TokenType::NUMBER, LangString(u".5"), 1, 1), "leading-dot number");
        check(isTok(at(toks, 1), nm::TokenType::NUMBER, LangString(u"5"), 1, 4),
              "'5.' -> NUMBER '5' (dot not followed by digit is not consumed)");
        check(isTok(at(toks, 2), nm::TokenType::DOT, LangString(u"."), 1, 5), "'5.' -> trailing DOT token");
        check(isTok(at(toks, 3), nm::TokenType::NUMBER, LangString(u"5.5"), 1, 7), "5.5 -> NUMBER");
        check(isTok(at(toks, 4), nm::TokenType::NUMBER, LangString(u"007"), 1, 11),
              "leading zeros preserved verbatim in lexeme");
    }

    // --- every RESERVED_KEYWORDS entry, plus a non-keyword ident ----------
    {
        const JsonArray toks = lexJson(LangString(
            u"search let render if elif else break continue return write write3d true false subchain notakeyword"));
        const LangStringList expected = {
            nm::TokenType::SEARCH, nm::TokenType::LET,     nm::TokenType::RENDER,   nm::TokenType::IF,
            nm::TokenType::ELIF,   nm::TokenType::ELSE,    nm::TokenType::BREAK,    nm::TokenType::CONTINUE,
            nm::TokenType::RETURN, nm::TokenType::WRITE,   nm::TokenType::WRITE3D,  nm::TokenType::TRUE,
            nm::TokenType::FALSE,  nm::TokenType::SUBCHAIN, nm::TokenType::IDENT,
        };
        bool allMatch = toks.size() == expected.size() + 1;
        for (int i = 0; allMatch && i < expected.size(); ++i) {
            allMatch = at(toks, i).type == expected.at(i);
        }
        check(allMatch, "every RESERVED_KEYWORDS entry lexes to its keyword type; non-keyword stays IDENT");
    }

    // --- EOF sentinel -------------------------------------------------------
    {
        const JsonArray toks = lexJson(LangString(u""));
        check(toks.size() == 1, "empty source -> EOF only");
        check(isTok(at(toks, 0), nm::TokenType::EOF_, LangString(u""), 1, 1), "EOF on empty source is line 1 col 1");
    }

    // --- structured lexer diagnostics (upstream 643b2be1) ------------------
    {
        struct Case {
            const char* name;
            LangString source;
            LangString code;
            LangString message;
            int line;
            int column;
            int spanStart;
            int spanEnd;
        };

        const LangVector<Case> cases = {
            {"unexpected character after CRLF, tab, and UTF-16 text",
             LangString::fromUtf8("// \xF0\x9F\x98\x80\r\n\t@"), LangString(u"L001"),
             LangString(u"Unexpected character '@' at line 2 col 2"), 2, 2, 8, 9},
            {"unterminated double-quoted string at EOF",
             LangString(u"\"abc"), LangString(u"L002"),
             LangString(u"Unterminated string literal at line 1 col 1"), 1, 1, 0, 4},
            {"unterminated single-quoted string at LF",
             LangString(u" 'abc\nnext"), LangString(u"L002"),
             LangString(u"Unterminated string literal at line 1 col 2"), 1, 2, 1, 5},
            {"unterminated triple-quoted string across lines",
             LangString(u"\n  \"\"\"a\nb"), LangString(u"L002"),
             LangString(u"Unterminated triple-quoted string at line 2 col 3"), 2, 3, 3, 9},
            {"unterminated block comment across lines",
             LangString(u"\n /* a\nb"), LangString(u"L003"),
             LangString(u"Unterminated comment at line 2 col 2"), 2, 2, 2, 8},
            {"out-of-range output reference",
             LangString(u"search synth\nrender(o99)"), LangString(u"L004"),
             LangString(u"Output surface reference 'o99' is out of range; expected o0-o7 at line 2 col 8"), 2, 8, 20, 23},
            {"UTF-16 columns after a string",
             LangString::fromUtf8("\"\xF0\x9F\x98\x80\" @"), LangString(u"L001"),
             LangString(u"Unexpected character '@' at line 1 col 6"), 1, 6, 5, 6},
            {"source coordinates after a multiline function token",
             LangString(u"() => (1\n + 2), @"), LangString(u"L001"),
             LangString(u"Unexpected character '@' at line 1 col 17"), 2, 8, 16, 17},
            {"source coordinates after an escaped LF in a string",
             LangString(u"\"a\\\nb\" @"), LangString(u"L001"),
             LangString(u"Unexpected character '@' at line 1 col 8"), 2, 4, 7, 8},
        };

        for (const auto& c : cases) {
            bool caught = false;
            try {
                lexJson(c.source);
            } catch (const nm::DslSyntaxError& err) {
                caught = true;
                check(err.message() == c.message,
                      LangString(u"diagnostic message matches for %1").arg(LangString::fromUtf8(c.name)).toStdString().c_str());
                const JsonObject d = JsonValue(err.diagnostic()).toObject();
                check(!d.isEmpty(),
                      LangString(u"diagnostic payload present for %1").arg(LangString::fromUtf8(c.name)).toStdString().c_str());
                check(d.value(LangString(u"code")).toString() == c.code,
                      LangString(u"diagnostic code matches for %1").arg(LangString::fromUtf8(c.name)).toStdString().c_str());
                check(d.value(LangString(u"stage")).toString() == LangString(u"lexer"),
                      LangString(u"diagnostic stage matches for %1").arg(LangString::fromUtf8(c.name)).toStdString().c_str());
                check(d.value(LangString(u"severity")).toString() == LangString(u"error"),
                      LangString(u"diagnostic severity matches for %1").arg(LangString::fromUtf8(c.name)).toStdString().c_str());
                check(d.value(LangString(u"message")).toString() == c.message,
                      LangString(u"diagnostic message field matches for %1").arg(LangString::fromUtf8(c.name)).toStdString().c_str());
                const JsonObject loc = d.value(LangString(u"location")).toObject();
                check(loc.value(LangString(u"line")).toInt() == c.line,
                      LangString(u"diagnostic location line matches for %1").arg(LangString::fromUtf8(c.name)).toStdString().c_str());
                check(loc.value(LangString(u"column")).toInt() == c.column,
                      LangString(u"diagnostic location column matches for %1").arg(LangString::fromUtf8(c.name)).toStdString().c_str());
                const JsonObject span = d.value(LangString(u"span")).toObject();
                check(span.value(LangString(u"start")).toInt() == c.spanStart,
                      LangString(u"diagnostic span start matches for %1").arg(LangString::fromUtf8(c.name)).toStdString().c_str());
                check(span.value(LangString(u"end")).toInt() == c.spanEnd,
                      LangString(u"diagnostic span end matches for %1").arg(LangString::fromUtf8(c.name)).toStdString().c_str());
            }
            check(caught, LangString(u"throws DslSyntaxError for %1").arg(LangString::fromUtf8(c.name)).toStdString().c_str());
        }

        // Verify successful tokens are preserved unchanged
        const JsonArray toks = lexJson(LangString::fromUtf8("/*x*/\nfoo.o99 \"\xF0\x9F\x98\x80\""));
        check(toks.size() == 6, "successful tokens count matches 6 (including EOF)");
        check(isTok(at(toks, 0), nm::TokenType::COMMENT, LangString(u"/*x*/"), 1, 1), "comment token preserved");
        check(isTok(at(toks, 1), nm::TokenType::IDENT, LangString(u"foo"), 2, 1), "ident token preserved");
        check(isTok(at(toks, 2), nm::TokenType::DOT, LangString(u"."), 2, 4), "dot token preserved");
        check(isTok(at(toks, 3), nm::TokenType::OUTPUT_REF, LangString(u"o99"), 2, 5), "o99 member ref token preserved");
        check(isTok(at(toks, 4), nm::TokenType::STRING, LangString::fromUtf8("\xF0\x9F\x98\x80"), 2, 9), "emoji string token preserved");
        check(isTok(at(toks, 5), nm::TokenType::EOF_, LangString(u""), 2, 13), "eof token col preserved");

        // Verify diagnostic table lookups
        check(nm::diagStage(LangString(u"L001")) == LangString(u"lexer"), "diagStage L001");
        check(nm::diagStage(LangString(u"P001")) == LangString(u"parser"), "diagStage P001");
        check(nm::diagStage(LangString(u"S001")) == LangString(u"semantic"), "diagStage S001");
        check(nm::diagStage(LangString(u"R001")) == LangString(u"runtime"), "diagStage R001");
        check(nm::diagSeverity(LangString(u"L001")) == LangString(u"error"), "diagSeverity L001 error");
        check(nm::diagSeverity(LangString(u"S002")) == LangString(u"warning"), "diagSeverity S002 warning");
        check(nm::diagSeverity(LangString(u"S007")) == LangString(u"warning"), "diagSeverity S007 warning");
        check(nm::diagSeverity(LangString(u"S008")) == LangString(u"warning"), "diagSeverity S008 warning");
        check(nm::diagDefaultMessage(LangString(u"L003")) == LangString(u"Unterminated comment"), "diagDefaultMessage L003");
        check(nm::diagDefaultMessage(LangString(u"L004")) == LangString(u"Output surface reference out of range"), "diagDefaultMessage L004");
    }

    // --- FUNC lexeme trimming is JS String.prototype.trim ----------------
    // (oracle-gated by parity/corpus/func_trim.dsl): U+FEFF and the Zs
    // spaces are trimmed; U+0085, which LangString::trimmed strips, is kept.
    {
        const JsonArray toks = lexJson(LangString::fromUtf16(u"f(a: () => \uFEFF\u00A0time\u0085, b: () => \u3000x\u2028)"));
        check(at(toks, 4).type == nm::TokenType::FUNC && at(toks, 4).lexeme == LangString::fromUtf16(u"time\u0085"),
              "FUNC trims U+FEFF and U+00A0 but keeps U+0085");
        check(at(toks, 8).type == nm::TokenType::FUNC && at(toks, 8).lexeme == LangString(u"x"),
              "FUNC trims U+3000 and U+2028");
    }

    {
        const auto astral = nm::lex(u"\"\U0001F600\" x");
        check(astral.size() >= 2 && astral[1].line == 1 && astral[1].col == 6,
              "astral string advances next token by two UTF-16 columns");
        const nm::Value tokenJson = nm::toJson(astral[1]);
        const nm::Value* position = tokenJson.as_object().find(u"position");
        check(position && position->is_object()
                  && position->as_object().find(u"start")->as_number() == 5
                  && position->as_object().find(u"end")->as_number() == 6,
              "token JSON preserves UTF-16 start and end offsets");
        const auto restored = nm::tokenFromJson(tokenJson);
        check(restored.hasPosition && restored.posStart == 5 && restored.posEnd == 6,
              "token JSON round-trip preserves position");
    }
    {
        const auto inherited = nm::lex(u"constructor __proto__ toString");
        const nm::Value functionToken = nm::toJson(inherited[0]);
        const nm::Value objectToken = nm::toJson(inherited[1]);
        check(!functionToken.as_object().has(u"type")
                  && objectToken.as_object().find(u"type")->is_object(),
              "inherited object-prototype token types preserve JSON omission/object shape");
        check(inherited[0].type == u"function Object() { [native code] }"
                  && inherited[1].type == u"[object Object]",
              "inherited token types preserve JS error-message coercion");
    }
    if (g_failures == 0) {
        std::printf("ALL PASS (test_lexer)\n");
        return 0;
    }
    std::printf("%d FAILURE(S) (test_lexer)\n", g_failures);
    return 1;
}
