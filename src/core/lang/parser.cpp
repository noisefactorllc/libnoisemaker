#include "core/lang/parser.h"

#include "core/lang/ast.h"
#include "core/lang/diagnostics.h"
#include "core/lang/tokens.h"


#include <utility>

namespace nm {

namespace {

// PARITY-CRITICAL behaviors replicated exactly from the reference
// shaders/src/lang/parser.js (source of truth), cross-checked against
// td/noisemaker/compiler/lang/parser.py and
// godot/addons/noisemaker/compiler/lang/parser.gd, and spot-verified
// against the reference oracle (NM_REFERENCE_ROOT tools/dump-ast.mjs) for
// every hazard called out below -- see task report "hazards encountered":
//   - Numeric +-*/ are CONSTANT-FOLDED at parse time in double,
//     left-to-right within a precedence level; operands MUST be Number
//     literals (else "Expected number"). Math.PI = 3.141592653589793.
//   - HEX color: int(pair,16)/255 in double; 3-digit char duplication;
//     alpha default 1.0; 8-digit form's alpha is the 4th pair / 255.
//   - Special-form transforms in this exact order: from, osc (4-way
//     heuristic), midi, audio, read, read3d.
//   - `search` mandatory + position-restricted (only before any other
//     statement); `render` terminates the program's statement loop
//     entirely (so the reference's "Duplicate render()" check is
//     unreachable dead code -- ported anyway for fidelity, but verified
//     via the oracle that two `render()` calls in one program instead
//     hits "Expected end of input").
//   - A DOTTED name immediately followed (across 1+ member segments) by a
//     call is ALWAYS routed through parseChain/parseCall, which then
//     unconditionally expects LPAREN right after the identifier: exactly
//     one dot before the call -> explicit "inline namespace syntax ...
//     is not allowed"; two or more dots before the call -> the generic
//     "Expect '('" (this second case is a reference bug-for-bug, verified
//     against the oracle: `foo.bar.baz()` throws "Expect '(' at line 1
//     col 4", NOT a namespace error and NOT a successful Member parse).
//   - Comments are legal ONLY where the reference explicitly calls
//     collectComments(): between top-level program statements, around
//     the '.' in a chain, and before each subchain-body element. A
//     comment immediately before a statement INSIDE an if/elif/else
//     block is a syntax error (verified against the oracle).
//   - JS falsy-OR quirk: subchain `name`/`id` use `kwargs.x?.value ||
//     null`, so an explicitly EMPTY STRING value also collapses to JSON
//     null, not "" (verified against the oracle) -- do not "fix" this.
//   - Read.surface / Read3D.tex3d are OMITTED entirely (no key at all)
//     when unresolved, matching the reference's bare `surface: surface`
//     / `tex3d: tex3d` (no `|| null` fallback) -- but Read3D.geo DOES
//     have `|| null` in the reference and is therefore ALWAYS present,
//     verified against the oracle for bare `read()` / `read3d()`.
//   - `search`'s VALID_NAMESPACES is the reference's frozen BUILT-IN set
//     (shaders/src/runtime/tags.js _builtinDescriptors) in declaration
//     order -- dump-ast.mjs only imports lexer.js+parser.js, so no effect
//     module ever calls registerNamespace() in the oracle's process, and
//     this exact 10-name list is all that is ever valid there (matches
//     godot/addons/noisemaker/compiler/lang/tags.gd's hard-coded list).

const LangStringList& validNamespaces() {
    static const LangStringList list = {
        LangString(u"io"),      LangString(u"classicNoisedeck"), LangString(u"synth"),
        LangString(u"mixer"),   LangString(u"filter"),           LangString(u"render"),
        LangString(u"points"),  LangString(u"synth3d"),          LangString(u"filter3d"),
        LangString(u"user"),
    };
    return list;
}

bool isValidNamespaceName(const LangString& ns) {
    return validNamespaces().contains(ns);
}

// Token types that can begin an expression (reference exprStartTokens).
const LangSet<LangString>& exprStartTokens() {
    static const LangSet<LangString> s = {
        TokenType::PLUS,       TokenType::MINUS,     TokenType::NUMBER,     TokenType::HEX,
        TokenType::FUNC,       TokenType::STRING,    TokenType::IDENT,      TokenType::OUTPUT_REF,
        TokenType::SOURCE_REF, TokenType::VOL_REF,   TokenType::GEO_REF,    TokenType::MESH_REF,
        TokenType::XYZ_REF,    TokenType::VEL_REF,   TokenType::RGBA_REF,   TokenType::LPAREN,
        TokenType::LBRACKET,   TokenType::TRUE,      TokenType::FALSE,
    };
    return s;
}

// Token types allowed as segments inside a dotted member/enum path
// (reference memberTokenTypes).
const LangSet<LangString>& memberTokenTypes() {
    static const LangSet<LangString> s = {
        TokenType::IDENT,   TokenType::SOURCE_REF, TokenType::OUTPUT_REF, TokenType::VOL_REF,
        TokenType::GEO_REF, TokenType::MESH_REF,   TokenType::XYZ_REF,    TokenType::VEL_REF,
        TokenType::RGBA_REF, TokenType::LET,       TokenType::RENDER,     TokenType::TRUE,
        TokenType::FALSE,   TokenType::IF,         TokenType::ELIF,       TokenType::ELSE,
        TokenType::BREAK,   TokenType::CONTINUE,   TokenType::RETURN,     TokenType::WRITE,
        TokenType::WRITE3D, TokenType::SUBCHAIN,
    };
    return s;
}

// Token types usable as a namespace identifier in a search directive
// (reference namespaceTokenTypes -- keywords are valid namespace names).
const LangSet<LangString>& namespaceTokenTypes() {
    static const LangSet<LangString> s = {
        TokenType::IDENT,  TokenType::RENDER,   TokenType::WRITE,    TokenType::WRITE3D,
        TokenType::TRUE,   TokenType::FALSE,    TokenType::IF,       TokenType::ELIF,
        TokenType::ELSE,   TokenType::BREAK,    TokenType::CONTINUE, TokenType::RETURN,
    };
    return s;
}

JsonValue stripPrivatePos(const JsonValue& val) {
    if (val.isObject()) {
        JsonObject obj = val.toObject();
        obj.remove(LangString(u"_pos"));
        for (const auto& key : obj.keys()) {
            JsonValue child = obj.value(key);
            if (child.isObject() || child.isArray()) obj.insert(key, stripPrivatePos(child));
        }
        return obj;
    } else if (val.isArray()) {
        JsonArray arr = val.toArray();
        for (int i = 0; i < static_cast<int>(arr.size()); ++i) {
            JsonValue child = arr.at(i);
            if (child.isObject() || child.isArray()) arr.replace(i, stripPrivatePos(child));
        }
        return arr;
    }
    return val;
}

JsonObject refNode(const LangString& type, const LangString& name) {
    JsonObject o;
    o.insert(LangString(u"type"), type);
    o.insert(LangString(u"name"), name);
    return o;
}

int hexPairToInt(const LangString& pair) {
    bool ok = false;
    const int v = pair.toInt(&ok, 16);
    return ok ? v : 0;
}

class Parser {
public:
    explicit Parser(LangVector<Token> tokens, bool strictSubchainArguments = false)
        : tokens_(std::move(tokens)), strictSubchainArguments_(strictSubchainArguments) {}

    JsonObject parseProgram();

private:
    LangVector<Token> tokens_;
    int current_ = 0;
    bool strictSubchainArguments_ = false;

    // Track the search order for the program (set by the search
    // directive -- REQUIRED). hasSearch_ mirrors the reference's
    // `programSearchOrder !== null`.
    bool hasSearch_ = false;
    LangStringList searchOrder_;
    JsonArray namespaceImports_;
    JsonObject namespaceDefault_;
    bool hasNamespaceDefault_ = false;

    // --- cursor helpers ---------------------------------------------
    bool inBounds(int idx) const { return idx >= 0 && idx < tokens_.size(); }
    // Clamped read (defensive: the reference's `undefined` semantics for
    // an out-of-range peek() never trigger on a well-formed grammar walk,
    // but clamping to the trailing EOF token avoids any UB if one ever
    // did -- same defensive posture as the godot port's _peek()).
    const Token& peek() const { return tokens_.at(current_ < tokens_.size() ? current_ : tokens_.size() - 1); }
    Token advance() {
        const Token t = peek();
        current_++;
        return t;
    }
    LangString typeAt(int idx) const { return inBounds(idx) ? tokens_.at(idx).type : LangString(); }
    const Token* tokenAt(int idx) const { return inBounds(idx) ? &tokens_.at(idx) : nullptr; }

