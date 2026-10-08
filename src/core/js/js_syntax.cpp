#include "js_syntax.h"

#include "js_regexp.h"
#include "js_unicode.h"
#include "text.h"


#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// js_syntax.cpp -- whether V8 accepts a Function-constructor body.
//
// The parser is a C++ port of acorn 8.16 (acorn/dist/acorn.js: tokenizer,
// statement, expression, lval and scope modules), extended with the places
// where V8 accepts or rejects what acorn does not. Acorn's code carries this
// notice:
//
//   MIT License
//
//   Copyright (C) 2012-2022 by various contributors (see AUTHORS)
//
//   Permission is hereby granted, free of charge, to any person obtaining a
//   copy of this software and associated documentation files (the
//   "Software"), to deal in the Software without restriction, including
//   without limitation the rights to use, copy, modify, merge, publish,
//   distribute, sublicense, and/or sell copies of the Software, and to permit
//   persons to whom the Software is furnished to do so, subject to the
//   following conditions:
//
//   The above copyright notice and this permission notice shall be included
//   in all copies or substantial portions of the Software.
//
//   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
//   OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
//   MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN
//   NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
//   DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
//   OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
//   USE OR OTHER DEALINGS IN THE SOFTWARE.
//
// V8 behaviour that differs from acorn, and that this port follows:
//   - A call expression is a valid target for `=`, compound assignment,
//     `++`/`--` and a for-in/of head (V8 defers the error to run time), but
//     not for logical assignment or inside a destructuring pattern.
//   - `let` starts a lexical declaration only when the next token is `[`,
//     `{`, an identifier or a contextual keyword, or (in sloppy code) a
//     strict-mode reserved word. Before any other keyword, `let` is an
//     identifier.
//   - `using` is never a declaration in a single-statement position, and
//     never heads a for-in loop.
//   - `const` and `using` may omit the initializer only in a for-in/of
//     head (acorn also allows it before an `of` or `in` on the next line).
//   - A labelled function declaration binds its name in the enclosing scope.

namespace nm::js {

namespace {

struct SyntaxFail {
    JsText why;
    std::ptrdiff_t pos;
};

struct Undecidable {
    JsText what;
};

// Nesting beyond this depth is not decided (V8 throws RangeError on deep
// enough input, at a depth that depends on its stack).
constexpr int kMaxDepth = 400;

enum class Tk : std::uint8_t {
    Eof, Name, PrivateId, Num, String, Regexp, Template,
    BracketL, BracketR, BraceL, BraceR, ParenL, ParenR, Comma, Semi, Colon, Dot, Question, QuestionDot, Arrow,
    Ellipsis, Eq, Assign, IncDec, Prefix, LogicalOR, LogicalAND, BitwiseOR, BitwiseXOR, BitwiseAND, Equality,
    Relational, BitShift, PlusMin, Modulo, Star, Slash, StarStar, Coalesce,
};

struct Token {
    Tk type = Tk::Eof;
    std::ptrdiff_t start = 0;
    std::ptrdiff_t end = 0;
    JsText value;          // Name/PrivateId: cooked name; String: cooked value; operators: source text
    bool escaped = false;   // Name/PrivateId written with a \u escape
    bool octal = false;     // Num: legacy octal or leading-zero decimal; String: octal or \8 \9 escape
    bool tail = false;      // Template: chunk ends with the closing backtick
    bool badEscape = false; // Template: NotEscapeSequence (valid in tagged templates only)
};

const std::unordered_set<JsText>& keywordSet() {
    static const std::unordered_set<JsText> s = [] {
        std::unordered_set<JsText> out;
        for (const char* w : {"break", "case", "catch", "continue", "debugger", "default", "do", "else", "finally",
                              "for", "function", "if", "return", "switch", "throw", "try", "var", "while", "with",
                              "null", "true", "false", "instanceof", "typeof", "void", "delete", "new", "in", "this",
                              "const", "class", "extends", "export", "import", "super"}) {
            out.insert(JsText::fromLatin1(w));
        }
        return out;
    }();
    return s;
}

const std::unordered_set<JsText>& strictReservedSet() {
    static const std::unordered_set<JsText> s = {
        JsText(u"implements"), JsText(u"interface"), JsText(u"let"),  JsText(u"package"),
        JsText(u"private"),    JsText(u"protected"), JsText(u"public"), JsText(u"static"),
        JsText(u"yield"),
    };
    return s;
}

// Keywords whose token type starts an expression (acorn `startsExpr`).
const std::unordered_set<JsText>& keywordStartsExprSet() {
    static const std::unordered_set<JsText> s = {
        JsText(u"function"), JsText(u"class"),  JsText(u"new"),   JsText(u"this"),
        JsText(u"super"),    JsText(u"import"), JsText(u"null"),  JsText(u"true"),
        JsText(u"false"),    JsText(u"typeof"), JsText(u"void"),  JsText(u"delete"),
    };
    return s;
}

bool isKeyword(const JsText& w) { return keywordSet().contains(w); }

// Scope flags (acorn scopeflags.js).
enum : int {
    SCOPE_TOP = 1,
    SCOPE_FUNCTION = 2,
    SCOPE_ASYNC = 4,
    SCOPE_GENERATOR = 8,
    SCOPE_ARROW = 16,
    SCOPE_SIMPLE_CATCH = 32,
    SCOPE_SUPER = 64,
    SCOPE_DIRECT_SUPER = 128,
    SCOPE_CLASS_STATIC_BLOCK = 256,
    SCOPE_CLASS_FIELD_INIT = 512,
    SCOPE_SWITCH = 1024,
    SCOPE_VAR = SCOPE_TOP | SCOPE_FUNCTION | SCOPE_CLASS_STATIC_BLOCK,
};

int functionFlags(bool async, bool generator) {
    return SCOPE_FUNCTION | (async ? SCOPE_ASYNC : 0) | (generator ? SCOPE_GENERATOR : 0);
}

enum class Bind { None, Var, Lexical, Function, SimpleCatch, Outside };

struct Scope {
    int flags = 0;
    std::unordered_set<JsText> var;
    std::unordered_set<JsText> lexical;
    std::unordered_set<JsText> functions;
    JsText firstLexical;
    bool hasFirstLexical = false;
};

enum class LabelKind { None, Loop, Switch };

struct Label {
    JsText name;
    LabelKind kind = LabelKind::None;
    std::ptrdiff_t statementStart = -1;
};

struct PrivateScope {
    std::unordered_map<JsText, JsText> declared;
    std::vector<std::pair<JsText, std::ptrdiff_t>> used;
};

// Destructuring bookkeeping (acorn DestructuringErrors); -1 = unset.
struct DErr {
    std::ptrdiff_t shorthandAssign = -1;
    std::ptrdiff_t trailingComma = -1;
    std::ptrdiff_t parenthesizedAssign = -1;
    std::ptrdiff_t parenthesizedBind = -1;
    std::ptrdiff_t doubleProto = -1;
};

enum class NK : std::uint8_t {
    Ident, PrivateName, Literal, StringLit, Regex, Template, TaggedTemplate, This, Super, Array, Object, Property,
    Spread, Assign, Member, Call, Chain, New, Unary, Update, Binary, Logical, Conditional, Sequence, Arrow, Function,
    Class, Yield, Await, Meta, ImportCall, ObjectPattern, ArrayPattern, AssignPattern, Rest,
};

struct Node {
    NK kind = NK::Literal;
    std::ptrdiff_t start = 0;
    std::ptrdiff_t end = 0;
    JsText name;              // Ident/PrivateName: name; StringLit: cooked; Property: kind; Assign/Unary/Update: operator
    int a = -1;                // object / left / argument / key / callee / expression
    int b = -1;                // property / right / value
    std::vector<int> list;     // elements (-1 = hole) / properties / arguments / params / expressions
    bool computed = false;
    bool shorthand = false;
    bool method = false;
    bool optional = false;
};

struct VarResult {
    int count = 0;
    bool firstHasInit = false;
    bool firstIsIdent = false;
};

enum FuncFlag : int { FUNC_STATEMENT = 1, FUNC_HANGING_STATEMENT = 2, FUNC_NULLABLE_ID = 4, FUNC_NO_DECLARE = 8 };

bool isNewLineCode(char16_t c) { return c == 0x0A || c == 0x0D || c == 0x2028 || c == 0x2029; }

class Parser {
public:
    explicit Parser(const JsText& src) : src_(src), n_(src.size()) {}

private:
    JsText src_;
    std::ptrdiff_t n_;

    // ---------------------------------------------------------- state
    std::ptrdiff_t pos_ = 0;
    Token tok_;
    std::ptrdiff_t lastTokStart_ = 0;
    std::ptrdiff_t lastTokEnd_ = 0;
    bool strict_ = false;
    std::ptrdiff_t potentialArrowAt_ = -1;
    bool potentialArrowInForAwait_ = false;
    std::ptrdiff_t yieldPos_ = -1;
    std::ptrdiff_t awaitPos_ = -1;
    std::ptrdiff_t awaitIdentPos_ = -1;
    std::vector<Label> labels_;
    std::vector<Scope> scopes_;
    std::vector<PrivateScope> privateNames_;
    std::vector<Node> nodes_;
    int depth_ = 0;

    struct DepthGuard {
        explicit DepthGuard(Parser& p) : p_(p) {
            if (++p_.depth_ > kMaxDepth) throw Undecidable{JsText(u"nesting deeper than %1").arg(kMaxDepth)};
        }
        ~DepthGuard() { --p_.depth_; }
        DepthGuard(const DepthGuard&) = delete;
        DepthGuard& operator=(const DepthGuard&) = delete;
        Parser& p_;
    };

    [[noreturn]] void raise(std::ptrdiff_t pos, const JsText& why) const { throw SyntaxFail{why, pos}; }
    [[noreturn]] void unexpected(std::ptrdiff_t pos = -1) const {
        raise(pos < 0 ? tok_.start : pos, JsText(u"Unexpected token"));
    }

    char16_t ch(std::ptrdiff_t i) const { return i >= 0 && i < n_ ? src_[static_cast<std::size_t>(i)] : char16_t(0); }
    bool has(std::ptrdiff_t i) const { return i >= 0 && i < n_; }

    char32_t fullCharCodeAt(std::ptrdiff_t i) const {
        if (i >= n_) return 0xFFFFFFFF;
        const char16_t c = ch(i);
        if (c < 0xD800 || c > 0xDBFF || i + 1 >= n_) return c;
        const char16_t next = ch(i + 1);
        if (next < 0xDC00 || next > 0xDFFF) return c;
        return 0x10000 + ((static_cast<char32_t>(c) - 0xD800) << 10) + (next - 0xDC00);
    }

    int nodeOf(NK kind, std::ptrdiff_t start) {
        Node nd;
        nd.kind = kind;
        nd.start = start;
        nodes_.push_back(nd);
        return static_cast<int>(nodes_.size() - 1);
    }
    Node& N(int i) { return nodes_[static_cast<size_t>(i)]; }
    const Node& N(int i) const { return nodes_[static_cast<size_t>(i)]; }
    int finish(int i) {
        N(i).end = lastTokEnd_;
        return i;
    }

    // ---------------------------------------------------------- scanner
    bool isKeywordToken() const { return tok_.type == Tk::Name && isKeyword(tok_.value); }
    bool isKw(const char* w) const { return tok_.type == Tk::Name && tok_.value == JsText::fromLatin1(w); }
    bool isContextual(const char* w) const { return tok_.type == Tk::Name && !tok_.escaped && tok_.value == JsText::fromLatin1(w); }

    void next(bool ignoreEscapeInKeyword = false) {
        if (!ignoreEscapeInKeyword && isKeywordToken() && tok_.escaped) {
            raise(tok_.start, JsText(u"Escape sequence in keyword %1").arg(tok_.value));
        }
        lastTokEnd_ = tok_.end;
        lastTokStart_ = tok_.start;
        nextToken();
    }

    bool eat(Tk t) {
        if (tok_.type == t) {
            next();
            return true;
        }
        return false;
    }
    bool eatKw(const char* w) {
        if (isKw(w)) {
            next();
            return true;
        }
        return false;
    }
    bool eatContextual(const char* w) {
        if (!isContextual(w)) return false;
        next();
        return true;
    }
    void expect(Tk t) {
        if (!eat(t)) unexpected();
    }
    bool hasLineBreak(std::ptrdiff_t from, std::ptrdiff_t to) const {
        for (std::ptrdiff_t i = from; i < to && i < n_; ++i) {
            if (isNewLineCode(ch(i))) return true;
        }
        return false;
    }
    bool canInsertSemicolon() const {
        return tok_.type == Tk::Eof || tok_.type == Tk::BraceR || hasLineBreak(lastTokEnd_, tok_.start);
    }
    bool insertSemicolon() const { return canInsertSemicolon(); }
    void semicolon() {
        if (!eat(Tk::Semi) && !insertSemicolon()) unexpected();
    }
    bool afterTrailingComma(Tk t, bool notNext = false) {
        if (tok_.type == t) {
            if (!notNext) next();
            return true;
        }
        return false;
    }

    struct SavedState {
        std::ptrdiff_t pos;
        Token tok;
        std::ptrdiff_t lastTokStart;
        std::ptrdiff_t lastTokEnd;
    };
    SavedState save() const { return SavedState{pos_, tok_, lastTokStart_, lastTokEnd_}; }
    void restore(const SavedState& s) {
        pos_ = s.pos;
        tok_ = s.tok;
        lastTokStart_ = s.lastTokStart;
        lastTokEnd_ = s.lastTokEnd;
    }
    // The token after the current one (scanned then discarded). A scan
    // error there reports an Eof token.
    Token peekToken() {
        const SavedState s = save();
        Token t;
        try {
            lastTokEnd_ = tok_.end;
            lastTokStart_ = tok_.start;
            nextToken();
            t = tok_;
        } catch (const SyntaxFail&) {
            t = Token{};
            t.type = Tk::Eof;
            t.start = n_;
            t.end = n_;
        }
        restore(s);
        return t;
    }

    void skipLineComment(std::ptrdiff_t startSkip) {
        pos_ += startSkip;
        while (pos_ < n_ && !isNewLineCode(ch(pos_))) ++pos_;
    }
    void skipBlockComment() {
        const std::ptrdiff_t start = pos_;
        const std::ptrdiff_t end = src_.indexOf(JsText(u"*/"), pos_ + 2);
        if (end == -1) raise(start, JsText(u"Unterminated comment"));
        pos_ = end + 2;
    }
    void skipSpace() {
        while (pos_ < n_) {
            const char16_t c = ch(pos_);
            if (c == 32 || c == 160) {
                ++pos_;
            } else if (c == 13) {
                if (ch(pos_ + 1) == 10) ++pos_;
                ++pos_;
            } else if (c == 10 || c == 0x2028 || c == 0x2029) {
                ++pos_;
            } else if (c == u'/') {
                const char16_t n2 = ch(pos_ + 1);
                if (n2 == u'*' && has(pos_ + 1)) skipBlockComment();
                else if (n2 == u'/' && has(pos_ + 1)) skipLineComment(2);
                else break;
            } else if ((c > 8 && c < 14) || (c >= 5760 && isWhiteSpace(c))) {
                ++pos_;
            } else {
                break;
            }
        }
    }

