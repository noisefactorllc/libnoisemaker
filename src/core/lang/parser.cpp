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

const QStringList& validNamespaces() {
    static const QStringList list = {
        QStringLiteral("io"),      QStringLiteral("classicNoisedeck"), QStringLiteral("synth"),
        QStringLiteral("mixer"),   QStringLiteral("filter"),           QStringLiteral("render"),
        QStringLiteral("points"),  QStringLiteral("synth3d"),          QStringLiteral("filter3d"),
        QStringLiteral("user"),
    };
    return list;
}

bool isValidNamespaceName(const LangString& ns) {
    return validNamespaces().contains(ns);
}

// Token types that can begin an expression (reference exprStartTokens).
const QSet<LangString>& exprStartTokens() {
    static const QSet<LangString> s = {
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
const QSet<LangString>& memberTokenTypes() {
    static const QSet<LangString> s = {
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
const QSet<LangString>& namespaceTokenTypes() {
    static const QSet<LangString> s = {
        TokenType::IDENT,  TokenType::RENDER,   TokenType::WRITE,    TokenType::WRITE3D,
        TokenType::TRUE,   TokenType::FALSE,    TokenType::IF,       TokenType::ELIF,
        TokenType::ELSE,   TokenType::BREAK,    TokenType::CONTINUE, TokenType::RETURN,
    };
    return s;
}

JsonValue stripPrivatePos(const JsonValue& val) {
    if (val.isObject()) {
        JsonObject obj = val.toObject();
        obj.remove(QStringLiteral("_pos"));
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
    o.insert(QStringLiteral("type"), type);
    o.insert(QStringLiteral("name"), name);
    return o;
}

int hexPairToInt(const LangString& pair) {
    bool ok = false;
    const int v = pair.toInt(&ok, 16);
    return ok ? v : 0;
}

class Parser {
public:
    explicit Parser(QVector<Token> tokens, bool strictSubchainArguments = false)
        : tokens_(std::move(tokens)), strictSubchainArguments_(strictSubchainArguments) {}

    JsonObject parseProgram();

private:
    QVector<Token> tokens_;
    int current_ = 0;
    bool strictSubchainArguments_ = false;

    // Track the search order for the program (set by the search
    // directive -- REQUIRED). hasSearch_ mirrors the reference's
    // `programSearchOrder !== null`.
    bool hasSearch_ = false;
    QStringList searchOrder_;
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
            const JsonValue lv = pos.value(QStringLiteral("line"));
            const JsonValue cv = pos.value(QStringLiteral("column"));
            const JsonValue sv = pos.value(QStringLiteral("start"));
            const JsonValue ev = pos.value(QStringLiteral("end"));
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
        diagnostic.insert(QStringLiteral("code"), code);
        diagnostic.insert(QStringLiteral("stage"), diagStage(code));
        const LangString severity = severityOverride.isEmpty() ? LangString(diagSeverity(code)) : severityOverride;
        diagnostic.insert(QStringLiteral("severity"), severity);
        diagnostic.insert(QStringLiteral("message"), message);
        if (hasPosition) {
            JsonObject loc;
            loc.insert(QStringLiteral("line"), pLine);
            loc.insert(QStringLiteral("column"), pCol);
            diagnostic.insert(QStringLiteral("location"), loc);

            JsonObject span;
            span.insert(QStringLiteral("start"), pStart);
            span.insert(QStringLiteral("end"), pEnd);
            diagnostic.insert(QStringLiteral("span"), span);
        } else {
            if (hasLocation) {
                JsonObject loc;
                loc.insert(QStringLiteral("line"), line);
                loc.insert(QStringLiteral("column"), col);
                diagnostic.insert(QStringLiteral("location"), loc);
            } else {
                diagnostic.insert(QStringLiteral("location"), JsonValue(JsonValue::Null));
            }
            diagnostic.insert(QStringLiteral("span"), JsonValue(JsonValue::Null));
        }

        const int errLine = hasPosition ? pLine : (hasLocation ? line : -1);
        const int errCol = hasPosition ? pCol : (hasLocation ? col : -1);
        return DslSyntaxError(message, errLine, errCol, diagnostic.native());
    }

    DslSyntaxError parserError(const LangString& code, const LangString& message, const Token& t,
                               const LangString& severityOverride = LangString()) const {
        JsonObject pos;
        if (t.hasPosition) {
            pos.insert(QStringLiteral("line"), t.posLine);
            pos.insert(QStringLiteral("column"), t.posColumn);
            pos.insert(QStringLiteral("start"), t.posStart);
            pos.insert(QStringLiteral("end"), t.posEnd);
        }
        const bool hasLine = (t.line > 0 && (t.rawLine.empty() || t.hasLine));
        const bool hasCol = (t.col > 0 && (t.rawCol.empty() || t.hasCol));
        return makeParserError(code, message, t.line, t.col, hasLine, hasCol, pos, severityOverride);
    }

    DslSyntaxError parserErrorAt(const LangString& code, const LangString& core, const Token& t, const LangString& suffix = LangString()) const {
        const LangString lineStr = t.rawLine.empty() ? (t.line > 0 ? LangString::number(t.line) : QStringLiteral("undefined")) : t.rawLine;
        const LangString colStr = t.rawCol.empty() ? (t.col > 0 ? LangString::number(t.col) : QStringLiteral("undefined")) : t.rawCol;
        const LangString message = QStringLiteral("%1 at line %2 col %3%4").arg(core, lineStr, colStr, suffix);
        return parserError(code, message, t);
    }

    Token expect(const LangString& type, const LangString& msg) {
        const Token t = peek();
        if (t.type == type) return advance();
        const LangString code = (type == TokenType::RPAREN) ? QStringLiteral("P002") : QStringLiteral("P001");
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
                                        const QStringList& kwargOrder);
    JsonObject transformAudioInvocation(const JsonObject& call, const Token& nameToken,
                                         const QStringList& kwargOrder);
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
    expect(TokenType::LPAREN, QStringLiteral("Expect '('"));
    if (peek().type != TokenType::OUTPUT_REF) {
        // Reference throws with NO location suffix in error.message, but carries structured location.
        throw parserError(QStringLiteral("P005"), QStringLiteral("Expected output reference in render()"), peek());
    }
    JsonObject out = refNode(NodeKind::OutputRef, advance().lexeme);
    expect(TokenType::RPAREN, QStringLiteral("Expect ')'"));
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
                throw parserErrorAt(QStringLiteral("P004"),
                                    QStringLiteral("'search' directive must appear before other statements"), t);
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
                throw parserErrorAt(QStringLiteral("P005"), QStringLiteral("Duplicate render() directive"), t);
            }
            render = parseRenderDirective();
            hasRender = true;
            while (peek().type == TokenType::SEMICOLON) advance();
            if (!leadingComments.isEmpty()) {
                render.insert(QStringLiteral("leadingComments"), leadingComments);
            }
            const JsonArray trailing = collectComments();
            for (const JsonValue& c : trailing) trailingComments.append(c);
            break;
        }
        JsonObject stmt = parseStatement();
        if (!leadingComments.isEmpty()) {
            stmt.insert(QStringLiteral("leadingComments"), leadingComments);
        }
        if (stmt.value(QStringLiteral("type")).toString() == NodeKind::VarAssign) {
            vars.append(stmt);
        } else {
            plans.append(stmt);
        }
        while (peek().type == TokenType::SEMICOLON) advance();
    }
    const Token eof = expect(TokenType::EOF_, QStringLiteral("Expected end of input"));
    if (!hasSearch_ || searchOrder_.isEmpty()) {
        throw parserError(QStringLiteral("P004"), QStringLiteral(
            "Missing required 'search' directive. Every program must start with 'search <namespace>, ...' "
            "to specify namespace search order."), eof);
    }

    JsonObject program;
    program.insert(QStringLiteral("type"), NodeKind::Program);
    program.insert(QStringLiteral("plans"), plans);
    program.insert(QStringLiteral("render"), hasRender ? JsonValue(render) : JsonValue(JsonValue::Null));
    if (!vars.isEmpty()) program.insert(QStringLiteral("vars"), vars);
    if (!trailingComments.isEmpty()) program.insert(QStringLiteral("trailingComments"), trailingComments);

    JsonArray searchOrderJson;
    for (const LangString& ns : searchOrder_) searchOrderJson.append(ns);
    JsonObject namespaceMeta;
    namespaceMeta.insert(QStringLiteral("imports"), namespaceImports_);
    namespaceMeta.insert(QStringLiteral("default"),
                          hasNamespaceDefault_ ? JsonValue(namespaceDefault_) : JsonValue(JsonValue::Null));
    namespaceMeta.insert(QStringLiteral("searchOrder"), searchOrderJson);
    program.insert(QStringLiteral("namespace"), namespaceMeta);
    return stripPrivatePos(program).toObject();
}

void Parser::parseSearchDirective() {
    if (hasSearch_) {
        const Token t = peek();
        throw parserErrorAt(QStringLiteral("P004"),
                            QStringLiteral("Only one search directive is allowed per program"), t);
    }
    advance(); // consume 'search'
    QStringList namespaces;

    const Token first = peek();
    if (!namespaceTokenTypes().contains(first.type)) {
        throw parserErrorAt(QStringLiteral("P004"),
                            QStringLiteral("Expected namespace identifier after search"), first);
    }
    advance();
    validateNamespace(first);
    namespaces.append(first.lexeme);

    while (peek().type == TokenType::COMMA) {
        advance();
        const Token nsToken = peek();
        if (!namespaceTokenTypes().contains(nsToken.type)) {
            throw parserErrorAt(QStringLiteral("P004"),
                                QStringLiteral("Expected namespace identifier after comma"), nsToken);
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
        imp.insert(QStringLiteral("name"), nm);
        imp.insert(QStringLiteral("source"), QStringLiteral("search"));
        imp.insert(QStringLiteral("explicit"), true);
        namespaceImports_.append(imp);
    }
    namespaceDefault_ = JsonObject();
    namespaceDefault_.insert(QStringLiteral("name"), namespaces.first());
    namespaceDefault_.insert(QStringLiteral("source"), QStringLiteral("search"));
    namespaceDefault_.insert(QStringLiteral("explicit"), true);
    hasNamespaceDefault_ = true;

    while (peek().type == TokenType::SEMICOLON) advance();
}

void Parser::validateNamespace(const Token& token) {
    if (!isValidNamespaceName(token.lexeme)) {
        throw parserErrorAt(QStringLiteral("P004"),
                            QStringLiteral("Invalid namespace '%1'").arg(token.lexeme),
                            token,
                            QStringLiteral(". Valid namespaces: %1").arg(validNamespaces().join(QStringLiteral(", "))));
    }
}

JsonArray Parser::parseBlock() {
    expect(TokenType::LBRACE, QStringLiteral("Expect '{'"));
    JsonArray body;
    while (peek().type != TokenType::RBRACE) {
        body.append(parseStatement());
        while (peek().type == TokenType::SEMICOLON) advance();
    }
    expect(TokenType::RBRACE, QStringLiteral("Expect '}'"));
    return body;
}

JsonObject Parser::parseStatement() {
    if (peek().type == TokenType::SEARCH) {
        const Token t = peek();
        throw parserErrorAt(QStringLiteral("P004"),
                            QStringLiteral("'search' directive is only allowed at the start of the program"), t);
    }
    if (peek().type == TokenType::LET) {
        advance();
        const LangString name = expect(TokenType::IDENT, QStringLiteral("Expected identifier")).lexeme;
        expect(TokenType::EQUAL, QStringLiteral("Expect '='"));
        if (!exprStartTokens().contains(peek().type)) {
            const Token t = peek();
            throw parserErrorAt(QStringLiteral("P001"), QStringLiteral("Expected expression after '='"), t);
        }
        const JsonObject expr = parseAdditive();
        JsonObject node;
        node.insert(QStringLiteral("type"), NodeKind::VarAssign);
        node.insert(QStringLiteral("name"), name);
        node.insert(QStringLiteral("expr"), expr);
        return node;
    }

    const LangString tt = peek().type;
    if (tt == TokenType::IF) {
        advance();
        expect(TokenType::LPAREN, QStringLiteral("Expect '('"));
        const JsonObject condition = parseAdditive();
        expect(TokenType::RPAREN, QStringLiteral("Expect ')'"));
        const JsonArray thenBlock = parseBlock();
        JsonArray elifList;
        while (peek().type == TokenType::ELIF) {
            advance();
            expect(TokenType::LPAREN, QStringLiteral("Expect '('"));
            const JsonObject ec = parseAdditive();
            expect(TokenType::RPAREN, QStringLiteral("Expect ')'"));
            const JsonArray body = parseBlock();
            JsonObject elifEntry;
            elifEntry.insert(QStringLiteral("condition"), ec);
            elifEntry.insert(QStringLiteral("then"), body);
            elifList.append(elifEntry);
        }
        JsonValue elseBranch = JsonValue(JsonValue::Null);
        if (peek().type == TokenType::ELSE) {
            advance();
            elseBranch = parseBlock();
        }
        JsonObject node;
        node.insert(QStringLiteral("type"), NodeKind::IfStmt);
        node.insert(QStringLiteral("condition"), condition);
        node.insert(QStringLiteral("then"), thenBlock);
        node.insert(QStringLiteral("elif"), elifList);
        node.insert(QStringLiteral("else"), elseBranch);
        return node;
    }
    if (tt == TokenType::BREAK) {
        advance();
        JsonObject node;
        node.insert(QStringLiteral("type"), NodeKind::Break);
        return node;
    }
    if (tt == TokenType::CONTINUE) {
        advance();
        JsonObject node;
        node.insert(QStringLiteral("type"), NodeKind::Continue);
        return node;
    }
    if (tt == TokenType::RETURN) {
        advance();
        JsonObject node;
        node.insert(QStringLiteral("type"), NodeKind::Return);
        if (exprStartTokens().contains(peek().type)) {
            node.insert(QStringLiteral("value"), parseAdditive());
        }
        return node;
    }

    const JsonArray chain = parseChain(QStringLiteral("statement"));
    // Extract write/write3d only if the chain TERMINATES with a
    // Write/Write3D node -- mid-chain writes don't count as terminal.
    JsonValue write = JsonValue(JsonValue::Null);
    JsonValue write3d = JsonValue(JsonValue::Null);
    if (!chain.isEmpty()) {
        const JsonObject lastNode = chain.last().toObject();
        const LangString lastType = lastNode.value(QStringLiteral("type")).toString();
        if (lastType == NodeKind::Write) {
            write = lastNode.value(QStringLiteral("surface"));
        } else if (lastType == NodeKind::Write3D) {
            JsonObject w3;
            w3.insert(QStringLiteral("tex3d"), lastNode.value(QStringLiteral("tex3d")));
            w3.insert(QStringLiteral("geo"), lastNode.value(QStringLiteral("geo")));
            write3d = w3;
        }
    }
    // NOTE: this wrapper deliberately has NO "type" key (identified by
    // the "chain" key alone) -- matches the reference exactly.
    JsonObject stmt;
    stmt.insert(QStringLiteral("chain"), chain);
    stmt.insert(QStringLiteral("write"), write);
    stmt.insert(QStringLiteral("write3d"), write3d);
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
            if (context == QStringLiteral("expression")) {
                const Token t = peek();
                throw parserErrorAt(QStringLiteral("P005"),
                                    QStringLiteral("'.write()' is only allowed in statement context"), t);
            }
            JsonObject writeNode = parseWriteCall();
            if (!allComments.isEmpty()) writeNode.insert(QStringLiteral("leadingComments"), allComments);
            calls.append(writeNode);
            continue;
        }
        if (nextType == TokenType::SUBCHAIN) {
            JsonObject subchainNode = parseSubchainCall();
            if (!allComments.isEmpty()) subchainNode.insert(QStringLiteral("leadingComments"), allComments);
            calls.append(subchainNode);
            continue;
        }
        JsonObject call = parseCall();
        if (!allComments.isEmpty()) call.insert(QStringLiteral("leadingComments"), allComments);
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
        expect(TokenType::LPAREN, QStringLiteral("Expect '('"));
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
        } else if (pt == TokenType::IDENT && peek().lexeme == QStringLiteral("none")) {
            // "none" is a valid target meaning "don't write to any surface".
            surface = refNode(NodeKind::OutputRef, advance().lexeme);
        } else {
            const Token p = peek();
            throw parserErrorAt(
                QStringLiteral("P005"),
                QStringLiteral(
                    "write() requires an explicit surface reference (e.g., o0, o1, xyz0, vel0, rgba0, mesh0, none)"),
                p);
        }
        expect(TokenType::RPAREN, QStringLiteral("Expect ')'"));
        JsonObject node;
        node.insert(QStringLiteral("type"), NodeKind::Write);
        node.insert(QStringLiteral("surface"), surface);
        node.insert(QStringLiteral("loc"), ast::loc(tokenLine, tokenCol));
        return node;
    }
    if (tokenType == TokenType::WRITE3D) {
        advance(); // consume 'write3d'
        expect(TokenType::LPAREN, QStringLiteral("Expect '('"));
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
            throw parserErrorAt(QStringLiteral("P005"), QStringLiteral("Expected tex3d reference in write3d()"), p);
        }
        expect(TokenType::COMMA, QStringLiteral("Expect ',' between tex3d and geo in write3d()"));
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
            throw parserErrorAt(QStringLiteral("P005"), QStringLiteral("Expected geo reference in write3d()"), p);
        }
        expect(TokenType::RPAREN, QStringLiteral("Expect ')'"));
        JsonObject node;
        node.insert(QStringLiteral("type"), NodeKind::Write3D);
        node.insert(QStringLiteral("tex3d"), tex3d);
        node.insert(QStringLiteral("geo"), geo);
        node.insert(QStringLiteral("loc"), ast::loc(tokenLine, tokenCol));
        return node;
    }
    Token dummy;
    dummy.line = tokenLine;
    dummy.col = tokenCol;
    dummy.hasLine = (tokenLine > 0);
    dummy.hasCol = (tokenCol > 0);
    throw parserError(QStringLiteral("P005"),
                      QStringLiteral("Expected write or write3d at line %1 col %2").arg(tokenLine).arg(tokenCol),
                      dummy);
}

