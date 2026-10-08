#include "effect_registry.h"
#include "core/catalog/catalog.h"

#include <algorithm>
#include <vector>


#include <stdexcept>

namespace nm {

namespace {

// An effect is a STARTER (valid as a chain root, needs no pipeline input)
// iff none of its passes consumes one of these upstream-surface inputs.
// This mirrors tools/export-graph.mjs / tools/dump-validate.mjs -- the
// rule the REAL graph-producing path uses -- NOT the per-effect `starter`
// JSON field (e.g. mixer.channelCombine has starter:false in its JSON but
// takes its inputs via surface kwargs, so it IS a starter and renders;
// verified empirically: derived-vs-baked starter status disagrees on
// exactly this one effect across the whole 210-effect catalog).
const JsSet<JsText>& starterInputSentinels() {
    static const JsSet<JsText> s = {
        JsText(u"inputTex"), JsText(u"inputTex3d"), JsText(u"src"),
        JsText(u"o0"), JsText(u"o1"),
    };
    return s;
}

bool isAlpha(char16_t c) {
    return (c >= u'a' && c <= u'z') || (c >= u'A' && c <= u'Z');
}
bool isDigitChar(char16_t c) { return c >= u'0' && c <= u'9'; }
bool isWs(char16_t c) {
    return c == u' ' || c == u'\t' || c == u'\n' || c == u'\r';
}

bool isValidIdentifier(const JsText& name) {
    if (name.isEmpty()) return false;
    if (!(isAlpha(name.at(0)) || name.at(0) == u'_')) return false;
    for (int i = 1; i < name.size(); ++i) {
        const char16_t c = name.at(i);
        if (!(isAlpha(c) || isDigitChar(c) || c == u'_')) return false;
    }
    return true;
}

// Verbatim port of renderer/canvas.js sanitizeEnumName: "Cell Scale" ->
// "CellScale" (uppercase the char after each whitespace run, drop the
// spaces), then strip remaining non-identifier chars; empty string if the
// result is not a valid identifier (reference returns null). Cross-checked
// against godot/addons/noisemaker/compiler/lang/effect_registry.gd's
// char-by-char transcription of the same reference regexes. Confirmed
// (dump-registry.mjs header comment) that this never actually changes a
// name across the current 210-effect catalog -- ported for completeness.
JsText sanitizeEnumName(const JsText& name) {
    JsText result;
    int i = 0;
    const int n = name.size();
    while (i < n) {
        if (isWs(name.at(i))) {
            while (i < n && isWs(name.at(i))) ++i;
            if (i < n) {
                result += static_cast<char16_t>(name.at(i) >= u'a' && name.at(i) <= u'z' ? name.at(i) - 32 : name.at(i));
                ++i;
            }
        } else {
            result += name.at(i);
            ++i;
        }
    }
    JsText stripped;
    for (const char16_t c : result) {
        if (isAlpha(c) || isDigitChar(c) || c == u'_') stripped += c;
    }
    return isValidIdentifier(stripped) ? stripped : JsText();
}

} // namespace

void EffectRegistry::loadEmbedded() {
    std::vector<const CatalogFile*> files;
    for (const auto& file : catalog_files()) {
        if (file.path.starts_with("effects/") && file.path.ends_with(".json")) files.push_back(&file);
    }
    std::sort(files.begin(), files.end(), [](const auto* a, const auto* b) { return a->path < b->path; });
    if (files.empty()) throw std::runtime_error("no embedded effect definitions");
    for (const auto* file : files) registerEffect(std::string(file->bytes));
}

void EffectRegistry::applySetup(const Value& actionValue) {
    const JsObject action(actionValue);
    const JsText op = action.value(JsText(u"op")).toString();
    if (op == JsText(u"registerOp")) {
        const JsText name = action.value(JsText(u"name")).toString();
        const JsObject source = action.value(JsText(u"spec")).toObject();
        OpSpec spec;
        spec.opName = name;
        spec.name = source.value(JsText(u"name")).isString()
            ? source.value(JsText(u"name")).toString()
            : name.mid(name.lastIndexOf(u'.') + 1);
        spec.rawSpec = source;
        for (const JsValue& argValue : source.value(JsText(u"args")).toArray()) {
            const JsObject arg = argValue.toObject();
            ParamDef def;
            def.name = arg.value(JsText(u"name")).toString();
            def.type = arg.value(JsText(u"type")).toString();
            def.defaultValue = arg.value(JsText(u"default"));
            def.enumPath = arg.value(JsText(u"enumPath"));
            if (def.enumPath.isUndefined()) def.enumPath = arg.value(JsText(u"enum"));
            def.minValue = arg.value(JsText(u"min"));
            def.maxValue = arg.value(JsText(u"max"));
            def.uniformValue = arg.value(JsText(u"uniform"));
            def.choicesValue = arg.value(JsText(u"choices"));
            def.defaultFromValue = arg.value(JsText(u"defaultFrom"));
            spec.args.append(def);
        }
        if (!ops_.contains(name)) opOrder_.append(name);
        ops_.insert(name, std::move(spec));
        return;
    }
    if (op == JsText(u"registerStarterOps")) {
        for (const JsValue& name : action.value(JsText(u"names")).toArray()) {
            if (name.isString() && !name.toString().isEmpty()) starterOps_.insert(name.toString());
        }
        return;
    }
    if (op == JsText(u"registerEffect")) {
        const JsText name = action.value(JsText(u"name")).toString();
        if (!name.isEmpty()) effects_.insert(name, action.value(JsText(u"definition")).toObject());
        return;
    }
    if (op == JsText(u"registerParamAliases")) {
        paramAliases_.insert(action.value(JsText(u"opName")).toString(),
                             action.value(JsText(u"aliases")).toObject());
        return;
    }
    if (op == JsText(u"registerEffectAlias")) {
        effectAliases_.insert(action.value(JsText(u"opName")).toString(),
                              action.value(JsText(u"newName")).toString());
        return;
    }
    if (op == JsText(u"mergeIntoEnums")) {
        auto merge = [&](auto&& self, const JsValue& value, JsList path) -> void {
            if (!value.isObject()) return;
            if (Enums::isLeaf(value)) {
                enums_.registerChoice(path, value.toObject().value(JsText(u"value")));
                return;
            }
            const JsObject table = value.toObject();
            for (auto it = table.constBegin(); it != table.constEnd(); ++it) {
                JsList child = path;
                child.append(it.key());
                self(self, it.value(), child);
            }
        };
        merge(merge, action.value(JsText(u"source")), {});
    }
}

// Mirror of renderer/canvas.js registerEffectWithRuntime() (and
// dump-registry.mjs/dump-validate.mjs's inlined re-implementation of it).
void EffectRegistry::registerEffect(const std::string& rawJson) {
    JsObject def;
    try { def = JsValue(json::parse(rawJson)).toObject(); }
    catch (const json::ParseError&) { return; }
    if (def.isEmpty()) return;

    const JsText ns = def.value(JsText(u"namespace")).toString();
    const JsText func = def.value(JsText(u"func")).toString();
    if (func.isEmpty()) return;

    // Multi-key effect registration. The reference's four keys are
    // func | ns.func | ns/func | ns.func (the 4th literally duplicates the
    // 2nd, since effectName===func for the whole catalog) -- three
    // DISTINCT keys land here.
    effects_.insert(func, def);
    if (!ns.isEmpty()) {
        effects_.insert(ns + u'.' + func, def);
        effects_.insert(ns + u'/' + func, def);
    }

    const JsText opName = ns.isEmpty() ? func : JsText(ns + u'.' + func);

    // T10: keep the raw bytes addressable by opName so the Expander can
    // recover pass-level `inputs`/`outputs` key order directly from source
    // text (see effect_registry.h's rawJson() doc comment).

    const JsObject globals = def.value(JsText(u"globals")).toObject();
    const JsList orderedKeys = globals.keys();

    OpSpec spec;
    spec.name = func;
    spec.opName = opName;

    for (const JsText& key : orderedKeys) {
        const JsValue specVal = globals.value(key);
        if (!specVal.isObject()) continue;
        const JsObject gspec = specVal.toObject();

        const JsText gtype = gspec.value(JsText(u"type")).toString();

        // enumPath = spec.enum || spec.enumPath (falsy-OR: an explicitly
        // empty-string `enum` ALSO falls through to `enumPath`).
        JsText enumPath;
        const JsValue enumVal = gspec.value(JsText(u"enum"));
        if (enumVal.isString() && !enumVal.toString().isEmpty()) {
            enumPath = enumVal.toString();
        } else {
            const JsValue enumPathVal = gspec.value(JsText(u"enumPath"));
            if (enumPathVal.isString() && !enumPathVal.toString().isEmpty()) enumPath = enumPathVal.toString();
        }

        const JsValue choicesVal = gspec.value(JsText(u"choices"));
        const bool hasChoicesField = choicesVal.isObject();

        // choices with no explicit enum: synthesize an enum path and
        // register the choices as project enums (canvas.js parity).
        if (enumPath.isEmpty() && hasChoicesField) {
            enumPath = opName + u'.' + key;
            const JsObject choices = choicesVal.toObject();
            for (auto cit = choices.constBegin(); cit != choices.constEnd(); ++cit) {
                const JsText cname = cit.key();
                if (cname.endsWith(u':')) continue; // UI group header, not a value
                const JsValue cval = cit.value();
                // NOTE: the reference blindly wraps WHATEVER a choices entry's
                // value is into {type:'Number', value: val} with no type
                // check -- filter.text's `font`/`justify` globals are
                // type:"string" with STRING-valued choices
                // ({"nunito":"Nunito", ...}) and still register as
                // {"type":"Number","value":"Nunito"} (verified against the
                // live oracle). Ported bug-for-bug: register whatever the
                // value is, not just numbers.
                enums_.registerChoice({ns, func, key, cname}, cval);
                const JsText sanitized = sanitizeEnumName(cname);
                if (!sanitized.isEmpty() && sanitized != cname) {
                    enums_.registerChoice({ns, func, key, sanitized}, cval);
                }
            }
        }

        // default/min/max/uniform are BLIND passthroughs in the reference
        // (dump-registry.mjs: `default: spec.default, min: spec.min,
        // max: spec.max, uniform: spec.uniform` -- no type filtering at
        // all). min/max are usually numbers but can be a 3-element array
        // for a vec3 param's per-component bounds (render.renderLit3d
        // cameraPosition min:[-1,-1,-1], verified against the oracle) --
        // ParamDef stores whatever is there; JsObject::value() already
        // yields Undefined for a genuinely absent key, so no extra "is
        // this the right type" gating belongs here.
        ParamDef pd;
        pd.name = key;
        pd.type = (gtype == JsText(u"vec4")) ? JsText(u"color") : gtype;
        pd.defaultValue = gspec.value(JsText(u"default"));
        if (!enumPath.isEmpty()) pd.enumPath = enumPath;
        pd.minValue = gspec.value(JsText(u"min"));
        pd.maxValue = gspec.value(JsText(u"max"));
        pd.uniformValue = gspec.value(JsText(u"uniform"));
        // Verbatim (incl. ':' group headers) -- the ops-dump consumer; NOT
        // the same filtering as the enum-registration loop above.
        if (hasChoicesField) pd.choicesValue = choicesVal;
        const JsValue defaultFromVal = gspec.value(JsText(u"defaultFrom"));
        if (defaultFromVal.isString()) pd.defaultFromValue = defaultFromVal;

        spec.args.append(pd);
    }

    if (!ops_.contains(opName)) opOrder_.append(opName);
    ops_.insert(opName, spec);

    if (isStarterDef(def)) {
        starterOps_.insert(opName);
        // The pinned reference registers both the callable name and its
        // namespaced form for starter-chain diagnostics.
        starterOps_.insert(func);
    }

    const JsValue paramAliasesVal = def.value(JsText(u"paramAliases"));
    if (paramAliasesVal.isObject() && !paramAliasesVal.toObject().isEmpty()) {
        paramAliases_.insert(opName, paramAliasesVal.toObject());
    }

    if (def.value(JsText(u"hidden")).toBool(false)) {
        const JsValue deprecatedBy = def.value(JsText(u"deprecatedBy"));
        if (deprecatedBy.isString()) {
            effectAliases_.insert(opName, deprecatedBy.toString());
        }
    }

    // define-map: globals carrying `define` -> {globalKey: DEFINE_NAME}
    // (Expander/orchestrator concern; not part of any T9 gate -- order is
    // irrelevant, this is a lookup map, not a compared array).
    JsObject defs;
    for (auto git = globals.constBegin(); git != globals.constEnd(); ++git) {
        if (!git.value().isObject()) continue;
        const JsValue defineVal = git.value().toObject().value(JsText(u"define"));
        if (defineVal.isString()) defs.insert(git.key(), defineVal);
    }
    if (!defs.isEmpty()) defineMap_.insert(opName, defs);
}

JsObject EffectRegistry::getEffect(const JsText& key) const { return effects_.value(key); }

bool EffectRegistry::hasEffect(const JsText& key) const { return effects_.contains(key); }

const OpSpec* EffectRegistry::getOp(const JsText& opName) const {
    const auto it = ops_.find(opName);
    return it == ops_.end() ? nullptr : &it.value();
}

bool EffectRegistry::isStarterOp(const JsText& name) const {
    // reference/02 SS9 isStarterOp.
    if (name.isEmpty()) return false;
    // Force particles to be non-starter (workaround for stale
    // manifest/cache -- reference comment, verbatim). Dead code on this
    // catalog (no `particles` effect exists), ported for fidelity.
    if (name == JsText(u"particles") || name == JsText(u"render.particles")) return false;
    if (starterOps_.contains(name)) return true;
    // For namespaced queries, a bare canonical starter must not make a
    // different namespace look like the registered starter.
    const JsList parts = name.split(u'.', false);
    if (parts.size() > 1) {
        const JsText canonical = parts.last();
        if (starterOps_.contains(canonical)) {
            for (const JsText& op : starterOps_) {
                if (op.endsWith(JsText(u".") + canonical)) return false;
            }
            return true;
        }
    }
    return false;
}

JsText EffectRegistry::checkEffectAlias(const JsText& opName) const {
    // reference/01 SS8.4.
    const auto it = effectAliases_.find(opName);
    if (it == effectAliases_.end()) return JsText();
    const JsText newName = it.value();
    const int dot = opName.lastIndexOf(u'.');
    const JsText oldName = dot >= 0 ? JsText(opName.mid(dot + 1)) : opName;
    return JsText(u"effect '%1' is deprecated, use '%2' instead. Aliases will be removed on 2026-09-01.")
        .arg(oldName, newName);
}

JsList EffectRegistry::resolveParamAliases(const JsText& opName, JsObject& kwargs) const {
    // reference/01 SS8.5 -- mutates kwargs in place; returns warnings.
    JsList warnings;
    const auto it = paramAliases_.find(opName);
    if (it == paramAliases_.end()) return warnings;
    const JsObject aliases = it.value();
    for (auto ait = aliases.constBegin(); ait != aliases.constEnd(); ++ait) {
        const JsText oldName = ait.key();
        if (!kwargs.contains(oldName)) continue;
        const JsText newName = ait.value().toString();
        if (!kwargs.contains(newName)) {
            kwargs.insert(newName, kwargs.value(oldName));
        }
        kwargs.remove(oldName);
        warnings.append(
            JsText(u"param '%1' is deprecated, use '%2' instead. Aliases will be removed on 2026-09-01.")
                .arg(oldName, newName));
    }
    return warnings;
}

bool EffectRegistry::isStarterDef(const JsObject& def) {
    const JsValue passesVal = def.value(JsText(u"passes"));
    if (!passesVal.isArray()) return true; // no passes => starter
    const JsSet<JsText>& sentinels = starterInputSentinels();
    for (const JsValue& passVal : passesVal.toArray()) {
        if (!passVal.isObject()) continue;
        const JsValue inputsVal = passVal.toObject().value(JsText(u"inputs"));
        if (!inputsVal.isObject()) continue;
        const JsObject inputs = inputsVal.toObject();
        for (auto iit = inputs.constBegin(); iit != inputs.constEnd(); ++iit) {
            if (iit.value().isString() && sentinels.contains(iit.value().toString())) {
                return false;
            }
        }
    }
    return true;
}

JsObject EffectRegistry::opSpecToJson(const OpSpec& spec) {
    if (spec.rawSpec.isObject()) return spec.rawSpec.toObject();
    JsArray args;
    for (const ParamDef& pd : spec.args) {
        JsObject arg;
        arg.insert(JsText(u"name"), pd.name);
        if (!pd.type.isEmpty()) arg.insert(JsText(u"type"), pd.type);
        if (pd.hasDefault()) arg.insert(JsText(u"default"), pd.defaultValue);
        if (pd.hasEnumPath()) {
            arg.insert(JsText(u"enum"), pd.enumPath);
            arg.insert(JsText(u"enumPath"), pd.enumPath);
        }
        if (pd.hasMin()) arg.insert(JsText(u"min"), pd.minValue);
        if (pd.hasMax()) arg.insert(JsText(u"max"), pd.maxValue);
        if (pd.hasUniform()) arg.insert(JsText(u"uniform"), pd.uniformValue);
        if (pd.hasChoices()) arg.insert(JsText(u"choices"), pd.choicesValue);
        // NOTE: defaultFrom is deliberately NOT dumped -- dump-registry.mjs's
        // own op-arg shape never includes it (validator-internal only).
        args.append(arg);
    }
    JsObject out;
    out.insert(JsText(u"name"), spec.name);
    out.insert(JsText(u"args"), args);
    return out;
}

JsObject EffectRegistry::dumpSummary() const {
    JsObject opsOut;
    for (const JsText& name : opOrder_) {
        const OpSpec* spec = getOp(name);
        if (spec) opsOut.insert(name, opSpecToJson(*spec));
    }
    JsObject paramAliasesOut;
    for (auto it = paramAliases_.constBegin(); it != paramAliases_.constEnd(); ++it) {
        paramAliasesOut.insert(it.key(), it.value());
    }
    JsObject effectAliasesOut;
    for (auto it = effectAliases_.constBegin(); it != effectAliases_.constEnd(); ++it) {
        effectAliasesOut.insert(it.key(), it.value());
    }
    JsObject effectKeysOut;
    for (auto it = effects_.constBegin(); it != effects_.constEnd(); ++it) {
        const JsObject def = it.value();
        const JsText ns = def.value(JsText(u"namespace")).toString();
        const JsText func = def.value(JsText(u"func")).toString();
        effectKeysOut.insert(it.key(), ns + u'.' + func);
    }

    JsObject out;
    out.insert(JsText(u"ops"), opsOut);
    out.insert(JsText(u"enums"), enums_.project());
    out.insert(JsText(u"paramAliases"), paramAliasesOut);
    out.insert(JsText(u"effectAliases"), effectAliasesOut);
    out.insert(JsText(u"effectKeys"), effectKeysOut);
    return out;
}

} // namespace nm
