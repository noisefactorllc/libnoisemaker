#include "core/graph/compat.h"
#include "core/graph/dsl_compiler.h"
#include "core/value/js_number.h"

#include "core/lang/effect_registry.h"
#include "core/graph/expander.h"
#include "core/lang/lexer.h"
#include "core/lang/parser.h"
#include "core/graph/resources.h"
#include "core/lang/validator.h"


#include <cmath>
#include <cstdint>
#include <stdexcept>

namespace nm {

namespace {

// ---------------------------------------------------------------------
// hashSource (compiler.js) — see dsl_compiler.h for the algorithm note.
// ---------------------------------------------------------------------

JsText toBase36(int32_t n) {
    if (n == 0) return JsText(u"0");
    const bool neg = n < 0;
    // Avoid negating INT32_MIN (UB): compute the magnitude in uint32_t.
    uint32_t mag = neg ? (0u - static_cast<uint32_t>(n)) : static_cast<uint32_t>(n);
    static const char kDigits[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    JsText s;
    while (mag > 0) {
        s.insert(s.begin(), static_cast<char16_t>(kDigits[mag % 36]));
        mag /= 36;
    }
    return neg ? (u'-' + s) : s;
}

// ---------------------------------------------------------------------
// JS truthy / template-literal-stringification helpers (compiler.js
// `effectSpec.width || 'screen'` etc.; export-graph.mjs's
// `` `${pass.effectNamespace}.${pass.effectFunc}` `` defineMap key).
// ---------------------------------------------------------------------

bool jsTruthy(const JsValue& v) {
    if (v.isUndefined() || v.isNull()) return false;
    if (v.isBool()) return v.toBool();
    if (v.isDouble()) return v.toDouble() != 0.0;
    if (v.isString()) return !v.toString().isEmpty();
    return true; // arrays/objects are always truthy in JS, even empty ones
}

// ---------------------------------------------------------------------
// extractTextureSpecs (compiler.js)
// ---------------------------------------------------------------------

JsObject extractTextureSpecs(const JsVector<ExpandedPass>& passes, const JsObject& textureSpecs) {
    JsObject textures;
    for (auto it = textureSpecs.constBegin(); it != textureSpecs.constEnd(); ++it) {
        const JsObject effectSpec = it.value().toObject();
        JsObject spec;
        const JsValue widthVal = effectSpec.value(JsText(u"width"));
        spec.insert(JsText(u"width"), jsTruthy(widthVal) ? widthVal : JsValue(JsText(u"screen")));
        const JsValue heightVal = effectSpec.value(JsText(u"height"));
        spec.insert(JsText(u"height"), jsTruthy(heightVal) ? heightVal : JsValue(JsText(u"screen")));
        const JsValue formatVal = effectSpec.value(JsText(u"format"));
        spec.insert(JsText(u"format"), jsTruthy(formatVal) ? formatVal : JsValue(JsText(u"rgba16f")));
        JsArray usage2d{JsText(u"render"), JsText(u"sample"), JsText(u"copySrc"), JsText(u"copyDst")};
        spec.insert(JsText(u"usage"), usage2d);

        if (jsTruthy(effectSpec.value(JsText(u"is3D")))) {
            const JsValue depthVal = effectSpec.value(JsText(u"depth"));
            JsValue depth = jsTruthy(depthVal) ? depthVal : JsValue();
            if (!jsTruthy(depth)) depth = jsTruthy(widthVal) ? widthVal : JsValue(64);
            spec.insert(JsText(u"depth"), depth);
            spec.insert(JsText(u"is3D"), true);
            JsArray usage3d{JsText(u"storage"), JsText(u"sample"), JsText(u"copySrc"), JsText(u"copyDst")};
            spec.insert(JsText(u"usage"), usage3d);
            const JsValue filterVal = effectSpec.value(JsText(u"filter"));
            if (jsTruthy(filterVal)) {
                spec.insert(JsText(u"filter"), filterVal);
            }
        } else {
            const JsValue mipmapsVal = effectSpec.value(JsText(u"mipmaps"));
            if (!mipmapsVal.isUndefined()) {
                spec.insert(JsText(u"mipmaps"), mipmapsVal);
            }
            const JsValue persistentVal = effectSpec.value(JsText(u"persistent"));
            if (!persistentVal.isUndefined()) {
                spec.insert(JsText(u"persistent"), persistentVal);
            }
        }
        textures.insert(it.key(), spec);
    }

    for (const ExpandedPass& pass : passes) {
        for (const auto& kv : pass.outputs) {
            const JsText& texId = kv.second;
            if (texId.startsWith(JsText(u"global_"))) continue;
            if (textures.contains(texId)) continue;
            JsObject spec;
            spec.insert(JsText(u"width"), JsText(u"screen"));
            spec.insert(JsText(u"height"), JsText(u"screen"));
            spec.insert(JsText(u"format"), JsText(u"rgba16f"));
            spec.insert(JsText(u"usage"),
                        JsArray{JsText(u"render"), JsText(u"sample"), JsText(u"copySrc"), JsText(u"copyDst")});
            textures.insert(texId, spec);
        }
    }
    return textures;
}

// Assemble the reference compiler graph. Shader text remains in programs.
JsObject normalizeGraph(const JsText& id, const JsText& source, const JsValue& renderSurface,
                            const JsVector<ExpandedPass>& passes, const JsObject& allocations,
                            const JsObject& textures, const JsObject& programs, const JsArray& mediaSteps) {
    JsObject out;
    out.insert(JsText(u"id"), id);
    out.insert(JsText(u"source"), source);
    JsArray normPasses;
    for (const ExpandedPass& p : passes) normPasses.append(toRawPassJson(p));
    out.insert(JsText(u"passes"), normPasses);
    out.insert(JsText(u"programs"), programs);
    out.insert(JsText(u"allocations"), allocations);
    out.insert(JsText(u"textures"), textures);
    out.insert(JsText(u"renderSurface"), (renderSurface.isUndefined() || renderSurface.isNull())
                                                     ? JsValue(JsValue::Null)
                                                     : renderSurface);
    out.insert(JsText(u"mediaSteps"), mediaSteps);
    return out;
}

} // namespace

namespace {

// JS String(value) for the JSON values a diagnostic location holds.
JsText jsString(const JsValue& value) {
    switch (value.type()) {
        case JsValue::Undefined:
            return JsText(u"undefined");
        case JsValue::Null:
            return JsText(u"null");
        case JsValue::Bool:
            return value.toBool() ? JsText(u"true") : JsText(u"false");
        case JsValue::Double:
            return js::numberToString(value.toDouble());
        case JsValue::String:
        case JsValue::Function:
            return value.toString();
        case JsValue::Array:
        case JsValue::Object:
            break;
    }
    return JsText(u"[object Object]");
}

} // namespace

CompilationError::CompilationError(const JsText& code, const JsArray& diagnostics, const JsArray& errors)
    : std::runtime_error(format(code, diagnostics, errors).toStdString()),
      code_(code),
      diagnostics_(diagnostics),
      errors_(errors) {}

JsText CompilationError::format(const JsText& code, const JsArray& diagnostics, const JsArray& errors) {
    JsList parts;
    if (code == JsText(u"ERR_COMPILATION_FAILED")) {
        for (const JsValue& dv : diagnostics) {
            const JsObject d = dv.toObject();
            if (d.value(JsText(u"severity")).toString() != JsText(u"error")) continue;
            JsText msg = d.value(JsText(u"message")).toString();
            if (msg.isEmpty()) msg = JsText(u"Unknown error");
            if (d.value(JsText(u"location")).isObject()) {
                const JsObject location = d.value(JsText(u"location")).toObject();
                msg += JsText(u" (line %1, col %2)")
                           .arg(jsString(location.value(JsText(u"line"))),
                                jsString(location.value(JsText(u"column"))));
            }
            parts.append(msg);
        }
        const JsText joined = parts.join(JsText(u"; "));
        return joined.isEmpty() ? JsText(u"Unknown compilation error") : joined;
    }
    for (const JsValue& ev : errors) {
        const JsValue message = ev.toObject().value(JsText(u"message"));
        parts.append(message.isString() && !message.toString().isEmpty() ? message.toString() : jsString(ev));
    }
    return parts.join(JsText(u"; "));
}

JsString hashSource(const JsString& source) {
    uint32_t hash = 0;
    for (const char16_t ch : source) {
        const uint32_t c = static_cast<uint32_t>(ch);
        // JS: hash = ((hash << 5) - hash) + c; hash = hash & hash (ToInt32
        // truncation). uint32_t wraparound arithmetic (well-defined, unlike
        // signed overflow) reproduces this bit-for-bit.
        hash = (hash << 5) - hash + c;
    }
    return toBase36(static_cast<int32_t>(hash));
}

Object compileGraphJson(const JsString& source, EffectRegistry& registry,
                        const Object& options) {
    const auto tokens = nm::lex(source);
    const JsObject ast(nm::parse(tokens, Value(options)));
    const JsObject validated = nm::validate(ast, registry);

    const JsArray diagnostics = validated.value(JsText(u"diagnostics")).toArray();
    for (const JsValue& dv : diagnostics) {
        if (dv.toObject().value(JsText(u"severity")).toString() == JsText(u"error")) {
            throw CompilationError(JsText(u"ERR_COMPILATION_FAILED"), diagnostics, JsArray());
        }
    }

    ExpandResult expanded = nm::expand(validated, registry, JsObject(Value(options)));
    if (!expanded.errors.isEmpty()) {
        throw CompilationError(JsText(u"ERR_EXPANSION_FAILED"), JsArray(), expanded.errors);
    }

    JsVector<PassIO> passIOs;
    passIOs.reserve(expanded.passes.size());
    for (const ExpandedPass& p : expanded.passes) {
        PassIO io;
        io.inputs.reserve(p.inputs.size());
        for (const auto& kv : p.inputs) io.inputs.append(kv.second);
        io.outputs.reserve(p.outputs.size());
        for (const auto& kv : p.outputs) io.outputs.append(kv.second);
        passIOs.append(io);
    }
    const auto allocationsMap = nm::allocateResources(passIOs);
    JsObject allocations;
    for (auto it = allocationsMap.constBegin(); it != allocationsMap.constEnd(); ++it) {
        allocations.insert(it.key(), it.value());
    }

    const JsObject textures = extractTextureSpecs(expanded.passes, expanded.textureSpecs);
    const JsText id = hashSource(source);

    return normalizeGraph(id, JsText(source), expanded.renderSurface, expanded.passes, allocations, textures,
                          expanded.programs, expanded.mediaSteps).raw().as_object();
}

nm::Graph compileGraph(const JsString& source, EffectRegistry& registry,
                       const Object& options) {
    return nm::Graph::fromJson(json::stringify(Value(compileGraphJson(source, registry, options))));
}

nm::Graph compileGraph(const JsString& source, const Object& options) {
    EffectRegistry registry;
    registry.loadEmbedded();
    return compileGraph(source, registry, options);
}

} // namespace nm