JsonObject Parser::parseSubchainCall() {
    const Token nameToken = peek();

    advance(); // consume 'subchain'
    expect(TokenType::LPAREN, QStringLiteral("Expect '(' after subchain"));

    // Machine-readable subchain-argument reports. Order follows the
    // offending token in the source. Default mode collects them onto the
    // Subchain node as non-enumerable metadata (surfaced by validate());
    // strict mode throws with the same codes.
    JsonArray argDiagnostics;
    auto reportArgIssue = [&](const LangString& code, const LangString& message, const Token& token) {
        if (strictSubchainArguments_) {
            throw parserError(code, message, token, QStringLiteral("error"));
        }
        JsonObject report;
        report.insert(QStringLiteral("code"), code);
        report.insert(QStringLiteral("message"), message);
        report.insert(QStringLiteral("severity"), diagSeverity(code));
        if (token.hasPosition) {
            JsonObject loc;
            loc.insert(QStringLiteral("line"), token.posLine);
            loc.insert(QStringLiteral("column"), token.posColumn);
            report.insert(QStringLiteral("location"), loc);

            JsonObject span;
            span.insert(QStringLiteral("start"), token.posStart);
            span.insert(QStringLiteral("end"), token.posEnd);
            report.insert(QStringLiteral("span"), span);
        } else if (token.line > 0 && token.col > 0 && (token.rawLine.empty() || token.hasLine) && (token.rawCol.empty() || token.hasCol)) {
            JsonObject loc;
            loc.insert(QStringLiteral("line"), token.line);
            loc.insert(QStringLiteral("column"), token.col);
            report.insert(QStringLiteral("location"), loc);
        }
        argDiagnostics.append(report);
    };

    static const QStringList subchainKeys = { QStringLiteral("name"), QStringLiteral("id") };

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
            nameVal.insert(QStringLiteral("type"), NodeKind::String);
            nameVal.insert(QStringLiteral("value"), advance().lexeme);
            kwargs.insert(QStringLiteral("name"), nameVal);
        } else if (peek().type == TokenType::IDENT && typeAt(current_ + 1) == TokenType::COLON) {
            // Keyword arguments: subchain(name: "...", id: "...")
            while (peek().type == TokenType::IDENT && typeAt(current_ + 1) == TokenType::COLON) {
                const Token keyToken = advance();
                const LangString key = keyToken.lexeme;
                advance(); // consume ':'
                if (peek().type != TokenType::STRING) {
                    throw parserErrorAt(QStringLiteral("P006"), QStringLiteral("Expected string value for subchain %1").arg(key),
                                        peek());
                }
                const LangString value = advance().lexeme;
                if (!subchainKeys.contains(key)) {
                    reportArgIssue(
                        QStringLiteral("P008"),
                        QStringLiteral("Unknown subchain argument '%1' at line %2 col %3. Valid keys: name, id. The value is discarded.")
                            .arg(key).arg(keyToken.line).arg(keyToken.col),
                        keyToken);
                } else if (kwargs.contains(key)) {
                    reportArgIssue(
                        QStringLiteral("P009"),
                        QStringLiteral("Duplicate subchain argument '%1' at line %2 col %3. The last value wins.")
                            .arg(key).arg(keyToken.line).arg(keyToken.col),
                        keyToken);
                }
                JsonObject val;
                val.insert(QStringLiteral("type"), NodeKind::String);
                val.insert(QStringLiteral("value"), value);
                kwargs.insert(key, val);
                if (peek().type == TokenType::COMMA) {
                    advance(); // consume ','
                } else if (peek().type == TokenType::IDENT && typeAt(current_ + 1) == TokenType::COLON) {
                    reportArgIssue(
                        QStringLiteral("P010"),
                        QStringLiteral("Missing ',' between subchain arguments at line %1 col %2")
                            .arg(peek().line).arg(peek().col),
                        peek());
                }
            }
        }
    }
    expect(TokenType::RPAREN, QStringLiteral("Expect ')' after subchain arguments"));
    expect(TokenType::LBRACE, QStringLiteral("Expect '{' to start subchain body"));

    JsonArray body;
    while (peek().type != TokenType::RBRACE) {
        const JsonArray leadingComments = collectComments();
        if (peek().type == TokenType::RBRACE) break;
        if (peek().type != TokenType::DOT) {
            throw parserErrorAt(QStringLiteral("P006"), QStringLiteral("Expected '.' before chain element in subchain body"),
                                peek());
        }
        advance(); // consume '.'
        const JsonArray postDotComments = collectComments();
        JsonArray allComments;
        for (const JsonValue& c : leadingComments) allComments.append(c);
        for (const JsonValue& c : postDotComments) allComments.append(c);
        JsonObject call = parseCall();
        if (!allComments.isEmpty()) call.insert(QStringLiteral("leadingComments"), allComments);
        body.append(call);
    }
    expect(TokenType::RBRACE, QStringLiteral("Expect '}' to end subchain body"));

    if (body.isEmpty()) {
        throw parserErrorAt(QStringLiteral("P006"), QStringLiteral("Subchain body cannot be empty"), nameToken);
    }

    // Reference: `kwargs.name?.value || null` -- a FALSY-OR, so an
    // explicitly empty-string value ALSO becomes null, not "". Ported
    // verbatim (see file header note); do not "fix" this.
    auto resolveFalsyStringOrNull = [&](const LangString& key) -> JsonValue {
        if (!kwargs.contains(key)) return JsonValue(JsonValue::Null);
        const LangString value = kwargs.value(key).toObject().value(QStringLiteral("value")).toString();
        return value.isEmpty() ? JsonValue(JsonValue::Null) : JsonValue(value);
    };

    JsonObject node;
    node.insert(QStringLiteral("type"), NodeKind::Subchain);
    node.insert(QStringLiteral("name"), resolveFalsyStringOrNull(QStringLiteral("name")));
    node.insert(QStringLiteral("id"), resolveFalsyStringOrNull(QStringLiteral("id")));
    node.insert(QStringLiteral("body"), body);
    node.insert(QStringLiteral("loc"), ast::loc(nameToken.line, nameToken.col));
    if (!argDiagnostics.isEmpty()) {
        node.insert(QStringLiteral("subchainArgumentDiagnostics"), argDiagnostics);
    }
    return node;
}

