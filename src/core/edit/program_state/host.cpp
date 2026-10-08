#include "core/edit/program_state/host.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>

namespace nm {
namespace {
Value field(const Value& value, const JsString& key) { return jsv::member(value, key); }
bool named(const std::vector<JsString>& names, const JsString& name) {
    return std::find(names.begin(), names.end(), name) != names.end();
}
double js_round(double value) {
    if (!std::isfinite(value) || value == 0.0) return value;
    if (value < 0.0 && value >= -0.5) return -0.0;
    const double floor = std::floor(value);
    return value - floor >= 0.5 ? floor + 1.0 : floor;
}
Value hex_to_rgb(const JsString& text) {
    Array rgb;
    for (std::size_t start : {std::size_t(1), std::size_t(3), std::size_t(5)}) {
        const std::size_t end = std::min(start + 2, text.size());
        rgb.emplace_back(start >= text.size() ? std::numeric_limits<double>::quiet_NaN()
                                             : parse_js_int_value(Value(text.substr(start, end - start)), 16) / 255.0);
    }
    return Value(std::move(rgb));
}
void record(MockPipeline& pipeline, const JsString& name,
            const std::vector<std::pair<JsString, Value>>& args) {
    Object call;
    call.set(u"fn", Value(name));
    for (const auto& [key, value] : args) call.set(key, value);
    pipeline.calls.emplace_back(std::move(call));
}
MockPipeline& require_pipeline(std::optional<MockPipeline>& pipeline, const JsString& member) {
    if (!pipeline) throw JsError(u"TypeError", u"Cannot read properties of null (reading '" + member + u"')");
    return *pipeline;
}
}  // namespace

namespace {
bool js_space(char16_t ch) {
    return ch == u' ' || (ch >= u'\t' && ch <= u'\r') || ch == u'\u00a0' || ch == u'\u1680' ||
           (ch >= u'\u2000' && ch <= u'\u200a') || ch == u'\u2028' || ch == u'\u2029' ||
           ch == u'\u202f' || ch == u'\u205f' || ch == u'\u3000' || ch == u'\ufeff';
}
bool ascii_digit(char16_t ch) { return ch >= u'0' && ch <= u'9'; }
int radix_digit(char16_t ch) {
    if (ch >= u'0' && ch <= u'9') return ch - u'0';
    if (ch >= u'a' && ch <= u'z') return ch - u'a' + 10;
    if (ch >= u'A' && ch <= u'Z') return ch - u'A' + 10;
    return -1;
}
}  // namespace

double parse_js_float_value(const Value& value) {
    if (value.is_number()) return value.as_number() == 0.0 ? 0.0 : value.as_number();
    const JsString text = jsv::to_string(value);
    std::size_t start = 0;
    while (start < text.size() && js_space(text[start])) ++start;
    std::size_t end = start;
    if (end < text.size() && (text[end] == u'+' || text[end] == u'-')) ++end;
    if (text.compare(end, 8, u"Infinity") == 0)
        return start < text.size() && text[start] == u'-' ? -std::numeric_limits<double>::infinity() : std::numeric_limits<double>::infinity();
    const std::size_t before = end;
    while (end < text.size() && ascii_digit(text[end])) ++end;
    bool digits = end > before;
    if (end < text.size() && text[end] == u'.') {
        ++end;
        const std::size_t after = end;
        while (end < text.size() && ascii_digit(text[end])) ++end;
        digits = digits || end > after;
    }
    if (!digits) return std::numeric_limits<double>::quiet_NaN();
    if (end < text.size() && (text[end] == u'e' || text[end] == u'E')) {
        std::size_t exponent = end + 1;
        if (exponent < text.size() && (text[exponent] == u'+' || text[exponent] == u'-')) ++exponent;
        const std::size_t exponent_start = exponent;
        while (exponent < text.size() && ascii_digit(text[exponent])) ++exponent;
        if (exponent > exponent_start) end = exponent;
    }
    return std::strtod(js_to_utf8(text.substr(start, end - start)).c_str(), nullptr);
}

double parse_js_int_value(const Value& value, int radix) {
    const JsString text = jsv::to_string(value);
    std::size_t start = 0;
    while (start < text.size() && js_space(text[start])) ++start;
    const bool negative = start < text.size() && text[start] == u'-';
    if (start < text.size() && (text[start] == u'+' || text[start] == u'-')) ++start;
    if ((radix == 0 || radix == 16) && start + 1 < text.size() && text[start] == u'0' &&
        (text[start + 1] == u'x' || text[start + 1] == u'X')) { radix = 16; start += 2; }
    if (radix == 0) radix = 10;
    std::size_t end = start;
    while (end < text.size() && radix_digit(text[end]) >= 0 && radix_digit(text[end]) < radix) ++end;
    if (end == start) return std::numeric_limits<double>::quiet_NaN();
    double magnitude = 0;
    if (radix == 10) magnitude = std::strtod(js_to_utf8(text.substr(start, end - start)).c_str(), nullptr);
    else for (std::size_t i = start; i < end; ++i) magnitude = magnitude * radix + radix_digit(text[i]);
    return negative ? -magnitude : magnitude;
}

MockMethods MockMethods::from_names(const std::vector<JsString>& names) {
    return {named(names, u"broadcastChainScopedParam"), named(names, u"checkAsyncRegen"),
            named(names, u"recreateTextures"), named(names, u"collectDefaultUniforms"),
            named(names, u"setUniform")};
}

Value resolve_enum_value(const Value& path, const Value& enums) {
    if (path.is_undefined() || path.is_null()) return Value::null();
    if (path.is_number() || path.is_bool()) return path;
    if (!path.is_string()) return Value::null();
    Value node = enums;
    const JsString& name = path.as_string();
    std::size_t start = 0;
    while (start <= name.size()) {
        const std::size_t stop = name.find(u'.', start);
        const JsString part = name.substr(start, stop == JsString::npos ? JsString::npos : stop - start);
        if (!part.empty()) {
            if (!jsv::truthy(node)) return Value::null();
            node = field(node, part);
            if (node.is_undefined()) return Value::null();
        }
        if (stop == JsString::npos) break;
        start = stop + 1;
    }
    if (node.is_number() || node.is_bool()) return node;
    if (node.is_object() || node.is_array()) {
        Value resolved = field(node, u"value");
        return resolved.is_undefined() ? Value::null() : resolved;
    }
    return Value::null();
}

Value convert_parameter_for_uniform(const Value& value, const Value& spec, const Value& enums) {
    if (!jsv::truthy(spec)) return value;
    const Value enum_spec = field(spec, u"enum");
    const Value enum_path = field(spec, u"enumPath");
    const Value type = field(spec, u"type");
    if ((jsv::truthy(enum_spec) || jsv::truthy(enum_path) || jsv::strict_equals(type, Value(u"member")))
        && value.is_string()) {
        Value resolved = resolve_enum_value(value, enums);
        if ((resolved.is_null() || resolved.is_undefined()) && (jsv::truthy(enum_spec) || jsv::truthy(enum_path))) {
            resolved = resolve_enum_value(Value(jsv::to_string(jsv::truthy(enum_spec) ? enum_spec : enum_path)
                                               + u'.' + value.as_string()), enums);
        }
        if (!resolved.is_null() && !resolved.is_undefined()) return resolved;
    }
    const JsString type_name = type.is_string() ? type.as_string() : JsString();
    if (type_name == u"boolean" || type_name == u"button") return Value(jsv::truthy(value));
    if (type_name == u"int") {
        if (value.is_bool()) return Value(value.as_bool() ? 1 : 0);
        return Value(value.is_number() ? js_round(value.as_number()) : parse_js_int_value(value, 10));
    }
    if (type_name == u"float") return value.is_number() ? value : Value(parse_js_float_value(value));
    if (type_name == u"color") {
        if (value.is_array()) {
            Array rgb;
            for (const Value& component : value.as_array()) {
                if (rgb.size() == 3) break;
                rgb.push_back(component.is_number() ? component : Value(parse_js_float_value(component)));
            }
            while (rgb.size() < 3) rgb.emplace_back(0);
            return Value(std::move(rgb));
        }
        if (value.is_string() && !value.as_string().empty() && value.as_string()[0] == u'#') {
            return hex_to_rgb(value.as_string());
        }
    }
    if ((type_name == u"vec3" || type_name == u"vec4") && value.is_array()) {
        Array values;
        for (const Value& component : value.as_array()) {
            values.push_back(component.is_number() ? component : Value(parse_js_float_value(component)));
        }
        return Value(std::move(values));
    }
    return value;
}

bool write_uniform_aliases(Value& pass, const JsString& param_name,
                           const Value& uniform_name, const Value& value) {
    if (!pass.is_object()) return false;
    const Value aliases = field(pass, u"uniformAliases");
    const Value uniforms = field(pass, u"uniforms");
    if (!jsv::truthy(aliases) || !jsv::truthy(uniforms)) return false;
    bool wrote = false;
    for (const auto& [shader_name, global_name] : jsv::entries(aliases)) {
        if (!jsv::strict_equals(global_name, Value(param_name)) &&
            !jsv::strict_equals(global_name, uniform_name)) continue;
        Value* target = &pass.as_object()[u"uniforms"];
        if (!target->is_object()) throw JsError(u"TypeError", u"Cannot set properties of undefined (setting '" + shader_name + u"')");
        jsv::set_plain(target->as_object(), shader_name, value);
        wrote = true;
    }
    return wrote;
}

Value MockHost::convert_parameter_for_uniform(const Value& value, const Value& spec) const {
    return convert == MockConvert::Passthrough ? value : nm::convert_parameter_for_uniform(value, spec, enums);
}
std::vector<Value>* MockHost::graph_passes() {
    if (!pipeline || !pipeline->graph.is_object()) return nullptr;
    if (!pipeline->graph.as_object().has(u"passes")) return nullptr;
    Value& passes = pipeline->graph.as_object()[u"passes"];
    return passes.is_array() ? &passes.as_array() : nullptr;
}
const MockMethods* MockHost::method_presence() const { return pipeline ? &pipeline->methods : nullptr; }
void MockHost::broadcast_chain_scoped_param(std::size_t index, const JsString& uniform, const JsString& scoped) {
    auto& p = require_pipeline(pipeline, u"broadcastChainScopedParam");
    record(p, u"broadcastChainScopedParam", {{u"pass", Value(static_cast<double>(index))},
                                             {u"uniformName", Value(uniform)}, {u"scopedName", Value(scoped)}});
}
void MockHost::check_async_regen(const Value& node_id, const Value& effect_key, const Object& values) {
    auto& p = require_pipeline(pipeline, u"checkAsyncRegen");
    record(p, u"checkAsyncRegen", {{u"nodeId", node_id}, {u"effectKey", effect_key},
                                   {u"stepValues", Value(values)}});
}
Object MockHost::collect_default_uniforms() {
    auto& p = require_pipeline(pipeline, u"collectDefaultUniforms");
    record(p, u"collectDefaultUniforms", {});
    Object values;
    const Value passes = field(p.graph, u"passes");
    if (passes.is_array()) for (const Value& pass : passes.as_array()) {
        for (const auto& [key, value] : jsv::entries(field(pass, u"uniforms"))) values.set(key, value);
    }
    return values;
}
void MockHost::recreate_textures(const Object& uniforms) {
    auto& p = require_pipeline(pipeline, u"recreateTextures");
    record(p, u"recreateTextures", {{u"uniforms", Value(uniforms)}});
}
void MockHost::set_uniform(const JsString& name, const Value& value) {
    auto& p = require_pipeline(pipeline, u"setUniform");
    record(p, u"setUniform", {{u"name", Value(name)}, {u"value", value}});
}
std::vector<Value> MockHost::take_calls() {
    if (!pipeline) return {};
    std::vector<Value> calls = std::move(pipeline->calls);
    pipeline->calls.clear();
    return calls;
}

}  // namespace nm
