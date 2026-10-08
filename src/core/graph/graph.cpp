#include "core/graph/compat.h"
#include "core/graph/graph.h"
#include "core/value/json.h"


#include <stdexcept>

namespace nm {

namespace {

[[noreturn]] void fail(const JsText& message) {
    throw std::runtime_error(message.toStdString());
}

JsText requireString(const JsObject& obj, const JsText& key, const JsText& context) {
    const JsValue value = obj.value(key);
    if (!value.isString()) {
        fail(JsText(u"%1: missing or non-string field '%2'").arg(context, key));
    }
    return value.toString();
}

// Returns the string at `key`, or an empty string if absent / JSON null
// (used for fields that are legitimately null for blit passes, e.g.
// "namespace"/"effectKey" -- docs/GRAPH-JSON-SCHEMA.md "Blit passes use...
// namespace:null... effectKey:null").
JsText optionalString(const JsObject& obj, const JsText& key) {
    const JsValue value = obj.value(key);
    return value.isString() ? value.toString() : JsText();
}

TextureSpec parseTextureSpec(const JsValue& value, const JsText& texId) {
    if (!value.isObject()) {
        fail(JsText(u"textures['%1']: expected an object").arg(texId));
    }
    const JsObject obj = value.toObject();
    if (!obj.contains(JsText(u"width")) || !obj.contains(JsText(u"height"))) {
        fail(JsText(u"textures['%1']: missing width/height").arg(texId));
    }

    TextureSpec spec;
    spec.width = obj.value(JsText(u"width"));
    spec.height = obj.value(JsText(u"height"));
    if (obj.contains(JsText(u"depth"))) {
        spec.depth = obj.value(JsText(u"depth"));
    }
    spec.is3D = obj.value(JsText(u"is3D")).toBool(false);
    spec.mipmaps = obj.value(JsText(u"mipmaps")).toBool(false);
    spec.persistent = obj.value(JsText(u"persistent")).toBool(false);
    spec.filter = obj.value(JsText(u"filter")).toString();
    spec.format = obj.value(JsText(u"format")).toString(JsText(u"rgba16f"));
    for (const JsValue& usageEntry : obj.value(JsText(u"usage")).toArray()) {
        spec.usage.append(usageEntry.toString());
    }
    return spec;
}

Pass parsePass(const JsValue& value, int index, const JsObject& programs) {
    if (!value.isObject()) {
        fail(JsText(u"passes[%1]: expected an object").arg(index));
    }
    const JsObject obj = value.toObject();
    const JsText context = JsText(u"passes[%1]").arg(index);

    Pass pass;
    pass.id = requireString(obj, JsText(u"id"), context);
    const bool raw = !obj.contains(JsText(u"passType"));
    pass.passType = raw
        ? (obj.value(JsText(u"program")).toString() == JsText(u"blit") &&
           !obj.contains(JsText(u"effectKey")) ? JsText(u"blit") : JsText(u"effect"))
        : requireString(obj, JsText(u"passType"), context);
    if (pass.passType != JsText(u"effect") && pass.passType != JsText(u"blit")) {
        fail(JsText(u"%1: unknown passType '%2'").arg(context, pass.passType));
    }
    pass.effectNamespace = optionalString(obj, raw ? JsText(u"effectNamespace") : JsText(u"namespace"));
    pass.func = raw ? (pass.passType == JsText(u"blit") ? JsText(u"blit")
                    : optionalString(obj, JsText(u"effectFunc")))
                    : requireString(obj, JsText(u"func"), context);
    pass.program = requireString(obj, JsText(u"program"), context);
    if (raw) {
        pass.progName = pass.program;
        const JsText prefix = optionalString(obj, JsText(u"nodeId")) + u'_';
        if (!prefix.empty() && pass.progName.startsWith(prefix)) pass.progName = pass.progName.substr(prefix.size());
        const auto suffix = pass.progName.find(JsText(u"__"));
        if (suffix != JsText::npos) pass.progName = pass.progName.substr(0, suffix);
    } else {
        pass.progName = requireString(obj, JsText(u"progName"), context);
    }
    pass.effectKey = optionalString(obj, JsText(u"effectKey"));
    pass.nodeId = optionalString(obj, JsText(u"nodeId"));
    pass.drawMode = optionalString(obj, JsText(u"drawMode"));
    pass.defines = raw ? programs.value(pass.program).toObject().value(JsText(u"defines")).toObject()
                       : obj.value(JsText(u"defines")).toObject();
    pass.inputs = obj.value(JsText(u"inputs")).toObject();
    pass.outputs = obj.value(JsText(u"outputs")).toObject();
    pass.uniforms = obj.value(JsText(u"uniforms")).toObject();
    pass.uniformSpecs = obj.value(JsText(u"uniformSpecs")).toObject();
    pass.repeat = obj.value(JsText(u"repeat"));
    pass.drawBuffers = obj.value(JsText(u"drawBuffers"));
    pass.count = obj.value(JsText(u"count"));
    pass.countUniform = obj.value(JsText(u"countUniform"));
    pass.blend = obj.value(JsText(u"blend"));
    pass.conditions = obj.value(JsText(u"conditions"));
    const JsValue stepIndex = obj.value(JsText(u"stepIndex"));
    if (stepIndex.isDouble()) {
        pass.stepIndex = stepIndex.toInt();
    }
    pass.inheritsVolumeSize = obj.value(JsText(u"inheritsVolumeSize")).toBool(false);
    pass.scopedParams = obj.value(JsText(u"scopedParams")).toObject();
    pass.uniformAliases = obj.value(JsText(u"uniformAliases")).toObject();
    return pass;
}

} // namespace

Graph Graph::fromJson(const std::string& json) {
    Value parsed;
    try { parsed = nm::json::parse(json); }
    catch (const nm::json::ParseError& e) {
        fail(JsText(u"graph JSON parse error: ") + JsText::fromUtf8(e.what()));
    }
    if (!parsed.is_object()) {
        fail(JsText(u"graph JSON: expected a top-level object"));
    }
    const JsObject root(parsed);

    Graph graph;
    graph.id = optionalString(root, JsText(u"id"));
    graph.source = optionalString(root, JsText(u"source"));
    graph.renderSurface = optionalString(root, JsText(u"renderSurface"));

    const JsValue passesValue = root.value(JsText(u"passes"));
    if (!passesValue.isArray()) {
        fail(JsText(u"graph JSON: missing or non-array 'passes'"));
    }
    const JsArray passesArray = passesValue.toArray();
    const JsObject programs = root.value(JsText(u"programs")).toObject();
    graph.passes.reserve(passesArray.size());
    for (int i = 0; i < passesArray.size(); ++i) {
        graph.passes.append(parsePass(passesArray.at(i), i, programs));
    }

    const JsObject allocations = root.value(JsText(u"allocations")).toObject();
    for (auto it = allocations.begin(); it != allocations.end(); ++it) {
        graph.allocations.insert(it.key(), it.value().toString());
    }

    const JsObject textures = root.value(JsText(u"textures")).toObject();
    for (auto it = textures.begin(); it != textures.end(); ++it) {
        graph.textures.insert(it.key(), parseTextureSpec(it.value(), it.key()));
    }

    return graph;
}

} // namespace nm