    void finishToken(Tk type, const JsText& value = JsText()) {
        tok_.type = type;
        tok_.end = pos_;
        tok_.value = value;
    }
    void finishOp(Tk type, std::ptrdiff_t size) {
        const JsText str = src_.mid(pos_, size);
        pos_ += size;
        finishToken(type, str);
    }

    void nextToken() {
        skipSpace();
        tok_ = Token{};
        tok_.start = pos_;
        if (pos_ >= n_) {
            finishToken(Tk::Eof);
            return;
        }
        readToken(fullCharCodeAt(pos_));
    }

    void readToken(char32_t code) {
        if (isIdStartCodePoint(code) || code == u'\\') {
            readWord();
            return;
        }
        getTokenFromCode(code);
    }

    void readWord() {
        bool escaped = false;
        const JsText word = readWord1(&escaped);
        finishToken(Tk::Name, word);
        tok_.escaped = escaped;
    }

    JsText readWord1(bool* containsEsc) {
        *containsEsc = false;
        JsText word;
        bool first = true;
        std::ptrdiff_t chunkStart = pos_;
        while (pos_ < n_) {
            const char32_t c = fullCharCodeAt(pos_);
            if (isIdPartCodePoint(c)) {
                pos_ += c <= 0xFFFF ? 1 : 2;
            } else if (c == u'\\') {
                *containsEsc = true;
                word += src_.mid(chunkStart, pos_ - chunkStart);
                const std::ptrdiff_t escStart = pos_;
                if (ch(++pos_) != u'u' || !has(pos_)) raise(pos_, JsText(u"Expecting Unicode escape sequence \\uXXXX"));
                ++pos_;
                const long esc = readCodePoint(false);
                if (esc < 0 || !(first ? isIdStartCodePoint(static_cast<char32_t>(esc)) : isIdPartCodePoint(static_cast<char32_t>(esc)))) {
                    raise(escStart, JsText(u"Invalid Unicode escape"));
                }
                const char32_t cp = static_cast<char32_t>(esc);
                word += JsText::fromUcs4(&cp, 1);
                chunkStart = pos_;
            } else {
                break;
            }
            first = false;
        }
        return word + src_.mid(chunkStart, pos_ - chunkStart);
    }

    // Reads the digits of \u{...} or \uXXXX (pos_ after the `u`). Returns
    // -1 for an invalid escape.
    long readCodePoint(bool /*unused*/) {
        if (ch(pos_) == u'{' && has(pos_)) {
            ++pos_;
            const std::ptrdiff_t close = src_.indexOf(u'}', pos_);
            const long code = readHexChar(close < 0 ? -1 : close - pos_);
            ++pos_;
            if (code < 0 || code > 0x10FFFF) return -1;
            return code;
        }
        return readHexChar(4);
    }

    long readHexChar(std::ptrdiff_t len) {
        if (len < 0) return -1;
        const std::ptrdiff_t start = pos_;
        long total = 0;
        std::ptrdiff_t i = 0;
        for (; i < len; ++i) {
            const char16_t c = ch(pos_);
            int val = -1;
            if (!has(pos_)) break;
            if (c >= u'0' && c <= u'9') val = c - u'0';
            else if (c >= u'a' && c <= u'f') val = c - u'a' + 10;
            else if (c >= u'A' && c <= u'F') val = c - u'A' + 10;
            if (val < 0) break;
            if (total <= 0x10FFFF) total = total * 16 + val;
            ++pos_;
        }
        if (pos_ - start != len || len == 0) return -1;
        return total;
    }

    void getTokenFromCode(char32_t code) {
        switch (code) {
            case u'.': {
                const char16_t next = ch(pos_ + 1);
                if (has(pos_ + 1) && next >= u'0' && next <= u'9') {
                    readNumber(true);
                    return;
                }
                if (next == u'.' && ch(pos_ + 2) == u'.' && has(pos_ + 2)) {
                    pos_ += 3;
                    finishToken(Tk::Ellipsis);
                    return;
                }
                ++pos_;
                finishToken(Tk::Dot);
                return;
            }
            case u'(': ++pos_; finishToken(Tk::ParenL); return;
            case u')': ++pos_; finishToken(Tk::ParenR); return;
            case u';': ++pos_; finishToken(Tk::Semi); return;
            case u',': ++pos_; finishToken(Tk::Comma); return;
            case u'[': ++pos_; finishToken(Tk::BracketL); return;
            case u']': ++pos_; finishToken(Tk::BracketR); return;
            case u'{': ++pos_; finishToken(Tk::BraceL); return;
            case u'}': ++pos_; finishToken(Tk::BraceR); return;
            case u':': ++pos_; finishToken(Tk::Colon); return;
            case u'`': readTemplateChunk(pos_ + 1); return;
            case u'0': {
                const char16_t next = ch(pos_ + 1);
                if (next == u'x' || next == u'X') { readRadixNumber(16); return; }
                if (next == u'o' || next == u'O') { readRadixNumber(8); return; }
                if (next == u'b' || next == u'B') { readRadixNumber(2); return; }
                readNumber(false);
                return;
            }
            case u'1': case u'2': case u'3': case u'4': case u'5': case u'6': case u'7': case u'8': case u'9':
                readNumber(false);
                return;
            case u'"': case u'\'':
                readString(static_cast<char16_t>(code));
                return;
            case u'/':
                if (ch(pos_ + 1) == u'=') finishOp(Tk::Assign, 2);
                else finishOp(Tk::Slash, 1);
                return;
            case u'%': case u'*': {
                std::ptrdiff_t size = 1;
                Tk type = code == u'*' ? Tk::Star : Tk::Modulo;
                char16_t next = ch(pos_ + 1);
                if (code == u'*' && next == u'*') {
                    ++size;
                    type = Tk::StarStar;
                    next = ch(pos_ + 2);
                }
                if (next == u'=' && has(pos_ + size)) finishOp(Tk::Assign, size + 1);
                else finishOp(type, size);
                return;
            }
            case u'|': case u'&': {
                const char16_t next = ch(pos_ + 1);
                if (static_cast<char32_t>(next) == code) {
                    if (ch(pos_ + 2) == u'=') { finishOp(Tk::Assign, 3); return; }
                    finishOp(code == u'|' ? Tk::LogicalOR : Tk::LogicalAND, 2);
                    return;
                }
                if (next == u'=') { finishOp(Tk::Assign, 2); return; }
                finishOp(code == u'|' ? Tk::BitwiseOR : Tk::BitwiseAND, 1);
                return;
            }
            case u'^':
                if (ch(pos_ + 1) == u'=') finishOp(Tk::Assign, 2);
                else finishOp(Tk::BitwiseXOR, 1);
                return;
            case u'+': case u'-': {
                const char16_t next = ch(pos_ + 1);
                if (static_cast<char32_t>(next) == code) {
                    if (next == u'-' && ch(pos_ + 2) == u'>' && (lastTokEnd_ == 0 || hasLineBreak(lastTokEnd_, pos_))) {
                        // A `-->` line comment (Annex B, script goal).
                        skipLineComment(3);
                        nextToken();
                        return;
                    }
                    finishOp(Tk::IncDec, 2);
                    return;
                }
                if (next == u'=') { finishOp(Tk::Assign, 2); return; }
                finishOp(Tk::PlusMin, 1);
                return;
            }
            case u'<': case u'>': {
                const char16_t next = ch(pos_ + 1);
                std::ptrdiff_t size = 1;
                if (static_cast<char32_t>(next) == code && has(pos_ + 1)) {
                    size = (code == u'>' && ch(pos_ + 2) == u'>') ? 3 : 2;
                    if (ch(pos_ + size) == u'=' && has(pos_ + size)) { finishOp(Tk::Assign, size + 1); return; }
                    finishOp(Tk::BitShift, size);
                    return;
                }
                if (next == u'!' && code == u'<' && ch(pos_ + 2) == u'-' && ch(pos_ + 3) == u'-') {
                    // `<!--`, an HTML-like line comment (Annex B, script goal).
                    skipLineComment(4);
                    nextToken();
                    return;
                }
                if (next == u'=') size = 2;
                finishOp(Tk::Relational, size);
                return;
            }
            case u'=': case u'!': {
                const char16_t next = ch(pos_ + 1);
                if (next == u'=') { finishOp(Tk::Equality, ch(pos_ + 2) == u'=' ? 3 : 2); return; }
                if (code == u'=' && next == u'>') {
                    pos_ += 2;
                    finishToken(Tk::Arrow);
                    return;
                }
                finishOp(code == u'=' ? Tk::Eq : Tk::Prefix, 1);
                return;
            }
            case u'?': {
                const char16_t next = ch(pos_ + 1);
                if (next == u'.') {
                    const char16_t next2 = ch(pos_ + 2);
                    if (!(has(pos_ + 2) && next2 >= u'0' && next2 <= u'9')) { finishOp(Tk::QuestionDot, 2); return; }
                }
                if (next == u'?') {
                    if (ch(pos_ + 2) == u'=') { finishOp(Tk::Assign, 3); return; }
                    finishOp(Tk::Coalesce, 2);
                    return;
                }
                finishOp(Tk::Question, 1);
                return;
            }
            case u'~':
                finishOp(Tk::Prefix, 1);
                return;
            case u'#': {
                ++pos_;
                const char32_t c = fullCharCodeAt(pos_);
                if (has(pos_) && (isIdStartCodePoint(c) || c == u'\\')) {
                    bool escaped = false;
                    const JsText word = readWord1(&escaped);
                    finishToken(Tk::PrivateId, word);
                    tok_.escaped = escaped;
                    return;
                }
                raise(pos_, JsText(u"Unexpected character '#'"));
            }
            default:
                break;
        }
        raise(pos_, JsText(u"Unexpected character"));
    }

    // Digits in `radix`; separators allowed when len < 0. Returns the digit
    // count, or -1 when none were read (or len was not met).
    std::ptrdiff_t readInt(int radix, std::ptrdiff_t len, bool maybeLegacyOctal) {
        const bool allowSeparators = len < 0;
        const bool isLegacyOctal = maybeLegacyOctal && ch(pos_) == u'0';
        const std::ptrdiff_t start = pos_;
        char16_t lastCode = 0;
        for (std::ptrdiff_t i = 0; len < 0 || i < len; ++i, ++pos_) {
            if (!has(pos_)) break;
            const char16_t code = ch(pos_);
            if (allowSeparators && code == u'_') {
                if (isLegacyOctal) raise(pos_, JsText(u"Numeric separator is not allowed in legacy octal numeric literals"));
                if (lastCode == u'_') raise(pos_, JsText(u"Numeric separator must be exactly one underscore"));
                if (i == 0) raise(pos_, JsText(u"Numeric separator is not allowed at the first of digits"));
                lastCode = code;
                continue;
            }
            int val = 99;
            if (code >= u'a') val = code - u'a' + 10;
            else if (code >= u'A') val = code - u'A' + 10;
            else if (code >= u'0' && code <= u'9') val = code - u'0';
            if (val >= radix) break;
            lastCode = code;
        }
        if (allowSeparators && lastCode == u'_') raise(pos_ - 1, JsText(u"Numeric separator is not allowed at the last of digits"));
        if (pos_ == start || (len >= 0 && pos_ - start != len)) return -1;
        return pos_ - start;
    }

    void readRadixNumber(int radix) {
        pos_ += 2;
        if (readInt(radix, -1, false) < 0) raise(tok_.start + 2, JsText(u"Expected number in radix %1").arg(radix));
        if (ch(pos_) == u'n' && has(pos_)) {
            ++pos_;
        }
        if (has(pos_) && isIdStartCodePoint(fullCharCodeAt(pos_))) raise(pos_, JsText(u"Identifier directly after number"));
        finishToken(Tk::Num);
    }

    void readNumber(bool startsWithDot) {
        const std::ptrdiff_t start = pos_;
        if (!startsWithDot && readInt(10, -1, true) < 0) raise(start, JsText(u"Invalid number"));
        bool octal = pos_ - start >= 2 && ch(start) == u'0';
        const bool leadingZero = octal;
        char16_t next = has(pos_) ? ch(pos_) : char16_t(0);
        if (!octal && !startsWithDot && next == u'n') {
            ++pos_;
            if (has(pos_) && isIdStartCodePoint(fullCharCodeAt(pos_))) raise(pos_, JsText(u"Identifier directly after number"));
            finishToken(Tk::Num);
            return;
        }
        if (octal) {
            for (std::ptrdiff_t i = start; i < pos_; ++i) {
                if (ch(i) == u'8' || ch(i) == u'9') {
                    octal = false;
                    break;
                }
            }
        }
        if (next == u'.' && !octal) {
            ++pos_;
            readInt(10, -1, false);
            next = has(pos_) ? ch(pos_) : char16_t(0);
        }
        if ((next == u'E' || next == u'e') && !octal) {
            next = ch(++pos_);
            if ((next == u'+' || next == u'-') && has(pos_)) ++pos_;
            if (readInt(10, -1, false) < 0) raise(start, JsText(u"Invalid number"));
        }
        if (has(pos_) && isIdStartCodePoint(fullCharCodeAt(pos_))) raise(pos_, JsText(u"Identifier directly after number"));
        finishToken(Tk::Num);
        tok_.octal = leadingZero;
    }

    void readString(char16_t quote) {
        JsText out;
        std::ptrdiff_t chunkStart = ++pos_;
        bool octal = false;
        for (;;) {
            if (pos_ >= n_) raise(tok_.start, JsText(u"Unterminated string constant"));
            const char16_t c = ch(pos_);
            if (c == quote) break;
            if (c == u'\\') {
                out += src_.mid(chunkStart, pos_ - chunkStart);
                bool bad = false;
                out += readEscapedChar(false, &octal, &bad);
                if (bad) raise(pos_, JsText(u"Bad character escape sequence"));
                chunkStart = pos_;
            } else if (c == 0x2028 || c == 0x2029) {
                ++pos_;
            } else {
                if (isNewLineCode(c)) raise(tok_.start, JsText(u"Unterminated string constant"));
                ++pos_;
            }
        }
        out += src_.mid(chunkStart, pos_ - chunkStart);
        ++pos_;
        finishToken(Tk::String, out);
        tok_.octal = octal;
    }