    static DslSyntaxError makeParserError(const LangString& code, const LangString& message,
                                         int line = -1, int col = -1,
                                         bool hasLine = false, bool hasCol = false,
                                         const JsonObject& pos = JsonObject(),
                                         const LangString& severityOverride = LangString()) {
        bool hasPosition = false;
        int pLine = 0, pCol = 0, pStart = -1, pEnd = -1;
        if (!pos.isEmpty()) {
            const JsonValue lv = pos.value(LangString(u"line"));
            const JsonValue cv = pos.value(LangString(u"column"));
            const JsonValue sv = pos.value(LangString(u"start"));
            const JsonValue ev = pos.value(LangString(u"end"));
            if (lv.isDouble() && cv.isDouble() && sv.isDouble() && ev.isDouble()) {
                pLine = lv.toInt();
                pCol = cv.toInt();
                pStart = sv.toInt();
                pEnd = ev.toInt();
                if (pLine > 0 && pCol > 0 && pStart >= 0 && pEnd >= pStart) {
                    hasPosition = true;
                }
            }
        }
        const bool hasLocation = (line > 0 && col > 0 && hasLine && hasCol);

        JsonObject diagnostic;
        diagnostic.insert(LangString(u"code"), code);
        diagnostic.insert(LangString(u"stage"), diagStage(code));
        const LangString severity = severityOverride.isEmpty() ? LangString(diagSeverity(code)) : severityOverride;
        diagnostic.insert(LangString(u"severity"), severity);
        diagnostic.insert(LangString(u"message"), message);
        if (hasPosition) {
            JsonObject loc;
            loc.insert(LangString(u"line"), pLine);
            loc.insert(LangString(u"column"), pCol);
            diagnostic.insert(LangString(u"location"), loc);

            JsonObject span;
            span.insert(LangString(u"start"), pStart);
            span.insert(LangString(u"end"), pEnd);
            diagnostic.insert(LangString(u"span"), span);
        } else {
            if (hasLocation) {
                JsonObject loc;
                loc.insert(LangString(u"line"), line);
                loc.insert(LangString(u"column"), col);
                diagnostic.insert(LangString(u"location"), loc);
            } else {
                diagnostic.insert(LangString(u"location"), JsonValue(JsonValue::Null));
            }
            diagnostic.insert(LangString(u"span"), JsonValue(JsonValue::Null));
        }

        const int errLine = hasPosition ? pLine : (hasLocation ? line : -1);
        const int errCol = hasPosition ? pCol : (hasLocation ? col : -1);
        return DslSyntaxError(message, errLine, errCol, diagnostic.native());
    }

    DslSyntaxError parserError(const LangString& code, const LangString& message, const Token& t,
                               const LangString& severityOverride = LangString()) const {
        JsonObject pos;
        if (t.hasPosition) {
            pos.insert(LangString(u"line"), t.posLine);
            pos.insert(LangString(u"column"), t.posColumn);
            pos.insert(LangString(u"start"), t.posStart);
            pos.insert(LangString(u"end"), t.posEnd);
        }
        const bool hasLine = (t.line > 0 && (t.rawLine.empty() || t.hasLine));
        const bool hasCol = (t.col > 0 && (t.rawCol.empty() || t.hasCol));
        return makeParserError(code, message, t.line, t.col, hasLine, hasCol, pos, severityOverride);
    }

    DslSyntaxError parserErrorAt(const LangString& code, const LangString& core, const Token& t, const LangString& suffix = LangString()) const {
        const LangString lineStr = t.rawLine.empty() ? (t.line > 0 ? LangString::number(t.line) : LangString(u"undefined")) : t.rawLine;
        const LangString colStr = t.rawCol.empty() ? (t.col > 0 ? LangString::number(t.col) : LangString(u"undefined")) : t.rawCol;
        const LangString message = LangString(u"%1 at line %2 col %3%4").arg(core, lineStr, colStr, suffix);
        return parserError(code, message, t);
    }

    Token expect(const LangString& type, const LangString& msg) {
        const Token t = peek();
        if (t.type == type) return advance();
        const LangString code = (type == TokenType::RPAREN) ? LangString(u"P002") : LangString(u"P001");
        throw parserErrorAt(code, msg, t);
    }

    // Collect and consume any pending COMMENT tokens; returns their
    // lexemes as a JSON array of strings.
    JsonArray collectComments() {
        JsonArray comments;
        while (peek().type == TokenType::COMMENT) {
            comments.append(advance().lexeme);
        }
        return comments;
    }

    bool hasCallAfterDot(int index) const;

    JsonObject parseRenderDirective();
    void parseSearchDirective();
    void validateNamespace(const Token& token);
    JsonArray parseBlock();
    JsonObject parseStatement();
    JsonArray parseChain(const LangString& context);
    JsonObject parseWriteCall();
    JsonObject parseSubchainCall();
    JsonObject parseCall();
    JsonObject parseArg() { return parseAdditive(); }
    void parseKwarg(JsonObject& obj);
    JsonObject parseAdditive();
    JsonObject parseMultiplicative();
    JsonObject parseUnary();
    JsonObject parsePrimary();
    static double toNumber(const JsonObject& node);

