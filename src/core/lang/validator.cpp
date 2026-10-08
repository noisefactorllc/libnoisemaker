#include "validator.h"
#include "core/value/js_number.h"

#include "ast.h"
#include "diagnostics.h"
#include "effect_registry.h"
#include "enums.h"
#include "core/js/js_syntax.h"
#include "core/edit/jsv.h"


#include <algorithm>
#include <cmath>
#include <optional>
#include <stdexcept>

namespace nm {

namespace {

// PARITY-CRITICAL behaviors replicated exactly from the reference
// shaders/src/lang/validator.js (source of truth), cross-checked against
// td/noisemaker/compiler/lang/validator.py (registry-passed-in class
// shape; UnsupportedDsl fail-loud sites) and godot/addons/noisemaker/
// compiler/lang/validator.gd (full non-throwing argument-resolution
// fidelity -- used to verify the SHARED logic paths, since godot never
// diverges from the reference there), and spot-verified against the
// reference oracle (NM_REFERENCE_ROOT tools/dump-validate.mjs) for every
// hazard called out below -- see task report "hazards encountered":
//   - `nodeId` in a diagnostic is `node?.id` -- NOT a validator-assigned
//     id. The raw object retains this key even when its value is undefined;
//     ordinary JSON serialization omits undefined-valued keys.
//   - `location` in a diagnostic is `{line, column}` when loc is present:
//     the reference reads `node.loc.column ?? node.loc.col` (upstream
//     e5bd2013), preserving column coordinates with precedence.
//   - ALLOWED_STRING_PARAMS includes the four text fields plus the
//     name/id identity fields accepted by midi() and audio().
//   - "member"-typed params (`def.type === 'member'`, e.g. filter.channel,
//     filter.palette, filter3d.palette3d, synth.osc2d -- FOUR effects
//     total) are the ONLY ones dispatched through the dedicated member
//     resolver; an unrecognized value SILENTLY falls back to the default,
//     NO diagnostic. Every other choices/enum-bearing global (e.g.
//     synth.noise's `type`/`loopOffset`/`colorMode`, any `palette`-typed
//     global) is declared plain "int"/"float"/"palette" and falls through
//     to the NUMERIC resolver's `def.enum`/`def.choices` Ident branches
//     instead, which DO push S003 on failure. Verified against the oracle
//     with both `filter.channel(channel: bogus)` (silent, default 0, no
//     diagnostic) and `noise(type: bogus)` (S003, default 10).
//   - vec2/mat3/palette (and anything else not one of the eight explicitly
//     dispatched types) fall through to the numeric resolver too -- a
//     bare Number argument clamps as a SCALAR even though the default may
//     be an array (verified: synth.media's vec2 imageSize with no
//     override keeps its [1024,1024] array default verbatim; with
//     `imageSize: 5` becomes the scalar 5).
//   - `clamp()` requires `typeof min/max === 'number'`; an array-valued
//     min/max (vec3 params' per-component bounds, e.g.
//     render.renderLit3d's cameraPosition min:[-1,-1,-1]) is never used
//     for clamping -- only ever relevant to numeric-typed params, whose
//     min/max are always plain numbers in the catalog, but ported as an
//     explicit type check (not assumed) for fidelity.
//   - Subchain begin/end steps carry NO `iterations` field -- TD's port
//     adds one (an intra-frame ping-pong loop count) that does not exist
//     anywhere in the JS reference; the parser (T8) does not produce an
//     `iterations` key on Subchain nodes either. Not ported.
//   - Control flow (if/elif/else, break, continue, return) compiles to the
//     reference's Branch/Break/Continue/Return plan entries, including the
//     shared temp counter, `evalExpr`'s JSON clone (a non-finite Number
//     condition reads back as null, so it is false) and the TypeError a
//     `let` inside a block raises. Nothing downstream executes these plans:
//     the reference expander fails on them, and so does nm::expand.
//   - The pinned reference registers both bare and namespaced starter names.
//     Missing-write starter chains therefore produce S006 even when their
//     call name is bare.

const JsSet<JsText>& stateSurfaces() {
    static const JsSet<JsText> s = {
        JsText(u"time"), JsText(u"frame"), JsText(u"mouse"),
        JsText(u"resolution"), JsText(u"seed"), JsText(u"a"),
    };
    return s;
}

const JsSet<JsText>& stateValues() {
    static const JsSet<JsText> s = {
        JsText(u"time"),  JsText(u"frame"),     JsText(u"mouse"), JsText(u"resolution"),
        JsText(u"seed"),  JsText(u"a"),         JsText(u"u1"),    JsText(u"u2"),
        JsText(u"u3"),    JsText(u"u4"),        JsText(u"s1"),    JsText(u"s2"),
        JsText(u"b1"),    JsText(u"b2"),        JsText(u"a1"),    JsText(u"a2"),
        JsText(u"deltaTime"),
    };
    return s;
}

// !! Do not expand -- strict allowlist for string params (reference
// ALLOWED_STRING_PARAMS; effect fields and input-identity fields only).
const JsSet<JsText>& allowedStringParams() {
    static const JsSet<JsText> s = {
        JsText(u"text.text"),
        JsText(u"text.font"),
        JsText(u"text.justify"),
        JsText(u"text.style"),
        JsText(u"midi.name"),
        JsText(u"midi.id"),
        JsText(u"audio.name"),
        JsText(u"audio.id"),
    };
    return s;
}

const JsMap<JsText, JsList>& automationFields() {
    static const JsMap<JsText, JsList> fields = {
        {NodeKind::Oscillator,
         {JsText(u"oscType"), JsText(u"min"), JsText(u"max"),
          JsText(u"speed"), JsText(u"offset"), JsText(u"seed")}},
        {NodeKind::Midi,
         {JsText(u"channel"), JsText(u"mode"), JsText(u"min"),
          JsText(u"max"), JsText(u"sensitivity"), JsText(u"name"),
          JsText(u"id"), JsText(u"cc"), JsText(u"nrpn"),
          JsText(u"zone"), JsText(u"members")}},
        {NodeKind::Audio,
         {JsText(u"band"), JsText(u"min"), JsText(u"max"),
          JsText(u"channel"), JsText(u"name"), JsText(u"id")}},
    };
    return fields;
}

constexpr int kMaxAutomationDepth = 8;

JsText decodeJsonStringLiteralContent(const JsText& raw) {
    try {
        const Value parsed = json::parse(std::string("[\"") + raw.toUtf8() + "\"]");
        if (parsed.is_array() && !parsed.as_array().empty() && parsed.as_array().front().is_string())
            return JsText(parsed.as_array().front().as_string());
    } catch (const json::ParseError&) {}

    JsText decoded;
    for (int i = 0; i < raw.size(); ++i) {
        if (raw.at(i) != u'\\' || i + 1 >= raw.size()) {
            decoded.append(raw.at(i));
            continue;
        }
        const char16_t next = raw.at(++i);
        if (next == u'n') decoded.append(u'\n');
        else if (next == u'r') decoded.append(u'\r');
        else if (next == u't') decoded.append(u'\t');
        else if (next == u'b') decoded.append(u'\b');
        else if (next == u'f') decoded.append(u'\f');
        else if (next == u'v') decoded.append(char16_t(0x0b));
        else if (next == u'0') decoded.append(char16_t(0));
        else if (next == u'\\' || next == u'\'' || next == u'"') decoded.append(next);
        else {
            decoded.append(u'\\');
            decoded.append(next);
        }
    }
    return decoded;
}

const JsSet<JsText>& surfacePassthroughCalls() {
    static const JsSet<JsText> s = {JsText(u"read")};
    return s;
}

bool isTruthy(const JsValue& v) {
    switch (v.type()) {
        case JsValue::Null:
        case JsValue::Undefined:
            return false;
        case JsValue::Bool:
            return v.toBool();
        case JsValue::Double:
            return v.toDouble() != 0.0 && !std::isnan(v.toDouble());
        case JsValue::String:
            return !v.toString().isEmpty();
        case JsValue::Array:
        case JsValue::Object:
        case JsValue::Function:
            return true;
    }
    return false;
}

JsValue cloneJson(const JsValue& value) {
    if (!value.isObject() && !value.isArray()) return value;
    return json::parse(json::stringify(value.raw()));
}

Value completeAutomationAst(const Value& raw, bool clonedAncestor = false) {
    if (raw.is_array()) {
        Array out;
        for (const Value& item : raw.as_array()) out.push_back(completeAutomationAst(item, clonedAncestor));
        return Value(std::move(out));
    }
    if (!raw.is_object()) return raw;
    const Object& source = raw.as_object();
    const Value* type = source.find(u"type");
    const bool cloned = clonedAncestor || source.has(u"_varRef");
    const bool midi = type && type->is_string() && type->as_string() == u"Midi";
    const bool audio = type && type->is_string() && type->as_string() == u"Audio";
    Object out;
    auto put = [&](const char16_t* key, bool optional) {
        if (const Value* value = source.find(key)) out.set(key, completeAutomationAst(*value, cloned));
        else if (optional && !cloned) out.set(key, Value());
    };
    if (midi) {
        put(u"type", false); put(u"channel", true); put(u"mode", false);
        put(u"min", false); put(u"max", false); put(u"sensitivity", false);
        put(u"cc", true); put(u"nrpn", true); put(u"zone", true);
        put(u"members", true); put(u"name", true); put(u"id", true);
        put(u"loc", false);
    } else if (audio) {
        put(u"type", false); put(u"band", false); put(u"min", false); put(u"max", false);
        put(u"channel", true); put(u"name", true); put(u"id", true); put(u"loc", false);
    }
    for (const JsString& key : source.keys()) {
        if (!out.has(key)) out.set(key, completeAutomationAst(*source.find(key), cloned));
    }
    return Value(std::move(out));
}

// value -> [non-empty string segments], or an EMPTY list for "null" (no
// path) -- mirrors normalizeMemberPath's null-collapsing exactly (an
// array with only empty/falsy segments, an empty string, or a value of
// neither array/string/number type, all become "no path"; a plain number
// becomes its single-segment string form).
JsList normalizeMemberPath(const JsValue& value) {
    if (value.isArray()) {
        JsList parts;
        for (const JsValue& seg : value.toArray()) {
            if (seg.isString() && !seg.toString().isEmpty()) parts.append(seg.toString());
        }
        return parts;
    }
    if (value.isString()) {
        JsList parts;
        const JsList segs = value.toString().split(u'.', false);
        for (const JsText& seg : segs) {
            const JsText t = seg.trimmed();
            if (!t.isEmpty()) parts.append(t);
        }
        return parts;
    }
    if (value.isDouble()) {
        return {js::numberToString(value.toDouble())};
    }
    return {};
}

bool pathStartsWith(const JsList& path, const JsList& prefix) {
    if (prefix.isEmpty()) return true;
    if (path.size() < prefix.size()) return false;
    for (int i = 0; i < prefix.size(); ++i) {
        if (path.at(i) != prefix.at(i)) return false;
    }
    return true;
}

JsList applyEnumPrefix(const JsList& path, const JsList& prefix) {
    if (path.isEmpty()) return path;
    if (prefix.isEmpty()) return path;
    if (pathStartsWith(path, prefix)) return path;
    for (int i = 1; i < prefix.size(); ++i) {
        const JsList suffix = prefix.mid(i);
        if (pathStartsWith(path, suffix)) {
            return prefix.mid(0, i) + path;
        }
    }
    return prefix + path;
}

// clamp(value, min, max): min/max only apply when they are PLAIN NUMBERS
// (reference `typeof min === 'number'`) -- an array-valued min/max (vec3
// per-component bounds) never clamps a scalar.
double clampValue(double value, const JsValue& min, const JsValue& max) {
    if (min.isDouble() && value < min.toDouble()) return min.toDouble();
    if (max.isDouble() && value > max.toDouble()) return max.toDouble();
    return value;
}

// Reference resolveEnum walks a path with
// `cur && Object.prototype.hasOwnProperty.call(cur, part)`. Arrays and
// strings have own index properties and an own `length`, so
// `let c = #ff8800` makes `c.value.length` resolve to 4.
std::optional<JsValue> jsOwnProperty(const JsValue& cur, const JsText& key) {
    if (cur.isObject()) {
        const JsObject o = cur.toObject();
        if (o.contains(key)) return o.value(key);
        return std::nullopt;
    }
    if (!cur.isArray() && !cur.isString()) return std::nullopt;
    const std::ptrdiff_t size = cur.isArray() ? cur.toArray().size() : cur.toString().size();
    if (key == JsText(u"length")) return JsValue(static_cast<double>(size));
    // Canonical array index: digits only, no leading zero except "0".
    bool isIndex = !key.isEmpty() && (key.size() == 1 || key.at(0) != u'0');
    for (const char16_t c : key) isIndex = isIndex && c >= u'0' && c <= u'9';
    if (!isIndex) return std::nullopt;
    bool ok = false;
    const long long index = key.toLongLong(&ok);
    if (!ok || index >= size) return std::nullopt;
    if (cur.isArray()) return cur.toArray().at(static_cast<std::ptrdiff_t>(index));
    return JsValue(JsText(cur.toString().at(static_cast<std::ptrdiff_t>(index))));
}

// The reference compiles a Func (`() => expr`) value with
// `new Function('state', `with(state){ return ${src}; }`)` and reports S001
// when that throws. nm::js::checkFunctionBody decides the same question for
// V8; input it cannot decide fails loud.
bool funcCompiles(const JsText& src) {
    const js::CheckResult result =
        js::checkFunctionBody(JsText(u"state"), JsText(u"with(state){ return ") + src + JsText(u"; }"));
    if (result.verdict == js::Verdict::Unsupported) {
        throw UnsupportedDsl(JsText(u"Func body not decided (%1): () => %2").arg(result.detail, src.left(50)));
    }
    return result.verdict == js::Verdict::Valid;
}

// `${src?.slice(0, 50) || 'unknown'}` in the reference S001 messages.
JsText funcSnippet(const JsText& src) { return src.isEmpty() ? JsText(u"unknown") : src.left(50); }

JsText nodeType(const JsValue& node) { return node.isObject() ? node.toObject().value(JsText(u"type")).toString() : JsText(); }

// ---------------------------------------------------------------- Validator

class Validator {
public:
    explicit Validator(EffectRegistry& reg) : reg_(reg) {}