    // Reads one escape (pos_ at the backslash). Sets *octal for legacy
    // octal escapes and \8 \9, and *bad for an invalid escape.
    JsText readEscapedChar(bool inTemplate, bool* octal, bool* bad) {
        if (!has(pos_ + 1)) {
            ++pos_;
            *bad = true;
            return JsText();
        }
        const char16_t c = ch(++pos_);
        ++pos_;
        switch (c) {
            case u'n': return JsText(u"\n");
            case u'r': return JsText(u"\r");
            case u't': return JsText(u"\t");
            case u'b': return JsText(u"\b");
            case u'v': return JsText(char16_t(0x0B));
            case u'f': return JsText(u"\f");
            case u'x': {
                const long v = readHexChar(2);
                if (v < 0) {
                    *bad = true;
                    return JsText();
                }
                return JsText(char16_t(static_cast<char16_t>(v)));
            }
            case u'u': {
                const long v = readCodePoint(false);
                if (v < 0) {
                    *bad = true;
                    return JsText();
                }
                const char32_t cp = static_cast<char32_t>(v);
                return JsText::fromUcs4(&cp, 1);
            }
            case 13:
                if (ch(pos_) == 10 && has(pos_)) ++pos_;
                return JsText();
            case 10:
                return JsText();
            case u'8': case u'9':
                *octal = true;
                if (inTemplate) *bad = true;
                return JsText(char16_t(c));
            default:
                break;
        }
        if (c >= u'0' && c <= u'7') {
            // Longest octal run of up to three digits whose value fits 0-255.
            std::ptrdiff_t len = 1;
            while (len < 3 && has(pos_ - 1 + len) && ch(pos_ - 1 + len) >= u'0' && ch(pos_ - 1 + len) <= u'7') ++len;
            JsText octalStr = src_.mid(pos_ - 1, len);
            if (octalStr.toInt(nullptr, 8) > 255) octalStr.chop(1);
            pos_ += octalStr.size() - 1;
            const char16_t after = has(pos_) ? ch(pos_) : char16_t(0);
            if (octalStr != JsText(u"0") || after == u'8' || after == u'9') {
                *octal = true;
                if (inTemplate) *bad = true;
            }
            return JsText(char16_t(static_cast<char16_t>(octalStr.toInt(nullptr, 8))));
        }
        if (isNewLineCode(c)) return JsText();
        return JsText(char16_t(c));
    }

    // A template chunk from `from` (just after the backtick or `}`) to `${`
    // or the closing backtick.
    void readTemplateChunk(std::ptrdiff_t from) {
        pos_ = from;
        bool bad = false;
        for (;;) {
            if (pos_ >= n_) raise(tok_.start, JsText(u"Unterminated template"));
            const char16_t c = ch(pos_);
            if (c == u'`') {
                ++pos_;
                finishToken(Tk::Template);
                tok_.tail = true;
                tok_.badEscape = bad;
                return;
            }
            if (c == u'$' && ch(pos_ + 1) == u'{' && has(pos_ + 1)) {
                pos_ += 2;
                finishToken(Tk::Template);
                tok_.tail = false;
                tok_.badEscape = bad;
                return;
            }
            if (c == u'\\') {
                bool octal = false;
                bool badEsc = false;
                readEscapedChar(true, &octal, &badEsc);
                if (badEsc) bad = true;
            } else {
                ++pos_;
            }
        }
    }

    // Re-reads the current `/` or `/=` token as a regular expression.
    void readRegexp() {
        const std::ptrdiff_t start = tok_.start + 1;
        pos_ = start;
        bool escaped = false;
        bool inClass = false;
        for (;;) {
            if (pos_ >= n_) raise(start, JsText(u"Unterminated regular expression"));
            const char16_t c = ch(pos_);
            if (isNewLineCode(c)) raise(start, JsText(u"Unterminated regular expression"));
            if (!escaped) {
                if (c == u'[') inClass = true;
                else if (c == u']' && inClass) inClass = false;
                else if (c == u'/' && !inClass) break;
                escaped = c == u'\\';
            } else {
                escaped = false;
            }
            ++pos_;
        }
        const JsText pattern = src_.mid(start, pos_ - start);
        ++pos_;
        const std::ptrdiff_t flagsStart = pos_;
        bool containsEsc = false;
        const JsText flags = readWord1(&containsEsc);
        if (containsEsc) unexpected(flagsStart);
        JsText error;
        const RegexCheck check = validateRegExpLiteral(pattern, flags, &error);
        if (check == RegexCheck::Undecidable) throw Undecidable{error};
        if (check == RegexCheck::Invalid) raise(start, error);
        finishToken(Tk::Regexp);
    }

    // ---------------------------------------------------------- scopes
    Scope& currentScope() { return scopes_.back(); }
    const Scope& currentVarScope() const {
        for (size_t i = scopes_.size(); i-- > 0;) {
            if (scopes_[i].flags & (SCOPE_VAR | SCOPE_CLASS_FIELD_INIT | SCOPE_CLASS_STATIC_BLOCK)) return scopes_[i];
        }
        return scopes_.front();
    }
    const Scope& currentThisScope() const {
        for (size_t i = scopes_.size(); i-- > 0;) {
            const int f = scopes_[i].flags;
            if ((f & (SCOPE_VAR | SCOPE_CLASS_FIELD_INIT | SCOPE_CLASS_STATIC_BLOCK)) && !(f & SCOPE_ARROW)) return scopes_[i];
        }
        return scopes_.front();
    }
    bool inFunction() const { return (currentVarScope().flags & SCOPE_FUNCTION) != 0; }
    bool inGenerator() const { return (currentVarScope().flags & SCOPE_GENERATOR) != 0; }
    bool inAsync() const { return (currentVarScope().flags & SCOPE_ASYNC) != 0; }
    bool canAwait() const {
        for (size_t i = scopes_.size(); i-- > 0;) {
            const int f = scopes_[i].flags;
            if (f & (SCOPE_CLASS_STATIC_BLOCK | SCOPE_CLASS_FIELD_INIT)) return false;
            if (f & SCOPE_FUNCTION) return (f & SCOPE_ASYNC) != 0;
        }
        return false;
    }
    bool allowSuper() const { return (currentThisScope().flags & SCOPE_SUPER) != 0; }
    bool allowDirectSuper() const { return (currentThisScope().flags & SCOPE_DIRECT_SUPER) != 0; }
    bool treatFunctionsAsVarInScope(const Scope& s) const { return (s.flags & SCOPE_FUNCTION) || (s.flags & SCOPE_TOP); }
    bool treatFunctionsAsVar() const { return treatFunctionsAsVarInScope(scopes_.back()); }
    bool allowNewDotTarget() const {
        for (size_t i = scopes_.size(); i-- > 0;) {
            const int f = scopes_[i].flags;
            if ((f & (SCOPE_CLASS_STATIC_BLOCK | SCOPE_CLASS_FIELD_INIT)) || ((f & SCOPE_FUNCTION) && !(f & SCOPE_ARROW))) return true;
        }
        return false;
    }
    bool allowUsing() const {
        const int f = scopes_.back().flags;
        if (f & SCOPE_SWITCH) return false;
        if (f & SCOPE_TOP) return false;
        return true;
    }
    bool inClassStaticBlock() const { return (currentVarScope().flags & SCOPE_CLASS_STATIC_BLOCK) != 0; }

    void enterScope(int flags) {
        Scope s;
        s.flags = flags;
        scopes_.push_back(s);
    }
    void exitScope() { scopes_.pop_back(); }

    void declareName(const JsText& name, Bind bindingType, std::ptrdiff_t pos) {
        bool redeclared = false;
        if (bindingType == Bind::Lexical) {
            Scope& scope = currentScope();
            redeclared = scope.lexical.contains(name) || scope.functions.contains(name) || scope.var.contains(name);
            addLexical(scope, name);
        } else if (bindingType == Bind::SimpleCatch) {
            addLexical(currentScope(), name);
        } else if (bindingType == Bind::Function) {
            Scope& scope = currentScope();
            if (treatFunctionsAsVar()) redeclared = scope.lexical.contains(name);
            else redeclared = scope.lexical.contains(name) || scope.var.contains(name);
            scope.functions.insert(name);
        } else {
            for (size_t i = scopes_.size(); i-- > 0;) {
                Scope& scope = scopes_[i];
                if ((scope.lexical.contains(name)
                     && !((scope.flags & SCOPE_SIMPLE_CATCH) && scope.hasFirstLexical && scope.firstLexical == name))
                    || (!treatFunctionsAsVarInScope(scope) && scope.functions.contains(name))) {
                    redeclared = true;
                    break;
                }
                scope.var.insert(name);
                if (scope.flags & SCOPE_VAR) break;
            }
        }
        if (redeclared) raise(pos, JsText(u"Identifier '%1' has already been declared").arg(name));
    }
    static void addLexical(Scope& scope, const JsText& name) {
        if (!scope.hasFirstLexical) {
            scope.hasFirstLexical = true;
            scope.firstLexical = name;
        }
        scope.lexical.insert(name);
    }

    // ---------------------------------------------------------- identifiers
    void checkUnreserved(const JsText& name, std::ptrdiff_t start) {
        if (inGenerator() && name == JsText(u"yield")) raise(start, JsText(u"Cannot use 'yield' as identifier inside a generator"));
        if (inAsync() && name == JsText(u"await")) raise(start, JsText(u"Cannot use 'await' as identifier inside an async function"));
        // V8 (unlike acorn) also rejects `arguments` in an arrow function
        // inside a class static block.
        if ((currentThisScope().flags & (SCOPE_CLASS_FIELD_INIT | SCOPE_CLASS_STATIC_BLOCK)) && name == JsText(u"arguments")) {
            raise(start, JsText(u"Cannot use 'arguments' in class field initializer or static block"));
        }
        if (inClassStaticBlock() && (name == JsText(u"arguments") || name == JsText(u"await"))) {
            raise(start, JsText(u"Cannot use %1 in class static initialization block").arg(name));
        }
        if (isKeyword(name)) raise(start, JsText(u"Unexpected keyword '%1'").arg(name));
        if (name == JsText(u"enum") || (strict_ && strictReservedSet().contains(name))) {
            raise(start, JsText(u"The keyword '%1' is reserved").arg(name));
        }
    }

    int parseIdent(bool liberal) {
        if (tok_.type != Tk::Name) unexpected();
        const int node = nodeOf(NK::Ident, tok_.start);
        N(node).name = tok_.value;
        next(liberal);
        finish(node);
        if (!liberal) {
            checkUnreserved(N(node).name, N(node).start);
            if (N(node).name == JsText(u"await") && awaitIdentPos_ < 0) awaitIdentPos_ = N(node).start;
        }
        return node;
    }

    int parsePrivateIdent() {
        if (tok_.type != Tk::PrivateId) unexpected();
        const int node = nodeOf(NK::PrivateName, tok_.start);
        N(node).name = tok_.value;
        next();
        finish(node);
        if (privateNames_.empty()) {
            raise(N(node).start, JsText(u"Private field '#%1' must be declared in an enclosing class").arg(N(node).name));
        }
        privateNames_.back().used.emplace_back(N(node).name, N(node).start);
        return node;
    }

public:
    void run(const JsText& param);

private:
    // Statements, expressions, lval: defined below.
    int parseStatement(const JsText& context, bool topLevel = false);
    int parseBreakContinueStatement(bool isBreak);
    int parseDoStatement();
    int parseForStatement();
    int parseForAfterInit(const VarResult& init, const JsText& kind, std::ptrdiff_t initStart, std::ptrdiff_t awaitAt);
    int parseFunctionStatement(bool isAsync, bool declarationPosition, bool declare);
    bool isNameTok() const { return tok_.type == Tk::Name && !isKeyword(tok_.value); }
    int parseIfStatement();
    int parseReturnStatement();
    int parseSwitchStatement();
    int parseThrowStatement();
    int parseTryStatement();
    int parseVarStatement(const JsText& kind);
    int parseWhileStatement();
    int parseWithStatement();
    int parseLabeledStatement(const JsText& maybeName, int expr, const JsText& context, std::ptrdiff_t nodeStart);
    void parseBlock(bool createNewLexicalScope);
    bool parseStatementsWithDirectives(Tk terminator);
    int parseFor();
    int parseForIn(const VarResult* decl, const JsText& kind, std::ptrdiff_t initStart);
    VarResult parseVar(bool isFor, const JsText& kind, bool allowMissingInitializer = false);
    int parseFunction(std::ptrdiff_t start, int statement, bool isAsync, int forInit);
    void parseFunctionParams(std::vector<int>& params);
    int parseClass(std::ptrdiff_t start, bool isStatement);
    void parseClassElement(bool constructorAllowsSuper, std::unordered_map<JsText, JsText>& privateNameMap, bool& hadConstructor);
    bool isClassElementNameStart() const;
    void parseClassStaticBlock();
    void exitClassBody();
    bool isLet(const JsText& context);
    bool isAsyncFunction();
    bool isUsingKeyword(bool isAwaitUsing, bool isFor);

    int parseExpression(int forInit = 0, DErr* ref = nullptr);
    int parseMaybeAssign(int forInit = 0, DErr* ref = nullptr);
    int parseMaybeConditional(int forInit, DErr* ref);
    int parseExprOps(int forInit, DErr* ref);
    int parseExprOp(int left, std::ptrdiff_t leftStart, int minPrec, int forInit);
    int buildBinary(std::ptrdiff_t start, int left, int right, const JsText& op, bool logical);
    int parseMaybeUnary(DErr* ref, bool sawUnary, bool incDec, int forInit);
    int parseExprSubscripts(DErr* ref, int forInit);
    int parseSubscripts(int base, std::ptrdiff_t start, bool noCalls, int forInit);
    int parseSubscript(int base, std::ptrdiff_t start, bool noCalls, bool maybeAsyncArrow, bool optionalChained, int forInit);
    int parseExprAtom(DErr* ref, int forInit, bool forNew = false);
    int parseExprImport(bool forNew);
    int parseLiteralToken();
    int parseParenAndDistinguishExpression(bool canBeArrow, int forInit);
    int parseNew();
    int parseTemplate(bool isTagged);
    int parseObj(bool isPattern, DErr* ref);
    int parseProperty(bool isPattern, DErr* ref);
    void parsePropertyName(int prop);
    void parsePropertyValue(int prop, bool isPattern, bool isGenerator, bool isAsync, std::ptrdiff_t startPos, DErr* ref, bool containsEsc);
    int parseMethod(bool isGenerator, bool isAsync, bool allowDirectSuper);
    int parseArrowExpression(std::ptrdiff_t start, std::vector<int> params, bool isAsync, int forInit);
    void parseFunctionBody(const std::vector<int>& params, bool isArrowFunction, bool isMethod, int forInit, int idNode);
    bool isSimpleParamList(const std::vector<int>& params) const;
    void checkParams(const std::vector<int>& params, bool allowDuplicates);
    std::vector<int> parseExprList(Tk close, bool allowTrailingComma, bool allowEmpty, DErr* ref);
    int parseYield(int forInit);
    int parseAwait(int forInit);
    bool tokStartsExpr() const;
    int binopPrec() const;

