#include "core/graph/normalize.h"

#include "core/edit/jsv.h"

#include <cmath>

// Each function mirrors the function of the same name in export-graph.mjs.
// A member the JavaScript object literal sets to undefined is left out, as
// JSON.stringify leaves it out.

namespace libnoisemaker {
namespace {

Value field(const Value& object, const JsString& key) { return jsv::get(object, key); }

Value nullish_to_null(const Value& value) {
    return value.is_undefined() || value.is_null() ? Value::null() : value;
}

Value or_value(const Value& value, Value fallback) { return jsv::truthy(value) ? value : std::move(fallback); }

void set_present(Object& out, const JsString& key, const Value& value) {
    if (!value.is_undefined()) out.set(key, value);
}

// `pass.program || ''` with the `${pass.nodeId}_` prefix removed.
JsString program_without_node(const Value& pass) {
    const Value raw = field(pass, u"program");
    if (!jsv::truthy(raw)) return {};
    if (!raw.is_string()) throw jsv::not_a_function(u"s.startsWith");
    JsString s = raw.as_string();
    const Value node = field(pass, u"nodeId");
    if (jsv::truthy(node)) {
        const JsString prefix = jsv::to_string(node) + u"_";
        if (s.starts_with(prefix)) s = s.substr(prefix.size());
    }
    return s;
}

Value deriveProgName(const Value& pass) {
    JsString s = program_without_node(pass);
    const size_t suffix = s.find(u"__");
    if (suffix != JsString::npos && suffix > 0) s = s.substr(0, suffix);
    if (!s.empty()) return Value(s);
    const Value func = field(pass, u"effectFunc");
    return jsv::truthy(func) ? func : Value(u"main");
}

bool is_integer_text(const JsString& text) {
    size_t i = text.starts_with(u"-") ? 1 : 0;
    if (i == text.size()) return false;
    for (; i < text.size(); ++i) {
        if (text[i] < u'0' || text[i] > u'9') return false;
    }
    return true;
}

Object definesForPass(const Value& pass, const Value& programs) {
    Object out;
    const Value prog = jsv::truthy(programs) ? jsv::get_v(programs, field(pass, u"program")) : programs;
    const Value defines = jsv::truthy(prog) ? field(prog, u"defines") : prog;
    if (jsv::truthy(defines)) {
        for (const auto& [key, value] : jsv::entries(defines)) jsv::set_plain(out, key, value);
    }
    const JsString s = program_without_node(pass);
    const size_t suffix = s.find(u"__");
    if (suffix == JsString::npos) return out;
    const JsString tail = s.substr(suffix);
    size_t start = 0;
    while (start <= tail.size()) {
        const size_t end = tail.find(u"__", start);
        const JsString segment = tail.substr(start, end == JsString::npos ? JsString::npos : end - start);
        start = end == JsString::npos ? tail.size() + 1 : end + 2;
        if (segment.empty()) continue;
        const size_t underscore = segment.rfind(u'_');
        if (underscore == JsString::npos) continue;
        const JsString key = segment.substr(0, underscore);
        const JsString value = segment.substr(underscore + 1);
        jsv::set_plain(out, key, is_integer_text(value) ? Value(jsv::to_number(Value(value))) : Value(value));
    }
    return out;
}

Value normalizePass(const Value& pass, const Value& programs, const Value& defineMap) {
    const bool isBlit = jsv::strict_equals(field(pass, u"type"), Value(u"blit")) ||
                        jsv::strict_equals(field(pass, u"program"), Value(u"blit")) ||
                        jsv::strict_equals(field(pass, u"effectFunc"), Value(u"blit"));
    Object out;
    set_present(out, u"id", field(pass, u"id"));
    out.set(u"passType", Value(isBlit ? u"blit" : u"effect"));
    out.set(u"namespace", isBlit ? Value::null() : nullish_to_null(field(pass, u"effectNamespace")));
    out.set(u"func", isBlit ? Value(u"blit") : nullish_to_null(field(pass, u"effectFunc")));
    out.set(u"progName", isBlit ? Value(u"blit") : deriveProgName(pass));
    out.set(u"program", nullish_to_null(field(pass, u"program")));
    Object defines = isBlit ? Object{} : definesForPass(pass, programs);
    out.set(u"inputs", or_value(field(pass, u"inputs"), Value(Object{})));
    out.set(u"outputs", or_value(field(pass, u"outputs"), Value(Object{})));
    Object uniforms;
    jsv::spread_into(uniforms, or_value(field(pass, u"uniforms"), Value(Object{})));
    set_present(out, u"uniformSpecs", or_value(field(pass, u"uniformSpecs"), Value(Object{})));

    // Promote compile-time-define globals from uniforms into defines, by their
    // define name.
    if (!isBlit && jsv::truthy(defineMap)) {
        const JsString key =
            jsv::to_string(field(pass, u"effectNamespace")) + u"." + jsv::to_string(field(pass, u"effectFunc"));
        const Value dm = or_value(jsv::get(defineMap, key), Value(Object{}));
        for (const auto& [globalKey, defineName] : jsv::entries(dm)) {
            const Value uniformsValue(uniforms);
            if (!jsv::in_operator(Value(globalKey), uniformsValue)) continue;
            const Value v = jsv::get(uniformsValue, globalKey);
            const double promoted = v.is_bool() ? (v.as_bool() ? 1 : 0) : std::trunc(jsv::to_number(v));
            jsv::set_plain(defines, jsv::to_property_key(defineName), Value(promoted));
            uniforms.erase(globalKey);
        }
    }
    // The object literal fixed the order of defines and uniforms before the
    // promotion filled them in.
    Object ordered;
    for (const JsString& key : out.keys()) {
        ordered.set(key, *out.find(key));
        if (key == u"program") ordered.set(u"defines", Value(defines));
        if (key == u"outputs") ordered.set(u"uniforms", Value(uniforms));
    }

    for (const char16_t* name : {u"drawMode", u"count", u"countUniform", u"drawBuffers", u"blend", u"repeat",
                                 u"conditions", u"clear"}) {
        set_present(ordered, name, field(pass, name));
    }
    ordered.set(u"effectKey", nullish_to_null(field(pass, u"effectKey")));
    ordered.set(u"nodeId", nullish_to_null(field(pass, u"nodeId")));
    set_present(ordered, u"stepIndex", field(pass, u"stepIndex"));
    set_present(ordered, u"inheritsVolumeSize", field(pass, u"inheritsVolumeSize"));
    ordered.set(u"scopedParams", or_value(field(pass, u"scopedParams"), Value::null()));
    ordered.set(u"uniformAliases", or_value(field(pass, u"uniformAliases"), Value::null()));
    return Value(ordered);
}

Value normalizePrograms(const Value& programs) {
    Object out;
    for (const auto& [id, prog] : jsv::entries(or_value(programs, Value(Object{})))) {
        Object entry;
        entry.set(u"uniformLayout", or_value(field(prog, u"uniformLayout"), Value(Object{})));
        entry.set(u"defines", or_value(field(prog, u"defines"), Value(Object{})));
        jsv::set_plain(out, id, Value(entry));
    }
    return Value(out);
}

}  // namespace

Value normalizeGraphJson(const Value& graph, const Value& defineMap) {
    const Value programs = or_value(field(graph, u"programs"), Value(Object{}));
    Object out;
    set_present(out, u"id", field(graph, u"id"));
    set_present(out, u"source", field(graph, u"source"));
    out.set(u"renderSurface", nullish_to_null(field(graph, u"renderSurface")));
    Array passes;
    for (const Value& pass : jsv::iterate(or_value(field(graph, u"passes"), Value(Array{})), u"graph.passes")) {
        passes.push_back(normalizePass(pass, programs, defineMap));
    }
    out.set(u"passes", Value(passes));
    out.set(u"allocations", or_value(field(graph, u"allocations"), Value(Object{})));
    out.set(u"textures", or_value(field(graph, u"textures"), Value(Object{})));
    out.set(u"programs", normalizePrograms(programs));
    return Value(out);
}

}  // namespace libnoisemaker