JsonObject Parser::parseCall() {
    const Token nameToken = expect(TokenType::IDENT, QStringLiteral("Expected identifier"));
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
                    QStringLiteral("P007"),
                    QStringLiteral("Inline namespace syntax '%1.%2()' is not allowed. Use 'search %1' at the start "
                                   "of the program instead,").arg(nameToken.lexeme, next->lexeme),
                    nameToken);
            }
        }
    }
    expect(TokenType::LPAREN, QStringLiteral("Expect '('"));
    JsonArray args;
    JsonObject kwargs;
    QStringList kwargOrder;
    bool keyword = false;
    bool positional = false;
    const bool allowMixed = nameToken.lexeme == QStringLiteral("midi")
        || nameToken.lexeme == QStringLiteral("audio");
    if (peek().type != TokenType::RPAREN) {
        while (true) {
            if (peek().type == TokenType::IDENT && typeAt(current_ + 1) == TokenType::COLON) {
                if (positional && !allowMixed) {
                    const Token t = peek();
                    throw parserErrorAt(QStringLiteral("P007"), QStringLiteral("Cannot mix positional and keyword arguments"), t);
                }
                keyword = true;
                const LangString kwargName = peek().lexeme;
                parseKwarg(kwargs);
                kwargOrder.append(kwargName);
            } else {
                if (keyword && !allowMixed) {
                    const Token t = peek();
                    throw parserErrorAt(QStringLiteral("P007"), QStringLiteral("Cannot mix positional and keyword arguments"), t);
                }
                positional = true;
                args.append(parseArg());
            }
            if (peek().type != TokenType::COMMA) break;
            advance();
            if (peek().type == TokenType::RPAREN) break;
        }
    }
    expect(TokenType::RPAREN, QStringLiteral("Expect ')'"));

    JsonObject call;
    call.insert(QStringLiteral("type"), NodeKind::Call);
    call.insert(QStringLiteral("name"), nameToken.lexeme);
    call.insert(QStringLiteral("args"), args);
    if (keyword) call.insert(QStringLiteral("kwargs"), kwargs);

    const LangString lexeme = nameToken.lexeme;
    if (lexeme == QStringLiteral("from")) {
        return transformFromInvocation(call, nameToken);
    }
    // osc() as a value oscillator (not the synth.osc generator effect) --
    // 4-way heuristic, checked in this order.
    if (lexeme == QStringLiteral("osc")) {
        static const QSet<LangString> oscKwargKeys = {
            QStringLiteral("type"), QStringLiteral("min"),    QStringLiteral("max"),
            QStringLiteral("speed"), QStringLiteral("offset"), QStringLiteral("seed"),
        };
        const bool hasTypeKwarg = kwargs.contains(QStringLiteral("type"));
        const bool firstArgIsOscKind = !args.isEmpty()
            && args.at(0).toObject().value(QStringLiteral("type")).toString() == NodeKind::Member
            && !args.at(0).toObject().value(QStringLiteral("path")).toArray().isEmpty()
            && args.at(0).toObject().value(QStringLiteral("path")).toArray().at(0).toString()
                   == QStringLiteral("oscKind");
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
    if (lexeme == QStringLiteral("midi")) {
        return transformMidiInvocation(call, nameToken, kwargOrder);
    }
    if (lexeme == QStringLiteral("audio")) {
        return transformAudioInvocation(call, nameToken, kwargOrder);
    }
    // read()/read3d() are pipeline built-ins. The raw Read AST owns an
    // undefined surface when no argument resolves; JSON omits that member.
    if (lexeme == QStringLiteral("read")) {
        JsonObject node;
        node.insert(QStringLiteral("type"), NodeKind::Read);
        if (!args.isEmpty()) {
            node.insert(QStringLiteral("surface"), args.at(0));
        } else if (kwargs.contains(QStringLiteral("tex"))) {
            node.insert(QStringLiteral("surface"), kwargs.value(QStringLiteral("tex")));
        } else if (kwargs.contains(QStringLiteral("surface"))) {
            node.insert(QStringLiteral("surface"), kwargs.value(QStringLiteral("surface")));
        } else {
            node.insert(QStringLiteral("surface"), JsonValue());
        }
        node.insert(QStringLiteral("loc"), ast::loc(nameToken.line, nameToken.col));
        const JsonObject skip = kwargs.value(QStringLiteral("_skip")).toObject();
        if (skip.value(QStringLiteral("type")).toString() == NodeKind::Boolean
            && skip.value(QStringLiteral("value")).toBool()) {
            node.insert(QStringLiteral("_skip"), true);
        }
        return node;
    }
    if (lexeme == QStringLiteral("read3d")) {
        JsonObject node;
        node.insert(QStringLiteral("type"), NodeKind::Read3D);
        if (!args.isEmpty()) {
            node.insert(QStringLiteral("tex3d"), args.at(0));
        } else if (kwargs.contains(QStringLiteral("tex3d"))) {
            node.insert(QStringLiteral("tex3d"), kwargs.value(QStringLiteral("tex3d")));
        }
        JsonValue geo;
        if (args.size() > 1) {
            geo = args.at(1);
        } else if (kwargs.contains(QStringLiteral("geo"))) {
            geo = kwargs.value(QStringLiteral("geo"));
        }
        node.insert(QStringLiteral("geo"), geo.isUndefined() ? JsonValue(JsonValue::Null) : geo);
        node.insert(QStringLiteral("loc"), ast::loc(nameToken.line, nameToken.col));
        const JsonObject skip = kwargs.value(QStringLiteral("_skip")).toObject();
        if (skip.value(QStringLiteral("type")).toString() == NodeKind::Boolean
            && skip.value(QStringLiteral("value")).toBool()) {
            node.insert(QStringLiteral("_skip"), true);
        }
        return node;
    }
    return call;
}

