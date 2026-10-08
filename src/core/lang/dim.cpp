#include "dim.h"


namespace nm::dim {

bool referencesParam(const JsValue& d) {
    if (!d.isObject()) return false;
    const JsObject o = d.toObject();
    return o.contains(JsText(u"param")) || o.contains(JsText(u"screenDivide"));
}

JsValue scope(const JsValue& d, const JsText& scopeSuffix, JsMap<JsText, JsText>& scopedParamMap,
                  const JsText& stateSizeScope) {
    if (!d.isObject()) return d;
    JsObject o = d.toObject();
    // `param` takes precedence, matching the reference's if/else-if chain
    // (scopeDimSpec checks `dimSpec.param !== undefined` before
    // `dimSpec.screenDivide !== undefined`).
    if (o.contains(JsText(u"param"))) {
        const JsText original = o.value(JsText(u"param")).toString();
        const JsText effectiveScope = (original == JsText(u"stateSize") && !stateSizeScope.isEmpty())
                                            ? stateSizeScope
                                            : scopeSuffix;
        const JsText scoped = original + u'_' + effectiveScope;
        scopedParamMap.insert(original, scoped);
        o.insert(JsText(u"param"), scoped);
        return o;
    }
    if (o.contains(JsText(u"screenDivide"))) {
        const JsText original = o.value(JsText(u"screenDivide")).toString();
        const JsText scoped = original + u'_' + scopeSuffix;
        scopedParamMap.insert(original, scoped);
        o.insert(JsText(u"screenDivide"), scoped);
        return o;
    }
    return d;
}

} // namespace nm::dim