    int toAssignable(int node, bool isBinding, DErr* ref);
    void toAssignableList(std::vector<int>& list, bool isBinding);
    int parseSpread(DErr* ref);
    int parseRestBinding();
    int parseBindingAtom();
    std::vector<int> parseBindingList(Tk close, bool allowEmpty, bool allowTrailingComma);
    int parseMaybeDefault(std::ptrdiff_t startPos, int left = -1);
    void checkLValSimple(int expr, Bind bindingType = Bind::None, std::unordered_set<JsText>* checkClashes = nullptr, bool allowCall = false);
    void checkLValPattern(int expr, Bind bindingType = Bind::None, std::unordered_set<JsText>* checkClashes = nullptr);
    void checkLValInnerPattern(int expr, Bind bindingType = Bind::None, std::unordered_set<JsText>* checkClashes = nullptr);
    void checkPatternErrors(const DErr* ref, bool isAssign);
    bool checkExpressionErrors(const DErr* ref, bool andThrow = false);
    void checkYieldAwaitInDefaultParams();
    bool isSimpleAssignTarget(int expr) const;
    void checkPropClash(int prop, bool& hasProto, DErr* ref);
    bool checkKeyName(int element, const char* name) const;
    bool isPrivateFieldAccess(int node) const;
    void collectBoundNames(int pattern, std::vector<std::pair<JsText, std::ptrdiff_t>>& out);
    static bool isStrictBindReserved(const JsText& name) {
        return strictReservedSet().contains(name) || name == JsText(u"eval") || name == JsText(u"arguments")
               || name == JsText(u"enum");
    }
};

// ============================================================== statements

bool Parser::isLet(const JsText& context) {
    if (!isContextual("let")) return false;
    const Token next = peekToken();
    // `let [` is excluded from ExpressionStatement; in any other
    // single-statement position `let` is an identifier.
    if (next.type == Tk::BracketL) return true;
    if (!context.empty()) return false;
    if (next.type == Tk::BraceL) return true;
    // V8: an identifier, a contextual keyword or a strict-mode reserved
    // word makes a declaration; a keyword (or `enum`) leaves `let` an
    // identifier.
    if (next.type == Tk::Name) return !isKeyword(next.value) && next.value != JsText(u"enum");
    return false;
}

bool Parser::isAsyncFunction() {
    if (!isContextual("async")) return false;
    const Token next = peekToken();
    return next.type == Tk::Name && next.value == JsText(u"function") && !next.escaped
           && !hasLineBreak(tok_.end, next.start);
}

bool Parser::isUsingKeyword(bool isAwaitUsing, bool isFor) {
    if (!isContextual(isAwaitUsing ? "await" : "using")) return false;
    const SavedState s = save();
    bool result = false;
    try {
        const std::ptrdiff_t firstEnd = tok_.end;
        lastTokEnd_ = tok_.end;
        lastTokStart_ = tok_.start;
        nextToken();
        if (!hasLineBreak(firstEnd, tok_.start)) {
            bool ok = true;
            if (isAwaitUsing) {
                ok = tok_.type == Tk::Name && tok_.value == JsText(u"using") && !tok_.escaped;
                if (ok) {
                    const std::ptrdiff_t usingEnd = tok_.end;
                    lastTokEnd_ = tok_.end;
                    lastTokStart_ = tok_.start;
                    nextToken();
                    ok = !hasLineBreak(usingEnd, tok_.start);
                }
            }
            if (ok && tok_.type == Tk::Name) {
                if (tok_.escaped) {
                    result = true;
                } else {
                    const JsText& id = tok_.value;
                    result = !(id == JsText(u"in") || id == JsText(u"instanceof") || (isFor && id == JsText(u"of")));
                }
            }
        }
    } catch (const SyntaxFail&) {
        result = false;
    }
    restore(s);
    return result;
}

int Parser::parseStatement(const JsText& context, bool topLevel) {
    static_cast<void>(topLevel);
    DepthGuard guard(*this);
    const std::ptrdiff_t start = tok_.start;

    if (isLet(context)) {
        if (!context.empty()) unexpected();
        return parseVarStatement(JsText(u"let"));
    }

    if (tok_.type == Tk::Name) {
        const JsText v = tok_.value;
        if (isKeyword(v)) {
            if (v == JsText(u"break") || v == JsText(u"continue")) return parseBreakContinueStatement(v == JsText(u"break"));
            if (v == JsText(u"debugger")) {
                next();
                semicolon();
                return -1;
            }
            if (v == JsText(u"do")) return parseDoStatement();
            if (v == JsText(u"for")) return parseForStatement();
            if (v == JsText(u"function")) {
                // A function is the sole body of an if statement or a labelled
                // statement only in sloppy code (Annex B).
                if (!context.empty() && (strict_ || (context != JsText(u"if") && context != JsText(u"label")))) unexpected();
                const bool declarationPosition = context.empty();
                return parseFunctionStatement(false, declarationPosition, context != JsText(u"if"));
            }
            if (v == JsText(u"class")) {
                if (!context.empty()) unexpected();
                return parseClass(start, true);
            }
            if (v == JsText(u"if")) return parseIfStatement();
            if (v == JsText(u"return")) return parseReturnStatement();
            if (v == JsText(u"switch")) return parseSwitchStatement();
            if (v == JsText(u"throw")) return parseThrowStatement();
            if (v == JsText(u"try")) return parseTryStatement();
            if (v == JsText(u"const") || v == JsText(u"var")) {
                if (!context.empty() && v != JsText(u"var")) unexpected();
                return parseVarStatement(v);
            }
            if (v == JsText(u"while")) return parseWhileStatement();
            if (v == JsText(u"with")) return parseWithStatement();
            if (v == JsText(u"export")) raise(tok_.start, JsText(u"'export' may appear only in a module"));
            if (v == JsText(u"import")) {
                const Token nt = peekToken();
                if (nt.type != Tk::ParenL && nt.type != Tk::Dot) raise(tok_.start, JsText(u"'import' may appear only in a module"));
                parseExpression();
                semicolon();
                return -1;
            }
        }
    } else if (tok_.type == Tk::BraceL) {
        parseBlock(true);
        return -1;
    } else if (tok_.type == Tk::Semi) {
        next();
        return -1;
    }

    if (isAsyncFunction()) {
        if (!context.empty()) unexpected();
        next();
        return parseFunctionStatement(true, context.empty(), true);
    }

    // V8 does not recognise `using` declarations in single-statement
    // positions.
    JsText usingKind;
    if (context.empty()) {
        if (isUsingKeyword(true, false)) usingKind = JsText(u"await using");
        else if (isUsingKeyword(false, false)) usingKind = JsText(u"using");
    }
    if (!usingKind.empty()) {
        if (!allowUsing()) raise(tok_.start, JsText(u"Using declaration cannot appear here"));
        if (usingKind == JsText(u"await using")) {
            if (!canAwait()) raise(tok_.start, JsText(u"Await using cannot appear outside of async function"));
            next();
        }
        next();
        parseVar(false, usingKind);
        semicolon();
        return -1;
    }

    const bool startsWithName = isNameTok();
    const JsText maybeName = tok_.value;
    const int expr = parseExpression();
    if (startsWithName && N(expr).kind == NK::Ident && eat(Tk::Colon)) {
        return parseLabeledStatement(maybeName, expr, context, start);
    }
    semicolon();
    return expr;
}

int Parser::parseBreakContinueStatement(bool isBreak) {
    const std::ptrdiff_t start = tok_.start;
    next();
    JsText label;
    bool hasLabel = false;
    if (eat(Tk::Semi) || insertSemicolon()) {
    } else if (!isNameTok()) {
        unexpected();
    } else {
        const int id = parseIdent(false);
        label = N(id).name;
        hasLabel = true;
        semicolon();
    }
    size_t i = 0;
    for (; i < labels_.size(); ++i) {
        const Label& lab = labels_[i];
        if (!hasLabel || lab.name == label) {
            if (lab.kind != LabelKind::None && (isBreak || lab.kind == LabelKind::Loop)) break;
            if (hasLabel && isBreak) break;
        }
    }
    if (i == labels_.size()) raise(start, isBreak ? JsText(u"Unsyntactic break") : JsText(u"Unsyntactic continue"));
    return -1;
}

int Parser::parseDoStatement() {
    next();
    labels_.push_back(Label{JsText(), LabelKind::Loop, -1});
    parseStatement(JsText(u"do"));
    labels_.pop_back();
    if (!isKw("while")) unexpected();
    next();
    expect(Tk::ParenL);
    parseExpression();
    expect(Tk::ParenR);
    eat(Tk::Semi);
    return -1;
}

int Parser::parseForStatement() {
    next();
    std::ptrdiff_t awaitAt = -1;
    if (canAwait() && isContextual("await")) {
        awaitAt = tok_.start;
        next();
    }
    labels_.push_back(Label{JsText(), LabelKind::Loop, -1});
    enterScope(0);
    expect(Tk::ParenL);
    if (tok_.type == Tk::Semi) {
        if (awaitAt > -1) unexpected(awaitAt);
        return parseFor();
    }
    const bool letDecl = isLet(JsText());
    if (isKw("var") || isKw("const") || letDecl) {
        const std::ptrdiff_t initStart = tok_.start;
        const JsText kind = letDecl ? JsText(u"let") : tok_.value;
        next();
        const VarResult init = parseVar(true, kind);
        return parseForAfterInit(init, kind, initStart, awaitAt);
    }
    const bool startsWithLet = isContextual("let");
    JsText usingKind;
    if (isUsingKeyword(false, true)) usingKind = JsText(u"using");
    else if (isUsingKeyword(true, true)) usingKind = JsText(u"await using");
    if (!usingKind.empty()) {
        const std::ptrdiff_t initStart = tok_.start;
        next();
        if (usingKind == JsText(u"await using")) {
            if (!canAwait()) raise(tok_.start, JsText(u"Await using cannot appear outside of async function"));
            next();
        }
        const VarResult init = parseVar(true, usingKind);
        return parseForAfterInit(init, usingKind, initStart, awaitAt);
    }
    const bool containsEsc = tok_.escaped;
    DErr ref;
    const std::ptrdiff_t initPos = tok_.start;
    const int init = awaitAt > -1 ? parseExprSubscripts(&ref, 2) : parseExpression(1, &ref);
    bool isForOf = false;
    if (isKw("in") || (isForOf = isContextual("of"))) {
        if (awaitAt > -1) {
            if (isKw("in")) unexpected(awaitAt);
        } else if (isForOf) {
            if (N(init).start == initPos && !containsEsc && N(init).kind == NK::Ident && N(init).name == JsText(u"async")) unexpected();
        }
        if (startsWithLet && isForOf) raise(N(init).start, JsText(u"The left-hand side of a for-of loop may not start with 'let'."));
        // V8 accepts a call expression here and throws at run time.
        if (N(init).kind != NK::Call) {
            toAssignable(init, false, &ref);
            checkLValPattern(init);
        }
        return parseForIn(nullptr, JsText(), initPos);
    }
    checkExpressionErrors(&ref, true);
    if (awaitAt > -1) unexpected(awaitAt);
    return parseFor();
}

int Parser::parseForAfterInit(const VarResult& init, const JsText& kind, std::ptrdiff_t initStart, std::ptrdiff_t awaitAt) {
    if ((isKw("in") || isContextual("of")) && init.count == 1) {
        if (isKw("in") && awaitAt > -1) unexpected(awaitAt);
        return parseForIn(&init, kind, initStart);
    }
    if (awaitAt > -1) unexpected(awaitAt);
    return parseFor();
}

int Parser::parseFor() {
    expect(Tk::Semi);
    if (tok_.type != Tk::Semi) parseExpression();
    expect(Tk::Semi);
    if (tok_.type != Tk::ParenR) parseExpression();
    expect(Tk::ParenR);
    parseStatement(JsText(u"for"));
    exitScope();
    labels_.pop_back();
    return -1;
}

int Parser::parseForIn(const VarResult* decl, const JsText& kind, std::ptrdiff_t initStart) {
    const bool isForIn = isKw("in");
    // V8: a using declaration cannot head a for-in loop.
    if (isForIn && (kind == JsText(u"using") || kind == JsText(u"await using"))) {
        raise(initStart, JsText(u"Invalid 'using' in for-in loop"));
    }
    next();
    if (decl && decl->firstHasInit
        && (!isForIn || strict_ || kind != JsText(u"var") || !decl->firstIsIdent)) {
        raise(initStart, isForIn ? JsText(u"for-in loop variable declaration may not have an initializer")
                                 : JsText(u"for-of loop variable declaration may not have an initializer"));
    }
    if (isForIn) parseExpression();
    else parseMaybeAssign();
    expect(Tk::ParenR);
    parseStatement(JsText(u"for"));
    exitScope();
    labels_.pop_back();
    return -1;
}

int Parser::parseFunctionStatement(bool isAsync, bool declarationPosition, bool declare) {
    const std::ptrdiff_t start = tok_.start;
    next();
    int flags = FUNC_STATEMENT | (declarationPosition ? 0 : FUNC_HANGING_STATEMENT);
    if (!declare) flags |= FUNC_NO_DECLARE;
    return parseFunction(start, flags, isAsync, 0);
}

int Parser::parseIfStatement() {
    next();
    expect(Tk::ParenL);
    parseExpression();
    expect(Tk::ParenR);
    parseStatement(JsText(u"if"));
    if (eatKw("else")) parseStatement(JsText(u"if"));
    return -1;
}

int Parser::parseReturnStatement() {
    if (!inFunction()) raise(tok_.start, JsText(u"'return' outside of function"));
    next();
    if (eat(Tk::Semi) || insertSemicolon()) return -1;
    parseExpression();
    semicolon();
    return -1;
}

int Parser::parseSwitchStatement() {
    next();
    expect(Tk::ParenL);
    parseExpression();
    expect(Tk::ParenR);
    expect(Tk::BraceL);
    labels_.push_back(Label{JsText(), LabelKind::Switch, -1});
    enterScope(SCOPE_SWITCH);
    bool haveCase = false;
    bool sawDefault = false;
    while (tok_.type != Tk::BraceR) {
        if (isKw("case") || isKw("default")) {
            const bool isCase = isKw("case");
            haveCase = true;
            next();
            if (isCase) {
                parseExpression();
            } else {
                if (sawDefault) raise(lastTokStart_, JsText(u"Multiple default clauses"));
                sawDefault = true;
            }
            expect(Tk::Colon);
        } else {
            if (!haveCase) unexpected();
            parseStatement(JsText());
        }
    }
    exitScope();
    next();
    labels_.pop_back();
    return -1;
}

int Parser::parseThrowStatement() {
    next();
    if (hasLineBreak(lastTokEnd_, tok_.start)) raise(lastTokEnd_, JsText(u"Illegal newline after throw"));
    parseExpression();
    semicolon();
    return -1;
}

int Parser::parseTryStatement() {
    next();
    parseBlock(true);
    bool handler = false;
    if (isKw("catch")) {
        next();
        if (eat(Tk::ParenL)) {
            const int param = parseBindingAtom();
            const bool simple = N(param).kind == NK::Ident;
            enterScope(simple ? SCOPE_SIMPLE_CATCH : 0);
            checkLValPattern(param, simple ? Bind::SimpleCatch : Bind::Lexical);
            expect(Tk::ParenR);
        } else {
            enterScope(0);
        }
        parseBlock(false);
        exitScope();
        handler = true;
    }
    const bool finalizer = eatKw("finally");
    if (finalizer) parseBlock(true);
    if (!handler && !finalizer) raise(tok_.start, JsText(u"Missing catch or finally clause"));
    return -1;
}

int Parser::parseVarStatement(const JsText& kind) {
    next();
    parseVar(false, kind);
    semicolon();
    return -1;
}

int Parser::parseWhileStatement() {
    next();
    expect(Tk::ParenL);
    parseExpression();
    expect(Tk::ParenR);
    labels_.push_back(Label{JsText(), LabelKind::Loop, -1});
    parseStatement(JsText(u"while"));
    labels_.pop_back();
    return -1;
}

int Parser::parseWithStatement() {
    if (strict_) raise(tok_.start, JsText(u"'with' in strict mode"));
    next();
    expect(Tk::ParenL);
    parseExpression();
    expect(Tk::ParenR);
    parseStatement(JsText(u"with"));
    return -1;
}

int Parser::parseLabeledStatement(const JsText& maybeName, int expr, const JsText& context, std::ptrdiff_t nodeStart) {
    for (const Label& label : labels_) {
        if (label.name == maybeName) raise(N(expr).start, JsText(u"Label '%1' is already declared").arg(maybeName));
    }
    LabelKind kind = LabelKind::None;
    if (isKw("do") || isKw("for") || isKw("while")) kind = LabelKind::Loop;
    else if (isKw("switch")) kind = LabelKind::Switch;
    for (size_t i = labels_.size(); i-- > 0;) {
        Label& label = labels_[i];
        if (label.statementStart == nodeStart) {
            label.statementStart = tok_.start;
            label.kind = kind;
        } else {
            break;
        }
    }
    labels_.push_back(Label{maybeName, kind, tok_.start});
    JsText inner = JsText(u"label");
    if (!context.empty()) inner = context.contains(JsText(u"label")) ? context : JsText(context + JsText(u"label"));
    parseStatement(inner);
    labels_.pop_back();
    return -1;
}

void Parser::parseBlock(bool createNewLexicalScope) {
    expect(Tk::BraceL);
    if (createNewLexicalScope) enterScope(0);
    while (tok_.type != Tk::BraceR) parseStatement(JsText());
    next();
    if (createNewLexicalScope) exitScope();
}

VarResult Parser::parseVar(bool isFor, const JsText& kind, bool allowMissingInitializer) {
    VarResult result;
    const bool isUsing = kind == JsText(u"using") || kind == JsText(u"await using");
    for (;;) {
        const int id = isUsing ? parseIdent(false) : parseBindingAtom();
        checkLValPattern(id, kind == JsText(u"var") ? Bind::Var : Bind::Lexical);
        bool hasInit = false;
        if (eat(Tk::Eq)) {
            parseMaybeAssign(isFor ? 1 : 0);
            hasInit = true;
        } else if (!allowMissingInitializer && kind == JsText(u"const") && !(isFor && (isKw("in") || isContextual("of")))) {
            // V8 (unlike acorn) exempts `in`/`of` only in a for head.
            unexpected();
        } else if (!allowMissingInitializer && isUsing && !(isFor && (isKw("in") || isContextual("of")))) {
            raise(lastTokEnd_, JsText(u"Missing initializer in %1 declaration").arg(kind));
        } else if (!allowMissingInitializer && N(id).kind != NK::Ident && !(isFor && (isKw("in") || isContextual("of")))) {
            raise(lastTokEnd_, JsText(u"Complex binding patterns require an initialization value"));
        }
        if (result.count == 0) {
            result.firstHasInit = hasInit;
            result.firstIsIdent = N(id).kind == NK::Ident;
        }
        result.count++;
        if (!eat(Tk::Comma)) break;
    }
    return result;
}

int Parser::parseFunction(std::ptrdiff_t start, int statement, bool isAsync, int forInit) {
    const int fn = nodeOf(NK::Function, start);
    if (tok_.type == Tk::Star && (statement & FUNC_HANGING_STATEMENT)) unexpected();
    const bool generator = eat(Tk::Star);
    int id = -1;
    if (statement & FUNC_STATEMENT) {
        id = ((statement & FUNC_NULLABLE_ID) && !isNameTok()) ? -1 : parseIdent(false);
        if (id >= 0 && !(statement & FUNC_NO_DECLARE)) {
            checkLValSimple(id, (strict_ || generator || isAsync) ? (treatFunctionsAsVar() ? Bind::Var : Bind::Lexical) : Bind::Function);
        }
    }
    const std::ptrdiff_t oldYieldPos = yieldPos_, oldAwaitPos = awaitPos_, oldAwaitIdentPos = awaitIdentPos_;
    yieldPos_ = awaitPos_ = awaitIdentPos_ = -1;
    enterScope(functionFlags(isAsync, generator));
    if (!(statement & FUNC_STATEMENT)) id = isNameTok() ? parseIdent(false) : -1;
    std::vector<int> params;
    parseFunctionParams(params);
    parseFunctionBody(params, false, false, forInit, id);
    yieldPos_ = oldYieldPos;
    awaitPos_ = oldAwaitPos;
    awaitIdentPos_ = oldAwaitIdentPos;
    N(fn).list = params;
    return finish(fn);
}

void Parser::parseFunctionParams(std::vector<int>& params) {
    expect(Tk::ParenL);
    params = parseBindingList(Tk::ParenR, false, true);
    checkYieldAwaitInDefaultParams();
}

bool Parser::checkKeyName(int element, const char* name) const {
    const Node& e = N(element);
    if (e.computed) return false;
    const Node& key = N(e.a);
    return (key.kind == NK::Ident && key.name == JsText::fromLatin1(name)) || (key.kind == NK::StringLit && key.name == JsText::fromLatin1(name));
}

bool Parser::isClassElementNameStart() const {
    return tok_.type == Tk::Name || tok_.type == Tk::PrivateId || tok_.type == Tk::Num || tok_.type == Tk::String
           || tok_.type == Tk::BracketL;
}

int Parser::parseClass(std::ptrdiff_t start, bool isStatement) {
    const int cls = nodeOf(NK::Class, start);
    next();
    const bool oldStrict = strict_;
    strict_ = true;
    if (isNameTok()) {
        const int id = parseIdent(false);
        if (isStatement) checkLValSimple(id, Bind::Lexical);
    } else if (isStatement) {
        unexpected();
    }
    bool hasSuper = false;
    if (eatKw("extends")) {
        parseExprSubscripts(nullptr, 0);
        hasSuper = true;
    }
    privateNames_.push_back(PrivateScope{});
    std::unordered_map<JsText, JsText> privateNameMap;
    bool hadConstructor = false;
    expect(Tk::BraceL);
    while (tok_.type != Tk::BraceR) parseClassElement(hasSuper, privateNameMap, hadConstructor);
    strict_ = oldStrict;
    next();
    exitClassBody();
    return finish(cls);
}

void Parser::parseClassElement(bool constructorAllowsSuper, std::unordered_map<JsText, JsText>& privateNameMap, bool& hadConstructor) {
    DepthGuard guard(*this);
    if (eat(Tk::Semi)) return;
    const int element = nodeOf(NK::Property, tok_.start);
    JsText keyName;
    bool isGenerator = false;
    bool isAsync = false;
    JsText kind = JsText(u"method");
    bool isStatic = false;

    if (eatContextual("static")) {
        if (eat(Tk::BraceL)) {
            parseClassStaticBlock();
            return;
        }
        if (isClassElementNameStart() || tok_.type == Tk::Star) isStatic = true;
        else keyName = JsText(u"static");
    }
    if (keyName.empty() && eatContextual("async")) {
        if ((isClassElementNameStart() || tok_.type == Tk::Star) && !canInsertSemicolon()) isAsync = true;
        else keyName = JsText(u"async");
    }
    if (keyName.empty() && eat(Tk::Star)) isGenerator = true;
    if (keyName.empty() && !isAsync && !isGenerator) {
        const JsText lastValue = tok_.value;
        if (eatContextual("get") || eatContextual("set")) {
            if (isClassElementNameStart()) kind = lastValue;
            else keyName = lastValue;
        }
    }

    if (!keyName.empty()) {
        N(element).computed = false;
        const int key = nodeOf(NK::Ident, lastTokStart_);
        N(key).name = keyName;
        N(key).end = lastTokEnd_;
        N(element).a = key;
    } else if (tok_.type == Tk::PrivateId) {
        if (tok_.value == JsText(u"constructor")) raise(tok_.start, JsText(u"Classes can't have an element named '#constructor'"));
        N(element).computed = false;
        N(element).a = parsePrivateIdent();
    } else {
        parsePropertyName(element);
    }

    const int key = N(element).a;
    bool isMethod = false;
    JsText methodKind;
    if (tok_.type == Tk::ParenL || kind != JsText(u"method") || isGenerator || isAsync) {
        const bool isConstructor = !isStatic && checkKeyName(element, "constructor");
        const bool allowsDirectSuper = isConstructor && constructorAllowsSuper;
        if (isConstructor && kind != JsText(u"method")) raise(N(key).start, JsText(u"Constructor can't have get/set modifier"));
        methodKind = isConstructor ? JsText(u"constructor") : kind;
        if (methodKind == JsText(u"constructor")) {
            if (isGenerator) raise(N(key).start, JsText(u"Constructor can't be a generator"));
            if (isAsync) raise(N(key).start, JsText(u"Constructor can't be an async method"));
        } else if (isStatic && checkKeyName(element, "prototype")) {
            raise(N(key).start, JsText(u"Classes may not have a static property named prototype"));
        }
        const int value = parseMethod(isGenerator, isAsync, allowsDirectSuper);
        const std::vector<int>& params = N(value).list;
        if (methodKind == JsText(u"get") && !params.empty()) raise(N(value).start, JsText(u"getter should have no params"));
        if (methodKind == JsText(u"set") && params.size() != 1) raise(N(value).start, JsText(u"setter should have exactly one param"));
        if (methodKind == JsText(u"set") && N(params[0]).kind == NK::Rest) raise(N(params[0]).start, JsText(u"Setter cannot use rest params"));
        isMethod = true;
    } else {
        if (checkKeyName(element, "constructor")) raise(N(key).start, JsText(u"Classes can't have a field named 'constructor'"));
        if (isStatic && checkKeyName(element, "prototype")) raise(N(key).start, JsText(u"Classes can't have a static field named 'prototype'"));
        if (eat(Tk::Eq)) {
            enterScope(SCOPE_CLASS_FIELD_INIT | SCOPE_SUPER);
            parseMaybeAssign();
            exitScope();
        }
        semicolon();
    }

    if (isMethod && methodKind == JsText(u"constructor")) {
        if (hadConstructor) raise(N(element).start, JsText(u"Duplicate constructor in the same class"));
        hadConstructor = true;
    } else if (N(key).kind == NK::PrivateName) {
        const JsText& name = N(key).name;
        const JsText curr = mapValue(privateNameMap, name);
        JsText nextKind = JsText(u"true");
        if (isMethod && (methodKind == JsText(u"get") || methodKind == JsText(u"set"))) {
            nextKind = (isStatic ? JsText(u"s") : JsText(u"i")) + methodKind;
        }
        if ((curr == JsText(u"iget") && nextKind == JsText(u"iset"))
            || (curr == JsText(u"iset") && nextKind == JsText(u"iget"))
            || (curr == JsText(u"sget") && nextKind == JsText(u"sset"))
            || (curr == JsText(u"sset") && nextKind == JsText(u"sget"))) {
            privateNameMap.insert_or_assign(name, JsText(u"true"));
        } else if (curr.empty()) {
            privateNameMap.insert_or_assign(name, nextKind);
        } else {
            raise(N(key).start, JsText(u"Identifier '#%1' has already been declared").arg(name));
        }
        privateNames_.back().declared.insert_or_assign(name, JsText(u"true"));
    }
}

void Parser::parseClassStaticBlock() {
    const std::vector<Label> oldLabels = labels_;
    labels_.clear();
    enterScope(SCOPE_CLASS_STATIC_BLOCK | SCOPE_SUPER);
    while (tok_.type != Tk::BraceR) parseStatement(JsText());
    next();
    exitScope();
    labels_ = oldLabels;
}

void Parser::exitClassBody() {
    PrivateScope scope = privateNames_.back();
    privateNames_.pop_back();
    for (const auto& used : scope.used) {
        if (!scope.declared.contains(used.first)) {
            if (!privateNames_.empty()) {
                privateNames_.back().used.push_back(used);
            } else {
                raise(used.second, JsText(u"Private field '#%1' must be declared in an enclosing class").arg(used.first));
            }
        }
    }
}

// ============================================================== expressions

int Parser::parseExpression(int forInit, DErr* ref) {
    const std::ptrdiff_t start = tok_.start;
    const int expr = parseMaybeAssign(forInit, ref);
    if (tok_.type == Tk::Comma) {
        const int seq = nodeOf(NK::Sequence, start);
        N(seq).list.push_back(expr);
        while (eat(Tk::Comma)) {
            const int e = parseMaybeAssign(forInit, ref);
            N(seq).list.push_back(e);
        }
        return finish(seq);
    }
    return expr;
}

int Parser::parseMaybeAssign(int forInit, DErr* ref) {
    DepthGuard guard(*this);
    if (isContextual("yield") && inGenerator()) return parseYield(forInit);

    bool ownDestructuringErrors = false;
    std::ptrdiff_t oldParenAssign = -1, oldTrailingComma = -1, oldDoubleProto = -1;
    DErr own;
    if (ref) {
        oldParenAssign = ref->parenthesizedAssign;
        oldTrailingComma = ref->trailingComma;
        oldDoubleProto = ref->doubleProto;
        ref->parenthesizedAssign = ref->trailingComma = -1;
    } else {
        ref = &own;
        ownDestructuringErrors = true;
    }

    const std::ptrdiff_t startPos = tok_.start;
    if (tok_.type == Tk::ParenL || isNameTok()) {
        potentialArrowAt_ = tok_.start;
        potentialArrowInForAwait_ = forInit == 2;
    }
    int left = parseMaybeConditional(forInit, ref);
    if (tok_.type == Tk::Eq || tok_.type == Tk::Assign) {
        const bool isEq = tok_.type == Tk::Eq;
        const JsText op = tok_.value;
        const int node = nodeOf(NK::Assign, startPos);
        N(node).name = op;
        // V8 accepts a call expression as the target of `=` and of compound
        // assignment (not logical assignment) and throws at run time.
        const bool callTarget = N(left).kind == NK::Call && op != JsText(u"&&=") && op != JsText(u"||=")
                                && op != JsText(u"?\?=");
        if (isEq && !callTarget) left = toAssignable(left, false, ref);
        if (!ownDestructuringErrors) ref->parenthesizedAssign = ref->trailingComma = ref->doubleProto = -1;
        if (ref->shorthandAssign >= N(left).start) ref->shorthandAssign = -1;
        if (!callTarget) {
            if (isEq) checkLValPattern(left);
            else checkLValSimple(left);
        }
        N(node).a = left;
        next();
        N(node).b = parseMaybeAssign(forInit);
        if (oldDoubleProto > -1) ref->doubleProto = oldDoubleProto;
        return finish(node);
    }
    if (ownDestructuringErrors) checkExpressionErrors(ref, true);
    if (oldParenAssign > -1) ref->parenthesizedAssign = oldParenAssign;
    if (oldTrailingComma > -1) ref->trailingComma = oldTrailingComma;
    return left;
}

int Parser::parseMaybeConditional(int forInit, DErr* ref) {
    const std::ptrdiff_t startPos = tok_.start;
    const int expr = parseExprOps(forInit, ref);
    if (checkExpressionErrors(ref)) return expr;
    if (eat(Tk::Question)) {
        const int node = nodeOf(NK::Conditional, startPos);
        N(node).a = expr;
        parseMaybeAssign();
        expect(Tk::Colon);
        N(node).b = parseMaybeAssign(forInit);
        return finish(node);
    }
    return expr;
}

int Parser::parseExprOps(int forInit, DErr* ref) {
    const std::ptrdiff_t startPos = tok_.start;
    const int expr = parseMaybeUnary(ref, false, false, forInit);
    if (checkExpressionErrors(ref)) return expr;
    if (N(expr).start == startPos && N(expr).kind == NK::Arrow) return expr;
    return parseExprOp(expr, startPos, -1, forInit);
}

int Parser::binopPrec() const {
    switch (tok_.type) {
        case Tk::LogicalOR: case Tk::Coalesce: return 1;
        case Tk::LogicalAND: return 2;
        case Tk::BitwiseOR: return 3;
        case Tk::BitwiseXOR: return 4;
        case Tk::BitwiseAND: return 5;
        case Tk::Equality: return 6;
        case Tk::Relational: return 7;
        case Tk::BitShift: return 8;
        case Tk::PlusMin: return 9;
        case Tk::Modulo: case Tk::Star: case Tk::Slash: return 10;
        case Tk::Name:
            if (tok_.value == JsText(u"in") || tok_.value == JsText(u"instanceof")) return 7;
            return -1;
        default:
            return -1;
    }
}

// Operator-precedence parsing (acorn parseExprOp), with the tail recursion
// over a left-associative chain written as a loop so that a long chain
// (`a + b + c + ...`) does not deepen the stack.
int Parser::parseExprOp(int left, std::ptrdiff_t leftStart, int minPrec, int forInit) {
    DepthGuard guard(*this);
    for (;;) {
        int prec = binopPrec();
        if (prec < 0 || (forInit && isKw("in")) || prec <= minPrec) return left;
        const bool logical = tok_.type == Tk::LogicalOR || tok_.type == Tk::LogicalAND;
        const bool coalesce = tok_.type == Tk::Coalesce;
        if (coalesce) prec = 2; // the precedence range of the logical operators
        const JsText op = tok_.value;
        next();
        const std::ptrdiff_t startPos = tok_.start;
        const int right = parseExprOp(parseMaybeUnary(nullptr, false, false, forInit), startPos, prec, forInit);
        left = buildBinary(leftStart, left, right, op, logical || coalesce);
        if ((logical && tok_.type == Tk::Coalesce) || (coalesce && (tok_.type == Tk::LogicalOR || tok_.type == Tk::LogicalAND))) {
            raise(tok_.start, JsText(u"Logical expressions and coalesce expressions cannot be mixed. Wrap either by parentheses"));
        }
    }
}

int Parser::buildBinary(std::ptrdiff_t start, int left, int right, const JsText& op, bool logical) {
    if (N(right).kind == NK::PrivateName) raise(N(right).start, JsText(u"Private identifier can only be left side of binary expression"));
    const int node = nodeOf(logical ? NK::Logical : NK::Binary, start);
    N(node).a = left;
    N(node).b = right;
    N(node).name = op;
    return finish(node);
}

int Parser::parseMaybeUnary(DErr* ref, bool sawUnary, bool incDec, int forInit) {
    DepthGuard guard(*this);
    const std::ptrdiff_t startPos = tok_.start;
    int expr = -1;
    const bool isPrefixKeyword = tok_.type == Tk::Name
                                 && (tok_.value == JsText(u"typeof") || tok_.value == JsText(u"void")
                                     || tok_.value == JsText(u"delete"));
    if (isContextual("await") && canAwait()) {
        expr = parseAwait(forInit);
        sawUnary = true;
    } else if (tok_.type == Tk::IncDec || tok_.type == Tk::Prefix || tok_.type == Tk::PlusMin || isPrefixKeyword) {
        const bool update = tok_.type == Tk::IncDec;
        const int node = nodeOf(update ? NK::Update : NK::Unary, startPos);
        N(node).name = tok_.value;
        next();
        N(node).a = parseMaybeUnary(nullptr, true, update, forInit);
        checkExpressionErrors(ref, true);
        const int arg = N(node).a;
        if (update) {
            checkLValSimple(arg, Bind::None, nullptr, true);
        } else if (strict_ && N(node).name == JsText(u"delete") && N(arg).kind == NK::Ident) {
            raise(N(node).start, JsText(u"Deleting local variable in strict mode"));
        } else if (N(node).name == JsText(u"delete") && isPrivateFieldAccess(arg)) {
            raise(N(node).start, JsText(u"Private fields can not be deleted"));
        } else {
            sawUnary = true;
        }
        expr = finish(node);
    } else if (!sawUnary && tok_.type == Tk::PrivateId) {
        if (forInit || privateNames_.empty()) unexpected();
        expr = parsePrivateIdent();
        if (!isKw("in")) unexpected();
    } else {
        expr = parseExprSubscripts(ref, forInit);
        if (checkExpressionErrors(ref)) return expr;
        while (tok_.type == Tk::IncDec && !canInsertSemicolon()) {
            const int node = nodeOf(NK::Update, startPos);
            N(node).name = tok_.value;
            N(node).a = expr;
            checkLValSimple(expr, Bind::None, nullptr, true);
            next();
            expr = finish(node);
        }
    }

    if (!incDec && eat(Tk::StarStar)) {
        if (sawUnary) unexpected(lastTokStart_);
        return buildBinary(startPos, expr, parseMaybeUnary(nullptr, false, false, forInit), JsText(u"**"), false);
    }
    return expr;
}

int Parser::parseExprSubscripts(DErr* ref, int forInit) {
    const std::ptrdiff_t startPos = tok_.start;
    const int expr = parseExprAtom(ref, forInit);
    if (N(expr).kind == NK::Arrow && src_.mid(lastTokStart_, lastTokEnd_ - lastTokStart_) != JsText(u")")) return expr;
    const int result = parseSubscripts(expr, startPos, false, forInit);
    if (ref && N(result).kind == NK::Member) {
        if (ref->parenthesizedAssign >= N(result).start) ref->parenthesizedAssign = -1;
        if (ref->parenthesizedBind >= N(result).start) ref->parenthesizedBind = -1;
        if (ref->trailingComma >= N(result).start) ref->trailingComma = -1;
    }
    return result;
}

int Parser::parseSubscripts(int base, std::ptrdiff_t start, bool noCalls, int forInit) {
    const Node& b = N(base);
    const bool maybeAsyncArrow = b.kind == NK::Ident && b.name == JsText(u"async") && lastTokEnd_ == b.end
                                 && !canInsertSemicolon() && b.end - b.start == 5 && potentialArrowAt_ == b.start;
    bool optionalChained = false;
    for (;;) {
        DepthGuard guard(*this);
        const int element = parseSubscript(base, start, noCalls, maybeAsyncArrow, optionalChained, forInit);
        if (N(element).optional) optionalChained = true;
        if (element == base || N(element).kind == NK::Arrow) {
            if (optionalChained) {
                const int chain = nodeOf(NK::Chain, start);
                N(chain).a = element;
                return finish(chain);
            }
            return element;
        }
        base = element;
    }
}

int Parser::parseSubscript(int base, std::ptrdiff_t start, bool noCalls, bool maybeAsyncArrow, bool optionalChained, int forInit) {
    const bool optional = eat(Tk::QuestionDot);
    if (noCalls && optional) raise(lastTokStart_, JsText(u"Optional chaining cannot appear in the callee of new expressions"));
    const bool computed = eat(Tk::BracketL);
    if (computed || (optional && tok_.type != Tk::ParenL && tok_.type != Tk::Template) || eat(Tk::Dot)) {
        const int node = nodeOf(NK::Member, start);
        N(node).a = base;
        if (computed) {
            N(node).b = parseExpression();
            expect(Tk::BracketR);
        } else if (tok_.type == Tk::PrivateId && N(base).kind != NK::Super) {
            N(node).b = parsePrivateIdent();
        } else {
            N(node).b = parseIdent(true);
        }
        N(node).computed = computed;
        N(node).optional = optional;
        return finish(node);
    }
    if (!noCalls && eat(Tk::ParenL)) {
        DErr ref;
        const std::ptrdiff_t oldYieldPos = yieldPos_, oldAwaitPos = awaitPos_, oldAwaitIdentPos = awaitIdentPos_;
        yieldPos_ = awaitPos_ = awaitIdentPos_ = -1;
        std::vector<int> exprList = parseExprList(Tk::ParenR, true, false, &ref);
        if (maybeAsyncArrow && !optional && !canInsertSemicolon() && eat(Tk::Arrow)) {
            checkPatternErrors(&ref, false);
            checkYieldAwaitInDefaultParams();
            if (awaitIdentPos_ > -1) raise(awaitIdentPos_, JsText(u"Cannot use 'await' as identifier inside an async function"));
            yieldPos_ = oldYieldPos;
            awaitPos_ = oldAwaitPos;
            awaitIdentPos_ = oldAwaitIdentPos;
            return parseArrowExpression(start, exprList, true, forInit);
        }
        checkExpressionErrors(&ref, true);
        if (oldYieldPos > -1) yieldPos_ = oldYieldPos;
        if (oldAwaitPos > -1) awaitPos_ = oldAwaitPos;
        if (oldAwaitIdentPos > -1) awaitIdentPos_ = oldAwaitIdentPos;
        const int node = nodeOf(NK::Call, start);
        N(node).a = base;
        N(node).list = exprList;
        N(node).optional = optional;
        return finish(node);
    }
    if (tok_.type == Tk::Template) {
        if (optional || optionalChained) raise(tok_.start, JsText(u"Optional chaining cannot appear in the tag of tagged template expressions"));
        const int node = nodeOf(NK::TaggedTemplate, start);
        N(node).a = base;
        parseTemplate(true);
        return finish(node);
    }
    return base;
}

int Parser::parseExprAtom(DErr* ref, int forInit, bool forNew) {
    DepthGuard guard(*this);
    if (tok_.type == Tk::Slash || (tok_.type == Tk::Assign && tok_.value == JsText(u"/="))) readRegexp();
    const bool canBeArrow = potentialArrowAt_ == tok_.start;
    const std::ptrdiff_t start = tok_.start;
    switch (tok_.type) {
        case Tk::Name: {
            const JsText v = tok_.value;
            if (v == JsText(u"super")) {
                if (!allowSuper()) raise(tok_.start, JsText(u"'super' keyword outside a method"));
                const int node = nodeOf(NK::Super, start);
                next();
                if (tok_.type == Tk::ParenL && !allowDirectSuper()) raise(start, JsText(u"super() call outside constructor of a subclass"));
                if (tok_.type != Tk::Dot && tok_.type != Tk::BracketL && tok_.type != Tk::ParenL) unexpected();
                return finish(node);
            }
            if (v == JsText(u"this")) {
                const int node = nodeOf(NK::This, start);
                next();
                return finish(node);
            }
            if (v == JsText(u"null") || v == JsText(u"true") || v == JsText(u"false")) {
                const int node = nodeOf(NK::Literal, start);
                next();
                return finish(node);
            }
            if (v == JsText(u"function")) {
                next();
                return parseFunction(start, 0, false, 0);
            }
            if (v == JsText(u"class")) return parseClass(start, false);
            if (v == JsText(u"new")) return parseNew();
            if (v == JsText(u"import")) return parseExprImport(forNew);
            if (isKeyword(v)) unexpected();
            const bool containsEsc = tok_.escaped;
            int id = parseIdent(false);
            if (!containsEsc && N(id).name == JsText(u"async") && !canInsertSemicolon() && isKw("function")) {
                next();
                return parseFunction(start, 0, true, forInit);
            }
            if (canBeArrow && !canInsertSemicolon()) {
                if (eat(Tk::Arrow)) return parseArrowExpression(start, {id}, false, forInit);
                if (N(id).name == JsText(u"async") && isNameTok() && !containsEsc
                    && (!potentialArrowInForAwait_ || tok_.value != JsText(u"of") || tok_.escaped)) {
                    id = parseIdent(false);
                    if (canInsertSemicolon() || !eat(Tk::Arrow)) unexpected();
                    return parseArrowExpression(start, {id}, true, forInit);
                }
            }
            return id;
        }
        case Tk::Regexp: {
            const int node = nodeOf(NK::Regex, start);
            next();
            return finish(node);
        }
        case Tk::Num:
        case Tk::String:
            return parseLiteralToken();
        case Tk::ParenL: {
            const int expr = parseParenAndDistinguishExpression(canBeArrow, forInit);
            if (ref) {
                if (ref->parenthesizedAssign < 0 && !isSimpleAssignTarget(expr)) ref->parenthesizedAssign = start;
                if (ref->parenthesizedBind < 0) ref->parenthesizedBind = start;
            }
            return expr;
        }
        case Tk::BracketL: {
            const int node = nodeOf(NK::Array, start);
            next();
            N(node).list = parseExprList(Tk::BracketR, true, true, ref);
            return finish(node);
        }
        case Tk::BraceL:
            return parseObj(false, ref);
        case Tk::Template:
            return parseTemplate(false);
        default:
            unexpected();
    }
}

int Parser::parseExprImport(bool forNew) {
    const std::ptrdiff_t start = tok_.start;
    if (tok_.escaped) raise(tok_.start, JsText(u"Escape sequence in keyword import"));
    next();
    if (tok_.type == Tk::ParenL && !forNew) {
        const int node = nodeOf(NK::ImportCall, start);
        next();
        parseMaybeAssign();
        if (!eat(Tk::ParenR)) {
            expect(Tk::Comma);
            if (!afterTrailingComma(Tk::ParenR)) {
                parseMaybeAssign();
                if (!eat(Tk::ParenR)) {
                    expect(Tk::Comma);
                    if (!afterTrailingComma(Tk::ParenR)) unexpected();
                }
            }
        }
        return finish(node);
    }
    if (tok_.type == Tk::Dot) {
        next();
        const bool containsEsc = tok_.escaped;
        const int prop = parseIdent(true);
        if (N(prop).name == JsText(u"source") && !containsEsc) {
            // Source phase import: `import.source(specifier)`, one argument.
            if (forNew) raise(start, JsText(u"Cannot use new with import"));
            const int node = nodeOf(NK::ImportCall, start);
            expect(Tk::ParenL);
            parseMaybeAssign();
            expect(Tk::ParenR);
            return finish(node);
        }
        if (N(prop).name != JsText(u"meta")) raise(N(prop).start, JsText(u"The only valid meta property for import is 'import.meta'"));
        raise(start, JsText(u"Cannot use 'import.meta' outside a module"));
    }
    unexpected();
}

int Parser::parseLiteralToken() {
    const int node = nodeOf(tok_.type == Tk::String ? NK::StringLit : NK::Literal, tok_.start);
    if (strict_ && tok_.octal) {
        raise(tok_.start, tok_.type == Tk::String ? JsText(u"Octal escape sequences are not allowed in strict mode")
                                                  : JsText(u"Octal literals are not allowed in strict mode"));
    }
    if (tok_.type == Tk::String) N(node).name = tok_.value;
    next();
    return finish(node);
}

int Parser::parseParenAndDistinguishExpression(bool canBeArrow, int forInit) {
    const std::ptrdiff_t startPos = tok_.start;
    next();
    const std::ptrdiff_t innerStartPos = tok_.start;
    std::vector<int> exprList;
    bool first = true;
    bool lastIsComma = false;
    DErr ref;
    const std::ptrdiff_t oldYieldPos = yieldPos_, oldAwaitPos = awaitPos_;
    std::ptrdiff_t spreadStart = -1;
    yieldPos_ = awaitPos_ = -1;
    while (tok_.type != Tk::ParenR) {
        if (first) first = false;
        else expect(Tk::Comma);
        if (afterTrailingComma(Tk::ParenR, true)) {
            lastIsComma = true;
            break;
        } else if (tok_.type == Tk::Ellipsis) {
            spreadStart = tok_.start;
            exprList.push_back(parseRestBinding());
            if (tok_.type == Tk::Comma) raise(tok_.start, JsText(u"Comma is not permitted after the rest element"));
            break;
        } else {
            exprList.push_back(parseMaybeAssign(0, &ref));
        }
    }
    const std::ptrdiff_t innerEndPos = lastTokEnd_;
    expect(Tk::ParenR);

    if (canBeArrow && !canInsertSemicolon() && eat(Tk::Arrow)) {
        checkPatternErrors(&ref, false);
        checkYieldAwaitInDefaultParams();
        yieldPos_ = oldYieldPos;
        awaitPos_ = oldAwaitPos;
        return parseArrowExpression(startPos, exprList, false, forInit);
    }
    if (exprList.empty() || lastIsComma) unexpected(lastTokStart_);
    if (spreadStart > -1) unexpected(spreadStart);
    checkExpressionErrors(&ref, true);
    if (oldYieldPos > -1) yieldPos_ = oldYieldPos;
    if (oldAwaitPos > -1) awaitPos_ = oldAwaitPos;
    if (exprList.size() > 1) {
        const int seq = nodeOf(NK::Sequence, innerStartPos);
        N(seq).list = exprList;
        N(seq).end = innerEndPos;
        return seq;
    }
    return exprList[0];
}

int Parser::parseNew() {
    const std::ptrdiff_t start = tok_.start;
    if (tok_.escaped) raise(tok_.start, JsText(u"Escape sequence in keyword new"));
    next();
    if (tok_.type == Tk::Dot) {
        const int node = nodeOf(NK::Meta, start);
        next();
        const bool containsEsc = tok_.escaped;
        const int prop = parseIdent(true);
        if (N(prop).name != JsText(u"target")) raise(N(prop).start, JsText(u"The only valid meta property for new is 'new.target'"));
        if (containsEsc) raise(start, JsText(u"'new.target' must not contain escaped characters"));
        if (!allowNewDotTarget()) raise(start, JsText(u"'new.target' can only be used in functions and class static block"));
        return finish(node);
    }
    const int node = nodeOf(NK::New, start);
    const std::ptrdiff_t calleeStart = tok_.start;
    N(node).a = parseSubscripts(parseExprAtom(nullptr, 0, true), calleeStart, true, 0);
    if (eat(Tk::ParenL)) N(node).list = parseExprList(Tk::ParenR, true, false, nullptr);
    return finish(node);
}

// The current token is the first chunk (from the backtick).
int Parser::parseTemplate(bool isTagged) {
    const int node = nodeOf(NK::Template, tok_.start);
    for (;;) {
        if (tok_.type != Tk::Template) unexpected();
        if (tok_.badEscape && !isTagged) raise(tok_.start, JsText(u"Bad escape sequence in untagged template literal"));
        if (tok_.tail) {
            next();
            return finish(node);
        }
        next();
        parseExpression();
        if (tok_.type != Tk::BraceR) unexpected();
        readTemplateChunk(tok_.start + 1);
    }
}

int Parser::parseObj(bool isPattern, DErr* ref) {
    DepthGuard guard(*this);
    const int node = nodeOf(isPattern ? NK::ObjectPattern : NK::Object, tok_.start);
    bool first = true;
    bool hasProto = false;
    next();
    while (!eat(Tk::BraceR)) {
        if (!first) {
            expect(Tk::Comma);
            if (afterTrailingComma(Tk::BraceR)) break;
        } else {
            first = false;
        }
        const int prop = parseProperty(isPattern, ref);
        if (!isPattern) checkPropClash(prop, hasProto, ref);
        N(node).list.push_back(prop);
    }
    return finish(node);
}

int Parser::parseProperty(bool isPattern, DErr* ref) {
    const std::ptrdiff_t start = tok_.start;
    if (eat(Tk::Ellipsis)) {
        if (isPattern) {
            const int rest = nodeOf(NK::Rest, start);
            N(rest).a = parseIdent(false);
            if (tok_.type == Tk::Comma) raise(tok_.start, JsText(u"Comma is not permitted after the rest element"));
            return finish(rest);
        }
        const int spread = nodeOf(NK::Spread, start);
        N(spread).a = parseMaybeAssign(0, ref);
        if (tok_.type == Tk::Comma && ref && ref->trailingComma < 0) ref->trailingComma = tok_.start;
        return finish(spread);
    }
    const int prop = nodeOf(NK::Property, start);
    bool isGenerator = false;
    bool isAsync = false;
    const std::ptrdiff_t startPos = tok_.start;
    if (!isPattern) isGenerator = eat(Tk::Star);
    const bool containsEsc = tok_.escaped;
    parsePropertyName(prop);
    const Node& key = N(N(prop).a);
    if (!isPattern && !containsEsc && !isGenerator && !N(prop).computed && key.kind == NK::Ident
        && key.name == JsText(u"async")
        && (tok_.type == Tk::Name || tok_.type == Tk::Num || tok_.type == Tk::String || tok_.type == Tk::BracketL || tok_.type == Tk::Star)
        && !hasLineBreak(lastTokEnd_, tok_.start)) {
        isAsync = true;
        isGenerator = eat(Tk::Star);
        parsePropertyName(prop);
    }
    parsePropertyValue(prop, isPattern, isGenerator, isAsync, startPos, ref, containsEsc);
    return finish(prop);
}

void Parser::parsePropertyName(int prop) {
    if (eat(Tk::BracketL)) {
        N(prop).computed = true;
        N(prop).a = parseMaybeAssign();
        expect(Tk::BracketR);
        return;
    }
    N(prop).computed = false;
    if (tok_.type == Tk::Num || tok_.type == Tk::String) N(prop).a = parseLiteralToken();
    else N(prop).a = parseIdent(true);
}

void Parser::parsePropertyValue(int prop, bool isPattern, bool isGenerator, bool isAsync, std::ptrdiff_t startPos, DErr* ref, bool containsEsc) {
    if ((isGenerator || isAsync) && tok_.type == Tk::Colon) unexpected();
    const int key = N(prop).a;
    if (eat(Tk::Colon)) {
        N(prop).b = isPattern ? parseMaybeDefault(tok_.start) : parseMaybeAssign(0, ref);
        N(prop).name = JsText(u"init");
    } else if (tok_.type == Tk::ParenL) {
        if (isPattern) unexpected();
        N(prop).method = true;
        N(prop).b = parseMethod(isGenerator, isAsync, false);
        N(prop).name = JsText(u"init");
    } else if (!isPattern && !containsEsc && !N(prop).computed && N(key).kind == NK::Ident
               && (N(key).name == JsText(u"get") || N(key).name == JsText(u"set"))
               && tok_.type != Tk::Comma && tok_.type != Tk::BraceR && tok_.type != Tk::Eq) {
        if (isGenerator || isAsync) unexpected();
        const JsText kind = N(key).name;
        parsePropertyName(prop);
        const int value = parseMethod(false, false, false);
        N(prop).b = value;
        N(prop).name = kind;
        const std::vector<int>& params = N(value).list;
        const size_t paramCount = kind == JsText(u"get") ? 0 : 1;
        if (params.size() != paramCount) {
            raise(N(value).start, kind == JsText(u"get") ? JsText(u"getter should have no params")
                                                               : JsText(u"setter should have exactly one param"));
        }
        if (kind == JsText(u"set") && N(params[0]).kind == NK::Rest) raise(N(params[0]).start, JsText(u"Setter cannot use rest params"));
    } else if (!N(prop).computed && N(key).kind == NK::Ident) {
        if (isGenerator || isAsync) unexpected();
        checkUnreserved(N(key).name, N(key).start);
        if (N(key).name == JsText(u"await") && awaitIdentPos_ < 0) awaitIdentPos_ = startPos;
        const int copy = nodeOf(NK::Ident, N(key).start);
        N(copy).name = N(key).name;
        N(copy).end = N(key).end;
        if (isPattern) {
            N(prop).b = parseMaybeDefault(startPos, copy);
        } else if (tok_.type == Tk::Eq && ref) {
            if (ref->shorthandAssign < 0) ref->shorthandAssign = tok_.start;
            N(prop).b = parseMaybeDefault(startPos, copy);
        } else {
            N(prop).b = copy;
        }
        N(prop).name = JsText(u"init");
        N(prop).shorthand = true;
    } else {
        unexpected();
    }
}

int Parser::parseMethod(bool isGenerator, bool isAsync, bool allowDirectSuper) {
    const int fn = nodeOf(NK::Function, tok_.start);
    const std::ptrdiff_t oldYieldPos = yieldPos_, oldAwaitPos = awaitPos_, oldAwaitIdentPos = awaitIdentPos_;
    yieldPos_ = awaitPos_ = awaitIdentPos_ = -1;
    enterScope(functionFlags(isAsync, isGenerator) | SCOPE_SUPER | (allowDirectSuper ? SCOPE_DIRECT_SUPER : 0));
    expect(Tk::ParenL);
    const std::vector<int> params = parseBindingList(Tk::ParenR, false, true);
    checkYieldAwaitInDefaultParams();
    parseFunctionBody(params, false, true, 0, -1);
    yieldPos_ = oldYieldPos;
    awaitPos_ = oldAwaitPos;
    awaitIdentPos_ = oldAwaitIdentPos;
    N(fn).list = params;
    return finish(fn);
}

int Parser::parseArrowExpression(std::ptrdiff_t start, std::vector<int> params, bool isAsync, int forInit) {
    const int fn = nodeOf(NK::Arrow, start);
    const std::ptrdiff_t oldYieldPos = yieldPos_, oldAwaitPos = awaitPos_, oldAwaitIdentPos = awaitIdentPos_;
    enterScope(functionFlags(isAsync, false) | SCOPE_ARROW);
    yieldPos_ = awaitPos_ = awaitIdentPos_ = -1;
    toAssignableList(params, true);
    parseFunctionBody(params, true, false, forInit, -1);
    yieldPos_ = oldYieldPos;
    awaitPos_ = oldAwaitPos;
    awaitIdentPos_ = oldAwaitIdentPos;
    N(fn).list = params;
    return finish(fn);
}

bool Parser::isSimpleParamList(const std::vector<int>& params) const {
    for (const int p : params) {
        if (N(p).kind != NK::Ident) return false;
    }
    return true;
}

void Parser::checkParams(const std::vector<int>& params, bool allowDuplicates) {
    std::unordered_set<JsText> nameHash;
    for (const int p : params) checkLValInnerPattern(p, Bind::Var, allowDuplicates ? nullptr : &nameHash);
}

// The function scope has been entered and the params are parsed. Parses
// the body (with its directive prologue), applies the checks that depend on
// the final strictness, and exits the scope.
void Parser::parseFunctionBody(const std::vector<int>& params, bool isArrowFunction, bool isMethod, int forInit, int idNode) {
    const bool isExpression = isArrowFunction && tok_.type != Tk::BraceL;
    const bool oldStrict = strict_;
    if (isExpression) {
        parseMaybeAssign(forInit);
        checkParams(params, false);
        exitScope();
        return;
    }
    const bool simple = isSimpleParamList(params);
    const std::vector<Label> oldLabels = labels_;
    labels_.clear();
    // Declare the parameters before the body so that a body `let` clashing
    // with one is reported. Duplicates and strict-mode names are judged once
    // the directive prologue has settled the function's strictness.
    checkParams(params, true);
    if (strict_ && idNode >= 0) checkLValSimple(idNode, Bind::Outside);
    const std::ptrdiff_t bodyStart = tok_.start;
    expect(Tk::BraceL);
    const bool useStrict = parseStatementsWithDirectives(Tk::BraceR);
    if (useStrict && !simple) raise(bodyStart, JsText(u"Illegal 'use strict' directive in function with non-simple parameter list"));
    const bool allowDuplicates = !oldStrict && !useStrict && !isArrowFunction && !isMethod && simple;
    const bool finalStrict = oldStrict || useStrict;
    std::vector<std::pair<JsText, std::ptrdiff_t>> names;
    for (const int p : params) collectBoundNames(p, names);
    std::unordered_set<JsText> seen;
    for (const auto& name : names) {
        if (finalStrict && isStrictBindReserved(name.first)) raise(name.second, JsText(u"Binding %1 in strict mode").arg(name.first));
        if (!allowDuplicates) {
            if (seen.contains(name.first)) raise(name.second, JsText(u"Argument name clash"));
            seen.insert(name.first);
        }
    }
    if (finalStrict && idNode >= 0 && isStrictBindReserved(N(idNode).name)) {
        raise(N(idNode).start, JsText(u"Binding %1 in strict mode").arg(N(idNode).name));
    }
    next(); // the closing brace
    strict_ = oldStrict;
    labels_ = oldLabels;
    exitScope();
}

std::vector<int> Parser::parseExprList(Tk close, bool allowTrailingComma, bool allowEmpty, DErr* ref) {
    std::vector<int> elts;
    bool first = true;
    while (!eat(close)) {
        if (!first) {
            expect(Tk::Comma);
            if (allowTrailingComma && afterTrailingComma(close)) break;
        } else {
            first = false;
        }
        int elt = -1;
        if (allowEmpty && tok_.type == Tk::Comma) {
            elt = -1;
        } else if (tok_.type == Tk::Ellipsis) {
            elt = parseSpread(ref);
            if (ref && tok_.type == Tk::Comma && ref->trailingComma < 0) ref->trailingComma = tok_.start;
        } else {
            elt = parseMaybeAssign(0, ref);
        }
        elts.push_back(elt);
    }
    return elts;
}

bool Parser::tokStartsExpr() const {
    switch (tok_.type) {
        case Tk::Name:
            return !isKeyword(tok_.value) || keywordStartsExprSet().contains(tok_.value);
        case Tk::PrivateId: case Tk::Num: case Tk::String: case Tk::Regexp: case Tk::Template: case Tk::BracketL:
        case Tk::BraceL: case Tk::ParenL: case Tk::IncDec: case Tk::Prefix: case Tk::PlusMin: case Tk::Slash:
            return true;
        case Tk::Assign:
            return tok_.value == JsText(u"/=");
        default:
            return false;
    }
}

int Parser::parseYield(int forInit) {
    if (yieldPos_ < 0) yieldPos_ = tok_.start;
    const int node = nodeOf(NK::Yield, tok_.start);
    next();
    if (tok_.type == Tk::Semi || canInsertSemicolon() || (tok_.type != Tk::Star && !tokStartsExpr())) {
        return finish(node);
    }
    eat(Tk::Star);
    N(node).a = parseMaybeAssign(forInit);
    return finish(node);
}

int Parser::parseAwait(int forInit) {
    if (awaitPos_ < 0) awaitPos_ = tok_.start;
    const int node = nodeOf(NK::Await, tok_.start);
    next();
    N(node).a = parseMaybeUnary(nullptr, true, false, forInit);
    return finish(node);
}

bool Parser::isPrivateFieldAccess(int node) const {
    const Node& nd = N(node);
    if (nd.kind == NK::Member) return nd.b >= 0 && N(nd.b).kind == NK::PrivateName;
    if (nd.kind == NK::Chain) return isPrivateFieldAccess(nd.a);
    return false;
}

bool Parser::isSimpleAssignTarget(int expr) const {
    const NK k = N(expr).kind;
    return k == NK::Ident || k == NK::Member;
}

void Parser::checkPropClash(int prop, bool& hasProto, DErr* ref) {
    const Node& p = N(prop);
    if (p.kind == NK::Spread) return;
    if (p.computed || p.method || p.shorthand) return;
    const Node& key = N(p.a);
    JsText name;
    if (key.kind == NK::Ident || key.kind == NK::StringLit) name = key.name;
    else return;
    if (name == JsText(u"__proto__") && p.name == JsText(u"init")) {
        if (hasProto) {
            if (ref) {
                if (ref->doubleProto < 0) ref->doubleProto = key.start;
            } else {
                raise(key.start, JsText(u"Redefinition of __proto__ property"));
            }
        }
        hasProto = true;
    }
}

void Parser::checkPatternErrors(const DErr* ref, bool isAssign) {
    if (!ref) return;
    if (ref->trailingComma > -1) raise(ref->trailingComma, JsText(u"Comma is not permitted after the rest element"));
    const std::ptrdiff_t parens = isAssign ? ref->parenthesizedAssign : ref->parenthesizedBind;
    if (parens > -1) raise(parens, isAssign ? JsText(u"Assigning to rvalue") : JsText(u"Parenthesized pattern"));
}

bool Parser::checkExpressionErrors(const DErr* ref, bool andThrow) {
    if (!ref) return false;
    if (!andThrow) return ref->shorthandAssign >= 0 || ref->doubleProto >= 0;
    if (ref->shorthandAssign >= 0) raise(ref->shorthandAssign, JsText(u"Shorthand property assignments are valid only in destructuring patterns"));
    if (ref->doubleProto >= 0) raise(ref->doubleProto, JsText(u"Redefinition of __proto__ property"));
    return false;
}

void Parser::checkYieldAwaitInDefaultParams() {
    if (yieldPos_ > -1 && (awaitPos_ < 0 || yieldPos_ < awaitPos_)) raise(yieldPos_, JsText(u"Yield expression cannot be a default value"));
    if (awaitPos_ > -1) raise(awaitPos_, JsText(u"Await expression cannot be a default value"));
}

// ============================================================== lval

int Parser::toAssignable(int node, bool isBinding, DErr* ref) {
    DepthGuard guard(*this);
    if (node < 0) {
        if (ref) checkPatternErrors(ref, true);
        return node;
    }
    Node& nd = N(node);
    switch (nd.kind) {
        case NK::Ident:
            if (inAsync() && nd.name == JsText(u"await")) raise(nd.start, JsText(u"Cannot use 'await' as identifier inside an async function"));
            break;
        case NK::ObjectPattern: case NK::ArrayPattern: case NK::AssignPattern: case NK::Rest:
            break;
        case NK::Object: {
            nd.kind = NK::ObjectPattern;
            if (ref) checkPatternErrors(ref, true);
            const std::vector<int> props = nd.list;
            for (const int prop : props) {
                toAssignable(prop, isBinding, nullptr);
                const Node& p = N(prop);
                if (p.kind == NK::Rest && (N(p.a).kind == NK::ArrayPattern || N(p.a).kind == NK::ObjectPattern)) {
                    raise(N(p.a).start, JsText(u"Unexpected token"));
                }
            }
            break;
        }
        case NK::Property: {
            if (nd.name != JsText(u"init")) raise(N(nd.a).start, JsText(u"Object pattern can't contain getter or setter"));
            if (nd.method) raise(N(nd.a).start, JsText(u"Object pattern can't contain methods"));
            toAssignable(nd.b, isBinding, nullptr);
            break;
        }
        case NK::Array: {
            nd.kind = NK::ArrayPattern;
            if (ref) checkPatternErrors(ref, true);
            std::vector<int> elems = nd.list;
            toAssignableList(elems, isBinding);
            N(node).list = elems;
            break;
        }
        case NK::Spread: {
            nd.kind = NK::Rest;
            const int arg = nd.a;
            toAssignable(arg, isBinding, nullptr);
            if (N(arg).kind == NK::AssignPattern) raise(N(arg).start, JsText(u"Rest elements cannot have a default value"));
            break;
        }
        case NK::Assign: {
            if (nd.name != JsText(u"=")) raise(N(nd.a).end, JsText(u"Only '=' operator can be used for specifying default value."));
            nd.kind = NK::AssignPattern;
            toAssignable(nd.a, isBinding, nullptr);
            break;
        }
        case NK::Chain:
            raise(nd.start, JsText(u"Optional chaining cannot appear in left-hand side"));
        case NK::Member:
            if (!isBinding) break;
            raise(nd.start, JsText(u"Assigning to rvalue"));
        default:
            raise(nd.start, JsText(u"Assigning to rvalue"));
    }
    return node;
}

void Parser::toAssignableList(std::vector<int>& list, bool isBinding) {
    for (const int elt : list) {
        if (elt >= 0) toAssignable(elt, isBinding, nullptr);
    }
}

int Parser::parseSpread(DErr* ref) {
    const int node = nodeOf(NK::Spread, tok_.start);
    next();
    N(node).a = parseMaybeAssign(0, ref);
    return finish(node);
}

int Parser::parseRestBinding() {
    const int node = nodeOf(NK::Rest, tok_.start);
    next();
    N(node).a = parseBindingAtom();
    return finish(node);
}

int Parser::parseBindingAtom() {
    DepthGuard guard(*this);
    if (tok_.type == Tk::BracketL) {
        const int node = nodeOf(NK::ArrayPattern, tok_.start);
        next();
        N(node).list = parseBindingList(Tk::BracketR, true, true);
        return finish(node);
    }
    if (tok_.type == Tk::BraceL) return parseObj(true, nullptr);
    return parseIdent(false);
}

std::vector<int> Parser::parseBindingList(Tk close, bool allowEmpty, bool allowTrailingComma) {
    std::vector<int> elts;
    bool first = true;
    while (!eat(close)) {
        if (first) first = false;
        else expect(Tk::Comma);
        if (allowEmpty && tok_.type == Tk::Comma) {
            elts.push_back(-1);
        } else if (allowTrailingComma && afterTrailingComma(close)) {
            break;
        } else if (tok_.type == Tk::Ellipsis) {
            elts.push_back(parseRestBinding());
            if (tok_.type == Tk::Comma) raise(tok_.start, JsText(u"Comma is not permitted after the rest element"));
            expect(close);
            break;
        } else {
            elts.push_back(parseMaybeDefault(tok_.start));
        }
    }
    return elts;
}

int Parser::parseMaybeDefault(std::ptrdiff_t startPos, int left) {
    if (left < 0) left = parseBindingAtom();
    if (!eat(Tk::Eq)) return left;
    const int node = nodeOf(NK::AssignPattern, startPos);
    N(node).a = left;
    N(node).b = parseMaybeAssign();
    return finish(node);
}

void Parser::checkLValSimple(int expr, Bind bindingType, std::unordered_set<JsText>* checkClashes, bool allowCall) {
    const bool isBind = bindingType != Bind::None;
    const Node& nd = N(expr);
    switch (nd.kind) {
        case NK::Ident:
            if (strict_ && isStrictBindReserved(nd.name)) {
                raise(nd.start, JsText(u"%1 %2 in strict mode").arg(isBind ? JsText(u"Binding") : JsText(u"Assigning to"), nd.name));
            }
            if (isBind) {
                if (bindingType == Bind::Lexical && nd.name == JsText(u"let")) raise(nd.start, JsText(u"let is disallowed as a lexically bound name"));
                if (checkClashes) {
                    if (checkClashes->contains(nd.name)) raise(nd.start, JsText(u"Argument name clash"));
                    checkClashes->insert(nd.name);
                }
                if (bindingType != Bind::Outside) declareName(nd.name, bindingType, nd.start);
            }
            return;
        case NK::Chain:
            raise(nd.start, JsText(u"Optional chaining cannot appear in left-hand side"));
        case NK::Member:
            if (isBind) raise(nd.start, JsText(u"Binding member expression"));
            return;
        case NK::Call:
            // V8 accepts a call expression as a simple assignment target and
            // throws at run time.
            if (allowCall && !isBind) return;
            raise(nd.start, JsText(u"Assigning to rvalue"));
        default:
            raise(nd.start, isBind ? JsText(u"Binding rvalue") : JsText(u"Assigning to rvalue"));
    }
}

void Parser::checkLValPattern(int expr, Bind bindingType, std::unordered_set<JsText>* checkClashes) {
    DepthGuard guard(*this);
    const Node& nd = N(expr);
    if (nd.kind == NK::ObjectPattern) {
        const std::vector<int> props = nd.list;
        for (const int prop : props) checkLValInnerPattern(prop, bindingType, checkClashes);
        return;
    }
    if (nd.kind == NK::ArrayPattern) {
        const std::vector<int> elems = nd.list;
        for (const int elem : elems) {
            if (elem >= 0) checkLValInnerPattern(elem, bindingType, checkClashes);
        }
        return;
    }
    checkLValSimple(expr, bindingType, checkClashes);
}

void Parser::checkLValInnerPattern(int expr, Bind bindingType, std::unordered_set<JsText>* checkClashes) {
    const Node& nd = N(expr);
    switch (nd.kind) {
        case NK::Property:
            checkLValInnerPattern(nd.b, bindingType, checkClashes);
            return;
        case NK::AssignPattern:
            checkLValPattern(nd.a, bindingType, checkClashes);
            return;
        case NK::Rest:
            checkLValPattern(nd.a, bindingType, checkClashes);
            return;
        default:
            checkLValPattern(expr, bindingType, checkClashes);
    }
}

void Parser::collectBoundNames(int pattern, std::vector<std::pair<JsText, std::ptrdiff_t>>& out) {
    DepthGuard guard(*this);
    const Node& nd = N(pattern);
    switch (nd.kind) {
        case NK::Ident:
            out.emplace_back(nd.name, nd.start);
            return;
        case NK::ObjectPattern:
        case NK::ArrayPattern: {
            const std::vector<int> items = nd.list;
            for (const int item : items) {
                if (item >= 0) collectBoundNames(item, out);
            }
            return;
        }
        case NK::Property:
            collectBoundNames(nd.b, out);
            return;
        case NK::AssignPattern:
        case NK::Rest:
            collectBoundNames(nd.a, out);
            return;
        default:
            return;
    }
}

// Parses statements until `terminator` (not consumed). A leading run of
// string-literal expression statements is the directive prologue; a
// "use strict" directive makes the rest strict. Returns whether it did.
bool Parser::parseStatementsWithDirectives(Tk terminator) {
    bool useStrict = false;
    bool inPrologue = true;
    bool octalInPrologue = false;
    while (tok_.type != terminator) {
        if (tok_.type == Tk::Eof) unexpected();
        if (inPrologue && tok_.type == Tk::String) {
            const Token strTok = tok_;
            const int stmt = parseStatement(JsText());
            if (stmt >= 0 && N(stmt).kind == NK::StringLit && N(stmt).start == strTok.start && N(stmt).end == strTok.end) {
                if (strTok.octal) octalInPrologue = true;
                if (src_.mid(strTok.start + 1, strTok.end - strTok.start - 2) == JsText(u"use strict")) {
                    if (octalInPrologue) raise(strTok.start, JsText(u"Octal escape sequence before 'use strict'"));
                    useStrict = true;
                    strict_ = true;
                }
                continue;
            }
            inPrologue = false;
            continue;
        }
        inPrologue = false;
        parseStatement(JsText());
    }
    return useStrict;
}

void Parser::run(const JsText& param) {
    enterScope(SCOPE_FUNCTION);
    nextToken();
    const int id = nodeOf(NK::Ident, 0);
    N(id).name = param;
    declareName(param, Bind::Var, 0);
    const bool useStrict = parseStatementsWithDirectives(Tk::Eof);
    if (useStrict && isStrictBindReserved(param)) raise(0, JsText(u"Binding %1 in strict mode").arg(param));
    exitScope();
}

} // namespace

CheckResult checkFunctionBody(const JsString& param, const JsString& body) {
    CheckResult result;
    try {
        Parser parser(body);
        parser.run(param);
        result.verdict = Verdict::Valid;
    } catch (const SyntaxFail& e) {
        result.verdict = Verdict::Invalid;
        result.detail = JsText(u"%1 (offset %2)").arg(e.why).arg(e.pos);
    } catch (const Undecidable& e) {
        result.verdict = Verdict::Unsupported;
        result.detail = e.what;
    }
    return result;
}

} // namespace nm::js