JsonObject Parser::transformOscInvocation(const JsonObject& call, const Token& nameToken) {
    const JsonArray args = call.value(QStringLiteral("args")).toArray();
    const JsonObject kwargs = call.value(QStringLiteral("kwargs")).toObject();
    static const QStringList paramOrder = {
        QStringLiteral("type"), QStringLiteral("min"),    QStringLiteral("max"),
        QStringLiteral("speed"), QStringLiteral("offset"), QStringLiteral("seed"),
    };

    for (const LangString& key : kwargs.keys()) {
        if (!paramOrder.contains(key)) {
            throw parserErrorAt(QStringLiteral("P003"),
                                QStringLiteral("osc() unknown parameter '%1'").arg(key),
                                nameToken,
                                QStringLiteral(". Valid: %1").arg(paramOrder.join(QStringLiteral(", "))));
        }
    }

    auto resolve = [&](const LangString& name, int index, const JsonValue& dflt) -> JsonValue {
        if (kwargs.contains(name)) return kwargs.value(name);
        if (index < args.size()) return args.at(index);
        return dflt;
    };

    JsonObject node;
    node.insert(QStringLiteral("type"), NodeKind::Oscillator);
    node.insert(QStringLiteral("oscType"),
                resolve(QStringLiteral("type"), 0, ast::memberOf(QStringLiteral("oscKind"), QStringLiteral("sine"))));
    node.insert(QStringLiteral("min"), resolve(QStringLiteral("min"), 1, ast::number(0)));
    node.insert(QStringLiteral("max"), resolve(QStringLiteral("max"), 2, ast::number(1)));
    node.insert(QStringLiteral("speed"), resolve(QStringLiteral("speed"), 3, ast::number(1)));
    node.insert(QStringLiteral("offset"), resolve(QStringLiteral("offset"), 4, ast::number(0)));
    node.insert(QStringLiteral("seed"), resolve(QStringLiteral("seed"), 5, ast::number(1)));
    node.insert(QStringLiteral("loc"), ast::loc(nameToken.line, nameToken.col));
    return node;
}