    JsObject run(const JsObject& ast);

private:
    EffectRegistry& reg_;
    JsArray diagnostics_;
    JsMap<JsText, JsValue> symbols_;
    JsSet<JsText> reportedAutomationCycles_;
    JsList programSearchOrder_;
    int tempIndex_ = 0;

    // -------------------------------------------------- diagnostics
    void pushDiag(const JsText& code, const JsValue& node, const JsText& message = JsText());
    static JsValue extractIdentifierName(const JsValue& node);

    // -------------------------------------------------- enum / symbol resolution
    JsValue resolveEnum(const JsList& path);
    bool canResolveOpName(const JsText& name);
    JsValue resolveCall(JsObject call);
    static JsValue firstChainCall(const JsValue& node);
    JsValue getStarterInfo(const JsValue& node);
    bool isStarterChain(const JsValue& node);
    JsValue substitute(const JsValue& node, const JsList& resolving = {});
    void bindVar(const JsObject& v);
    static JsValue buildNamespaceSnapshot(const JsValue& callNamespace);

    // -------------------------------------------------- control flow
    JsValue evalExpr(const JsValue& node);
    JsValue evalCondition(const JsValue& node);
    JsArray compileBlock(const JsValue& body);

    // -------------------------------------------------- statement / chain compilation
    JsValue compileStmt(const JsObject& stmt); // Undefined => produced nothing
    JsObject compileChainStatement(const JsObject& stmt);
    int processChain(JsArray& chain, const JsArray& calls, int input, bool allowStarterless,
                      const JsText& writeName);

    // -------------------------------------------------- surfaces
    static JsValue toSurface(const JsValue& arg);
    JsValue callToSurface(const JsValue& node);
    static JsValue make3dRef(const JsValue& node, const JsText& defaultKind);
    static JsText surfaceName(const JsValue& node);

