#include "dim.h"

#include <cmath>

namespace libnoisemaker::dim {

namespace {

// JavaScript truthiness of a dimension field.
bool truthy(const JsValue& v) {
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

} // namespace

bool referencesParam(const JsValue& d) {
    if (!d.isObject()) return false;
    const JsObject o = d.toObject();
    return o.contains(JsText(u"param")) || o.contains(JsText(u"screenDivide"));
}

JsValue scope(const JsValue& d, const JsText& scopeSuffix, JsMap<JsText, JsText>& scopedParamMap,
                  const JsText& stateSizeScope, const SizeParamClaim& claimSizeParam) {
    if (!d.isObject()) return d;
    JsObject o = d.toObject();
    // `param` takes precedence, matching the reference's if/else-if chain
    // (scopeDimSpec checks `dimSpec.param !== undefined` before
    // `dimSpec.screenDivide !== undefined`).
    if (o.contains(JsText(u"param"))) {
        const JsText original = o.value(JsText(u"param")).toString();
        // A particle-pipeline `stateSize` reads the pipeline's own state
        // resolution and bypasses the size-param claim, exactly like the
        // reference's inline `dimensionScope` ternary ahead of
        // `chainSizeParam`.
        JsText scoped;
        if (original == JsText(u"stateSize") && !stateSizeScope.isEmpty()) {
            scoped = original + u'_' + stateSizeScope;
        } else {
            const JsText candidate = original + u'_' + scopeSuffix;
            scoped = claimSizeParam ? claimSizeParam(candidate, truthy(o.value(JsText(u"inherit"))))
                                    : candidate;
        }
        // An inherited dimension reads the scope's value and never writes
        // this effect's own value over it.
        if (!truthy(o.value(JsText(u"inherit")))) scopedParamMap.insert(original, scoped);
        o.insert(JsText(u"param"), scoped);
        return o;
    }
    if (o.contains(JsText(u"screenDivide"))) {
        const JsText original = o.value(JsText(u"screenDivide")).toString();
        const JsText candidate = original + u'_' + scopeSuffix;
        const JsText scoped = claimSizeParam ? claimSizeParam(candidate, false) : candidate;
        scopedParamMap.insert(original, scoped);
        o.insert(JsText(u"screenDivide"), scoped);
        return o;
    }
    return d;
}

} // namespace libnoisemaker::dim