JsonObject Parser::transformMidiInvocation(const JsonObject& call, const Token& nameToken,
                                            const QStringList& kwargOrder) {
    const JsonArray args = call.value(QStringLiteral("args")).toArray();
    const JsonObject kwargs = call.value(QStringLiteral("kwargs")).toObject();

    static const QStringList paramOrder = {
        QStringLiteral("channel"), QStringLiteral("mode"), QStringLiteral("min"),
        QStringLiteral("max"), QStringLiteral("sensitivity"),
    };
    static const QStringList keywordOnlyParams = {
        QStringLiteral("name"), QStringLiteral("id"), QStringLiteral("cc"),
        QStringLiteral("nrpn"), QStringLiteral("zone"), QStringLiteral("members"),
    };
    QStringList validParams = paramOrder;
    validParams.append(keywordOnlyParams);
    if (args.size() > paramOrder.size()) {
        throw parserErrorAt(QStringLiteral("P003"),
                            QStringLiteral("midi() name, id, cc, nrpn, zone and members are keyword-only"),
                            nameToken);
    }
    for (const LangString& key : kwargOrder) {
        if (!validParams.contains(key)) {
            throw parserErrorAt(QStringLiteral("P003"),
                                QStringLiteral("midi() unknown parameter '%1'").arg(key),
                                nameToken,
                                QStringLiteral(". Valid: %1").arg(validParams.join(QStringLiteral(", "))));
        }
    }

    const JsonObject defaults = {
        {QStringLiteral("mode"), ast::memberOf(QStringLiteral("midiMode"), QStringLiteral("velocity"))},
        {QStringLiteral("min"), ast::number(0)},
        {QStringLiteral("max"), ast::number(1)},
        {QStringLiteral("sensitivity"), ast::number(1)},
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
        throw parserErrorAt(QStringLiteral("P003"),
                            QStringLiteral("midi() has an excess positional argument"),
                            nameToken);
    }

    const JsonValue channel = resolved.value(QStringLiteral("channel"));
    if (channel.isUndefined() && !kwargs.contains(QStringLiteral("zone"))) {
        throw parserErrorAt(QStringLiteral("P003"),
                            QStringLiteral("midi() requires 'channel' or 'zone' argument"),
                            nameToken);
    }
    if (!channel.isUndefined() && kwargs.contains(QStringLiteral("zone"))) {
        throw parserErrorAt(QStringLiteral("P003"),
                            QStringLiteral("midi() 'channel' and 'zone' are mutually exclusive"),
                            nameToken);
    }
    if (kwargs.contains(QStringLiteral("members")) && !kwargs.contains(QStringLiteral("zone"))) {
        throw parserErrorAt(QStringLiteral("P003"),
                            QStringLiteral("midi() 'members' requires 'zone'"),
                            nameToken);
    }
    if (kwargs.contains(QStringLiteral("id")) && !kwargs.contains(QStringLiteral("name"))) {
        throw parserErrorAt(QStringLiteral("P003"),
                            QStringLiteral("midi() 'id' requires readable 'name'"),
                            nameToken);
    }
    for (const LangString& paramName : {QStringLiteral("name"), QStringLiteral("id")}) {
        if (!kwargs.contains(paramName)) continue;
        const JsonObject value = kwargs.value(paramName).toObject();
        if (value.value(QStringLiteral("type")).toString() != NodeKind::String) {
            throw parserErrorAt(QStringLiteral("P003"),
                                QStringLiteral("midi() '%1' requires a quoted string").arg(paramName),
                                nameToken);
        }
        if (value.value(QStringLiteral("value")).toString().isEmpty()) {
            throw parserErrorAt(QStringLiteral("P003"),
                                QStringLiteral("midi() '%1' must not be empty").arg(paramName),
                                nameToken);
        }
    }

    JsonObject node;
    node.insert(QStringLiteral("type"), NodeKind::Midi);
    node.insert(QStringLiteral("channel"), channel);
    node.insert(QStringLiteral("mode"), resolved.value(QStringLiteral("mode")));
    node.insert(QStringLiteral("min"), resolved.value(QStringLiteral("min")));
    node.insert(QStringLiteral("max"), resolved.value(QStringLiteral("max")));
    node.insert(QStringLiteral("sensitivity"), resolved.value(QStringLiteral("sensitivity")));
    for (const LangString& field : {QStringLiteral("cc"), QStringLiteral("nrpn"), QStringLiteral("zone"),
                                    QStringLiteral("members"), QStringLiteral("name"), QStringLiteral("id")}) {
        node.insert(field, kwargs.value(field));
    }
    node.insert(QStringLiteral("loc"), ast::loc(nameToken.line, nameToken.col));
    return node;
}