    JsonObject transformOscInvocation(const JsonObject& call, const Token& nameToken);
    JsonObject transformMidiInvocation(const JsonObject& call, const Token& nameToken,
                                        const LangStringList& kwargOrder);
    JsonObject transformAudioInvocation(const JsonObject& call, const Token& nameToken,
                                         const LangStringList& kwargOrder);
    JsonObject transformFromInvocation(const JsonObject& call, const Token& nameToken);
};

bool Parser::hasCallAfterDot(int index) const {
    int i = index + 1;
    if (typeAt(i) != TokenType::DOT) return false;
    while (typeAt(i) == TokenType::DOT) {
        const Token* seg = tokenAt(i + 1);
        if (!seg || !memberTokenTypes().contains(seg->type)) return false;
        i += 2;
    }
    return typeAt(i) == TokenType::LPAREN;
}

JsonObject Parser::parseRenderDirective() {
    advance();
    expect(TokenType::LPAREN, LangString(u"Expect '('"));
    if (peek().type != TokenType::OUTPUT_REF) {
        // Reference throws with NO location suffix in error.message, but carries structured location.
        throw parserError(LangString(u"P005"), LangString(u"Expected output reference in render()"), peek());
    }
    JsonObject out = refNode(NodeKind::OutputRef, advance().lexeme);
    expect(TokenType::RPAREN, LangString(u"Expect ')'"));
    return out;
}

JsonObject Parser::parseProgram() {
    JsonArray plans;
    JsonArray vars;
    JsonObject render;
    bool hasRender = false;
    JsonArray trailingComments;

    while (peek().type != TokenType::EOF_) {
        if (peek().type == TokenType::SEMICOLON) {
            advance();
            continue;
        }
        const JsonArray leadingComments = collectComments();
        if (peek().type == TokenType::EOF_) {
            for (const JsonValue& c : leadingComments) trailingComments.append(c);
            break;
        }
        // A semicolon right after a comment routes back to the top of the
        // loop (which then advances it) rather than falling into the
        // SEARCH/RENDER/statement checks below -- matches the reference's
        // otherwise-identical `if (peek().type === 'SEMICOLON') { continue }`.
        if (peek().type == TokenType::SEMICOLON) {
            continue;
        }
        if (peek().type == TokenType::SEARCH) {
            if (!plans.isEmpty() || !vars.isEmpty() || hasRender) {
                const Token t = peek();
                throw parserErrorAt(LangString(u"P004"),
                                    LangString(u"'search' directive must appear before other statements"), t);
            }
            parseSearchDirective();
            continue;
        }
        if (peek().type == TokenType::RENDER) {
            // consumeRender() inlined: the reference's "Duplicate render()"
            // guard is unreachable given the unconditional `break` right
            // after (see file header note) -- ported for structural
            // fidelity anyway.
            if (hasRender) {
                const Token t = peek();
                throw parserErrorAt(LangString(u"P005"), LangString(u"Duplicate render() directive"), t);
            }
            render = parseRenderDirective();
            hasRender = true;
            while (peek().type == TokenType::SEMICOLON) advance();
            if (!leadingComments.isEmpty()) {
                render.insert(LangString(u"leadingComments"), leadingComments);
            }
            const JsonArray trailing = collectComments();
            for (const JsonValue& c : trailing) trailingComments.append(c);
            break;
        }
        JsonObject stmt = parseStatement();
        if (!leadingComments.isEmpty()) {
            stmt.insert(LangString(u"leadingComments"), leadingComments);
        }
        if (stmt.value(LangString(u"type")).toString() == NodeKind::VarAssign) {
            vars.append(stmt);
        } else {
            plans.append(stmt);
        }
        while (peek().type == TokenType::SEMICOLON) advance();
    }
    const Token eof = expect(TokenType::EOF_, LangString(u"Expected end of input"));
    if (!hasSearch_ || searchOrder_.isEmpty()) {
        throw parserError(LangString(u"P004"), LangString(
            u"Missing required 'search' directive. Every program must start with 'search <namespace>, ...' "
            "to specify namespace search order."), eof);
    }

    JsonObject program;
    program.insert(LangString(u"type"), NodeKind::Program);
    program.insert(LangString(u"plans"), plans);
    program.insert(LangString(u"render"), hasRender ? JsonValue(render) : JsonValue(JsonValue::Null));
    if (!vars.isEmpty()) program.insert(LangString(u"vars"), vars);
    if (!trailingComments.isEmpty()) program.insert(LangString(u"trailingComments"), trailingComments);

    JsonArray searchOrderJson;
    for (const LangString& ns : searchOrder_) searchOrderJson.append(ns);
    JsonObject namespaceMeta;
    namespaceMeta.insert(LangString(u"imports"), namespaceImports_);
    namespaceMeta.insert(LangString(u"default"),
                          hasNamespaceDefault_ ? JsonValue(namespaceDefault_) : JsonValue(JsonValue::Null));
    namespaceMeta.insert(LangString(u"searchOrder"), searchOrderJson);
    program.insert(LangString(u"namespace"), namespaceMeta);
    return stripPrivatePos(program).toObject();
}

void Parser::parseSearchDirective() {
    if (hasSearch_) {
        const Token t = peek();
        throw parserErrorAt(LangString(u"P004"),
                            LangString(u"Only one search directive is allowed per program"), t);
    }
    advance(); // consume 'search'
    LangStringList namespaces;

    const Token first = peek();
    if (!namespaceTokenTypes().contains(first.type)) {
        throw parserErrorAt(LangString(u"P004"),
                            LangString(u"Expected namespace identifier after search"), first);
    }
    advance();
    validateNamespace(first);
    namespaces.append(first.lexeme);

    while (peek().type == TokenType::COMMA) {
        advance();
        const Token nsToken = peek();
        if (!namespaceTokenTypes().contains(nsToken.type)) {
            throw parserErrorAt(LangString(u"P004"),
                                LangString(u"Expected namespace identifier after comma"), nsToken);
        }
        advance();
        validateNamespace(nsToken);
        namespaces.append(nsToken.lexeme);
    }

    hasSearch_ = true;
    searchOrder_ = namespaces;

    namespaceImports_ = JsonArray();
    for (const LangString& nm : namespaces) {
        JsonObject imp;
        imp.insert(LangString(u"name"), nm);
        imp.insert(LangString(u"source"), LangString(u"search"));
        imp.insert(LangString(u"explicit"), true);
        namespaceImports_.append(imp);
    }
    namespaceDefault_ = JsonObject();
    namespaceDefault_.insert(LangString(u"name"), namespaces.first());
    namespaceDefault_.insert(LangString(u"source"), LangString(u"search"));
    namespaceDefault_.insert(LangString(u"explicit"), true);
    hasNamespaceDefault_ = true;

    while (peek().type == TokenType::SEMICOLON) advance();
}

void Parser::validateNamespace(const Token& token) {
    if (!isValidNamespaceName(token.lexeme)) {
        throw parserErrorAt(LangString(u"P004"),
                            LangString(u"Invalid namespace '%1'").arg(token.lexeme),
                            token,
                            LangString(u". Valid namespaces: %1").arg(validNamespaces().join(LangString(u", "))));
    }
}

JsonArray Parser::parseBlock() {
    expect(TokenType::LBRACE, LangString(u"Expect '{'"));
    JsonArray body;
    while (peek().type != TokenType::RBRACE) {
        body.append(parseStatement());
        while (peek().type == TokenType::SEMICOLON) advance();
    }
    expect(TokenType::RBRACE, LangString(u"Expect '}'"));
    return body;
}

JsonObject Parser::parseStatement() {
    if (peek().type == TokenType::SEARCH) {
        const Token t = peek();
        throw parserErrorAt(LangString(u"P004"),
                            LangString(u"'search' directive is only allowed at the start of the program"), t);
    }
    if (peek().type == TokenType::LET) {
        advance();
        const LangString name = expect(TokenType::IDENT, LangString(u"Expected identifier")).lexeme;
        expect(TokenType::EQUAL, LangString(u"Expect '='"));
        if (!exprStartTokens().contains(peek().type)) {
            const Token t = peek();
            throw parserErrorAt(LangString(u"P001"), LangString(u"Expected expression after '='"), t);
        }
        const JsonObject expr = parseAdditive();
        JsonObject node;
        node.insert(LangString(u"type"), NodeKind::VarAssign);
        node.insert(LangString(u"name"), name);
        node.insert(LangString(u"expr"), expr);
        return node;
    }

    const LangString tt = peek().type;
    if (tt == TokenType::IF) {
        advance();
        expect(TokenType::LPAREN, LangString(u"Expect '('"));
        const JsonObject condition = parseAdditive();
        expect(TokenType::RPAREN, LangString(u"Expect ')'"));
        const JsonArray thenBlock = parseBlock();
        JsonArray elifList;
        while (peek().type == TokenType::ELIF) {
            advance();
            expect(TokenType::LPAREN, LangString(u"Expect '('"));
            const JsonObject ec = parseAdditive();
            expect(TokenType::RPAREN, LangString(u"Expect ')'"));
            const JsonArray body = parseBlock();
            JsonObject elifEntry;
            elifEntry.insert(LangString(u"condition"), ec);
            elifEntry.insert(LangString(u"then"), body);
            elifList.append(elifEntry);
        }
        JsonValue elseBranch = JsonValue(JsonValue::Null);
        if (peek().type == TokenType::ELSE) {
            advance();
            elseBranch = parseBlock();
        }
        JsonObject node;
        node.insert(LangString(u"type"), NodeKind::IfStmt);
        node.insert(LangString(u"condition"), condition);
        node.insert(LangString(u"then"), thenBlock);
        node.insert(LangString(u"elif"), elifList);
        node.insert(LangString(u"else"), elseBranch);
        return node;
    }
    if (tt == TokenType::BREAK) {
        advance();
        JsonObject node;
        node.insert(LangString(u"type"), NodeKind::Break);
        return node;
    }
    if (tt == TokenType::CONTINUE) {
        advance();
        JsonObject node;
        node.insert(LangString(u"type"), NodeKind::Continue);
        return node;
    }
    if (tt == TokenType::RETURN) {
        advance();
        JsonObject node;
        node.insert(LangString(u"type"), NodeKind::Return);
        if (exprStartTokens().contains(peek().type)) {
            node.insert(LangString(u"value"), parseAdditive());
        }
        return node;
    }

    const JsonArray chain = parseChain(LangString(u"statement"));
    // Extract write/write3d only if the chain TERMINATES with a
    // Write/Write3D node -- mid-chain writes don't count as terminal.
    JsonValue write = JsonValue(JsonValue::Null);
    JsonValue write3d = JsonValue(JsonValue::Null);
    if (!chain.isEmpty()) {
        const JsonObject lastNode = chain.last().toObject();
        const LangString lastType = lastNode.value(LangString(u"type")).toString();
        if (lastType == NodeKind::Write) {
            write = lastNode.value(LangString(u"surface"));
        } else if (lastType == NodeKind::Write3D) {
            JsonObject w3;
            w3.insert(LangString(u"tex3d"), lastNode.value(LangString(u"tex3d")));
            w3.insert(LangString(u"geo"), lastNode.value(LangString(u"geo")));
            write3d = w3;
        }
    }
    // NOTE: this wrapper deliberately has NO "type" key (identified by
    // the "chain" key alone) -- matches the reference exactly.
    JsonObject stmt;
    stmt.insert(LangString(u"chain"), chain);
    stmt.insert(LangString(u"write"), write);
    stmt.insert(LangString(u"write3d"), write3d);
    return stmt;
}

JsonArray Parser::parseChain(const LangString& context) {
    const JsonObject firstCall = parseCall();
    JsonArray calls;
    calls.append(firstCall);
    while (true) {
        // Comments can appear before the DOT in a chain (e.g. `noise() \n
        // // comment \n .bloom()`); if there is no DOT after them, restore
        // position so they belong to whatever follows this chain instead.
        const int savedPos = current_;
        const JsonArray leadingComments = collectComments();
        if (peek().type != TokenType::DOT) {
            current_ = savedPos;
            break;
        }
        advance(); // consume '.'
        const JsonArray postDotComments = collectComments();
        JsonArray allComments;
        for (const JsonValue& c : leadingComments) allComments.append(c);
        for (const JsonValue& c : postDotComments) allComments.append(c);

        const LangString nextType = peek().type;
        if (nextType == TokenType::WRITE || nextType == TokenType::WRITE3D) {
            if (context == LangString(u"expression")) {
                const Token t = peek();
                throw parserErrorAt(LangString(u"P005"),
                                    LangString(u"'.write()' is only allowed in statement context"), t);
            }
            JsonObject writeNode = parseWriteCall();
            if (!allComments.isEmpty()) writeNode.insert(LangString(u"leadingComments"), allComments);
            calls.append(writeNode);
            continue;
        }
        if (nextType == TokenType::SUBCHAIN) {
            JsonObject subchainNode = parseSubchainCall();
            if (!allComments.isEmpty()) subchainNode.insert(LangString(u"leadingComments"), allComments);
            calls.append(subchainNode);
            continue;
        }
        JsonObject call = parseCall();
        if (!allComments.isEmpty()) call.insert(LangString(u"leadingComments"), allComments);
        calls.append(call);
    }
    return calls;
}

JsonObject Parser::parseWriteCall() {
    const Token tok = peek();
    const LangString tokenType = tok.type;
    const int tokenLine = tok.line;
    const int tokenCol = tok.col;

    if (tokenType == TokenType::WRITE) {
        advance(); // consume 'write'
        expect(TokenType::LPAREN, LangString(u"Expect '('"));
        JsonObject surface;
        const LangString pt = peek().type;
        if (pt == TokenType::OUTPUT_REF) {
            surface = refNode(NodeKind::OutputRef, advance().lexeme);
        } else if (pt == TokenType::XYZ_REF) {
            surface = refNode(NodeKind::XyzRef, advance().lexeme);
        } else if (pt == TokenType::VEL_REF) {
            surface = refNode(NodeKind::VelRef, advance().lexeme);
        } else if (pt == TokenType::RGBA_REF) {
            surface = refNode(NodeKind::RgbaRef, advance().lexeme);
        } else if (pt == TokenType::MESH_REF) {
            surface = refNode(NodeKind::MeshRef, advance().lexeme);
        } else if (pt == TokenType::IDENT && peek().lexeme == LangString(u"none")) {
            // "none" is a valid target meaning "don't write to any surface".
            surface = refNode(NodeKind::OutputRef, advance().lexeme);
        } else {
            const Token p = peek();
            throw parserErrorAt(
                LangString(u"P005"),
                LangString(
                    u"write() requires an explicit surface reference (e.g., o0, o1, xyz0, vel0, rgba0, mesh0, none)"),
                p);
        }
        expect(TokenType::RPAREN, LangString(u"Expect ')'"));
        JsonObject node;
        node.insert(LangString(u"type"), NodeKind::Write);
        node.insert(LangString(u"surface"), surface);
        node.insert(LangString(u"loc"), ast::loc(tokenLine, tokenCol));
        return node;
    }
    if (tokenType == TokenType::WRITE3D) {
        advance(); // consume 'write3d'
        expect(TokenType::LPAREN, LangString(u"Expect '('"));
        JsonObject tex3d;
        LangString pt = peek().type;
        if (pt == TokenType::IDENT || pt == TokenType::OUTPUT_REF || pt == TokenType::VOL_REF) {
            if (pt == TokenType::OUTPUT_REF) {
                tex3d = refNode(NodeKind::OutputRef, advance().lexeme);
            } else if (pt == TokenType::VOL_REF) {
                tex3d = refNode(NodeKind::VolRef, advance().lexeme);
            } else {
                tex3d = refNode(NodeKind::Ident, advance().lexeme);
            }
        } else {
            const Token p = peek();
            throw parserErrorAt(LangString(u"P005"), LangString(u"Expected tex3d reference in write3d()"), p);
        }
        expect(TokenType::COMMA, LangString(u"Expect ',' between tex3d and geo in write3d()"));
        JsonObject geo;
        pt = peek().type;
        if (pt == TokenType::IDENT || pt == TokenType::OUTPUT_REF || pt == TokenType::GEO_REF) {
            if (pt == TokenType::OUTPUT_REF) {
                geo = refNode(NodeKind::OutputRef, advance().lexeme);
            } else if (pt == TokenType::GEO_REF) {
                geo = refNode(NodeKind::GeoRef, advance().lexeme);
            } else {
                geo = refNode(NodeKind::Ident, advance().lexeme);
            }
        } else {
            const Token p = peek();
            throw parserErrorAt(LangString(u"P005"), LangString(u"Expected geo reference in write3d()"), p);
        }
        expect(TokenType::RPAREN, LangString(u"Expect ')'"));
        JsonObject node;
        node.insert(LangString(u"type"), NodeKind::Write3D);
        node.insert(LangString(u"tex3d"), tex3d);
        node.insert(LangString(u"geo"), geo);
        node.insert(LangString(u"loc"), ast::loc(tokenLine, tokenCol));
        return node;
    }
    Token dummy;
    dummy.line = tokenLine;
    dummy.col = tokenCol;
    dummy.hasLine = (tokenLine > 0);
    dummy.hasCol = (tokenCol > 0);
    throw parserError(LangString(u"P005"),
                      LangString(u"Expected write or write3d at line %1 col %2").arg(tokenLine).arg(tokenCol),
                      dummy);
}

JsonObject Parser::parseSubchainCall() {
    const Token nameToken = peek();

    advance(); // consume 'subchain'
    expect(TokenType::LPAREN, LangString(u"Expect '(' after subchain"));

    // Machine-readable subchain-argument reports. Order follows the
    // offending token in the source. Default mode collects them onto the
    // Subchain node as non-enumerable metadata (surfaced by validate());
    // strict mode throws with the same codes.
    JsonArray argDiagnostics;
    auto reportArgIssue = [&](const LangString& code, const LangString& message, const Token& token) {
        if (strictSubchainArguments_) {
            throw parserError(code, message, token, LangString(u"error"));
        }
        JsonObject report;
        report.insert(LangString(u"code"), code);
        report.insert(LangString(u"message"), message);
        report.insert(LangString(u"severity"), diagSeverity(code));
        if (token.hasPosition) {
            JsonObject loc;
            loc.insert(LangString(u"line"), token.posLine);
            loc.insert(LangString(u"column"), token.posColumn);
            report.insert(LangString(u"location"), loc);

            JsonObject span;
            span.insert(LangString(u"start"), token.posStart);
            span.insert(LangString(u"end"), token.posEnd);
            report.insert(LangString(u"span"), span);
        } else if (token.line > 0 && token.col > 0 && (token.rawLine.empty() || token.hasLine) && (token.rawCol.empty() || token.hasCol)) {
            JsonObject loc;
            loc.insert(LangString(u"line"), token.line);
            loc.insert(LangString(u"column"), token.col);
            report.insert(LangString(u"location"), loc);
        }
        argDiagnostics.append(report);
    };

    static const LangStringList subchainKeys = { LangString(u"name"), LangString(u"id") };

    // key -> {type:'String', value:...}; ANY identifier key is syntactically
    // accepted here (matches the reference), but only "name"/"id" are ever
    // read back below -- everything else is silently discarded.
    JsonObject kwargs;
    if (peek().type != TokenType::RPAREN) {
        if (peek().type == TokenType::STRING) {
            // Positional name: subchain("name"). NOTE this is an if/else-if
            // with the keyword-loop branch below -- a positional string
            // can NEVER be followed by more args (e.g. `subchain("n", id:
            // "x")` is a syntax error, not "name + id"): only one of the
            // two branches ever runs.
            JsonObject nameVal;
            nameVal.insert(LangString(u"type"), NodeKind::String);
            nameVal.insert(LangString(u"value"), advance().lexeme);
            kwargs.insert(LangString(u"name"), nameVal);
        } else if (peek().type == TokenType::IDENT && typeAt(current_ + 1) == TokenType::COLON) {
            // Keyword arguments: subchain(name: "...", id: "...")
            while (peek().type == TokenType::IDENT && typeAt(current_ + 1) == TokenType::COLON) {
                const Token keyToken = advance();
                const LangString key = keyToken.lexeme;
                advance(); // consume ':'
                if (peek().type != TokenType::STRING) {
                    throw parserErrorAt(LangString(u"P006"), LangString(u"Expected string value for subchain %1").arg(key),
                                        peek());
                }
                const LangString value = advance().lexeme;
                if (!subchainKeys.contains(key)) {
                    reportArgIssue(
                        LangString(u"P008"),
                        LangString(u"Unknown subchain argument '%1' at line %2 col %3. Valid keys: name, id. The value is discarded.")
                            .arg(key).arg(keyToken.line).arg(keyToken.col),
                        keyToken);
                } else if (kwargs.contains(key)) {
                    reportArgIssue(
                        LangString(u"P009"),
                        LangString(u"Duplicate subchain argument '%1' at line %2 col %3. The last value wins.")
                            .arg(key).arg(keyToken.line).arg(keyToken.col),
                        keyToken);
                }
                JsonObject val;
                val.insert(LangString(u"type"), NodeKind::String);
                val.insert(LangString(u"value"), value);
                kwargs.insert(key, val);
                if (peek().type == TokenType::COMMA) {
                    advance(); // consume ','
                } else if (peek().type == TokenType::IDENT && typeAt(current_ + 1) == TokenType::COLON) {
                    reportArgIssue(
                        LangString(u"P010"),
                        LangString(u"Missing ',' between subchain arguments at line %1 col %2")
                            .arg(peek().line).arg(peek().col),
                        peek());
                }
            }
        }
    }
    expect(TokenType::RPAREN, LangString(u"Expect ')' after subchain arguments"));
    expect(TokenType::LBRACE, LangString(u"Expect '{' to start subchain body"));

    JsonArray body;
    while (peek().type != TokenType::RBRACE) {
        const JsonArray leadingComments = collectComments();
        if (peek().type == TokenType::RBRACE) break;
        if (peek().type != TokenType::DOT) {
            throw parserErrorAt(LangString(u"P006"), LangString(u"Expected '.' before chain element in subchain body"),
                                peek());
        }
        advance(); // consume '.'
        const JsonArray postDotComments = collectComments();
        JsonArray allComments;
        for (const JsonValue& c : leadingComments) allComments.append(c);
        for (const JsonValue& c : postDotComments) allComments.append(c);
        JsonObject call = parseCall();
        if (!allComments.isEmpty()) call.insert(LangString(u"leadingComments"), allComments);
        body.append(call);
    }
    expect(TokenType::RBRACE, LangString(u"Expect '}' to end subchain body"));

    if (body.isEmpty()) {
        throw parserErrorAt(LangString(u"P006"), LangString(u"Subchain body cannot be empty"), nameToken);
    }

    // Reference: `kwargs.name?.value || null` -- a FALSY-OR, so an
    // explicitly empty-string value ALSO becomes null, not "". Ported
    // verbatim (see file header note); do not "fix" this.
    auto resolveFalsyStringOrNull = [&](const LangString& key) -> JsonValue {
        if (!kwargs.contains(key)) return JsonValue(JsonValue::Null);
        const LangString value = kwargs.value(key).toObject().value(LangString(u"value")).toString();
        return value.isEmpty() ? JsonValue(JsonValue::Null) : JsonValue(value);
    };

    JsonObject node;
    node.insert(LangString(u"type"), NodeKind::Subchain);
    node.insert(LangString(u"name"), resolveFalsyStringOrNull(LangString(u"name")));
    node.insert(LangString(u"id"), resolveFalsyStringOrNull(LangString(u"id")));
    node.insert(LangString(u"body"), body);
    node.insert(LangString(u"loc"), ast::loc(nameToken.line, nameToken.col));
    if (!argDiagnostics.isEmpty()) {
        node.insert(LangString(u"subchainArgumentDiagnostics"), argDiagnostics);
    }
    return node;
}

JsonObject Parser::parseCall() {
    const Token nameToken = expect(TokenType::IDENT, LangString(u"Expected identifier"));
    // Inline namespace syntax (e.g., nd.noise()) is forbidden. This ONLY
    // fires for exactly one dot segment immediately followed by a call;
    // two-or-more-segment dotted calls fall through to the unconditional
    // expect(LPAREN) below and hit the generic "Expect '('" instead (a
    // reference bug-for-bug -- see file header note).
    if (peek().type == TokenType::DOT) {
        const Token* next = tokenAt(current_ + 1);
        if (next && next->type == TokenType::IDENT) {
            const Token* after = tokenAt(current_ + 2);
            if (after && after->type == TokenType::LPAREN) {
                throw parserErrorAt(
                    LangString(u"P007"),
                    LangString(u"Inline namespace syntax '%1.%2()' is not allowed. Use 'search %1' at the start "
                                   "of the program instead,").arg(nameToken.lexeme, next->lexeme),
                    nameToken);
            }
        }
    }
    expect(TokenType::LPAREN, LangString(u"Expect '('"));
    JsonArray args;
    JsonObject kwargs;
    LangStringList kwargOrder;
    bool keyword = false;
    bool positional = false;
    const bool allowMixed = nameToken.lexeme == LangString(u"midi")
        || nameToken.lexeme == LangString(u"audio");
    if (peek().type != TokenType::RPAREN) {
        while (true) {
            if (peek().type == TokenType::IDENT && typeAt(current_ + 1) == TokenType::COLON) {
                if (positional && !allowMixed) {
                    const Token t = peek();
                    throw parserErrorAt(LangString(u"P007"), LangString(u"Cannot mix positional and keyword arguments"), t);
                }
                keyword = true;
                const LangString kwargName = peek().lexeme;
                parseKwarg(kwargs);
                kwargOrder.append(kwargName);
            } else {
                if (keyword && !allowMixed) {
                    const Token t = peek();
                    throw parserErrorAt(LangString(u"P007"), LangString(u"Cannot mix positional and keyword arguments"), t);
                }
                positional = true;
                args.append(parseArg());
            }
            if (peek().type != TokenType::COMMA) break;
            advance();
            if (peek().type == TokenType::RPAREN) break;
        }
    }
    expect(TokenType::RPAREN, LangString(u"Expect ')'"));

    JsonObject call;
    call.insert(LangString(u"type"), NodeKind::Call);
    call.insert(LangString(u"name"), nameToken.lexeme);
    call.insert(LangString(u"args"), args);
    if (keyword) call.insert(LangString(u"kwargs"), kwargs);

    const LangString lexeme = nameToken.lexeme;
    if (lexeme == LangString(u"from")) {
        return transformFromInvocation(call, nameToken);
    }
    // osc() as a value oscillator (not the synth.osc generator effect) --
    // 4-way heuristic, checked in this order.
    if (lexeme == LangString(u"osc")) {
        static const LangSet<LangString> oscKwargKeys = {
            LangString(u"type"), LangString(u"min"),    LangString(u"max"),
            LangString(u"speed"), LangString(u"offset"), LangString(u"seed"),
        };
        const bool hasTypeKwarg = kwargs.contains(LangString(u"type"));
        const bool firstArgIsOscKind = !args.isEmpty()
            && args.at(0).toObject().value(LangString(u"type")).toString() == NodeKind::Member
            && !args.at(0).toObject().value(LangString(u"path")).toArray().isEmpty()
            && args.at(0).toObject().value(LangString(u"path")).toArray().at(0).toString()
                   == LangString(u"oscKind");
        const bool isBareOsc = args.isEmpty() && kwargs.isEmpty();
        bool hasOnlyOscKwargs = !kwargs.isEmpty();
        if (hasOnlyOscKwargs) {
            for (const LangString& k : kwargs.keys()) {
                if (!oscKwargKeys.contains(k)) {
                    hasOnlyOscKwargs = false;
                    break;
                }
            }
        }
        if (hasTypeKwarg || firstArgIsOscKind || isBareOsc || hasOnlyOscKwargs) {
            return transformOscInvocation(call, nameToken);
        }
        // else fall through to return as a regular Call node for the
        // synth effect (e.g. a positional non-oscKind arg with no kwargs).
    }
    if (lexeme == LangString(u"midi")) {
        return transformMidiInvocation(call, nameToken, kwargOrder);
    }
    if (lexeme == LangString(u"audio")) {
        return transformAudioInvocation(call, nameToken, kwargOrder);
    }
    // read()/read3d() are pipeline built-ins. The raw Read AST owns an
    // undefined surface when no argument resolves; JSON omits that member.
    if (lexeme == LangString(u"read")) {
        JsonObject node;
        node.insert(LangString(u"type"), NodeKind::Read);
        if (!args.isEmpty()) {
            node.insert(LangString(u"surface"), args.at(0));
        } else if (kwargs.contains(LangString(u"tex"))) {
            node.insert(LangString(u"surface"), kwargs.value(LangString(u"tex")));
        } else if (kwargs.contains(LangString(u"surface"))) {
            node.insert(LangString(u"surface"), kwargs.value(LangString(u"surface")));
        } else {
            node.insert(LangString(u"surface"), JsonValue());
        }
        node.insert(LangString(u"loc"), ast::loc(nameToken.line, nameToken.col));
        const JsonObject skip = kwargs.value(LangString(u"_skip")).toObject();
        if (skip.value(LangString(u"type")).toString() == NodeKind::Boolean
            && skip.value(LangString(u"value")).toBool()) {
            node.insert(LangString(u"_skip"), true);
        }
        return node;
    }
    if (lexeme == LangString(u"read3d")) {
        JsonObject node;
        node.insert(LangString(u"type"), NodeKind::Read3D);
        if (!args.isEmpty()) {
            node.insert(LangString(u"tex3d"), args.at(0));
        } else if (kwargs.contains(LangString(u"tex3d"))) {
            node.insert(LangString(u"tex3d"), kwargs.value(LangString(u"tex3d")));
        }
        JsonValue geo;
        if (args.size() > 1) {
            geo = args.at(1);
        } else if (kwargs.contains(LangString(u"geo"))) {
            geo = kwargs.value(LangString(u"geo"));
        }
        node.insert(LangString(u"geo"), geo.isUndefined() ? JsonValue(JsonValue::Null) : geo);
        node.insert(LangString(u"loc"), ast::loc(nameToken.line, nameToken.col));
        const JsonObject skip = kwargs.value(LangString(u"_skip")).toObject();
        if (skip.value(LangString(u"type")).toString() == NodeKind::Boolean
            && skip.value(LangString(u"value")).toBool()) {
            node.insert(LangString(u"_skip"), true);
        }
        return node;
    }
    return call;
}

JsonObject Parser::transformOscInvocation(const JsonObject& call, const Token& nameToken) {
    const JsonArray args = call.value(LangString(u"args")).toArray();
    const JsonObject kwargs = call.value(LangString(u"kwargs")).toObject();
    static const LangStringList paramOrder = {
        LangString(u"type"), LangString(u"min"),    LangString(u"max"),
        LangString(u"speed"), LangString(u"offset"), LangString(u"seed"),
    };

    for (const LangString& key : kwargs.keys()) {
        if (!paramOrder.contains(key)) {
            throw parserErrorAt(LangString(u"P003"),
                                LangString(u"osc() unknown parameter '%1'").arg(key),
                                nameToken,
                                LangString(u". Valid: %1").arg(paramOrder.join(LangString(u", "))));
        }
    }

    auto resolve = [&](const LangString& name, int index, const JsonValue& dflt) -> JsonValue {
        if (kwargs.contains(name)) return kwargs.value(name);
        if (index < args.size()) return args.at(index);
        return dflt;
    };

    JsonObject node;
    node.insert(LangString(u"type"), NodeKind::Oscillator);
    node.insert(LangString(u"oscType"),
                resolve(LangString(u"type"), 0, ast::memberOf(LangString(u"oscKind"), LangString(u"sine"))));
    node.insert(LangString(u"min"), resolve(LangString(u"min"), 1, ast::number(0)));
    node.insert(LangString(u"max"), resolve(LangString(u"max"), 2, ast::number(1)));
    node.insert(LangString(u"speed"), resolve(LangString(u"speed"), 3, ast::number(1)));
    node.insert(LangString(u"offset"), resolve(LangString(u"offset"), 4, ast::number(0)));
    node.insert(LangString(u"seed"), resolve(LangString(u"seed"), 5, ast::number(1)));
    node.insert(LangString(u"loc"), ast::loc(nameToken.line, nameToken.col));
    return node;
}

JsonObject Parser::transformMidiInvocation(const JsonObject& call, const Token& nameToken,
                                            const LangStringList& kwargOrder) {
    const JsonArray args = call.value(LangString(u"args")).toArray();
    const JsonObject kwargs = call.value(LangString(u"kwargs")).toObject();

    static const LangStringList paramOrder = {
        LangString(u"channel"), LangString(u"mode"), LangString(u"min"),
        LangString(u"max"), LangString(u"sensitivity"),
    };
    static const LangStringList keywordOnlyParams = {
        LangString(u"name"), LangString(u"id"), LangString(u"cc"),
        LangString(u"nrpn"), LangString(u"zone"), LangString(u"members"),
    };
    LangStringList validParams = paramOrder;
    validParams.append(keywordOnlyParams);
    if (args.size() > paramOrder.size()) {
        throw parserErrorAt(LangString(u"P003"),
                            LangString(u"midi() name, id, cc, nrpn, zone and members are keyword-only"),
                            nameToken);
    }
    for (const LangString& key : kwargOrder) {
        if (!validParams.contains(key)) {
            throw parserErrorAt(LangString(u"P003"),
                                LangString(u"midi() unknown parameter '%1'").arg(key),
                                nameToken,
                                LangString(u". Valid: %1").arg(validParams.join(LangString(u", "))));
        }
    }

    const JsonObject defaults = {
        {LangString(u"mode"), ast::memberOf(LangString(u"midiMode"), LangString(u"velocity"))},
        {LangString(u"min"), ast::number(0)},
        {LangString(u"max"), ast::number(1)},
        {LangString(u"sensitivity"), ast::number(1)},
    };
    JsonObject resolved;
    int posCursor = 0;
    for (const LangString& paramName : paramOrder) {
        if (kwargs.contains(paramName)) {
            resolved.insert(paramName, kwargs.value(paramName));
        } else if (posCursor < args.size()) {
            resolved.insert(paramName, args.at(posCursor++));
        } else if (defaults.contains(paramName)) {
            resolved.insert(paramName, defaults.value(paramName));
        }
    }
    if (posCursor < args.size()) {
        throw parserErrorAt(LangString(u"P003"),
                            LangString(u"midi() has an excess positional argument"),
                            nameToken);
    }

    const JsonValue channel = resolved.value(LangString(u"channel"));
    if (channel.isUndefined() && !kwargs.contains(LangString(u"zone"))) {
        throw parserErrorAt(LangString(u"P003"),
                            LangString(u"midi() requires 'channel' or 'zone' argument"),
                            nameToken);
    }
    if (!channel.isUndefined() && kwargs.contains(LangString(u"zone"))) {
        throw parserErrorAt(LangString(u"P003"),
                            LangString(u"midi() 'channel' and 'zone' are mutually exclusive"),
                            nameToken);
    }
    if (kwargs.contains(LangString(u"members")) && !kwargs.contains(LangString(u"zone"))) {
        throw parserErrorAt(LangString(u"P003"),
                            LangString(u"midi() 'members' requires 'zone'"),
                            nameToken);
    }
    if (kwargs.contains(LangString(u"id")) && !kwargs.contains(LangString(u"name"))) {
        throw parserErrorAt(LangString(u"P003"),
                            LangString(u"midi() 'id' requires readable 'name'"),
                            nameToken);
    }
    for (const LangString& paramName : {LangString(u"name"), LangString(u"id")}) {
        if (!kwargs.contains(paramName)) continue;
        const JsonObject value = kwargs.value(paramName).toObject();
        if (value.value(LangString(u"type")).toString() != NodeKind::String) {
            throw parserErrorAt(LangString(u"P003"),
                                LangString(u"midi() '%1' requires a quoted string").arg(paramName),
                                nameToken);
        }
        if (value.value(LangString(u"value")).toString().isEmpty()) {
            throw parserErrorAt(LangString(u"P003"),
                                LangString(u"midi() '%1' must not be empty").arg(paramName),
                                nameToken);
        }
    }

    JsonObject node;
    node.insert(LangString(u"type"), NodeKind::Midi);
    node.insert(LangString(u"channel"), channel);
    node.insert(LangString(u"mode"), resolved.value(LangString(u"mode")));
    node.insert(LangString(u"min"), resolved.value(LangString(u"min")));
    node.insert(LangString(u"max"), resolved.value(LangString(u"max")));
    node.insert(LangString(u"sensitivity"), resolved.value(LangString(u"sensitivity")));
    for (const LangString& field : {LangString(u"cc"), LangString(u"nrpn"), LangString(u"zone"),
                                    LangString(u"members"), LangString(u"name"), LangString(u"id")}) {
        node.insert(field, kwargs.value(field));
    }
    node.insert(LangString(u"loc"), ast::loc(nameToken.line, nameToken.col));
    return node;
}

JsonObject Parser::transformAudioInvocation(const JsonObject& call, const Token& nameToken,
                                             const LangStringList& kwargOrder) {
    const JsonArray args = call.value(LangString(u"args")).toArray();
    const JsonObject kwargs = call.value(LangString(u"kwargs")).toObject();

    static const LangStringList paramOrder = {
        LangString(u"band"), LangString(u"min"), LangString(u"max"),
    };
    static const LangStringList keywordOnlyParams = {
        LangString(u"channel"), LangString(u"name"), LangString(u"id"),
    };
    LangStringList validParams = paramOrder;
    validParams.append(keywordOnlyParams);
    if (args.size() > paramOrder.size()) {
        throw parserErrorAt(LangString(u"P003"),
                            LangString(u"audio() channel, name and id are keyword-only"),
                            nameToken);
    }
    for (const LangString& key : kwargOrder) {
        if (!validParams.contains(key)) {
            throw parserErrorAt(LangString(u"P003"),
                                LangString(u"audio() unknown parameter '%1'").arg(key),
                                nameToken,
                                LangString(u". Valid: %1").arg(validParams.join(LangString(u", "))));
        }
    }

    const JsonObject defaults = {
        {LangString(u"min"), ast::number(0)},
        {LangString(u"max"), ast::number(1)},
    };
    JsonObject resolved;
    int posCursor = 0;
    for (const LangString& paramName : paramOrder) {
        if (kwargs.contains(paramName)) {
            resolved.insert(paramName, kwargs.value(paramName));
        } else if (posCursor < args.size()) {
            resolved.insert(paramName, args.at(posCursor++));
        } else if (defaults.contains(paramName)) {
            resolved.insert(paramName, defaults.value(paramName));
        }
    }
    if (posCursor < args.size()) {
        throw parserErrorAt(LangString(u"P003"),
                            LangString(u"audio() has an excess positional argument"),
                            nameToken);
    }

    const JsonValue band = resolved.value(LangString(u"band"));
    if (band.isUndefined()) {
        throw parserErrorAt(LangString(u"P003"),
                            LangString(u"audio() requires 'band' argument"),
                            nameToken);
    }
    if (kwargs.contains(LangString(u"id")) && !kwargs.contains(LangString(u"name"))) {
        throw parserErrorAt(LangString(u"P003"),
                            LangString(u"audio() 'id' requires readable 'name'"),
                            nameToken);
    }
    if (kwargs.contains(LangString(u"name")) && !kwargs.contains(LangString(u"channel"))) {
        throw parserErrorAt(LangString(u"P003"),
                            LangString(u"audio() selected device requires both 'name' and 'channel'"),
                            nameToken);
    }
    for (const LangString& paramName : {LangString(u"name"), LangString(u"id")}) {
        if (!kwargs.contains(paramName)) continue;
        const JsonObject value = kwargs.value(paramName).toObject();
        if (value.value(LangString(u"type")).toString() != NodeKind::String) {
            throw parserErrorAt(LangString(u"P003"),
                                LangString(u"audio() '%1' requires a quoted string").arg(paramName),
                                nameToken);
        }
        if (value.value(LangString(u"value")).toString().isEmpty()) {
            throw parserErrorAt(LangString(u"P003"),
                                LangString(u"audio() '%1' must not be empty").arg(paramName),
                                nameToken);
        }
    }

    JsonObject node;
    node.insert(LangString(u"type"), NodeKind::Audio);
    node.insert(LangString(u"band"), band);
    node.insert(LangString(u"min"), resolved.value(LangString(u"min")));
    node.insert(LangString(u"max"), resolved.value(LangString(u"max")));
    node.insert(LangString(u"channel"), kwargs.value(LangString(u"channel")));
    node.insert(LangString(u"name"), kwargs.value(LangString(u"name")));
    node.insert(LangString(u"id"), kwargs.value(LangString(u"id")));
    node.insert(LangString(u"loc"), ast::loc(nameToken.line, nameToken.col));
    return node;
}

JsonObject Parser::transformFromInvocation(const JsonObject& call, const Token& nameToken) {
    auto fail = [&](const LangString& message) {
        const bool hasCoordinates = (nameToken.line > 0 && nameToken.col > 0
            && (nameToken.rawLine.empty() || nameToken.hasLine)
            && (nameToken.rawCol.empty() || nameToken.hasCol));
        if (hasCoordinates) {
            throw parserErrorAt(LangString(u"P007"), message, nameToken);
        }
        throw parserError(LangString(u"P007"), message, nameToken);
    };

    const JsonObject kwargs = call.value(LangString(u"kwargs")).toObject();
    if (!kwargs.isEmpty()) {
        fail(LangString(u"'from' does not support named arguments"));
    }
    const JsonArray args = call.value(LangString(u"args")).toArray();
    if (args.size() != 2) {
        fail(LangString(u"'from' requires exactly two arguments (namespace, call)"));
    }
    const JsonObject namespaceArg = args.at(0).toObject();
    const JsonObject targetArg = args.at(1).toObject();
    const LangString namespaceArgType = namespaceArg.value(LangString(u"type")).toString();
    if (namespaceArgType != NodeKind::Ident && namespaceArgType != NodeKind::Member) {
        fail(LangString(u"'from' namespace argument must be an identifier"));
    }
    LangString namespaceName;
    if (namespaceArgType == NodeKind::Member) {
        LangStringList segs;
        for (const JsonValue& v : namespaceArg.value(LangString(u"path")).toArray()) segs.append(v.toString());
        namespaceName = segs.join(u'.');
    } else {
        namespaceName = namespaceArg.value(LangString(u"name")).toString();
    }
    if (namespaceName.isEmpty()) {
        fail(LangString(u"'from' namespace argument must be non-empty"));
    }

    JsonObject targetCall;
    bool haveTargetCall = false;
    const LangString targetType = targetArg.value(LangString(u"type")).toString();
    if (targetType == NodeKind::Call) {
        targetCall = targetArg;
        haveTargetCall = true;
    } else if (targetType == NodeKind::Chain) {
        // Unreachable in practice: parsePrimary always unwraps a
        // length-1 chain to its single element directly, so a Chain node
        // here can only ever have 2+ elements. Ported anyway for fidelity
        // with the reference's own defensive check.
        const JsonArray innerChain = targetArg.value(LangString(u"chain")).toArray();
        if (innerChain.size() == 1) {
            const JsonObject head = innerChain.at(0).toObject();
            if (head.value(LangString(u"type")).toString() == NodeKind::Call) {
                targetCall = head;
                haveTargetCall = true;
            }
        }
    }
    if (!haveTargetCall) {
        fail(LangString(u"'from' second argument must be a call expression"));
    }

    JsonObject replacement;
    replacement.insert(LangString(u"type"), NodeKind::Call);
    replacement.insert(LangString(u"name"), targetCall.value(LangString(u"name")));
    replacement.insert(LangString(u"args"), targetCall.value(LangString(u"args")).toArray());
    if (targetCall.contains(LangString(u"kwargs"))) {
        replacement.insert(LangString(u"kwargs"), targetCall.value(LangString(u"kwargs")).toObject());
    }
    JsonObject overrideNamespace;
    overrideNamespace.insert(LangString(u"name"), namespaceName);
    JsonArray pathArr;
    pathArr.append(namespaceName);
    overrideNamespace.insert(LangString(u"path"), pathArr);
    overrideNamespace.insert(LangString(u"explicit"), true);
    overrideNamespace.insert(LangString(u"source"), LangString(u"from"));
    overrideNamespace.insert(LangString(u"resolved"), namespaceName);
    JsonArray searchOrderArr;
    searchOrderArr.append(namespaceName);
    overrideNamespace.insert(LangString(u"searchOrder"), searchOrderArr);
    overrideNamespace.insert(LangString(u"fromOverride"), true);
    replacement.insert(LangString(u"namespace"), overrideNamespace);
    return replacement;
}

void Parser::parseKwarg(JsonObject& obj) {
    const LangString key = expect(TokenType::IDENT, LangString(u"Expected identifier")).lexeme;
    expect(TokenType::COLON, LangString(u"Expect ':'"));
    if (!exprStartTokens().contains(peek().type)) {
        // NOTE: reference message says "after '='" even though a kwarg
        // uses ':' -- a copy-paste artifact in the source of truth, kept
        // verbatim (never "clean up" a reference wording quirk).
        const Token t = peek();
        throw parserErrorAt(LangString(u"P001"), LangString(u"Expected expression after '='"), t);
    }
    obj.insert(key, parseArg());
}

JsonObject Parser::parseAdditive() {
    JsonObject node = parseMultiplicative();
    while (peek().type == TokenType::PLUS || peek().type == TokenType::MINUS) {
        const LangString op = advance().type;
        const JsonObject right = parseMultiplicative();
        const double l = toNumber(node);
        const double r = toNumber(right);
        node = ast::number(op == TokenType::PLUS ? l + r : l - r);
    }
    return node;
}

JsonObject Parser::parseMultiplicative() {
    JsonObject node = parseUnary();
    while (peek().type == TokenType::STAR || peek().type == TokenType::SLASH) {
        const LangString op = advance().type;
        const JsonObject right = parseUnary();
        const double l = toNumber(node);
        const double r = toNumber(right);
        node = ast::number(op == TokenType::STAR ? l * r : l / r);
    }
    return node;
}

JsonObject Parser::parseUnary() {
    if (peek().type == TokenType::PLUS) {
        advance();
        return parseUnary();
    }
    if (peek().type == TokenType::MINUS) {
        advance();
        const JsonObject val = parseUnary();
        return ast::number(-toNumber(val));
    }
    return parsePrimary();
}

double Parser::toNumber(const JsonObject& node) {
    if (node.value(LangString(u"type")).toString() != NodeKind::Number) {
        // Number coercion failures locate the offending AST node's private
        // source position when present, its parser-authored loc otherwise,
        // and are explicitly null when neither carries valid coordinates.
        JsonObject pos = node.value(LangString(u"_pos")).toObject();
        int line = -1;
        int col = -1;
        bool hasLine = false;
        bool hasCol = false;
        if (node.contains(LangString(u"loc"))) {
            const JsonObject loc = node.value(LangString(u"loc")).toObject();
            if (loc.contains(LangString(u"line")) && loc.value(LangString(u"line")).isDouble()) {
                line = loc.value(LangString(u"line")).toInt();
                hasLine = (line > 0);
            }
            if (loc.contains(LangString(u"col")) && loc.value(LangString(u"col")).isDouble()) {
                col = loc.value(LangString(u"col")).toInt();
                hasCol = (col > 0);
            }
        }
        throw makeParserError(LangString(u"P001"), LangString(u"Expected number"),
                              line, col, hasLine, hasCol, pos);
    }
    return node.value(LangString(u"value")).toDouble();
}

JsonObject Parser::parsePrimary() {
    const Token token = peek();
    const LangString tt = token.type;

    if (tt == TokenType::NUMBER) {
        advance();
        return ast::number(LangString(token.lexeme).toDouble());
    }
    if (tt == TokenType::STRING) {
        advance();
        JsonObject node;
        node.insert(LangString(u"type"), NodeKind::String);
        node.insert(LangString(u"value"), token.lexeme);
        return node;
    }
    if (tt == TokenType::HEX) {
        advance();
        const LangString hex = LangString(token.lexeme).mid(1);
        double r = 0.0, g = 0.0, b = 0.0, a = 1.0;
        if (hex.length() == 3) {
            r = hexPairToInt(LangString(2, hex.at(0)));
            g = hexPairToInt(LangString(2, hex.at(1)));
            b = hexPairToInt(LangString(2, hex.at(2)));
        } else if (hex.length() == 6) {
            r = hexPairToInt(hex.mid(0, 2));
            g = hexPairToInt(hex.mid(2, 2));
            b = hexPairToInt(hex.mid(4, 2));
        } else if (hex.length() == 8) {
            r = hexPairToInt(hex.mid(0, 2));
            g = hexPairToInt(hex.mid(2, 2));
            b = hexPairToInt(hex.mid(4, 2));
            a = hexPairToInt(hex.mid(6, 2)) / 255.0;
        }
        JsonObject node;
        node.insert(LangString(u"type"), NodeKind::Color);
        JsonArray value;
        value.append(r / 255.0);
        value.append(g / 255.0);
        value.append(b / 255.0);
        value.append(a);
        node.insert(LangString(u"value"), value);
        return node;
    }
    if (tt == TokenType::LBRACKET) {
        // Array literal -- comma-separated arg expressions, an alternate
        // input form for vec2/vec3/vec4 parameters.
        const int startLine = token.line;
        const int startCol = token.col;
        advance();
        JsonArray elements;
        if (peek().type != TokenType::RBRACKET) {
            elements.append(parseArg());
            while (peek().type == TokenType::COMMA) {
                advance();
                elements.append(parseArg());
            }
        }
        if (peek().type != TokenType::RBRACKET) {
            const Token t = peek();
            throw parserErrorAt(LangString(u"P001"), LangString(u"Expected ']'"), t);
        }
        advance();
        JsonObject node;
        node.insert(LangString(u"type"), NodeKind::ArrayLiteral);
        node.insert(LangString(u"elements"), elements);
        node.insert(LangString(u"loc"), ast::loc(startLine, startCol));
        if (token.hasPosition) {
            JsonObject pos;
            pos.insert(LangString(u"line"), token.posLine);
            pos.insert(LangString(u"column"), token.posColumn);
            pos.insert(LangString(u"start"), token.posStart);
            pos.insert(LangString(u"end"), token.posEnd);
            node.insert(LangString(u"_pos"), pos);
        }
        return node;
    }
    if (tt == TokenType::FUNC) {
        advance();
        JsonObject node;
        node.insert(LangString(u"type"), NodeKind::Func);
        node.insert(LangString(u"src"), token.lexeme);
        return node;
    }
    if (tt == TokenType::TRUE) {
        advance();
        JsonObject node;
        node.insert(LangString(u"type"), NodeKind::Boolean);
        node.insert(LangString(u"value"), true);
        return node;
    }
    if (tt == TokenType::FALSE) {
        advance();
        JsonObject node;
        node.insert(LangString(u"type"), NodeKind::Boolean);
        node.insert(LangString(u"value"), false);
        return node;
    }
    if (tt == TokenType::IDENT) {
        const Token* next = tokenAt(current_ + 1);
        const Token* next2 = tokenAt(current_ + 2);
        if (token.lexeme == LangString(u"Math") && next && next->type == TokenType::DOT && next2
            && next2->type == TokenType::IDENT && next2->lexeme == LangString(u"PI")) {
            advance();
            advance();
            advance();
            return ast::number(3.141592653589793);
        }
        // member-then-call OR direct call -> parse as a chain (expression
        // context). See file header note: for 2+ dotted segments this
        // ultimately still fails inside parseCall (bug-for-bug parity).
        if ((next && next->type == TokenType::LPAREN) || hasCallAfterDot(current_)) {
            const JsonArray chain = parseChain(LangString(u"expression"));
            if (chain.size() == 1) return chain.at(0).toObject();
            JsonObject node;
            node.insert(LangString(u"type"), NodeKind::Chain);
            node.insert(LangString(u"chain"), chain);
            return node;
        }
        // dotted enum/member path (no call at the end).
        advance();
        LangStringList path;
        path.append(token.lexeme);
        while (peek().type == TokenType::DOT) {
            const Token* n = tokenAt(current_ + 1);
            if (!n) break;
            const Token* after = tokenAt(current_ + 2);
            if (after && after->type == TokenType::LPAREN) break; // dot begins a call
            if (!memberTokenTypes().contains(n->type)) {
                throw parserErrorAt(LangString(u"P001"), LangString(u"Expected identifier after '.'"), *n);
            }
            advance(); // consume '.'
            advance(); // consume segment token
            path.append(n->lexeme);
        }
        if (path.size() > 1) {
            JsonObject node;
            node.insert(LangString(u"type"), NodeKind::Member);
            JsonArray pathArr;
            for (const LangString& s : path) pathArr.append(s);
            node.insert(LangString(u"path"), pathArr);
            return node;
        }
        JsonObject node;
        node.insert(LangString(u"type"), NodeKind::Ident);
        node.insert(LangString(u"name"), path.first());
        return node;
    }
    if (tt == TokenType::OUTPUT_REF) {
        advance();
        return refNode(NodeKind::OutputRef, token.lexeme);
    }
    if (tt == TokenType::SOURCE_REF) {
        advance();
        return refNode(NodeKind::SourceRef, token.lexeme);
    }
    if (tt == TokenType::VOL_REF) {
        advance();
        return refNode(NodeKind::VolRef, token.lexeme);
    }
    if (tt == TokenType::GEO_REF) {
        advance();
        return refNode(NodeKind::GeoRef, token.lexeme);
    }
    if (tt == TokenType::XYZ_REF) {
        advance();
        return refNode(NodeKind::XyzRef, token.lexeme);
    }
    if (tt == TokenType::VEL_REF) {
        advance();
        return refNode(NodeKind::VelRef, token.lexeme);
    }
    if (tt == TokenType::RGBA_REF) {
        advance();
        return refNode(NodeKind::RgbaRef, token.lexeme);
    }
    if (tt == TokenType::MESH_REF) {
        advance();
        return refNode(NodeKind::MeshRef, token.lexeme);
    }
    if (tt == TokenType::LPAREN) {
        advance();
        const JsonObject expr = parseAdditive();
        expect(TokenType::RPAREN, LangString(u"Expect ')'"));
        return expr;
    }
    throw parserErrorAt(LangString(u"P001"), LangString(u"Unexpected token %1").arg(token.type), token);
}

} // namespace

Value parse(const std::vector<Token>& tokens) { return parse(tokens, Value()); }

Value parse(const std::vector<Token>& tokens, const Value& options) {
    LangVector<Token> tokenVec;
    tokenVec.assign(tokens.begin(), tokens.end());
    const JsonValue opts(options);
    const bool strictSubchain = opts.toObject().value(LangString(u"subchainArguments")).toString() == LangString(u"strict");
    Parser parser(std::move(tokenVec), strictSubchain);
    return parser.parseProgram().native();
}

} // namespace nm