    // -------------------------------------------------- argument resolution
    // Reference 29e76468 validator.js isOwnChoice: a bare name the parameter
    // defines itself, as an inline choice or a member of its enum, means that
    // value even where it shadows a state value such as `seed` or `a`. The
    // unparser writes choices by bare name, so `geometry: seed` and
    // `channel: a` must read back as written.
    bool isOwnChoice(const ParamDef& def, const JsText& name);
    JsValue resolveArgs(JsArray& chain, const OpSpec& spec, const JsObject& call, const JsText& opName,
                            const JsObject& original, JsObject& args, const JsText& writeName,
                            JsObject* resolvedKwargs);
    JsValue mirrorNestedMemberPaths(const JsValue& node);
    void resolveSurfaceArg(JsArray& chain, const ParamDef& def, const JsValue& node, const JsObject& call,
                            JsObject& args, const JsText& argKey, const JsText& writeName);
    void resolveColorArg(const ParamDef& def, const JsValue& node, const JsObject& call, JsObject& args,
                          const JsText& argKey);
    void resolveVecArg(const ParamDef& def, const JsValue& node, const JsObject& call, JsObject& args,
                        const JsText& argKey, int n);
    void resolveBooleanArg(const ParamDef& def, const JsValue& node, const JsObject& call, JsObject& args,
                            const JsText& argKey);
    void resolveMemberArg(const ParamDef& def, const JsValue& node, const JsObject& call, JsObject& args,
                           const JsText& argKey, JsList* selectedPath);
    void resolveVolumeOrGeometryArg(const ParamDef& def, const JsValue& node, JsObject& args,
                                     const JsText& argKey, const JsText& kind);
    void resolveStringArg(const ParamDef& def, const JsValue& node, const JsText& opName,
                           const JsObject& original, JsObject& args, const JsText& argKey);
    void resolveNumericArg(const ParamDef& def, const JsValue& node, const JsObject& call, const OpSpec& spec,
                            JsObject& args, const JsText& argKey);
    struct AutomationNumberOptions {
        bool allowBoolean = false;
        bool allowAutomation = false;
        bool allowMember = true;
        bool clamp01 = false;
        bool integer = false;
        std::optional<double> minimum;
        std::optional<double> maximum;
        bool* invalid = nullptr;
    };
    JsValue resolveAutomationEnum(const JsValue& node, const JsText& enumName,
                                     const JsValue& fallback, const JsSet<int>& validValues,
                                     const JsText& descriptorName, const JsText& fieldName);
    JsValue resolveAutomationString(const JsValue& node, const JsText& descriptorName,
                                       const JsText& fieldName);
    JsValue resolveAutomationNumber(const JsValue& node, const JsText& descriptorName,
                                       const JsText& fieldName, const JsValue& fallback,
                                       AutomationNumberOptions options, int depth = 0);
    JsValue compileAutomationDescriptor(const JsObject& node, int depth = 0);
};

// ---------------------------------------------------------------- diagnostics

JsValue Validator::extractIdentifierName(const JsValue& nodeVal) {
    if (!nodeVal.isObject()) return JsValue(JsValue::Undefined);
    const JsObject node = nodeVal.toObject();
    const JsValue typeValue = node.value(JsText(u"type"));
    const JsText t = typeValue.toString();
    if (t == NodeKind::Ident) return node.value(JsText(u"name"));
    if (t == NodeKind::Member && node.value(JsText(u"path")).isArray()) {
        JsList segs;
        for (const JsValue& s : node.value(JsText(u"path")).toArray()) segs.append(s.toString());
        return segs.join(u'.');
    }
    if (t == NodeKind::Call) return node.value(JsText(u"name"));
    if (t == NodeKind::Func && isTruthy(node.value(JsText(u"src")))) {
        const JsText src = node.value(JsText(u"src")).toString();
        return JsText(u"{%1%2}").arg(src.left(30), src.size() > 30 ? JsText(u"...") : JsText());
    }
    if (isTruthy(node.value(JsText(u"name")))) return node.value(JsText(u"name"));
    if (isTruthy(node.value(JsText(u"value")))) {
        const JsValue v = node.value(JsText(u"value"));
        return v.isDouble() ? js::numberToString(v.toDouble()) : v.toVariant().toString();
    }
    return JsText(u"[%1]").arg(isTruthy(typeValue) ? typeValue.toVariant() : JsText(u"unknown"));
}

void Validator::pushDiag(const JsText& code, const JsValue& nodeVal, const JsText& message) {
    const JsText msg = message.isEmpty() ? JsText(diagDefaultMessage(code)) : message;
    const JsValue identName = extractIdentifierName(nodeVal);
    const JsText identText = identName.isUndefined() ? JsText() : identName.toVariant();
    JsText enriched = msg;
    if (isTruthy(identName) && !msg.contains(identText) && !msg.contains(u'\'')) {
        enriched = JsText(u"%1: '%2'").arg(msg, identText);
    }

    Object diag;
    diag.set(u"code", code);
    diag.set(u"message", enriched);
    diag.set(u"severity", diagSeverity(code));
    // The reference object literal always owns nodeId, including undefined.
    const JsObject node = nodeVal.toObject();
    diag.set(u"nodeId", node.value(JsText(u"id")).raw());
    // location: {line, column} if loc is present (upstream e5bd2013).
    const JsValue locVal = node.value(JsText(u"loc"));
    if (locVal.isObject()) {
        const JsObject loc = locVal.toObject();
        JsObject location;
        location.insert(JsText(u"line"), loc.value(JsText(u"line")));
        JsValue colVal = loc.value(JsText(u"column"));
        if (colVal.isNull() || colVal.isUndefined()) {
            colVal = loc.value(JsText(u"col"));
        }
        if (colVal.isDouble()) {
            location.insert(JsText(u"column"), colVal.toInt());
        }
        diag.set(u"location", location.raw());
    }
    if (isTruthy(identName)) diag.set(u"identifier", identName.raw());
    diagnostics_.append(Value(std::move(diag)));
}

// ---------------------------------------------------------------- enum resolution

JsValue Validator::resolveEnum(const JsList& path) {
    if (path.isEmpty()) return JsValue(JsValue::Undefined);
    const JsText head = path.first();
    JsValue cur;
    if (symbols_.contains(head)) {
        cur = symbols_.value(head);
        const JsText t = nodeType(cur);
        if (t == NodeKind::Number || t == NodeKind::Boolean) {
            cur = cur.toObject().value(JsText(u"value"));
        }
    } else {
        cur = reg_.enums().tryGetHead(head);
        if (cur.isUndefined()) return JsValue(JsValue::Undefined);
    }
    for (int i = 1; i < path.size(); ++i) {
        if (!isTruthy(cur)) return JsValue(JsValue::Undefined);
        const std::optional<JsValue> next = jsOwnProperty(cur, path.at(i));
        if (!next) return JsValue(JsValue::Undefined);
        cur = *next;
    }
    if (cur.isObject()) {
        const JsText t = cur.toObject().value(JsText(u"type")).toString();
        if (t == JsText(u"Number") || t == JsText(u"Boolean")) {
            return cur.toObject().value(JsText(u"value"));
        }
    }
    return cur;
}

bool Validator::canResolveOpName(const JsText& name) {
    for (const JsText& ns : programSearchOrder_) {
        if (reg_.getOp(ns + u'.' + name) != nullptr) return true;
    }
    return false;
}

JsValue Validator::resolveCall(JsObject call) {
    const JsText name = call.value(JsText(u"name")).toString();
    if (!symbols_.contains(name)) return call;
    const JsValue val = symbols_.value(name);
    const JsText vt = nodeType(val);
    if (vt == NodeKind::Ident) {
        JsObject out = call;
        out.insert(JsText(u"name"), val.toObject().value(JsText(u"name")));
        return out;
    }
    if (vt == NodeKind::Call) {
        const JsObject valObj = val.toObject();
        JsArray mergedArgs = valObj.value(JsText(u"args")).toArray();
        for (const JsValue& a : call.value(JsText(u"args")).toArray()) mergedArgs.append(a); // APPEND
        JsObject mergedKw;
        bool haveKw = false;
        if (valObj.contains(JsText(u"kwargs"))) {
            mergedKw = valObj.value(JsText(u"kwargs")).toObject();
            haveKw = true;
        }
        if (call.contains(JsText(u"kwargs"))) {
            haveKw = true;
            const JsObject ck = call.value(JsText(u"kwargs")).toObject();
            for (auto it = ck.constBegin(); it != ck.constEnd(); ++it) mergedKw.insert(it.key(), it.value());
        }
        JsObject merged;
        merged.insert(JsText(u"type"), NodeKind::Call);
        merged.insert(JsText(u"name"), valObj.value(JsText(u"name")));
        merged.insert(JsText(u"args"), mergedArgs);
        if (haveKw) merged.insert(JsText(u"kwargs"), mergedKw);
        if (call.contains(JsText(u"namespace"))) {
            merged.insert(JsText(u"namespace"), call.value(JsText(u"namespace")));
        } else if (valObj.contains(JsText(u"namespace"))) {
            merged.insert(JsText(u"namespace"), valObj.value(JsText(u"namespace")));
        }
        return merged;
    }
    return call;
}

JsValue Validator::firstChainCall(const JsValue& node) {
    if (!node.isObject()) return JsValue(JsValue::Undefined);
    const JsObject o = node.toObject();
    const JsText t = o.value(JsText(u"type")).toString();
    if (t == NodeKind::Call) return node;
    if (t == NodeKind::Chain) {
        const JsArray chain = o.value(JsText(u"chain")).toArray();
        if (!chain.isEmpty() && nodeType(chain.first()) == NodeKind::Call) return chain.first();
    }
    return JsValue(JsValue::Undefined);
}

// {call,index} of the first STARTER op in the chain, or Undefined. NOTE:
// the QUERY name here is the call's BARE name UNLESS its namespace was
// explicitly resolved (from()) -- see file header re: isStarterOp's
// bare-name limitation, ported bug-for-bug.
JsValue Validator::getStarterInfo(const JsValue& node) {
    if (!node.isObject()) return JsValue(JsValue::Undefined);
    const JsObject o = node.toObject();
    const JsText t = o.value(JsText(u"type")).toString();
    auto queryName = [](const JsObject& call) {
        JsText name = call.value(JsText(u"name")).toString();
        const JsValue ns = call.value(JsText(u"namespace"));
        if (ns.isObject() && isTruthy(ns.toObject().value(JsText(u"resolved")))) {
            name = ns.toObject().value(JsText(u"resolved")).toString() + u'.' + name;
        }
        return name;
    };
    if (t == NodeKind::Call) {
        if (reg_.isStarterOp(queryName(o))) {
            JsObject info;
            info.insert(JsText(u"call"), o);
            info.insert(JsText(u"index"), 0);
            return info;
        }
        return JsValue(JsValue::Undefined);
    }
    if (t == NodeKind::Chain) {
        const JsArray chain = o.value(JsText(u"chain")).toArray();
        for (int i = 0; i < chain.size(); ++i) {
            if (nodeType(chain.at(i)) == NodeKind::Call && reg_.isStarterOp(queryName(chain.at(i).toObject()))) {
                JsObject info;
                info.insert(JsText(u"call"), chain.at(i));
                info.insert(JsText(u"index"), i);
                return info;
            }
        }
    }
    return JsValue(JsValue::Undefined);
}

bool Validator::isStarterChain(const JsValue& node) {
    if (nodeType(node) != NodeKind::Chain) return false;
    const JsValue info = getStarterInfo(node);
    return info.isObject() && info.toObject().value(JsText(u"index")).toInt(-1) == 0;
}

JsValue Validator::substitute(const JsValue& nodeVal, const JsList& resolving) {
    if (!nodeVal.isObject()) return nodeVal;
    const JsObject node = nodeVal.toObject();
    const JsText t = node.value(JsText(u"type")).toString();
    if (t == NodeKind::Ident) {
        const JsText name = node.value(JsText(u"name")).toString();
        if (resolving.contains(name)) {
            const int cycleStart = resolving.indexOf(name);
            JsList cycle = resolving.mid(cycleStart);
            cycle.append(name);
            const JsText cycleKey = cycle.join(JsText(u" -> "));
            if (!reportedAutomationCycles_.contains(cycleKey)) {
                reportedAutomationCycles_.insert(cycleKey);
                pushDiag(JsText(u"S001"), node,
                         JsText(u"Automation cycle detected: %1").arg(cycleKey));
            }
            JsObject invalid(ast::number(0).native());
            invalid.insert(JsText(u"_automationInvalid"), true);
            return invalid;
        }
    }
    if (t == NodeKind::Ident && symbols_.contains(node.value(JsText(u"name")).toString())) {
        const JsText name = node.value(JsText(u"name")).toString();
        JsList nestedResolving = resolving;
        nestedResolving.append(name);
        JsValue result = substitute(cloneJson(symbols_.value(name)), nestedResolving);
        // Marks the substituted node as having come from a variable
        // reference -- resolveNumericArg's Number/Boolean case and
        // compileAutomationDescriptor both read this back (verified against the
        // oracle: `let o = osc(...); noise(scaleX: o)` embeds `_varRef`
        // BOTH at the Oscillator value's top level AND nested inside its
        // `_ast` copy, because the mutation happens on the shared node
        // object here, before compileAutomationDescriptor captures it as `_ast`).
        if (result.isObject()) {
            JsObject r = result.toObject();
            r.insert(JsText(u"_varRef"), node.value(JsText(u"name")));
            result = r;
        }
        return result;
    }
    if (automationFields().contains(t)) {
        JsObject mapped = node;
        for (const JsText& field : automationFields().value(t)) {
            if (node.contains(field)) mapped.insert(field, substitute(node.value(field), resolving));
        }
        return mapped;
    }
    if (t == NodeKind::Chain) {
        JsArray mapped;
        for (const JsValue& cVal : node.value(JsText(u"chain")).toArray()) {
            const JsObject c = cVal.toObject();
            if (!c.value(JsText(u"args")).isArray()) {
                throw JsError(u"TypeError", u"Cannot read properties of undefined (reading 'map')");
            }
            JsArray mappedArgs;
            for (const JsValue& a : c.value(JsText(u"args")).toArray()) mappedArgs.append(substitute(a, resolving));
            JsObject mappedCall;
            mappedCall.insert(JsText(u"type"), NodeKind::Call);
            mappedCall.insert(JsText(u"name"), c.value(JsText(u"name")));
            mappedCall.insert(JsText(u"args"), mappedArgs);
            if (c.contains(JsText(u"kwargs"))) {
                JsObject kw;
                const JsObject srcKw = c.value(JsText(u"kwargs")).toObject();
                for (auto it = srcKw.constBegin(); it != srcKw.constEnd(); ++it) kw.insert(it.key(), substitute(it.value(), resolving));
                mappedCall.insert(JsText(u"kwargs"), kw);
            }
            mapped.append(resolveCall(mappedCall));
        }
        JsObject out;
        out.insert(JsText(u"type"), NodeKind::Chain);
        out.insert(JsText(u"chain"), mapped);
        return out;
    }
    if (t == NodeKind::Call) {
        if (!node.value(JsText(u"args")).isArray()) {
            throw JsError(u"TypeError", u"Cannot read properties of undefined (reading 'map')");
        }
        JsArray mappedArgs;
        for (const JsValue& a : node.value(JsText(u"args")).toArray()) mappedArgs.append(substitute(a, resolving));
        JsObject mappedCall;
        mappedCall.insert(JsText(u"type"), NodeKind::Call);
        mappedCall.insert(JsText(u"name"), node.value(JsText(u"name")));
        mappedCall.insert(JsText(u"args"), mappedArgs);
        if (node.contains(JsText(u"kwargs"))) {
            JsObject kw;
            const JsObject srcKw = node.value(JsText(u"kwargs")).toObject();
            for (auto it = srcKw.constBegin(); it != srcKw.constEnd(); ++it) kw.insert(it.key(), substitute(it.value(), resolving));
            mappedCall.insert(JsText(u"kwargs"), kw);
        }
        return resolveCall(mappedCall);
    }
    return nodeVal;
}

void Validator::bindVar(const JsObject& v) {
    const JsText name = v.value(JsText(u"name")).toString();
    const JsValue expr = substitute(cloneJson(v.value(JsText(u"expr"))), {name});
    if (isStarterChain(expr)) {
        const JsValue head = firstChainCall(expr);
        if (!head.isUndefined()) pushDiag(JsText(u"S006"), head);
    }
    const JsText exprType = nodeType(expr);
    if (expr.isUndefined() || expr.isNull()
        || (exprType == NodeKind::Ident
            && (expr.toObject().value(JsText(u"name")).toString() == JsText(u"null")
                || expr.toObject().value(JsText(u"name")).toString() == JsText(u"undefined")))) {
        pushDiag(JsText(u"S004"), v);
        return;
    }
    if (exprType == NodeKind::Ident) {
        const JsText identName = expr.toObject().value(JsText(u"name")).toString();
        if (!symbols_.contains(identName) && !stateValues().contains(identName) && reg_.getOp(identName) == nullptr
            && !canResolveOpName(identName)) {
            pushDiag(JsText(u"S003"), expr);
            return;
        }
    }
    if (exprType == NodeKind::Chain && expr.toObject().value(JsText(u"chain")).toArray().size() == 1) {
        symbols_.insert(name, expr.toObject().value(JsText(u"chain")).toArray().first());
    } else if (exprType == NodeKind::Member) {
        const JsValue resolved = resolveEnum(normalizeMemberPath(expr.toObject().value(JsText(u"path"))));
        if (resolved.isDouble()) {
            symbols_.insert(name, Enums::leaf(resolved.toDouble()));
        } else if (!resolved.isUndefined()) {
            symbols_.insert(name, resolved);
        } else {
            symbols_.insert(name, expr);
        }
    } else {
        symbols_.insert(name, expr);
    }
}

JsValue Validator::buildNamespaceSnapshot(const JsValue& callNamespaceVal) {
    if (!callNamespaceVal.isObject()) return JsValue(JsValue::Undefined);
    const JsObject cn = callNamespaceVal.toObject();
    JsObject callSnap;
    callSnap.insert(JsText(u"name"), cn.value(JsText(u"name")).isString()
                                                 ? cn.value(JsText(u"name"))
                                                 : JsValue(JsValue::Null));
    callSnap.insert(JsText(u"resolved"), cn.value(JsText(u"resolved")).isString()
                                                     ? cn.value(JsText(u"resolved"))
                                                     : JsValue(JsValue::Null));
    callSnap.insert(JsText(u"explicit"), isTruthy(cn.value(JsText(u"explicit"))));
    callSnap.insert(JsText(u"source"), cn.value(JsText(u"source")).isString()
                                                   ? cn.value(JsText(u"source"))
                                                   : JsValue(JsValue::Null));
    if (cn.value(JsText(u"searchOrder")).isArray()) {
        callSnap.insert(JsText(u"searchOrder"), cn.value(JsText(u"searchOrder")));
    }
    if (isTruthy(cn.value(JsText(u"fromOverride")))) {
        callSnap.insert(JsText(u"fromOverride"), true);
    }
    JsObject snapshot;
    snapshot.insert(JsText(u"call"), callSnap);
    if (isTruthy(cn.value(JsText(u"resolved")))) {
        snapshot.insert(JsText(u"resolved"), cn.value(JsText(u"resolved")));
    }
    return snapshot;
}

// ---------------------------------------------------------------- surfaces

JsValue Validator::toSurface(const JsValue& argVal) {
    if (!argVal.isObject()) return JsValue(JsValue::Undefined);
    const JsObject arg = argVal.toObject();
    const JsText t = arg.value(JsText(u"type")).toString();
    auto ref = [&](const char* kind) {
        JsObject o;
        o.insert(JsText(u"kind"), JsText::fromLatin1(kind));
        o.insert(JsText(u"name"), arg.value(JsText(u"name")));
        return JsValue(o);
    };
    if (t == NodeKind::OutputRef) return ref("output");
    if (t == NodeKind::SourceRef) return ref("source");
    if (t == NodeKind::XyzRef) return ref("xyz");
    if (t == NodeKind::VelRef) return ref("vel");
    if (t == NodeKind::RgbaRef) return ref("rgba");
    if (t == NodeKind::MeshRef) return ref("mesh");
    if (t == NodeKind::Ident && arg.value(JsText(u"name")).toString() == JsText(u"none")) {
        return ref("output");
    }
    if (t == NodeKind::Ident && stateSurfaces().contains(arg.value(JsText(u"name")).toString())) {
        return ref("state");
    }
    return JsValue(JsValue::Undefined);
}

JsValue Validator::callToSurface(const JsValue& nodeVal) {
    if (!nodeVal.isObject()) return JsValue(JsValue::Undefined);
    const JsObject node = nodeVal.toObject();
    if (node.value(JsText(u"type")).toString() == NodeKind::Chain
        && node.value(JsText(u"chain")).toArray().size() == 1) {
        return callToSurface(node.value(JsText(u"chain")).toArray().first());
    }
    if (node.value(JsText(u"type")).toString() != NodeKind::Call
        || !surfacePassthroughCalls().contains(node.value(JsText(u"name")).toString())) {
        return JsValue(JsValue::Undefined);
    }
    JsValue target;
    const JsArray args = node.value(JsText(u"args")).toArray();
    if (!args.isEmpty()) {
        target = args.first();
    } else if (node.value(JsText(u"kwargs")).isObject()) {
        target = node.value(JsText(u"kwargs")).toObject().value(JsText(u"tex"));
    }
    if (target.isUndefined() || target.isNull()) return JsValue(JsValue::Undefined);
    return toSurface(target);
}

JsText Validator::surfaceName(const JsValue& nVal) {
    if (!nVal.isObject()) return JsText();
    const JsObject n = nVal.toObject();
    return n.value(JsText(u"name")).toString();
}

JsValue Validator::make3dRef(const JsValue& nVal, const JsText& defaultKind) {
    const JsText name = surfaceName(nVal);
    if (name.isEmpty()) return JsValue(JsValue::Undefined);
    JsText kind = defaultKind;
    if (defaultKind == JsText(u"vol")) {
        kind = (nodeType(nVal) == NodeKind::VolRef) ? JsText(u"vol") : JsText(u"tex3d");
    } else {
        kind = JsText(u"geo");
    }
    JsObject o;
    o.insert(JsText(u"kind"), kind);
    o.insert(JsText(u"name"), name);
    return o;
}

// ---------------------------------------------------------------- control flow

// Reference evalExpr: substitute(clone(node)), S006 for a starter chain, and
// enum resolution of a Member (a number becomes a Number node; any other
// resolved value is returned as is).
JsValue Validator::evalExpr(const JsValue& node) {
    const JsValue expr = substitute(cloneJson(node));
    if (isStarterChain(expr)) {
        const JsValue head = firstChainCall(expr);
        if (!head.isUndefined()) pushDiag(JsText(u"S006"), head);
    }
    if (nodeType(expr) == NodeKind::Member) {
        const JsValue resolved = resolveEnum(normalizeMemberPath(expr.toObject().value(JsText(u"path"))));
        if (resolved.isDouble()) return JsValue(ast::number(resolved.toDouble()).native());
        if (!resolved.isUndefined()) return resolved;
    }
    return expr;
}

// Reference evalCondition. Function conditions retain an fn property in the
// raw Value; ordinary JSON serialization omits it.
JsValue Validator::evalCondition(const JsValue& node) {
    const JsValue expr = evalExpr(node);
    if (!isTruthy(expr)) return false;
    const JsText t = nodeType(expr);
    const JsObject o = expr.toObject();
    if (t == NodeKind::Number) {
        // evalExpr's clone is a JSON round trip, so NaN and Infinity read
        // back as null, and toBoolean(null) is false.
        const JsValue v = o.value(JsText(u"value"));
        if (v.isDouble()) return std::isfinite(v.toDouble()) && v.toDouble() != 0.0;
        return isTruthy(v);
    }
    if (t == NodeKind::Boolean) return isTruthy(o.value(JsText(u"value")));
    if (t == NodeKind::Func) {
        const JsText src = o.value(JsText(u"src")).toString();
        if (funcCompiles(src)) return JsObject{{JsText(u"fn"), Value::function(src)}};
        pushDiag(JsText(u"S001"), expr, JsText(u"Invalid function expression: '%1'").arg(funcSnippet(src)));
        return false;
    }
    if (t == NodeKind::Ident) {
        const JsText name = o.value(JsText(u"name")).toString();
        if (symbols_.contains(name)) return evalCondition(symbols_.value(name));
        if (stateValues().contains(name)) return JsObject{{JsText(u"fn"), Value::function(name)}};
        pushDiag(JsText(u"S003"), expr);
        return false;
    }
    if (t == NodeKind::Member) {
        const JsValue pathVal = o.value(JsText(u"path"));
        const JsValue cur = resolveEnum(normalizeMemberPath(pathVal));
        if (!cur.isUndefined()) return isTruthy(cur);
        JsList segs;
        for (const JsValue& s : pathVal.toArray()) segs.append(s.toString());
        const JsText pathText = segs.join(u'.');
        pushDiag(JsText(u"S001"), expr,
                 JsText(u"Unknown enum path: '%1'").arg(pathText.isEmpty() ? JsText(u"unknown") : pathText));
        return false;
    }
    return false;
}

JsArray Validator::compileBlock(const JsValue& body) {
    JsArray result;
    for (const JsValue& s : body.toArray()) {
        const JsValue compiled = compileStmt(s.toObject());
        if (!compiled.isUndefined()) result.append(compiled);
    }
    return result;
}

// ---------------------------------------------------------------- statement compilation

// Reference compileStmt. Nothing downstream executes Branch, Break,
// Continue or Return plans: the reference expander iterates `plan.chain`
// and fails on them, and nm::expand does the same.
JsValue Validator::compileStmt(const JsObject& stmt) {
    const JsText t = stmt.value(JsText(u"type")).toString();
    if (t == NodeKind::IfStmt) {
        const JsValue cond = evalCondition(stmt.value(JsText(u"condition")));
        const JsArray thenBranch = compileBlock(stmt.value(JsText(u"then")));
        JsArray elif;
        for (const JsValue& e : stmt.value(JsText(u"elif")).toArray()) {
            const JsObject eo = e.toObject();
            JsObject entry;
            entry.insert(JsText(u"cond"), evalCondition(eo.value(JsText(u"condition"))));
            entry.insert(JsText(u"then"), compileBlock(eo.value(JsText(u"then"))));
            elif.append(entry);
        }
        const JsArray elseBranch = compileBlock(stmt.value(JsText(u"else")));
        JsObject branch;
        branch.insert(JsText(u"type"), JsText(u"Branch"));
        branch.insert(JsText(u"cond"), cond);
        branch.insert(JsText(u"then"), thenBranch);
        branch.insert(JsText(u"elif"), elif);
        branch.insert(JsText(u"else"), elseBranch);
        return branch;
    }
    if (t == NodeKind::Break || t == NodeKind::Continue) {
        JsObject node;
        node.insert(JsText(u"type"), t);
        return node;
    }
    if (t == NodeKind::Return) {
        JsObject node;
        node.insert(JsText(u"type"), NodeKind::Return);
        if (isTruthy(stmt.value(JsText(u"value")))) {
            node.insert(JsText(u"value"), evalExpr(stmt.value(JsText(u"value"))));
        }
        return node;
    }
    // Any other statement goes to compileChainStatement. Only a block can
    // hold a statement with no chain (`let` inside if/elif/else); the
    // reference then reads `stmt.chain[0]` and raises this TypeError.
    if (!stmt.value(JsText(u"chain")).isArray()) {
        throw JsError(u"TypeError", u"Cannot read properties of undefined (reading '0')");
    }
    // compileChainStatement returns an EMPTY object as its "null plan"
    // sentinel (the missing-write() error path -- JS `return null`); a
    // real plan always has at least a "chain" key, so emptiness alone
    // distinguishes the two cases.
    const JsObject compiled = compileChainStatement(stmt);
    return compiled.isEmpty() ? JsValue(JsValue::Undefined) : JsValue(compiled);
}

JsObject Validator::compileChainStatement(const JsObject& stmt) {
    JsArray chain;
    const JsArray stmtChain = stmt.value(JsText(u"chain")).toArray();

    JsObject chainNode;
    chainNode.insert(JsText(u"type"), NodeKind::Chain);
    chainNode.insert(JsText(u"chain"), stmtChain);
    const bool hasWrite = !stmt.value(JsText(u"write")).isNull() || !stmt.value(JsText(u"write3d")).isNull();

    if (!hasWrite && isStarterChain(chainNode)) {
        pushDiag(JsText(u"S006"), stmtChain.isEmpty() ? JsValue(JsValue::Undefined) : stmtChain.first());
    }
    if (!hasWrite) {
        pushDiag(JsText(u"S001"), stmtChain.isEmpty() ? JsValue(JsValue::Undefined) : stmtChain.first(),
                  JsText(u"Chain must have explicit write() or write3d() target"));
        return JsObject(); // null plan (JS `return null`)
    }

    const JsValue writeVal = stmt.value(JsText(u"write"));
    const JsText writeName = writeVal.isObject() ? writeVal.toObject().value(JsText(u"name")).toString() : JsText();

    JsValue write3dTarget = JsValue(JsValue::Null);
    if (!stmt.value(JsText(u"write3d")).isNull()) {
        const JsObject w3 = stmt.value(JsText(u"write3d")).toObject();
        JsObject tex3d;
        tex3d.insert(JsText(u"kind"), JsText(u"vol"));
        tex3d.insert(JsText(u"name"), surfaceName(w3.value(JsText(u"tex3d"))));
        JsObject geo;
        geo.insert(JsText(u"kind"), JsText(u"geo"));
        geo.insert(JsText(u"name"), surfaceName(w3.value(JsText(u"geo"))));
        JsObject w3out;
        w3out.insert(JsText(u"tex3d"), tex3d);
        w3out.insert(JsText(u"geo"), geo);
        write3dTarget = w3out;
    }

    const int finalIndex = processChain(chain, stmtChain, -1, false, writeName);

    JsValue writeSurf = JsValue(JsValue::Null);
    if (writeVal.isObject()) {
        JsObject ws;
        ws.insert(JsText(u"kind"), JsText(u"output"));
        ws.insert(JsText(u"name"), writeVal.toObject().value(JsText(u"name")));
        writeSurf = ws;
    }

    JsObject plan;
    plan.insert(JsText(u"chain"), chain);
    plan.insert(JsText(u"write"), writeSurf);
    plan.insert(JsText(u"write3d"), write3dTarget);
    plan.insert(JsText(u"final"), finalIndex < 0 ? JsValue(JsValue::Null) : JsValue(finalIndex));
    plan.insert(JsText(u"states"), JsArray()); // no validatorHooks are ever registered (dead feature)
    if (stmt.contains(JsText(u"leadingComments"))) {
        plan.insert(JsText(u"leadingComments"), stmt.value(JsText(u"leadingComments")));
    }
    return plan;
}

// ---------------------------------------------------------------- chain flattening (reference/02 SS5)

int Validator::processChain(JsArray& chain, const JsArray& calls, int input, bool allowStarterless,
                             const JsText& writeName) {
    int current = input;
    for (const JsValue& originalVal : calls) {
        const JsObject original = originalVal.toObject();
        const JsText ot = original.value(JsText(u"type")).toString();

        // read() builtin -- a STARTER node; illegal to chain inline.
        if (ot == NodeKind::Read) {
            if (current != -1) {
                pushDiag(JsText(u"S001"), originalVal,
                          JsText(u"read() is a starter node and cannot be chained inline. Use standalone "
                                         "read() to start a new chain."));
                continue;
            }
            const JsValue surface = toSurface(original.value(JsText(u"surface")));
            if (surface.isUndefined()) {
                pushDiag(JsText(u"S001"), originalVal, JsText(u"read() requires a valid surface reference"));
                continue;
            }
            const int idx = tempIndex_++;
            JsObject stepArgs;
            stepArgs.insert(JsText(u"tex"), surface);
            if (original.value(JsText(u"_skip")).toBool(false)) stepArgs.insert(JsText(u"_skip"), true);
            JsObject step;
            step.insert(JsText(u"op"), JsText(u"_read"));
            step.insert(JsText(u"args"), stepArgs);
            step.insert(JsText(u"from"), JsValue(JsValue::Null));
            step.insert(JsText(u"temp"), idx);
            step.insert(JsText(u"builtin"), true);
            if (original.contains(JsText(u"leadingComments"))) {
                step.insert(JsText(u"leadingComments"), original.value(JsText(u"leadingComments")));
            }
            chain.append(step);
            current = idx;
            continue;
        }

        // read3d() two-arg starter form (single-arg read3d() is handled
        // inside volume/geometry param resolution, not here).
        if (ot == NodeKind::Read3D && !original.value(JsText(u"geo")).isNull()) {
            if (current != -1) {
                pushDiag(JsText(u"S001"), originalVal,
                          JsText(u"read3d() is a starter node and cannot be chained inline. Use standalone "
                                         "read3d() to start a new chain."));
                continue;
            }
            const JsValue tex3d = make3dRef(original.value(JsText(u"tex3d")), JsText(u"vol"));
            const JsValue geo = make3dRef(original.value(JsText(u"geo")), JsText(u"geo"));
            if (tex3d.isUndefined() || geo.isUndefined()) {
                pushDiag(JsText(u"S001"), originalVal, JsText(u"read3d() as starter requires tex3d and geo references"));
                continue;
            }
            const int idx = tempIndex_++;
            JsObject stepArgs;
            stepArgs.insert(JsText(u"tex3d"), tex3d);
            stepArgs.insert(JsText(u"geo"), geo);
            if (original.value(JsText(u"_skip")).toBool(false)) stepArgs.insert(JsText(u"_skip"), true);
            JsObject step;
            step.insert(JsText(u"op"), JsText(u"_read3d"));
            step.insert(JsText(u"args"), stepArgs);
            step.insert(JsText(u"from"), JsValue(JsValue::Null));
            step.insert(JsText(u"temp"), idx);
            step.insert(JsText(u"builtin"), true);
            if (original.contains(JsText(u"leadingComments"))) {
                step.insert(JsText(u"leadingComments"), original.value(JsText(u"leadingComments")));
            }
            chain.append(step);
            current = idx;
            continue;
        }

        // write() builtin -- chainable, requires an input.
        if (ot == NodeKind::Write) {
            const JsValue surface = toSurface(original.value(JsText(u"surface")));
            if (surface.isUndefined()) {
                pushDiag(JsText(u"S001"), originalVal, JsText(u"write() requires a valid surface reference"));
                continue;
            }
            if (current == -1) {
                pushDiag(JsText(u"S005"), originalVal, JsText(u"write() requires an input - cannot be first in chain"));
                continue;
            }
            const int idx = tempIndex_++;
            JsObject stepArgs;
            stepArgs.insert(JsText(u"tex"), surface);
            JsObject step;
            step.insert(JsText(u"op"), JsText(u"_write"));
            step.insert(JsText(u"args"), stepArgs);
            step.insert(JsText(u"from"), current);
            step.insert(JsText(u"temp"), idx);
            step.insert(JsText(u"builtin"), true);
            if (original.contains(JsText(u"leadingComments"))) {
                step.insert(JsText(u"leadingComments"), original.value(JsText(u"leadingComments")));
            }
            chain.append(step);
            current = idx;
            continue;
        }

        // write3d() chain node.
        if (ot == NodeKind::Write3D) {
            const JsValue tex3d = make3dRef(original.value(JsText(u"tex3d")), JsText(u"vol"));
            const JsValue geo = make3dRef(original.value(JsText(u"geo")), JsText(u"geo"));
            if (tex3d.isUndefined() || geo.isUndefined()) {
                pushDiag(JsText(u"S001"), originalVal, JsText(u"write3d() requires tex3d and geo references"));
                continue;
            }
            if (current == -1) {
                pushDiag(JsText(u"S005"), originalVal, JsText(u"write3d() requires an input - cannot be first in chain"));
                continue;
            }
            const int idx = tempIndex_++;
            JsObject stepArgs;
            stepArgs.insert(JsText(u"tex3d"), tex3d);
            stepArgs.insert(JsText(u"geo"), geo);
            JsObject step;
            step.insert(JsText(u"op"), JsText(u"_write3d"));
            step.insert(JsText(u"args"), stepArgs);
            step.insert(JsText(u"from"), current);
            step.insert(JsText(u"temp"), idx);
            step.insert(JsText(u"builtin"), true);
            if (original.contains(JsText(u"leadingComments"))) {
                step.insert(JsText(u"leadingComments"), original.value(JsText(u"leadingComments")));
            }
            chain.append(step);
            current = idx;
            continue;
        }

        // subchain() -- first-class grouping bracket. NOTE: no
        // "iterations" field anywhere (see file header).
        if (ot == NodeKind::Subchain) {
            // Surface parser-attached subchain-argument reports
            // once per subchain node, in source order.
            const JsValue argDiagsVal = original.value(JsText(u"subchainArgumentDiagnostics"));
            if (argDiagsVal.isArray()) {
                const JsArray argDiags = argDiagsVal.toArray();
                for (const JsValue& dv : argDiags) {
                    if (!dv.isObject()) continue;
                    const JsObject rep = dv.toObject();
                    JsObject diag;
                    diag.insert(JsText(u"code"), rep.value(JsText(u"code")));
                    diag.insert(JsText(u"message"), rep.value(JsText(u"message")));
                    diag.insert(JsText(u"severity"), rep.value(JsText(u"severity")));
                    diag.insert(JsText(u"nodeId"), original.value(JsText(u"id")));
                    if (rep.contains(JsText(u"location"))) {
                        diag.insert(JsText(u"location"), rep.value(JsText(u"location")));
                    }
                    diagnostics_.append(diag);
                }
            }

            if (current == -1) {
                pushDiag(JsText(u"S005"), originalVal, JsText(u"subchain() requires an input - cannot be first in chain"));
                continue;
            }
            const int beginIdx = tempIndex_++;
            JsObject beginArgs;
            beginArgs.insert(JsText(u"name"), original.value(JsText(u"name")));
            beginArgs.insert(JsText(u"id"), original.value(JsText(u"id")));
            JsObject beginStep;
            beginStep.insert(JsText(u"op"), JsText(u"_subchain_begin"));
            beginStep.insert(JsText(u"args"), beginArgs);
            beginStep.insert(JsText(u"from"), current);
            beginStep.insert(JsText(u"temp"), beginIdx);
            beginStep.insert(JsText(u"builtin"), true);
            if (original.contains(JsText(u"leadingComments"))) {
                beginStep.insert(JsText(u"leadingComments"), original.value(JsText(u"leadingComments")));
            }
            chain.append(beginStep);
            current = beginIdx;

            current = processChain(chain, original.value(JsText(u"body")).toArray(), current, false, writeName);

            const int endIdx = tempIndex_++;
            JsObject endArgs;
            endArgs.insert(JsText(u"name"), original.value(JsText(u"name")));
            endArgs.insert(JsText(u"id"), original.value(JsText(u"id")));
            JsObject endStep;
            endStep.insert(JsText(u"op"), JsText(u"_subchain_end"));
            endStep.insert(JsText(u"args"), endArgs);
            endStep.insert(JsText(u"from"), current == -1 ? JsValue(JsValue::Null) : JsValue(current));
            endStep.insert(JsText(u"temp"), endIdx);
            endStep.insert(JsText(u"builtin"), true);
            chain.append(endStep);
            current = endIdx;
            continue;
        }

        // regular effect call (reference/02 SS5.2)
        JsObject call = resolveCall(original).toObject();
        const JsValue callNs = call.value(JsText(u"namespace"));
        JsList searchOrder = programSearchOrder_;
        if (callNs.isObject() && callNs.toObject().value(JsText(u"searchOrder")).isArray()) {
            searchOrder.clear();
            for (const JsValue& v : callNs.toObject().value(JsText(u"searchOrder")).toArray()) searchOrder.append(v.toString());
        }
        JsList candidateNames;
        if (callNs.isObject() && isTruthy(callNs.toObject().value(JsText(u"resolved")))) {
            candidateNames.append(callNs.toObject().value(JsText(u"resolved")).toString() + u'.'
                                   + call.value(JsText(u"name")).toString());
        }
        for (const JsText& ns : searchOrder) candidateNames.append(ns + u'.' + call.value(JsText(u"name")).toString());
        JsText opName;
        const OpSpec* spec = nullptr;
        for (const JsText& candidate : candidateNames) {
            if (candidate.isEmpty()) continue;
            if (const OpSpec* s = reg_.getOp(candidate)) {
                opName = candidate;
                spec = s;
                break;
            }
        }
        if (!spec) {
            pushDiag(JsText(u"S001"), originalVal,
                      JsText(u"Unknown effect: '%1'").arg(call.value(JsText(u"name")).toVariant()));
            continue;
        }
        const JsText aliasWarning = reg_.checkEffectAlias(opName);
        if (!aliasWarning.isEmpty()) pushDiag(JsText(u"S008"), originalVal, aliasWarning);

        if (opName == JsText(u"prev")) {
            const int idx = tempIndex_++;
            JsObject prevTex;
            prevTex.insert(JsText(u"kind"), JsText(u"output"));
            prevTex.insert(JsText(u"name"), writeName);
            JsObject prevArgs;
            prevArgs.insert(JsText(u"tex"), prevTex);
            JsObject step;
            step.insert(JsText(u"op"), opName);
            step.insert(JsText(u"args"), prevArgs);
            step.insert(JsText(u"from"), current == -1 ? JsValue(JsValue::Null) : JsValue(current));
            step.insert(JsText(u"temp"), idx);
            const JsValue nsSnap = buildNamespaceSnapshot(callNs);
            if (!nsSnap.isUndefined()) step.insert(JsText(u"namespace"), nsSnap);
            if (original.contains(JsText(u"leadingComments"))) {
                step.insert(JsText(u"leadingComments"), original.value(JsText(u"leadingComments")));
            }
            chain.append(step);
            current = idx;
            continue;
        }

        const bool starter = reg_.isStarterOp(opName);
        const bool starterlessRoot = (current == -1);
        const bool allowPassthroughRoot = allowStarterless && surfacePassthroughCalls().contains(opName);
        if (starterlessRoot && !starter && !allowPassthroughRoot) {
            pushDiag(JsText(u"S005"), originalVal);
            continue;
        }
        const bool starterHasInput = starter && current != -1;
        const int fromInput = starterHasInput ? -1 : current;
        if (starterHasInput) pushDiag(JsText(u"S005"), originalVal);

        JsObject args;
        JsObject resolvedKwargs;
        const JsValue argSources = resolveArgs(chain, *spec, call, opName, original, args, writeName,
                                               &resolvedKwargs);
        // resolveCall spreads a stored Call's kwargs, retaining references
        // to nested argument objects in JS. Member-path normalization of an
        // inherited argument is therefore visible in a later `return var`.
        const JsText symbolName = original.value(JsText(u"name")).toString();
        if (symbols_.contains(symbolName) && nodeType(symbols_.value(symbolName)) == NodeKind::Call) {
            JsObject stored = symbols_.value(symbolName).toObject();
            JsObject storedKwargs = stored.value(JsText(u"kwargs")).toObject();
            const JsObject callKwargs = original.value(JsText(u"kwargs")).toObject();
            for (const ParamDef& def : spec->args) {
                if (def.type != JsText(u"member") || callKwargs.contains(def.name)
                    || nodeType(storedKwargs.value(def.name)) != NodeKind::Member
                    || nodeType(resolvedKwargs.value(def.name)) != NodeKind::Member) continue;
                storedKwargs.insert(def.name, resolvedKwargs.value(def.name));
            }
            stored.insert(JsText(u"kwargs"), storedKwargs);
            symbols_.insert(symbolName, stored);
        }

        const int idx = tempIndex_++;
        JsObject step;
        step.insert(JsText(u"op"), opName);
        step.insert(JsText(u"args"), args);
        step.insert(JsText(u"from"), fromInput == -1 ? JsValue(JsValue::Null) : JsValue(fromInput));
        step.insert(JsText(u"temp"), idx);
        const JsValue nsSnap = buildNamespaceSnapshot(callNs);
        if (!nsSnap.isUndefined()) step.insert(JsText(u"namespace"), nsSnap);
        if (original.contains(JsText(u"leadingComments"))) {
            step.insert(JsText(u"leadingComments"), original.value(JsText(u"leadingComments")));
        }
        if (original.value(JsText(u"kwargs")).isObject() && !original.value(JsText(u"kwargs")).toObject().isEmpty()) {
            // resolveCall returns the original call object when there is no
            // symbol binding. In JS the alias and Member-path writes then
            // mutate the same kwargs object used for rawKwargs.
            const bool sharedCall = !symbols_.contains(original.value(JsText(u"name")).toString());
            step.insert(JsText(u"rawKwargs"), sharedCall ? JsValue(resolvedKwargs)
                                                          : original.value(JsText(u"kwargs")));
        }
        if (!argSources.isUndefined()) step.insert(JsText(u"argSources"), argSources);
        chain.append(step);
        current = idx;
    }
    return current;
}

// ---------------------------------------------------------------- argument resolution (reference/02 SS6)

bool matchesRefPattern(const JsText& name, const JsText& prefix) {
    if (name.size() != prefix.size() + 1 || !name.startsWith(prefix)) return false;
    const char16_t d = name.at(prefix.size());
    return d >= u'0' && d <= u'7';
}

JsValue Validator::resolveArgs(JsArray& chain, const OpSpec& spec, const JsObject& call,
                                   const JsText& opName, const JsObject& original, JsObject& args,
                                   const JsText& writeName, JsObject* resolvedKwargs) {
    const bool hasKw = call.contains(JsText(u"kwargs"));
    JsObject kw = hasKw ? call.value(JsText(u"kwargs")).toObject() : JsObject();
    if (hasKw) {
        const JsList warnings = reg_.resolveParamAliases(opName, kw);
        for (const JsText& w : warnings) pushDiag(JsText(u"S007"), call, w);
    }
    const JsArray callArgs = call.value(JsText(u"args")).toArray();
    JsSet<JsText> seen;
    JsObject argSources;
    bool haveArgSources = false;

    for (int i = 0; i < spec.args.size(); ++i) {
        const ParamDef& def = spec.args.at(i);
        JsValue node = (hasKw && kw.contains(def.name)) ? kw.value(def.name)
                                                             : (i < callArgs.size() ? callArgs.at(i) : JsValue(JsValue::Undefined));
        node = substitute(node);
        const JsText argKey = def.name;

        // color-splat special case: a bare Color literal spread into THREE
        // consecutive r/g/b NUMERIC params (only fires positional-only,
        // never with kwargs). Structurally unreachable on this catalog (no
        // effect declares consecutive r/g/b globals) -- ported for
        // fidelity, never exercised.
        if (!hasKw && nodeType(node) == NodeKind::Color && def.type != JsText(u"color") && def.name == JsText(u"r")
            && i + 2 < spec.args.size() && spec.args.at(i + 1).name == JsText(u"g")
            && spec.args.at(i + 2).name == JsText(u"b")) {
            const JsArray cv = node.toObject().value(JsText(u"value")).toArray();
            args.insert(JsText(u"r"), cv.at(0));
            args.insert(spec.args.at(i + 1).name, cv.at(1));
            args.insert(spec.args.at(i + 2).name, cv.at(2));
            i += 2;
            continue;
        }

        if (hasKw && kw.contains(def.name)) seen.insert(def.name);

        // array literal -- additive numeric input form.
        if (nodeType(node) == NodeKind::ArrayLiteral) {
            JsArray value;
            for (const JsValue& el : node.toObject().value(JsText(u"elements")).toArray()) {
                if (nodeType(el) == NodeKind::Number) {
                    value.append(el.toObject().value(JsText(u"value")));
                } else {
                    pushDiag(JsText(u"S002"), el,
                              JsText(u"Array element must be a number for '%1' in %2()")
                                  .arg(def.name, call.value(JsText(u"name")).toString()));
                    value.append(0);
                }
            }
            args.insert(argKey, value);
            argSources.insert(argKey, JsText(u"array"));
            haveArgSources = true;
            continue;
        }

        const JsText ty = def.type;
        if (ty == JsText(u"surface")) {
            resolveSurfaceArg(chain, def, node, call, args, argKey, writeName);
            if (hasKw && kw.contains(def.name)) {
                kw.insert(def.name, mirrorNestedMemberPaths(kw.value(def.name)));
            }
        } else if (ty == JsText(u"color")) {
            resolveColorArg(def, node, call, args, argKey);
        } else if (ty == JsText(u"vec3")) {
            resolveVecArg(def, node, call, args, argKey, 3);
        } else if (ty == JsText(u"vec4")) {
            resolveVecArg(def, node, call, args, argKey, 4);
        } else if (ty == JsText(u"boolean")) {
            resolveBooleanArg(def, node, call, args, argKey);
        } else if (ty == JsText(u"member")) {
            JsList selectedPath;
            resolveMemberArg(def, node, call, args, argKey, &selectedPath);
            if (hasKw && nodeType(kw.value(def.name)) == NodeKind::Member && !selectedPath.isEmpty()) {
                JsObject member = kw.value(def.name).toObject();
                JsArray path;
                for (const JsText& segment : selectedPath) path.append(segment);
                member.insert(JsText(u"path"), path);
                kw.insert(def.name, member);
            }
        } else if (ty == JsText(u"volume")) {
            resolveVolumeOrGeometryArg(def, node, args, argKey, JsText(u"vol"));
        } else if (ty == JsText(u"geometry")) {
            resolveVolumeOrGeometryArg(def, node, args, argKey, JsText(u"geo"));
        } else if (ty == JsText(u"string")) {
            resolveStringArg(def, node, opName, original, args, argKey);
        } else {
            // numeric catch-all: int/float/palette/vec2/mat3/anything else
            // not one of the eight explicitly-dispatched types (see file
            // header).
            resolveNumericArg(def, node, call, spec, args, argKey);
        }
    }

    // _skip meta-argument (reference/02 SS6.14).
    if (hasKw && kw.contains(JsText(u"_skip"))) {
        const JsValue skipNode = kw.value(JsText(u"_skip"));
        args.insert(JsText(u"_skip"),
                     nodeType(skipNode) == NodeKind::Boolean && skipNode.toObject().value(JsText(u"value")).toBool());
        seen.insert(JsText(u"_skip"));
    }
    // unknown-kwarg sweep.
    if (hasKw) {
        for (auto it = kw.constBegin(); it != kw.constEnd(); ++it) {
            if (!seen.contains(it.key())) {
                pushDiag(JsText(u"S001"), it.value(),
                          JsText(u"Unknown argument '%1' for %2()").arg(it.key(), call.value(JsText(u"name")).toString()));
            }
        }
    }
    if (resolvedKwargs) *resolvedKwargs = kw;
    return haveArgSources ? JsValue(argSources) : JsValue(JsValue::Undefined);
}

// A direct call's rawKwargs holds the original AST. Nested Member argument
// objects are shared with the nested call in JS, so the member-path rewrite
// performed while compiling an inline surface also appears in rawKwargs.
// The native Value tree uses value copies; reproduce that visible mutation.
JsValue Validator::mirrorNestedMemberPaths(const JsValue& nodeVal) {
    if (!nodeVal.isObject()) return nodeVal;
    JsObject node = nodeVal.toObject();
    const JsText type = node.value(JsText(u"type")).toString();
    if (type == NodeKind::Chain) {
        JsArray rewritten;
        for (const JsValue& part : node.value(JsText(u"chain")).toArray()) {
            rewritten.append(mirrorNestedMemberPaths(part));
        }
        node.insert(JsText(u"chain"), rewritten);
        return node;
    }
    if (type != NodeKind::Call || !node.value(JsText(u"kwargs")).isObject()) return node;

    JsObject kwargs = node.value(JsText(u"kwargs")).toObject();
    const JsObject effective = resolveCall(node).toObject();
    const JsText name = effective.value(JsText(u"name")).toString();
    const JsObject callNamespace = effective.value(JsText(u"namespace")).toObject();
    JsList searchOrder = programSearchOrder_;
    if (callNamespace.value(JsText(u"searchOrder")).isArray()) {
        searchOrder.clear();
        for (const JsValue& ns : callNamespace.value(JsText(u"searchOrder")).toArray()) {
            searchOrder.append(ns.toString());
        }
    }
    JsList candidates;
    if (isTruthy(callNamespace.value(JsText(u"resolved")))) {
        candidates.append(callNamespace.value(JsText(u"resolved")).toString() + u'.' + name);
    }
    for (const JsText& ns : searchOrder) candidates.append(ns + u'.' + name);
    const OpSpec* spec = nullptr;
    for (const JsText& candidate : candidates) {
        spec = reg_.getOp(candidate);
        if (spec) break;
    }
    if (spec) {
        for (const ParamDef& def : spec->args) {
            const JsValue originalArg = kwargs.value(def.name);
            if (def.type != JsText(u"member") || nodeType(originalArg) != NodeKind::Member) continue;
            JsList path = normalizeMemberPath(originalArg.toObject().value(JsText(u"path")));
            const JsList prefix = normalizeMemberPath(def.hasEnumPath() ? def.enumPath : JsValue());
            if (!resolveEnum(path).isDouble()) {
                path = applyEnumPrefix(path, prefix);
                if (!prefix.isEmpty() && !pathStartsWith(path, prefix)) path = prefix;
            }
            JsArray pathArray;
            for (const JsText& segment : path) pathArray.append(segment);
            JsObject member = originalArg.toObject();
            member.insert(JsText(u"path"), pathArray);
            kwargs.insert(def.name, member);
        }
    }
    // Descend into deeper inline surfaces without renaming this call's
    // kwargs: alias renames mutate the nested call's copy, not its parent AST.
    for (auto it = kwargs.constBegin(); it != kwargs.constEnd(); ++it) {
        kwargs.insert(it.key(), mirrorNestedMemberPaths(it.value()));
    }
    node.insert(JsText(u"kwargs"), kwargs);
    return node;
}

// 6.1 surface
void Validator::resolveSurfaceArg(JsArray& chain, const ParamDef& def, const JsValue& node,
                                   const JsObject& call, JsObject& args, const JsText& argKey,
                                   const JsText& writeName) {
    if (nodeType(node) == NodeKind::String) {
        pushDiag(JsText(u"S001"), node, JsText(u"String literal not allowed for surface parameter '%1'").arg(def.name));
        JsValue dflt = JsValue(JsValue::Null);
        if (def.defaultValue.isString()) {
            JsObject ident;
            ident.insert(JsText(u"type"), NodeKind::Ident);
            ident.insert(JsText(u"name"), def.defaultValue);
            const JsValue s = toSurface(ident);
            if (!s.isUndefined()) dflt = s;
        }
        args.insert(argKey, dflt);
        return;
    }
    JsValue surf = JsValue(JsValue::Undefined);
    bool invalidStarterChain = false;
    const JsValue starter = node.isUndefined() ? JsValue(JsValue::Undefined) : getStarterInfo(node);

    if (nodeType(node) == NodeKind::Read && node.toObject().value(JsText(u"surface")).isObject()) {
        surf = toSurface(node.toObject().value(JsText(u"surface")));
    }
    const JsValue inlineSurface = !surf.isUndefined() ? surf : callToSurface(node);
    if (!inlineSurface.isUndefined()) {
        surf = inlineSurface;
    } else if (nodeType(node) == NodeKind::Chain) {
        const int idx = processChain(chain, node.toObject().value(JsText(u"chain")).toArray(), -1, true, writeName);
        if (idx != -1) {
            JsObject t;
            t.insert(JsText(u"kind"), JsText(u"temp"));
            t.insert(JsText(u"index"), idx);
            surf = t;
        }
    } else if (nodeType(node) == NodeKind::Call) {
        JsArray single;
        single.append(node);
        const int idx = processChain(chain, single, -1, true, writeName);
        if (idx != -1) {
            JsObject t;
            t.insert(JsText(u"kind"), JsText(u"temp"));
            t.insert(JsText(u"index"), idx);
            surf = t;
        }
    } else if (!starter.isUndefined()) {
        pushDiag(JsText(u"S005"), starter.toObject().value(JsText(u"call")));
        invalidStarterChain = true;
    } else {
        surf = toSurface(node);
    }

    if (surf.isUndefined()) {
        if (invalidStarterChain) {
            args.insert(argKey, JsValue(JsValue::Null));
            return;
        }
        const bool hasDefault = def.defaultValue.isString();
        if (!hasDefault) {
            if (node.isUndefined()) {
                pushDiag(JsText(u"S001"), call,
                          JsText(u"Missing required surface argument '%1' for %2()")
                              .arg(def.name, call.value(JsText(u"name")).toString()));
            } else if (nodeType(node) == NodeKind::Ident
                       && !symbols_.contains(node.toObject().value(JsText(u"name")).toString())) {
                pushDiag(JsText(u"S003"), node,
                          JsText(u"Undefined variable '%1' for '%2' in %3()")
                              .arg(node.toObject().value(JsText(u"name")).toString(), def.name,
                                   call.value(JsText(u"name")).toString()));
            } else {
                const JsObject n = node.toObject();
                JsText nodeName;
                if (isTruthy(n.value(JsText(u"name")))) {
                    nodeName = n.value(JsText(u"name")).toString();
                } else if (n.value(JsText(u"path")).isArray()) {
                    JsList segs;
                    for (const JsValue& s : n.value(JsText(u"path")).toArray()) segs.append(s.toString());
                    nodeName = segs.join(u'.');
                } else if (isTruthy(n.value(JsText(u"value")))) {
                    const JsValue v = n.value(JsText(u"value"));
                    nodeName = v.isDouble() ? js::numberToString(v.toDouble()) : v.toVariant().toString();
                } else if (isTruthy(n.value(JsText(u"type")))) {
                    nodeName = n.value(JsText(u"type")).toString();
                } else {
                    nodeName = JsText(u"invalid");
                }
                pushDiag(JsText(u"S001"), node,
                          JsText(u"Invalid surface reference '%1' for '%2' in %3()")
                              .arg(nodeName, def.name, call.value(JsText(u"name")).toString()));
            }
        }
        if (hasDefault) {
            JsObject ident;
            ident.insert(JsText(u"type"), NodeKind::Ident);
            ident.insert(JsText(u"name"), def.defaultValue);
            const JsValue s = toSurface(ident);
            if (!s.isUndefined()) {
                surf = s;
            } else {
                JsObject pipe;
                pipe.insert(JsText(u"kind"), JsText(u"pipeline"));
                pipe.insert(JsText(u"name"), def.defaultValue);
                surf = pipe;
            }
        }
    }
    args.insert(argKey, surf.isUndefined() ? JsValue(JsValue::Null) : surf);
}

// 6.2 color
void Validator::resolveColorArg(const ParamDef& def, const JsValue& node, const JsObject& call,
                                 JsObject& args, const JsText& argKey) {
    if (nodeType(node) == NodeKind::String) {
        pushDiag(JsText(u"S001"), node, JsText(u"String literal not allowed for color parameter '%1'").arg(def.name));
        args.insert(argKey, def.defaultValue);
        return;
    }
    if (nodeType(node) == NodeKind::Color) {
        // reference: node.hex || node.value -- T8's parser never emits a
        // `.hex` field on Color nodes (verified: 339/339 PARSE parity with
        // only `.value`), so this always resolves to `.value`.
        args.insert(argKey, node.toObject().value(JsText(u"value")));
        return;
    }
    if (!node.isUndefined() && nodeType(node) != NodeKind::Ident) {
        pushDiag(JsText(u"S002"), node,
                  JsText(u"Argument out of range for '%1' in %2()").arg(def.name, call.value(JsText(u"name")).toString()));
    }
    args.insert(argKey, def.defaultValue);
}

// 6.3 / 6.4 vec3 / vec4
void Validator::resolveVecArg(const ParamDef& def, const JsValue& node, const JsObject& call, JsObject& args,
                               const JsText& argKey, int n) {
    const JsText ctor = n == 3 ? JsText(u"vec3") : JsText(u"vec4");
    auto defaultOrZero = [&]() -> JsValue {
        if (def.defaultValue.isArray()) return def.defaultValue;
        JsArray z;
        for (int i = 0; i < n; ++i) z.append(0.0);
        if (n == 4) z[3] = 1.0;
        return z;
    };
    if (nodeType(node) == NodeKind::String) {
        pushDiag(JsText(u"S001"), node, JsText(u"String literal not allowed for %1 parameter '%2'").arg(ctor, def.name));
        args.insert(argKey, defaultOrZero());
        return;
    }
    if (nodeType(node) == NodeKind::Call && node.toObject().value(JsText(u"name")).toString() == ctor
        && node.toObject().value(JsText(u"args")).toArray().size() == n) {
        JsArray value;
        for (const JsValue& a : node.toObject().value(JsText(u"args")).toArray()) {
            if (nodeType(a) == NodeKind::Number) {
                value.append(a.toObject().value(JsText(u"value")));
            } else {
                pushDiag(JsText(u"S002"), a,
                          JsText(u"Argument out of range for '%1' in %2()").arg(def.name, call.value(JsText(u"name")).toString()));
                value.append(0);
            }
        }
        args.insert(argKey, value);
        return;
    }
    if (nodeType(node) == NodeKind::Color) {
        const JsArray cv = node.toObject().value(JsText(u"value")).toArray();
        JsArray value;
        for (int i = 0; i < n && i < cv.size(); ++i) value.append(cv.at(i));
        args.insert(argKey, value);
        return;
    }
    if (!node.isUndefined() && nodeType(node) != NodeKind::Ident) {
        pushDiag(JsText(u"S002"), node,
                  JsText(u"Argument out of range for '%1' in %2()").arg(def.name, call.value(JsText(u"name")).toString()));
    }
    args.insert(argKey, defaultOrZero());
}

// 6.5 boolean
void Validator::resolveBooleanArg(const ParamDef& def, const JsValue& node, const JsObject& call,
                                   JsObject& args, const JsText& argKey) {
    auto defaultBool = [&]() { return isTruthy(def.defaultValue) ? true : false; };
    if (node.isUndefined()) {
        args.insert(argKey, defaultBool());
        return;
    }
    const JsText t = nodeType(node);
    if (t == NodeKind::String) {
        pushDiag(JsText(u"S001"), node, JsText(u"String literal not allowed for boolean parameter '%1'").arg(def.name));
        args.insert(argKey, defaultBool());
        return;
    }
    if (t == NodeKind::Boolean) {
        args.insert(argKey, node.toObject().value(JsText(u"value")).toBool());
        return;
    }
    if (t == NodeKind::Number) {
        args.insert(argKey, node.toObject().value(JsText(u"value")).toDouble() != 0.0);
        return;
    }
    // `() => expr`: retain the function property for raw ProgramState values.
    if (t == NodeKind::Func) {
        const JsText src = node.toObject().value(JsText(u"src")).toString();
        if (funcCompiles(src)) {
            args.insert(argKey, JsObject{{JsText(u"fn"), Value::function(src)}});
        } else {
            pushDiag(JsText(u"S001"), node,
                     JsText(u"Invalid function for '%1': '%2'").arg(def.name, funcSnippet(src)));
            args.insert(argKey, defaultBool());
        }
        return;
    }
    const JsText identName = node.toObject().value(JsText(u"name")).toString();
    // A bare state-value ident (time/frame/...) retains its fn property.
    if (t == NodeKind::Ident && stateValues().contains(identName)) {
        args.insert(argKey, JsObject{{JsText(u"fn"), Value::function(identName)}});
        return;
    }
    if (t == NodeKind::Ident) {
        pushDiag(JsText(u"S003"), node);
    } else {
        pushDiag(JsText(u"S002"), node,
                  JsText(u"Argument out of range for '%1' in %2()").arg(def.name, call.value(JsText(u"name")).toString()));
    }
    args.insert(argKey, defaultBool());
}

// 6.6 member (dedicated enum-typed param -- e.g. filter.channel,
// filter.palette, filter3d.palette3d, synth.osc2d ONLY; see file header).
// Falls back to 0 on total failure; NEVER pushes a diagnostic for an
// unresolved value (verified against the oracle:
// `filter.channel(channel: bogus)` -> channel=0, diagnostics:[]).
bool Validator::isOwnChoice(const ParamDef& def, const JsText& name) {
    // Reference 29e76468 validator.js isOwnChoice: an inline choice whose
    // value is a number, or a member of the param's own enum (enumPath or
    // enum; ParamDef.enumPath already merges `spec.enum || spec.enumPath`),
    // resolves as that choice even when the name shadows a state value.
    if (def.hasChoices() && def.choicesObject().value(name).isDouble()) return true;
    if (!def.hasEnumPath()) return false;
    const JsValue resolved = resolveEnum(applyEnumPrefix(JsList{name}, normalizeMemberPath(def.enumPath)));
    return resolved.isDouble();
}

void Validator::resolveMemberArg(const ParamDef& def, const JsValue& node, const JsObject& call,
                                  JsObject& args, const JsText& argKey, JsList* selectedPath) {
    if (nodeType(node) == NodeKind::String) {
        pushDiag(JsText(u"S001"), node, JsText(u"String literal not allowed for member/enum parameter '%1'").arg(def.name));
        args.insert(argKey, def.defaultValue);
        return;
    }
    const JsList prefix = normalizeMemberPath(def.hasEnumPath() ? def.enumPath : JsValue(JsValue::Undefined));
    JsList path;
    const JsText t = nodeType(node);
    if (t == NodeKind::Member) {
        path = normalizeMemberPath(node.toObject().value(JsText(u"path")));
    } else if (t == NodeKind::Number || t == NodeKind::Boolean) {
        const JsObject n = node.toObject();
        args.insert(argKey, t == NodeKind::Boolean ? JsValue(n.value(JsText(u"value")).toBool() ? 1 : 0)
                                                     : n.value(JsText(u"value")));
        return;
    } else if (t == NodeKind::Ident && stateValues().contains(node.toObject().value(JsText(u"name")).toString())
               && !isOwnChoice(def, node.toObject().value(JsText(u"name")).toString())) {
        // Reference member branch: `{fn: (state) => state[key]}`.
        args.insert(argKey, JsObject{{JsText(u"fn"), Value::function(node.toObject().value(JsText(u"name")).toString())}});
        return;
    } else if (t == NodeKind::Ident) {
        path = {node.toObject().value(JsText(u"name")).toString()};
    }
    if (path.isEmpty()) path = normalizeMemberPath(def.defaultValue);

    JsValue resolved = path.isEmpty() ? JsValue(JsValue::Undefined) : resolveEnum(path);
    if (!resolved.isDouble()) {
        path = applyEnumPrefix(path, prefix);
        if (!prefix.isEmpty() && !pathStartsWith(path, prefix)) {
            pushDiag(JsText(u"S001"), !node.isUndefined() ? node : JsValue(call),
                      JsText(u"Invalid enum value for '%1': expected path starting with '%2'").arg(def.name, prefix.join(u'.')));
            path = prefix;
        }
        resolved = path.isEmpty() ? JsValue(JsValue::Undefined) : resolveEnum(path);
    }
    if (!resolved.isDouble()) {
        const JsList fallback = normalizeMemberPath(def.defaultValue);
        const JsValue fallbackValue = fallback.isEmpty() ? JsValue(JsValue::Undefined) : resolveEnum(fallback);
        resolved = fallbackValue.isDouble() ? fallbackValue : JsValue(0.0);
    }
    args.insert(argKey, resolved);
    if (selectedPath) *selectedPath = path;
}

// 6.7 / 6.8 volume / geometry
void Validator::resolveVolumeOrGeometryArg(const ParamDef& def, const JsValue& node, JsObject& args,
                                            const JsText& argKey, const JsText& kind) {
    const JsText label = kind == JsText(u"vol") ? JsText(u"volume") : JsText(u"geometry");
    const JsText refType = kind == JsText(u"vol") ? NodeKind::VolRef : NodeKind::GeoRef;
    // Plain concatenation (not JsText::arg with a repeated placeholder
    // number) to avoid any ambiguity: "vol0-vol7" / "geo0-geo7".
    const JsText rangeText = kind + JsText(u"0-") + kind + JsText(u"7");
    auto refObj = [&](const JsText& name) { JsObject o; o.insert(JsText(u"kind"), kind); o.insert(JsText(u"name"), name); return JsValue(o); };
    auto defaultOrNull = [&]() { return def.defaultValue.isString() ? refObj(def.defaultValue.toString()) : JsValue(JsValue::Null); };

    if (nodeType(node) == NodeKind::String) {
        pushDiag(JsText(u"S001"), node, JsText(u"String literal not allowed for %1 parameter '%2'").arg(label, def.name));
        args.insert(argKey, defaultOrNull());
        return;
    }
    JsValue value = JsValue(JsValue::Null);

    if (nodeType(node) == NodeKind::Read3D && node.toObject().value(JsText(u"tex3d")).isObject()
        && node.toObject().value(JsText(u"geo")).isNull()) {
        const JsText nm = surfaceName(node.toObject().value(JsText(u"tex3d")));
        if (matchesRefPattern(nm, kind)) {
            value = refObj(nm);
        } else {
            // NOTE: this message (unlike the Ident-branch one below) has NO
            // "or none" suffix -- verified against the exact reference text
            // (shaders/src/lang/validator.js). TD's port drops the
            // interpolated name and the whole "- expected ..." suffix here
            // entirely; that does not match the live reference -- do not
            // copy it.
            pushDiag(JsText(u"S001"), node,
                      JsText(u"Invalid %1 reference '%2' in read3d() for '%3' - expected %4").arg(label, nm, def.name, rangeText));
            value = defaultOrNull();
        }
    } else if (nodeType(node) == refType) {
        value = refObj(node.toObject().value(JsText(u"name")).toString());
    } else if (nodeType(node) == NodeKind::Ident) {
        const JsText nm = node.toObject().value(JsText(u"name")).toString();
        if (nm == JsText(u"none")) {
            value = refObj(JsText(u"none"));
        } else if (matchesRefPattern(nm, kind)) {
            value = refObj(nm);
        } else {
            pushDiag(JsText(u"S001"), node,
                      JsText(u"Invalid %1 reference '%2' for '%3' - expected %4 or none").arg(label, nm, def.name, rangeText));
            value = defaultOrNull();
        }
    } else if (node.isUndefined() && def.defaultValue.isString()) {
        value = refObj(def.defaultValue.toString());
    }
    args.insert(argKey, value);
}

// 6.9 string (STRICT allowlist)
void Validator::resolveStringArg(const ParamDef& def, const JsValue& node, const JsText& opName,
                                  const JsObject& original, JsObject& args, const JsText& argKey) {
    const int dot = opName.lastIndexOf(u'.');
    const JsText funcName = dot >= 0 ? JsText(opName.mid(dot + 1)) : opName;
    const JsText allowlistKey = funcName + u'.' + def.name;
    if (!allowedStringParams().contains(allowlistKey)) {
        pushDiag(JsText(u"S001"), !node.isUndefined() ? node : JsValue(original),
                  JsText(u"String parameter '%1' on effect '%2' is NOT in the allowed string params list. "
                                 "String params are strictly controlled - use enums or choices instead.")
                      .arg(def.name, funcName));
        args.insert(argKey, def.defaultValue);
        return;
    }
    if (nodeType(node) == NodeKind::String) {
        args.insert(argKey, node.toObject().value(JsText(u"value")));
        return;
    }
    if (nodeType(node) == NodeKind::Ident && def.hasChoices()) {
        const JsText name = node.toObject().value(JsText(u"name")).toString();
        const JsObject choices = def.choicesObject();
        if (choices.contains(name)) {
            args.insert(argKey, choices.value(name));
        } else {
            pushDiag(JsText(u"S001"), node, JsText(u"Invalid choice '%1' for string parameter '%2'").arg(name, def.name));
            args.insert(argKey, def.defaultValue);
        }
        return;
    }
    if (!node.isUndefined()) {
        pushDiag(JsText(u"S001"), node,
                  JsText(u"String parameter '%1' requires a quoted string literal, got %2").arg(def.name, nodeType(node)));
        args.insert(argKey, def.defaultValue);
        return;
    }
    args.insert(argKey, def.defaultValue);
}

// 6.10 numeric (the catch-all: int/float/palette/vec2/mat3/...)
void Validator::resolveNumericArg(const ParamDef& def, const JsValue& node, const JsObject& call,
                                   const OpSpec& spec, JsObject& args, const JsText& argKey) {
    auto numericDefault = [&]() {
        if (def.hasDefaultFrom()) {
            JsText refKey = def.defaultFromString();
            for (const ParamDef& d : spec.args) {
                if (d.name == def.defaultFromString()) {
                    refKey = d.name;
                    break;
                }
            }
            if (args.contains(refKey)) return args.value(refKey);
            return def.defaultValue;
        }
        return def.defaultValue;
    };

    if (node.isUndefined()) {
        args.insert(argKey, numericDefault());
        return;
    }
    const JsText t = nodeType(node);
    if (t == NodeKind::String) {
        pushDiag(JsText(u"S001"), node,
                  JsText(u"String literal not allowed for numeric parameter '%1' - strings are only valid "
                                 "for type: \"string\" parameters")
                      .arg(def.name));
        args.insert(argKey, def.defaultValue);
        return;
    }
    if (t == NodeKind::Number || t == NodeKind::Boolean) {
        const JsObject n = node.toObject();
        const JsValue raw = n.value(JsText(u"value"));
        const JsValue value = (t == NodeKind::Boolean) ? JsValue(raw.toBool() ? 1.0 : 0.0) : raw;
        // JSON cloning turns nonfinite Number values into null. JS compares
        // null numerically as zero but leaves it null when no bound clamps it.
        JsValue clamped = value;
        if (value.isDouble() || value.isNull()) {
            const double comparable = value.isNull() ? 0.0 : value.toDouble();
            if (def.minValue.isDouble() && comparable < def.minValue.toDouble()) clamped = def.minValue;
            else if (def.maxValue.isDouble() && comparable > def.maxValue.toDouble()) clamped = def.maxValue;
        }
        if (clamped != value) {
            pushDiag(JsText(u"S002"), node,
                      JsText(u"Argument out of range for '%1' in %2() (got %3, clamped to %4)")
                          .arg(def.name, call.value(JsText(u"name")).toString(), value.toVariant(),
                               clamped.toVariant()));
        }
        if (n.contains(JsText(u"_varRef"))) {
            JsObject wrapped;
            wrapped.insert(JsText(u"_varRef"), n.value(JsText(u"_varRef")));
            wrapped.insert(JsText(u"value"), clamped);
            args.insert(argKey, wrapped);
        } else {
            args.insert(argKey, clamped);
        }
        return;
    }
    // `() => expr`: keep the function and both bounds in the raw value.
    if (t == NodeKind::Func) {
        const JsText src = node.toObject().value(JsText(u"src")).toString();
        if (funcCompiles(src)) {
            Object value;
            value.set(u"fn", Value::function(src));
            value.set(u"min", def.minValue.raw());
            value.set(u"max", def.maxValue.raw());
            args.insert(argKey, Value(std::move(value)));
        } else {
            pushDiag(JsText(u"S001"), node,
                     JsText(u"Invalid function for '%1': '%2'").arg(def.name, funcSnippet(src)));
            args.insert(argKey, def.defaultValue);
        }
        return;
    }
    if (automationFields().contains(t)) {
        args.insert(argKey, compileAutomationDescriptor(node.toObject()));
        return;
    }
    if (t == NodeKind::Member) {
        const JsValue cur = resolveEnum(normalizeMemberPath(node.toObject().value(JsText(u"path"))));
        if (cur.isDouble()) {
            const double v = clampValue(cur.toDouble(), def.minValue, def.maxValue);
            if (v != cur.toDouble()) {
                pushDiag(JsText(u"S002"), node,
                          JsText(u"Argument out of range for '%1' in %2() (got %3, clamped to %4)")
                              .arg(def.name, call.value(JsText(u"name")).toString(), js::numberToString(cur.toDouble()),
                                   js::numberToString(v)));
            }
            args.insert(argKey, v);
        } else {
            JsList segs;
            const JsValue pathVal = node.toObject().value(JsText(u"path"));
            JsText pathText;
            if (pathVal.isArray()) {
                for (const JsValue& s : pathVal.toArray()) segs.append(s.toString());
                pathText = segs.join(u'.');
            }
            if (pathText.isEmpty()) pathText = isTruthy(node.toObject().value(JsText(u"name")))
                                                    ? node.toObject().value(JsText(u"name")).toString()
                                                    : JsText(u"unknown");
            pushDiag(JsText(u"S001"), node, JsText(u"Cannot resolve enum value for '%1': '%2'").arg(def.name, pathText));
            args.insert(argKey, def.defaultValue);
        }
        return;
    }
    const JsText identName = node.toObject().value(JsText(u"name")).toString();
    // A bare state-value ident retains its fn, bounds and AST.
    if (t == NodeKind::Ident && stateValues().contains(identName) && !isOwnChoice(def, identName)) {
        Object value;
        value.set(u"fn", Value::function(identName));
        value.set(u"min", def.minValue.raw());
        value.set(u"max", def.maxValue.raw());
        value.set(u"_ast", node.raw());
        args.insert(argKey, Value(std::move(value)));
        return;
    }
    if (t == NodeKind::Ident && def.hasEnumPath()) {
        const JsList prefix = normalizeMemberPath(def.enumPath);
        const JsList path = prefix.isEmpty() ? JsList{identName} : (prefix + JsList{identName});
        const JsValue resolved = resolveEnum(path);
        if (resolved.isDouble()) {
            args.insert(argKey, clampValue(resolved.toDouble(), def.minValue, def.maxValue));
        } else {
            pushDiag(JsText(u"S003"), node);
            args.insert(argKey, def.defaultValue);
        }
        return;
    }
    if (t == NodeKind::Ident && def.hasChoices()) {
        const JsValue choiceVal = def.choicesObject().value(identName);
        if (choiceVal.isDouble()) {
            args.insert(argKey, clampValue(choiceVal.toDouble(), def.minValue, def.maxValue));
        } else {
            pushDiag(JsText(u"S003"), node);
            args.insert(argKey, def.defaultValue);
        }
        return;
    }
    if (t == NodeKind::Ident) {
        pushDiag(JsText(u"S003"), node);
    } else {
        pushDiag(JsText(u"S002"), node,
                  JsText(u"Argument out of range for '%1' in %2()").arg(def.name, call.value(JsText(u"name")).toString()));
    }
    args.insert(argKey, numericDefault());
}

JsValue Validator::resolveAutomationEnum(const JsValue& nodeVal, const JsText& enumName,
                                             const JsValue& fallback, const JsSet<int>& validValues,
                                             const JsText& descriptorName, const JsText& fieldName) {
    JsValue resolved(JsValue::Undefined);
    const JsObject node = nodeVal.toObject();
    const JsText type = nodeType(nodeVal);
    if (type == NodeKind::Number) {
        resolved = node.value(JsText(u"value"));
    } else if (type == NodeKind::Member) {
        resolved = resolveEnum(normalizeMemberPath(node.value(JsText(u"path"))));
    } else if (type == NodeKind::Ident) {
        resolved = resolveEnum({enumName, node.value(JsText(u"name")).toString()});
    }
    if (resolved.isObject() && nodeType(resolved) == NodeKind::Number) {
        resolved = resolved.toObject().value(JsText(u"value"));
    }
    if (resolved.isDouble()) {
        const double value = resolved.toDouble();
        if (std::isfinite(value) && std::floor(value) == value && validValues.contains(static_cast<int>(value))) {
            return value;
        }
    }

    if (type == NodeKind::String) {
        pushDiag(JsText(u"S001"), nodeVal,
                 JsText(u"String literal not allowed for %1() %2").arg(descriptorName, fieldName));
    } else {
        JsText message = JsText(u"%1() %2 must resolve to a supported enum value")
                              .arg(descriptorName, fieldName);
        if (descriptorName == JsText(u"audio") && fieldName == JsText(u"band")) {
            const JsText got = resolved.isUndefined() ? JsText(u"undefined")
                                                       : (resolved.isDouble() ? js::numberToString(resolved.toDouble())
                                                                              : resolved.toVariant().toString());
            message = JsText(u"audio() band must resolve to an integer from 0 to 4 (got %1)").arg(got);
        }
        pushDiag(JsText(u"S002"), nodeVal, message);
    }
    return fallback;
}

JsValue Validator::resolveAutomationString(const JsValue& nodeVal, const JsText& descriptorName,
                                               const JsText& fieldName) {
    if (nodeVal.isUndefined() || nodeVal.isNull()) return JsValue(JsValue::Undefined);
    const JsText allowlistKey = descriptorName + u'.' + fieldName;
    if (!allowedStringParams().contains(allowlistKey)) {
        pushDiag(JsText(u"S001"), nodeVal,
                 JsText(u"String parameter '%1' is not allowlisted").arg(allowlistKey));
        return JsValue(JsValue::Undefined);
    }
    const JsObject node = nodeVal.toObject();
    if (nodeType(nodeVal) != NodeKind::String) {
        pushDiag(JsText(u"S001"), nodeVal,
                 JsText(u"%1() %2 requires a quoted string").arg(descriptorName, fieldName));
        return JsValue(JsValue::Undefined);
    }
    const JsText raw = node.value(JsText(u"value")).toString();
    if (raw.isEmpty()) {
        pushDiag(JsText(u"S001"), nodeVal,
                 JsText(u"%1() %2 must not be empty").arg(descriptorName, fieldName));
        return JsValue(JsValue::Undefined);
    }
    return decodeJsonStringLiteralContent(raw);
}

JsValue Validator::resolveAutomationNumber(const JsValue& nodeVal, const JsText& descriptorName,
                                               const JsText& fieldName, const JsValue& fallback,
                                               AutomationNumberOptions options, int depth) {
    if (nodeVal.isUndefined() || nodeVal.isNull()) return fallback;
    auto reject = [&](const JsText& code, const JsText& message) {
        if (options.invalid) *options.invalid = true;
        pushDiag(code, nodeVal, message);
        return fallback;
    };

    const JsObject node = nodeVal.toObject();
    const JsText type = nodeType(nodeVal);
    JsValue resolved(JsValue::Undefined);
    if (type == NodeKind::Number) {
        resolved = node.value(JsText(u"value"));
    } else if (options.allowBoolean && type == NodeKind::Boolean) {
        resolved = node.value(JsText(u"value")).toBool() ? 1.0 : 0.0;
    } else if (type == NodeKind::Member && options.allowMember) {
        resolved = resolveEnum(normalizeMemberPath(node.value(JsText(u"path"))));
        if (resolved.isObject() && nodeType(resolved) == NodeKind::Number) {
            resolved = resolved.toObject().value(JsText(u"value"));
        }
    } else if (automationFields().contains(type) && options.allowAutomation) {
        const JsValue compiled = compileAutomationDescriptor(node, depth + 1);
        if (compiled.isObject() && compiled.toObject().value(JsText(u"_invalid")).toBool(false)
            && options.invalid) {
            *options.invalid = true;
        }
        return compiled;
    } else if (type == NodeKind::String) {
        return reject(JsText(u"S001"),
                      JsText(u"String literal not allowed for %1() %2").arg(descriptorName, fieldName));
    } else if (type == NodeKind::Ident) {
        return reject(JsText(u"S003"),
                      JsText(u"Undefined automation source '%1' for %2() %3")
                          .arg(node.value(JsText(u"name")).toString(), descriptorName, fieldName));
    } else {
        return reject(JsText(u"S002"),
                      JsText(u"%1() %2 must be a number%3")
                          .arg(descriptorName, fieldName,
                               options.allowAutomation ? JsText(u" or automation source") : JsText()));
    }

    if (!resolved.isDouble() || !std::isfinite(resolved.toDouble())) {
        return reject(JsText(u"S002"),
                      JsText(u"%1() %2 must resolve to a finite number").arg(descriptorName, fieldName));
    }
    double value = resolved.toDouble();
    if (options.integer && std::floor(value) != value) {
        return reject(JsText(u"S002"), JsText(u"%1() %2 must be an integer").arg(descriptorName, fieldName));
    }
    if (options.minimum && value < *options.minimum) {
        return reject(JsText(u"S002"), JsText(u"%1() %2 must be at least %3 (got %4)")
            .arg(descriptorName, fieldName, js::numberToString(*options.minimum), js::numberToString(value)));
    }
    if (options.maximum && value > *options.maximum) {
        return reject(JsText(u"S002"), JsText(u"%1() %2 must be at most %3 (got %4)")
            .arg(descriptorName, fieldName, js::numberToString(*options.maximum), js::numberToString(value)));
    }
    if (options.clamp01) value = std::clamp(value, 0.0, 1.0);
    return value;
}

JsValue Validator::compileAutomationDescriptor(const JsObject& node, int depth) {
    if (depth > kMaxAutomationDepth) {
        pushDiag(JsText(u"S001"), node,
                 JsText(u"Automation nesting exceeds the maximum depth of %1").arg(kMaxAutomationDepth));
        return 0.0;
    }

    const JsText type = node.value(JsText(u"type")).toString();
    if (type == NodeKind::Oscillator) {
        AutomationNumberOptions nestedUnit;
        nestedUnit.allowBoolean = true;
        nestedUnit.allowAutomation = true;
        nestedUnit.clamp01 = true;
        AutomationNumberOptions nestedNumber;
        nestedNumber.allowBoolean = true;
        nestedNumber.allowAutomation = true;

        JsObject value;
        value.insert(JsText(u"type"), NodeKind::Oscillator);
        value.insert(JsText(u"oscType"), resolveAutomationEnum(
            node.value(JsText(u"oscType")), JsText(u"oscKind"), 0.0,
            {0, 1, 2, 3, 4, 5, 6}, JsText(u"osc"), JsText(u"type")));
        value.insert(JsText(u"min"), resolveAutomationNumber(
            node.value(JsText(u"min")), JsText(u"osc"), JsText(u"min"), 0.0, nestedUnit, depth));
        value.insert(JsText(u"max"), resolveAutomationNumber(
            node.value(JsText(u"max")), JsText(u"osc"), JsText(u"max"), 1.0, nestedUnit, depth));
        value.insert(JsText(u"speed"), resolveAutomationNumber(
            node.value(JsText(u"speed")), JsText(u"osc"), JsText(u"speed"), 1.0, nestedNumber, depth));
        value.insert(JsText(u"offset"), resolveAutomationNumber(
            node.value(JsText(u"offset")), JsText(u"osc"), JsText(u"offset"), 0.0, nestedNumber, depth));
        value.insert(JsText(u"seed"), resolveAutomationNumber(
            node.value(JsText(u"seed")), JsText(u"osc"), JsText(u"seed"), 1.0, nestedNumber, depth));
        value.insert(JsText(u"_ast"), completeAutomationAst(node.raw()));
        if (node.contains(JsText(u"_varRef"))) value.insert(JsText(u"_varRef"), node.value(JsText(u"_varRef")));
        return value;
    }

    if (type == NodeKind::Midi) {
        const auto undefined = JsValue(JsValue::Undefined);
        const auto mode = resolveAutomationEnum(node.value(JsText(u"mode")), JsText(u"midiMode"), 4.0,
            {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10}, JsText(u"midi"), JsText(u"mode"));
        const bool hasZone = node.contains(JsText(u"zone"));
        const auto zone = hasZone ? resolveAutomationEnum(node.value(JsText(u"zone")), JsText(u"midiZone"), undefined,
            {0, 1}, JsText(u"midi"), JsText(u"zone")) : undefined;
        bool invalidSelection = hasZone && (zone.isUndefined() || node.contains(JsText(u"channel")));
        bool invalidChannel = false;
        bool invalidCc = false;
        auto selectorOptions = [](double minimum, double maximum, bool* invalid) {
            AutomationNumberOptions options;
            options.integer = true; options.allowMember = false;
            options.minimum = minimum; options.maximum = maximum; options.invalid = invalid;
            return options;
        };
        auto members = undefined;
        if (node.contains(JsText(u"members"))) {
            members = resolveAutomationNumber(node.value(JsText(u"members")), JsText(u"midi"), JsText(u"members"), undefined,
                selectorOptions(1, 15, &invalidSelection), depth);
            if (!hasZone) invalidSelection = true;
        }
        const auto channel = hasZone ? undefined : resolveAutomationNumber(node.value(JsText(u"channel")), JsText(u"midi"), JsText(u"channel"), 1.0,
            selectorOptions(1, 16, &invalidChannel), depth);
        auto cc = undefined;
        if (node.contains(JsText(u"cc")) || mode.toInt() == 5 || mode.toInt() == 6) {
            cc = resolveAutomationNumber(node.value(JsText(u"cc")), JsText(u"midi"), JsText(u"cc"), 1.0,
                selectorOptions(0, mode.toInt() == 6 ? 31 : 127, &invalidCc), depth);
        }
        auto nrpn = undefined;
        if (node.contains(JsText(u"nrpn")) || mode.toInt() == 7) {
            if (!node.contains(JsText(u"nrpn"))) {
                pushDiag(JsText(u"S002"), node, JsText(u"midi() nrpn mode requires a parameter number"));
                invalidSelection = true;
            }
            nrpn = resolveAutomationNumber(node.value(JsText(u"nrpn")), JsText(u"midi"), JsText(u"nrpn"), undefined,
                selectorOptions(0, 16382, &invalidSelection), depth);
        }
        AutomationNumberOptions nestedUnit;
        nestedUnit.allowBoolean = true; nestedUnit.allowAutomation = true; nestedUnit.clamp01 = true;
        AutomationNumberOptions nestedSensitivity;
        nestedSensitivity.allowBoolean = true; nestedSensitivity.allowAutomation = true;
        const JsValue minimum = resolveAutomationNumber(
            node.value(JsText(u"min")), JsText(u"midi"), JsText(u"min"), 0.0, nestedUnit, depth);
        const JsValue maximum = resolveAutomationNumber(
            node.value(JsText(u"max")), JsText(u"midi"), JsText(u"max"), 1.0, nestedUnit, depth);
        const JsValue sensitivity = resolveAutomationNumber(
            node.value(JsText(u"sensitivity")), JsText(u"midi"), JsText(u"sensitivity"), 1.0,
            nestedSensitivity, depth);
        Object value;
        value.set(u"type", NodeKind::Midi);
        value.set(u"channel", channel.raw());
        value.set(u"mode", mode.raw());
        if (!cc.isUndefined()) value.set(u"cc", cc.raw());
        if (!nrpn.isUndefined()) value.set(u"nrpn", nrpn.raw());
        if (hasZone) value.set(u"zone", zone.raw());
        if (node.contains(JsText(u"members"))) value.set(u"members", members.raw());
        if (invalidSelection || invalidChannel || invalidCc) value.set(u"_invalid", true);
        value.set(u"min", minimum.raw());
        value.set(u"max", maximum.raw());
        value.set(u"sensitivity", sensitivity.raw());
        value.set(u"name", resolveAutomationString(node.value(JsText(u"name")), JsText(u"midi"), JsText(u"name")).raw());
        value.set(u"id", resolveAutomationString(node.value(JsText(u"id")), JsText(u"midi"), JsText(u"id")).raw());
        value.set(u"_ast", completeAutomationAst(node.raw()));
        if (node.contains(JsText(u"_varRef"))) value.set(u"_varRef", node.value(JsText(u"_varRef")).raw());
        return Value(std::move(value));
    }

    if (type == NodeKind::Audio) {
        const JsValue band = resolveAutomationEnum(
            node.value(JsText(u"band")), JsText(u"audioBand"), JsValue(JsValue::Undefined),
            {0, 1, 2, 3, 4}, JsText(u"audio"), JsText(u"band"));
        bool invalidMin = false;
        bool invalidMax = false;
        AutomationNumberOptions minOptions;
        minOptions.allowAutomation = true;
        minOptions.allowMember = false;
        minOptions.clamp01 = true;
        minOptions.invalid = &invalidMin;
        AutomationNumberOptions maxOptions = minOptions;
        maxOptions.invalid = &invalidMax;
        const JsValue minimum = resolveAutomationNumber(
            node.value(JsText(u"min")), JsText(u"audio"), JsText(u"min"), 0.0, minOptions, depth);
        const JsValue maximum = resolveAutomationNumber(
            node.value(JsText(u"max")), JsText(u"audio"), JsText(u"max"), 1.0, maxOptions, depth);

        JsValue channel(JsValue::Undefined);
        bool validChannel = true;
        if (node.contains(JsText(u"channel"))) {
            const JsValue channelNode = node.value(JsText(u"channel"));
            const JsValue raw = channelNode.toObject().value(JsText(u"value"));
            if (nodeType(channelNode) == NodeKind::Number && raw.isDouble()
                && std::floor(raw.toDouble()) == raw.toDouble() && raw.toDouble() >= 1.0 && raw.toDouble() <= 32.0) {
                channel = raw;
            } else {
                validChannel = false;
                if (nodeType(channelNode) == NodeKind::String) {
                    pushDiag(JsText(u"S001"), channelNode,
                             JsText(u"String literal not allowed for audio() channel"));
                } else {
                    JsText got = nodeType(channelNode);
                    if (raw.isDouble()) got = js::numberToString(raw.toDouble());
                    else if (raw.isBool()) got = raw.toBool() ? JsText(u"true") : JsText(u"false");
                    else if (raw.isArray() || raw.isObject()) got = raw.toVariant();
                    else if (channelNode.toObject().value(JsText(u"name")).isString()) {
                        got = channelNode.toObject().value(JsText(u"name")).toString();
                    }
                    pushDiag(JsText(u"S002"), channelNode,
                             JsText(u"audio() channel must be a positive integer from 1 to 32 (got %1)").arg(got));
                }
            }
        }
        const JsValue name = resolveAutomationString(
            node.value(JsText(u"name")), JsText(u"audio"), JsText(u"name"));
        const JsValue id = resolveAutomationString(
            node.value(JsText(u"id")), JsText(u"audio"), JsText(u"id"));
        const bool validName = !node.contains(JsText(u"name")) || !name.isUndefined();
        const bool validId = !node.contains(JsText(u"id")) || !id.isUndefined();

        Object value;
        value.set(u"type", NodeKind::Audio);
        value.set(u"band", band.raw());
        value.set(u"min", minimum.raw());
        value.set(u"max", maximum.raw());
        value.set(u"channel", channel.raw());
        value.set(u"name", name.raw());
        value.set(u"id", id.raw());
        value.set(u"_invalid", band.isUndefined() || invalidMin || invalidMax
                                                 || !validName || !validId || !validChannel);
        value.set(u"_ast", completeAutomationAst(node.raw()));
        if (node.contains(JsText(u"_varRef"))) value.set(u"_varRef", node.value(JsText(u"_varRef")).raw());
        return Value(std::move(value));
    }

    return 0.0;
}

// ---------------------------------------------------------------- entry point

JsObject Validator::run(const JsObject& ast) {
    const JsValue renderVal = ast.value(JsText(u"render"));
    const JsValue render = renderVal.isObject() ? renderVal.toObject().value(JsText(u"name")) : JsValue(JsValue::Null);

    const JsValue nsMeta = ast.value(JsText(u"namespace"));
    if (nsMeta.isObject() && nsMeta.toObject().value(JsText(u"searchOrder")).isArray()) {
        for (const JsValue& v : nsMeta.toObject().value(JsText(u"searchOrder")).toArray()) {
            programSearchOrder_.append(v.toString());
        }
    }
    if (programSearchOrder_.isEmpty()) {
        // Dead in practice: nm::parse (T8) already guarantees a non-empty
        // search directive, mirroring the reference's OWN parser.js
        // enforcement (identical message text) -- this validator-level
        // check exists only for callers that hand-build an AST bypassing
        // the parser (e.g. unit tests exercising this function directly).
        throw std::runtime_error(
            "Missing required 'search' directive. Every program must start with 'search <namespace>, ...' "
            "to specify namespace search order.");
    }

    for (const JsValue& v : ast.value(JsText(u"vars")).toArray()) bindVar(v.toObject());

    JsArray plans;
    for (const JsValue& stmtVal : ast.value(JsText(u"plans")).toArray()) {
        const JsValue compiled = compileStmt(stmtVal.toObject());
        if (!compiled.isUndefined()) plans.append(compiled);
    }

    JsObject result;
    result.insert(JsText(u"plans"), plans);
    result.insert(JsText(u"diagnostics"), diagnostics_);
    result.insert(JsText(u"render"), render);
    result.insert(JsText(u"vars"), ast.value(JsText(u"vars")).isArray() ? ast.value(JsText(u"vars")) : JsValue(JsArray()));
    JsArray searchNamespaces;
    for (const JsText& ns : programSearchOrder_) searchNamespaces.append(ns);
    result.insert(JsText(u"searchNamespaces"), searchNamespaces);
    if (ast.contains(JsText(u"trailingComments"))) {
        result.insert(JsText(u"trailingComments"), ast.value(JsText(u"trailingComments")));
    }
    return result;
}

} // namespace

JsObject validate(const JsObject& ast, EffectRegistry& registry) {
    Validator v(registry);
    return v.run(ast);
}

Value validate(const Value& ast, EffectRegistry& registry) {
    return validate(JsObject(ast), registry).raw();
}

} // namespace nm