JsonObject Parser::transformAudioInvocation(const JsonObject& call, const Token& nameToken,
                                             const QStringList& kwargOrder) {
    const JsonArray args = call.value(QStringLiteral("args")).toArray();
    const JsonObject kwargs = call.value(QStringLiteral("kwargs")).toObject();

    static const QStringList paramOrder = {
        QStringLiteral("band"), QStringLiteral("min"), QStringLiteral("max"),
    };
    static const QStringList keywordOnlyParams = {
        QStringLiteral("channel"), QStringLiteral("name"), QStringLiteral("id"),
    };
    QStringList validParams = paramOrder;
    validParams.append(keywordOnlyParams);
    if (args.size() > paramOrder.size()) {
        throw parserErrorAt(QStringLiteral("P003"),
                            QStringLiteral("audio() channel, name and id are keyword-only"),
                            nameToken);
    }
    for (const LangString& key : kwargOrder) {
        if (!validParams.contains(key)) {
            throw parserErrorAt(QStringLiteral("P003"),
                                QStringLiteral("audio() unknown parameter '%1'").arg(key),
                                nameToken,
                                QStringLiteral(". Valid: %1").arg(validParams.join(QStringLiteral(", "))));
        }
    }

    const JsonObject defaults = {
        {QStringLiteral("min"), ast::number(0)},
        {QStringLiteral("max"), ast::number(1)},
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
        throw parserErrorAt(QStringLiteral("P003"),
                            QStringLiteral("audio() has an excess positional argument"),
                            nameToken);
    }

    const JsonValue band = resolved.value(QStringLiteral("band"));
    if (band.isUndefined()) {
        throw parserErrorAt(QStringLiteral("P003"),
                            QStringLiteral("audio() requires 'band' argument"),
                            nameToken);
    }
    if (kwargs.contains(QStringLiteral("id")) && !kwargs.contains(QStringLiteral("name"))) {
        throw parserErrorAt(QStringLiteral("P003"),
                            QStringLiteral("audio() 'id' requires readable 'name'"),
                            nameToken);
    }
    if (kwargs.contains(QStringLiteral("name")) && !kwargs.contains(QStringLiteral("channel"))) {
        throw parserErrorAt(QStringLiteral("P003"),
                            QStringLiteral("audio() selected device requires both 'name' and 'channel'"),
                            nameToken);
    }
    for (const LangString& paramName : {QStringLiteral("name"), QStringLiteral("id")}) {
        if (!kwargs.contains(paramName)) continue;
        const JsonObject value = kwargs.value(paramName).toObject();
        if (value.value(QStringLiteral("type")).toString() != NodeKind::String) {
            throw parserErrorAt(QStringLiteral("P003"),
                                QStringLiteral("audio() '%1' requires a quoted string").arg(paramName),
                                nameToken);
        }
        if (value.value(QStringLiteral("value")).toString().isEmpty()) {
            throw parserErrorAt(QStringLiteral("P003"),
                                QStringLiteral("audio() '%1' must not be empty").arg(paramName),
                                nameToken);
        }
    }

    JsonObject node;
    node.insert(QStringLiteral("type"), NodeKind::Audio);
    node.insert(QStringLiteral("band"), band);
    node.insert(QStringLiteral("min"), resolved.value(QStringLiteral("min")));
    node.insert(QStringLiteral("max"), resolved.value(QStringLiteral("max")));
    node.insert(QStringLiteral("channel"), kwargs.value(QStringLiteral("channel")));
    node.insert(QStringLiteral("name"), kwargs.value(QStringLiteral("name")));
    node.insert(QStringLiteral("id"), kwargs.value(QStringLiteral("id")));
    node.insert(QStringLiteral("loc"), ast::loc(nameToken.line, nameToken.col));
    return node;
}

JsonObject Parser::transformFromInvocation(const JsonObject& call, const Token& nameToken) {
    auto fail = [&](const LangString& message) {
        const bool hasCoordinates = (nameToken.line > 0 && nameToken.col > 0
            && (nameToken.rawLine.empty() || nameToken.hasLine)
            && (nameToken.rawCol.empty() || nameToken.hasCol));
        if (hasCoordinates) {
            throw parserErrorAt(QStringLiteral("P007"), message, nameToken);
        }
        throw parserError(QStringLiteral("P007"), message, nameToken);
    };

    const JsonObject kwargs = call.value(QStringLiteral("kwargs")).toObject();
    if (!kwargs.isEmpty()) {
        fail(QStringLiteral("'from' does not support named arguments"));
    }
    const JsonArray args = call.value(QStringLiteral("args")).toArray();
    if (args.size() != 2) {
        fail(QStringLiteral("'from' requires exactly two arguments (namespace, call)"));
    }
    const JsonObject namespaceArg = args.at(0).toObject();
    const JsonObject targetArg = args.at(1).toObject();
    const LangString namespaceArgType = namespaceArg.value(QStringLiteral("type")).toString();
    if (namespaceArgType != NodeKind::Ident && namespaceArgType != NodeKind::Member) {
        fail(QStringLiteral("'from' namespace argument must be an identifier"));
    }
    LangString namespaceName;
    if (namespaceArgType == NodeKind::Member) {
        QStringList segs;
        for (const JsonValue& v : namespaceArg.value(QStringLiteral("path")).toArray()) segs.append(v.toString());
        namespaceName = segs.join(QLatin1Char('.'));
    } else {
        namespaceName = namespaceArg.value(QStringLiteral("name")).toString();
    }
    if (namespaceName.isEmpty()) {
        fail(QStringLiteral("'from' namespace argument must be non-empty"));
    }

    JsonObject targetCall;
    bool haveTargetCall = false;
    const LangString targetType = targetArg.value(QStringLiteral("type")).toString();
    if (targetType == NodeKind::Call) {
        targetCall = targetArg;
        haveTargetCall = true;
    } else if (targetType == NodeKind::Chain) {
        // Unreachable in practice: parsePrimary always unwraps a
        // length-1 chain to its single element directly, so a Chain node
        // here can only ever have 2+ elements. Ported anyway for fidelity
        // with the reference's own defensive check.
        const JsonArray innerChain = targetArg.value(QStringLiteral("chain")).toArray();
        if (innerChain.size() == 1) {
            const JsonObject head = innerChain.at(0).toObject();
            if (head.value(QStringLiteral("type")).toString() == NodeKind::Call) {
                targetCall = head;
                haveTargetCall = true;
            }
        }
    }
    if (!haveTargetCall) {
        fail(QStringLiteral("'from' second argument must be a call expression"));
    }

    JsonObject replacement;
    replacement.insert(QStringLiteral("type"), NodeKind::Call);
    replacement.insert(QStringLiteral("name"), targetCall.value(QStringLiteral("name")));
    replacement.insert(QStringLiteral("args"), targetCall.value(QStringLiteral("args")).toArray());
    if (targetCall.contains(QStringLiteral("kwargs"))) {
        replacement.insert(QStringLiteral("kwargs"), targetCall.value(QStringLiteral("kwargs")).toObject());
    }
    JsonObject overrideNamespace;
    overrideNamespace.insert(QStringLiteral("name"), namespaceName);
    JsonArray pathArr;
    pathArr.append(namespaceName);
    overrideNamespace.insert(QStringLiteral("path"), pathArr);
    overrideNamespace.insert(QStringLiteral("explicit"), true);
    overrideNamespace.insert(QStringLiteral("source"), QStringLiteral("from"));
    overrideNamespace.insert(QStringLiteral("resolved"), namespaceName);
    JsonArray searchOrderArr;
    searchOrderArr.append(namespaceName);
    overrideNamespace.insert(QStringLiteral("searchOrder"), searchOrderArr);
    overrideNamespace.insert(QStringLiteral("fromOverride"), true);
    replacement.insert(QStringLiteral("namespace"), overrideNamespace);
    return replacement;
}

void Parser::parseKwarg(JsonObject& obj) {
    const LangString key = expect(TokenType::IDENT, QStringLiteral("Expected identifier")).lexeme;
    expect(TokenType::COLON, QStringLiteral("Expect ':'"));
    if (!exprStartTokens().contains(peek().type)) {
        // NOTE: reference message says "after '='" even though a kwarg
        // uses ':' -- a copy-paste artifact in the source of truth, kept
        // verbatim (never "clean up" a reference wording quirk).
        const Token t = peek();
        throw parserErrorAt(QStringLiteral("P001"), QStringLiteral("Expected expression after '='"), t);
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
    if (node.value(QStringLiteral("type")).toString() != NodeKind::Number) {
        // Number coercion failures locate the offending AST node's private
        // source position when present, its parser-authored loc otherwise,
        // and are explicitly null when neither carries valid coordinates.
        JsonObject pos = node.value(QStringLiteral("_pos")).toObject();
        int line = -1;
        int col = -1;
        bool hasLine = false;
        bool hasCol = false;
        if (node.contains(QStringLiteral("loc"))) {
            const JsonObject loc = node.value(QStringLiteral("loc")).toObject();
            if (loc.contains(QStringLiteral("line")) && loc.value(QStringLiteral("line")).isDouble()) {
                line = loc.value(QStringLiteral("line")).toInt();
                hasLine = (line > 0);
            }
            if (loc.contains(QStringLiteral("col")) && loc.value(QStringLiteral("col")).isDouble()) {
                col = loc.value(QStringLiteral("col")).toInt();
                hasCol = (col > 0);
            }
        }
        throw makeParserError(QStringLiteral("P001"), QStringLiteral("Expected number"),
                              line, col, hasLine, hasCol, pos);
    }
    return node.value(QStringLiteral("value")).toDouble();
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
        node.insert(QStringLiteral("type"), NodeKind::String);
        node.insert(QStringLiteral("value"), token.lexeme);
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
        node.insert(QStringLiteral("type"), NodeKind::Color);
        JsonArray value;
        value.append(r / 255.0);
        value.append(g / 255.0);
        value.append(b / 255.0);
        value.append(a);
        node.insert(QStringLiteral("value"), value);
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
            throw parserErrorAt(QStringLiteral("P001"), QStringLiteral("Expected ']'"), t);
        }
        advance();
        JsonObject node;
        node.insert(QStringLiteral("type"), NodeKind::ArrayLiteral);
        node.insert(QStringLiteral("elements"), elements);
        node.insert(QStringLiteral("loc"), ast::loc(startLine, startCol));
        if (token.hasPosition) {
            JsonObject pos;
            pos.insert(QStringLiteral("line"), token.posLine);
            pos.insert(QStringLiteral("column"), token.posColumn);
            pos.insert(QStringLiteral("start"), token.posStart);
            pos.insert(QStringLiteral("end"), token.posEnd);
            node.insert(QStringLiteral("_pos"), pos);
        }
        return node;
    }
    if (tt == TokenType::FUNC) {
        advance();
        JsonObject node;
        node.insert(QStringLiteral("type"), NodeKind::Func);
        node.insert(QStringLiteral("src"), token.lexeme);
        return node;
    }
    if (tt == TokenType::TRUE) {
        advance();
        JsonObject node;
        node.insert(QStringLiteral("type"), NodeKind::Boolean);
        node.insert(QStringLiteral("value"), true);
        return node;
    }
    if (tt == TokenType::FALSE) {
        advance();
        JsonObject node;
        node.insert(QStringLiteral("type"), NodeKind::Boolean);
        node.insert(QStringLiteral("value"), false);
        return node;
    }
    if (tt == TokenType::IDENT) {
        const Token* next = tokenAt(current_ + 1);
        const Token* next2 = tokenAt(current_ + 2);
        if (token.lexeme == QStringLiteral("Math") && next && next->type == TokenType::DOT && next2
            && next2->type == TokenType::IDENT && next2->lexeme == QStringLiteral("PI")) {
            advance();
            advance();
            advance();
            return ast::number(3.141592653589793);
        }
        // member-then-call OR direct call -> parse as a chain (expression
        // context). See file header note: for 2+ dotted segments this
        // ultimately still fails inside parseCall (bug-for-bug parity).
        if ((next && next->type == TokenType::LPAREN) || hasCallAfterDot(current_)) {
            const JsonArray chain = parseChain(QStringLiteral("expression"));
            if (chain.size() == 1) return chain.at(0).toObject();
            JsonObject node;
            node.insert(QStringLiteral("type"), NodeKind::Chain);
            node.insert(QStringLiteral("chain"), chain);
            return node;
        }
        // dotted enum/member path (no call at the end).
        advance();
        QStringList path;
        path.append(token.lexeme);
        while (peek().type == TokenType::DOT) {
            const Token* n = tokenAt(current_ + 1);
            if (!n) break;
            const Token* after = tokenAt(current_ + 2);
            if (after && after->type == TokenType::LPAREN) break; // dot begins a call
            if (!memberTokenTypes().contains(n->type)) {
                throw parserErrorAt(QStringLiteral("P001"), QStringLiteral("Expected identifier after '.'"), *n);
            }
            advance(); // consume '.'
            advance(); // consume segment token
            path.append(n->lexeme);
        }
        if (path.size() > 1) {
            JsonObject node;
            node.insert(QStringLiteral("type"), NodeKind::Member);
            JsonArray pathArr;
            for (const LangString& s : path) pathArr.append(s);
            node.insert(QStringLiteral("path"), pathArr);
            return node;
        }
        JsonObject node;
        node.insert(QStringLiteral("type"), NodeKind::Ident);
        node.insert(QStringLiteral("name"), path.first());
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
        expect(TokenType::RPAREN, QStringLiteral("Expect ')'"));
        return expr;
    }
    throw parserErrorAt(QStringLiteral("P001"), QStringLiteral("Unexpected token %1").arg(token.type), token);
}

} // namespace

Value parse(const std::vector<Token>& tokens) { return parse(tokens, Value()); }

Value parse(const std::vector<Token>& tokens, const Value& options) {
    QVector<Token> tokenVec;
    tokenVec.assign(tokens.begin(), tokens.end());
    const JsonValue opts(options);
    const bool strictSubchain = opts.toObject().value(QStringLiteral("subchainArguments")).toString() == QStringLiteral("strict");
    Parser parser(std::move(tokenVec), strictSubchain);
    return parser.parseProgram().native();
}

} // namespace nm
