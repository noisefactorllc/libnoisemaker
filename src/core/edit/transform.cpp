#include "core/edit/transform.h"
#include "core/edit/jsv.h"
#include "core/value/js_number.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>

namespace nm {
namespace {
Value prop(const Value& value, const JsString& name) { return jsv::get_opt(value, name); }
Value object(std::initializer_list<std::pair<JsString, Value>> fields) {
    Object out;
    for (const auto& [key, value] : fields) jsv::set_plain(out, key, value);
    return Value(std::move(out));
}
Value failure(const JsString& message) { return object({{u"success", false}, {u"error", message}}); }
Value or_value(const Value& value, const Value& fallback) { return jsv::truthy(value) ? value : fallback; }
Value default_object(const Value& value) { return value.is_undefined() ? Value(Object{}) : value; }
JsString string(const Value& value) { return jsv::to_string(value); }
JsString type_of(const Value& value) {
    switch (value.kind()) {
    case Value::Kind::Undefined: return u"undefined";
    case Value::Kind::Null: return u"object";
    case Value::Kind::Bool: return u"boolean";
    case Value::Kind::Number: return u"number";
    case Value::Kind::String: return u"string";
    case Value::Kind::Array: case Value::Kind::Object: return u"object";
    case Value::Kind::Function: return u"function";
    }
    return u"undefined";
}
Value deep_clone(const Value& source) {
    if (source.is_array()) {
        Array copy;
        for (const auto& item : source.as_array()) copy.push_back(deep_clone(item));
        return Value(std::move(copy));
    }
    if (source.is_object()) {
        Object copy;
        for (const auto& key : source.as_object().keys()) copy.set(key, deep_clone(*source.as_object().find(key)));
        return Value(std::move(copy));
    }
    return source;
}
Value registry_summary(const EffectRegistry& registry) { return registry.dumpSummaryValue(); }
Value ops(const EffectRegistry& registry) { return prop(registry_summary(registry), u"ops"); }
Value aliases(const EffectRegistry& registry, const Value& name) {
    return or_value(prop(prop(registry_summary(registry), u"paramAliases"), jsv::to_property_key(name)), Value(Object{}));
}
Value op_spec(const EffectRegistry& registry, const Value& name) { return jsv::member(ops(registry), jsv::to_property_key(name)); }
Value effect_instance(const EffectRegistry& registry, const Value& name) {
    return name.is_string() && registry.hasEffect(JsText(name.as_string()))
        ? registry.getEffectValue(name.as_string()) : Value::null();
}
Value search_order(const Value& options, const Value& compiled) {
    return or_value(jsv::get(options, u"searchOrder"), or_value(jsv::get(compiled, u"searchNamespaces"), Value(Array{})));
}
bool starter(const EffectRegistry& registry, const Value& name) {
    return name.is_string() && registry.isStarterOp(JsText(name.as_string()));
}
bool check_starter(const EffectRegistry& registry, const Value& name, const Value& order) {
    if (starter(registry, name)) return true;
    if (!name.is_string()) {
        if (name.is_null() || name.is_undefined()) throw jsv::cannot_read(name, u"includes");
        throw jsv::not_a_function(u"effectName.includes");
    }
    if (name.as_string().find(u'.') != JsString::npos || !(jsv::to_number(jsv::member(order, u"length")) > 0)) return false;
    for (const auto& ns : jsv::iterate(order, u"searchOrder"))
        if (starter(registry, Value(string(ns) + u"." + name.as_string()))) return true;
    return false;
}
Value effect_spec(const EffectRegistry& registry, const Value& name, const Value& order) {
    Value direct = op_spec(registry, name);
    if (jsv::truthy(direct)) return direct;
    if (!name.is_string()) {
        if (name.is_null() || name.is_undefined()) throw jsv::cannot_read(name, u"includes");
        throw jsv::not_a_function(u"effectName.includes");
    }
    if (name.as_string().find(u'.') == JsString::npos && jsv::to_number(jsv::member(order, u"length")) > 0)
        for (const auto& ns : jsv::iterate(order, u"searchOrder")) {
            Value spec = op_spec(registry, Value(string(ns) + u"." + name.as_string()));
            if (jsv::truthy(spec)) return spec;
        }
    return Value::null();
}
struct Location { size_t plan, chain; Value step; };
std::optional<Location> locate(const Value& compiled, const Value& step_index) {
    Value plans = prop(compiled, u"plans");
    if (!jsv::truthy(plans)) return {};
    double n = jsv::to_number(jsv::member(plans, u"length"));
    for (size_t pi = 0; static_cast<double>(pi) < n; ++pi) {
        Value plan = jsv::member(plans, utf8_to_js(std::to_string(pi)));
        Value chain = prop(plan, u"chain");
        if (!jsv::truthy(chain)) continue;
        double m = jsv::to_number(jsv::member(chain, u"length"));
        for (size_t ci = 0; static_cast<double>(ci) < m; ++ci) {
            Value step = jsv::member(chain, utf8_to_js(std::to_string(ci)));
            if (!jsv::truthy(jsv::get(step, u"builtin")) && jsv::strict_equals(jsv::get(step, u"temp"), step_index))
                return Location{pi, ci, step};
        }
    }
    return {};
}
bool starter_position(const Location& loc, const EffectRegistry& registry, const Value& order) {
    bool current_is_starter = check_starter(registry, prop(loc.step, u"op"), order);
    return loc.chain == 0 || (current_is_starter &&
                              (prop(loc.step, u"from").is_undefined() || prop(loc.step, u"from").is_null()));
}
bool in_set(const Array& values, const Value& target) {
    return std::any_of(values.begin(), values.end(), [&](const Value& item) { return jsv::same_value_zero(item, target); });
}
void add_set(Array& values, Value value) { if (!in_set(values, value)) values.push_back(std::move(value)); }
bool type_matches(const Value& value, const Value& declared) {
    if (value.is_null() || value.is_undefined() || !declared.is_string()) return true;
    const JsString& t = declared.as_string();
    if (t == u"float" || t == u"int" || t == u"number") return value.is_number();
    if (t == u"color") return value.is_string() && !value.as_string().empty() && value.as_string()[0] == u'#';
    if (t == u"bool" || t == u"boolean") return value.is_bool();
    if (t == u"surface" || t == u"tex") return value.is_string();
    return true;
}
Value canonical(const Value& map, const JsString& key) { return or_value(jsv::member(map, key), Value(key)); }
Value topology(const Value& instance) {
    if (!jsv::truthy(instance)) return {};
    Array textures, inputs;
    Value tex = prop(instance, u"textures");
    if (jsv::truthy(tex)) for (const auto& key : jsv::keys(tex)) textures.emplace_back(key);
    for (const auto& pass : jsv::iterate_anon(or_value(prop(instance, u"passes"), Value(Array{})))) {
        for (const auto& value : jsv::values(or_value(prop(pass, u"inputs"), Value(Object{}))))
            if (value.is_string()) add_set(inputs, value);
    }
    return object({{u"internalTextures", textures}, {u"passInputs", inputs}});
}
Value passes_and_outputs(const Value& instance) {
    if (!jsv::truthy(instance)) return {};
    Value raw = or_value(jsv::get(instance, u"passes"), Value(Array{}));
    if (!raw.is_array()) throw jsv::not_a_function(u"(instance.passes || []).map");
    Array passes;
    for (const auto& pass : raw.as_array()) {
        Object inputs, outputs;
        jsv::spread_into(inputs, prop(pass, u"inputs"));
        jsv::spread_into(outputs, prop(pass, u"outputs"));
        passes.push_back(object({{u"name", prop(pass, u"name")}, {u"program", prop(pass, u"program")},
                                 {u"inputs", inputs}, {u"outputs", outputs}, {u"drawBuffers", prop(pass, u"drawBuffers")}}));
    }
    Value geo = prop(instance, u"outputGeo"), tex3d = prop(instance, u"outputTex3d");
    return object({{u"passes", passes}, {u"outputs", object({{u"geo", geo.is_undefined() ? Value::null() : geo},
                                                                    {u"tex3d", tex3d.is_undefined() ? Value::null() : tex3d}})}});
}
Value backend_support(const Value& instance, const Value& name, const Value& manifest) {
    if (!jsv::truthy(manifest) || !jsv::truthy(instance) || !jsv::truthy(prop(instance, u"passes"))) return {};
    if (!name.is_string()) throw jsv::not_a_function(u"resolvedName.split");
    auto dot = name.as_string().find(u'.');
    Value ns = or_value(prop(instance, u"namespace"), Value(name.as_string().substr(0, dot)));
    auto last_dot = name.as_string().rfind(u'.');
    Value last(name.as_string().substr(last_dot == JsString::npos ? 0 : last_dot + 1));
    Value entry = Value::null();
    for (const Value& candidate : {prop(instance, u"name"), prop(instance, u"func"), last}) {
        if (!jsv::truthy(candidate)) continue;
        Value found = jsv::member(manifest, string(ns) + u"/" + string(candidate));
        if (jsv::truthy(found)) { entry = found; break; }
    }
    if (!jsv::truthy(entry)) return object({{u"webgl2", false}, {u"webgpu", false}});
    Value raw = prop(instance, u"passes");
    if (!raw.is_array()) throw jsv::not_a_function(u"instance.passes.map");
    Array programs;
    for (const auto& pass : raw.as_array()) if (jsv::truthy(prop(pass, u"program"))) programs.push_back(prop(pass, u"program"));
    auto cover = [&](const Value& table) -> Value {
        if (!jsv::truthy(table)) return Value(false);
        if (programs.empty()) return {};
        size_t hits = 0;
        for (const auto& p : programs) if (jsv::in_operator(p, table)) ++hits;
        if (!hits) return Value(false);
        return hits == programs.size() ? Value(true) : Value(u"partial");
    };
    return object({{u"webgl2", cover(prop(entry, u"glsl"))}, {u"webgpu", cover(prop(entry, u"wgsl"))}});
}
double js_round(double n) {
    double result=std::floor(n+0.5);
    return result==0&&std::signbit(n)?-0.0:result;
}
}

Value list_steps(const Value& compiled, const Value& options_source, const EffectRegistry& registry) {
    Value options = default_object(options_source), plans = prop(compiled, u"plans");
    if (!jsv::truthy(plans)) return Value(Array{});
    Value order = search_order(options, compiled);
    Array result;
    double n = jsv::to_number(jsv::member(plans, u"length"));
    for (size_t pi = 0; static_cast<double>(pi) < n; ++pi) {
        Value chain = prop(jsv::member(plans, utf8_to_js(std::to_string(pi))), u"chain");
        if (!jsv::truthy(chain)) continue;
        double m = jsv::to_number(jsv::member(chain, u"length"));
        for (size_t ci = 0; static_cast<double>(ci) < m; ++ci) {
            Value step = jsv::member(chain, utf8_to_js(std::to_string(ci)));
            if (jsv::truthy(jsv::get(step, u"builtin"))) continue;
            Value op = jsv::get(step, u"op");
            bool is_starter = check_starter(registry, op, order);
            bool position = ci == 0 || (is_starter && (prop(step, u"from").is_null() || prop(step, u"from").is_undefined()));
            result.push_back(object({{u"stepIndex", jsv::get(step, u"temp")}, {u"planIndex", static_cast<double>(pi)},
                                     {u"chainIndex", static_cast<double>(ci)}, {u"effectName", op},
                                     {u"isStarter", is_starter}, {u"isStarterPosition", position},
                                     {u"canReplaceWithStarter", position}, {u"canReplaceWithNonStarter", !position},
                                     {u"args", or_value(jsv::get(step, u"args"), Value(Object{}))}}));
        }
    }
    return Value(result);
}

Value predict_replacement(const Value& resolved_name, const Value& spec, const Value& new_args,
                          const Value& old_instance, const Value& options_source, const EffectRegistry& registry) {
    Value options = default_object(options_source);
    Value instance = effect_instance(registry, resolved_name);
    Value available;
    Array issues;
    if (jsv::truthy(instance)) available = Value(true);
    else if (!jsv::keys(prop(registry_summary(registry), u"effectKeys")).empty()) {
        available = Value(false);
        issues.push_back(object({{u"dimension", u"shader-availability"},
                                 {u"message", u"No registered effect definition for '" + string(resolved_name) + u"'"}}));
    }

    Value alias_map = aliases(registry, resolved_name);
    Array accepted;
    for (const auto& def : jsv::iterate_anon(or_value(prop(spec, u"args"), Value(Array{})))) {
        Value name = prop(def, u"name");
        if (jsv::truthy(name)) add_set(accepted, name);
    }
    Value globals = prop(instance, u"globals");
    if (jsv::truthy(globals)) for (const auto& key : jsv::keys(globals)) add_set(accepted, Value(key));
    for (const auto& key : jsv::keys(alias_map)) add_set(accepted, Value(key));
    for (const auto& value : jsv::values(alias_map)) add_set(accepted, value);

    Value provided = or_value(new_args, Value(Object{}));
    Array provided_canonical, unknown;
    for (const auto& key : jsv::keys(provided)) {
        Value c = canonical(alias_map, key);
        add_set(provided_canonical, c);
        if (!in_set(accepted, c)) unknown.emplace_back(key);
    }
    std::vector<std::pair<Value, Value>> defs;
    auto map_set = [&](Value name, Value def) {
        for (auto& item : defs) if (jsv::same_value_zero(item.first, name)) { item.second = std::move(def); return; }
        defs.emplace_back(std::move(name), std::move(def));
    };
    for (const auto& def : jsv::iterate_anon(or_value(prop(spec, u"args"), Value(Array{})))) map_set(jsv::get(def, u"name"), def);
    if (jsv::truthy(globals)) for (const auto& [key, def] : jsv::entries(globals)) {
        Value name(key);
        bool known = std::any_of(defs.begin(), defs.end(), [&](const auto& item) { return jsv::same_value_zero(item.first, name); });
        if (!known) map_set(name, def);
    }
    Array missing;
    for (const auto& [name, def] : defs) if (prop(def, u"default").is_undefined() && !in_set(provided_canonical, name)) missing.push_back(name);
    if (!unknown.empty())
        issues.push_back(object({{u"dimension", u"arguments"},
                                 {u"message", u"Unknown argument(s) for '" + string(resolved_name) + u"': " + jsv::join(unknown, u", ")}}));

    Array types, ranges;
    for (const auto& [key, value] : jsv::entries(provided)) {
        Value c = canonical(alias_map, key), def;
        for (const auto& entry : defs) if (jsv::same_value_zero(entry.first, c)) { def = entry.second; break; }
        if (!jsv::truthy(def)) continue;
        Value expected = jsv::get(def, u"type");
        if (!type_matches(value, expected))
            types.push_back(object({{u"arg", key}, {u"expected", expected}, {u"actual", type_of(value)}}));
        if (value.is_number()) {
            Value min = prop(def, u"min"), max = prop(def, u"max");
            if (!min.is_undefined() && value.as_number() < jsv::to_number(min))
                ranges.push_back(object({{u"arg", c}, {u"value", value}, {u"min", min}, {u"max", max}}));
            if (!max.is_undefined() && value.as_number() > jsv::to_number(max))
                ranges.push_back(object({{u"arg", c}, {u"value", value}, {u"min", min}, {u"max", max}}));
        }
        Value choices = prop(def, u"choices");
        if (jsv::truthy(choices) && value.is_number()) {
            Array allowed = jsv::values(choices);
            if (!in_set(allowed, value)) ranges.push_back(object({{u"arg", c}, {u"value", value}, {u"choices", allowed}}));
        }
    }
    if (!types.empty()) {
        JsString msg;
        for (const auto& item : types) {
            if (!msg.empty()) msg += u"; ";
            msg += u"Argument '" + string(prop(item, u"arg")) + u"' for '" + string(resolved_name) + u"' expects " +
                   string(prop(item, u"expected")) + u", got " + string(prop(item, u"actual"));
        }
        issues.push_back(object({{u"dimension", u"types"}, {u"message", msg}}));
    }
    if (!ranges.empty()) {
        JsString msg;
        for (const auto& item : ranges) {
            if (!msg.empty()) msg += u"; ";
            Value choices = prop(item, u"choices");
            if (jsv::truthy(choices))
                msg += u"Argument '" + string(prop(item, u"arg")) + u"' for '" + string(resolved_name) + u"' value " +
                       string(prop(item, u"value")) + u" is not one of " + jsv::join(choices.as_array(), u", ");
            else
                msg += u"Argument '" + string(prop(item, u"arg")) + u"' for '" + string(resolved_name) + u"' value " +
                       string(prop(item, u"value")) + u" outside range [" + string(prop(item, u"min")) + u", " +
                       string(prop(item, u"max")) + u"]";
        }
        issues.push_back(object({{u"dimension", u"ranges"}, {u"message", msg}}));
    }
    Value passes = passes_and_outputs(instance);
    Value sampler = topology(instance);
    if (jsv::truthy(sampler) && jsv::truthy(old_instance)) {
        Value before = topology(old_instance);
        if (jsv::truthy(before)) sampler.as_object().set(u"changedFrom", object({{u"internalTextures", prop(before, u"internalTextures")},
                                                                                     {u"passInputs", prop(before, u"passInputs")}}));
    }
    Value backend = backend_support(instance, resolved_name, jsv::get(options, u"manifest"));
    return object({{u"effect", resolved_name}, {u"available", available},
                   {u"arguments", object({{u"unknown", unknown}, {u"missing", missing}})},
                   {u"types", types}, {u"ranges", ranges}, {u"passes", passes}, {u"outputs", Value()},
                   {u"samplerTopology", sampler}, {u"backendSupport", backend}, {u"issues", issues}});
}

Value replace_effect(const Value& compiled, const Value& step_index, const Value& new_effect_name,
                     const Value& new_args_source, const Value& options_source, const EffectRegistry& registry) {
    Value new_args = default_object(new_args_source), options = default_object(options_source);
    if (!jsv::truthy(prop(compiled, u"plans"))) return failure(u"Invalid compiled program: missing plans");
    Value order = search_order(options, compiled);
    auto location = locate(compiled, step_index);
    if (!location) return failure(u"Step with index " + string(step_index) + u" not found");
    Value old_name = jsv::get(location->step, u"op");
    bool position = starter_position(*location, registry, order);
    bool new_is_starter = check_starter(registry, new_effect_name, order);
    Value spec = effect_spec(registry, new_effect_name, order);
    if (!jsv::truthy(spec)) return failure(u"Effect '" + string(new_effect_name) + u"' not found");
    if (position && !new_is_starter)
        return failure(u"Cannot replace starter effect '" + string(old_name) + u"' with non-starter effect '" + string(new_effect_name) +
                       u"'. The first effect in a chain must be a starting effect.");
    if (!position && new_is_starter)
        return failure(u"Cannot replace non-starter effect '" + string(old_name) + u"' with starter effect '" + string(new_effect_name) +
                       u"'. Starting effects can only appear at the beginning of a chain.");
    Value program = deep_clone(compiled);
    Object final_args;
    for (const auto& def : jsv::iterate(or_value(jsv::get(spec, u"args"), Value(Array{})), u"specArgs")) {
        Value default_value = jsv::get(def, u"default");
        if (!default_value.is_undefined()) jsv::set_plain(final_args, jsv::to_property_key(jsv::get(def, u"name")), default_value);
    }
    for (const auto& [key, value] : jsv::entries_strict(new_args)) {
        Value rounded = value;
        if (value.is_number() && std::floor(value.as_number()) != value.as_number())
            rounded = Value(js_round(value.as_number() * 1000.0) / 1000.0);
        jsv::set_plain(final_args, key, rounded);
    }
    Value resolved = new_effect_name, ns = Value::null();
    if (!new_effect_name.is_string()) {
        if (new_effect_name.is_null() || new_effect_name.is_undefined()) throw jsv::cannot_read(new_effect_name, u"includes");
        throw jsv::not_a_function(u"newEffectName.includes");
    }
    const JsString& name = new_effect_name.as_string();
    auto dot = name.find(u'.');
    if (dot != JsString::npos) {
        ns = Value(name.substr(0, dot));
        if (!jsv::truthy(op_spec(registry, new_effect_name))) return failure(u"Effect '" + name + u"' not found");
    } else {
        for (const auto& prefix : jsv::iterate(order, u"searchOrder")) {
            Value candidate(string(prefix) + u"." + name);
            if (jsv::truthy(op_spec(registry, candidate))) { resolved = candidate; ns = prefix; break; }
        }
        if (!jsv::truthy(ns)) {
            JsString suffix = u"." + name;
            for (const auto& candidate : jsv::keys(ops(registry))) {
                if (candidate.size() >= suffix.size() && candidate.substr(candidate.size() - suffix.size()) == suffix) {
                    resolved = Value(candidate); ns = Value(candidate.substr(0, candidate.find(u'.'))); break;
                }
            }
        }
    }
    Value prediction = predict_replacement(resolved, spec, new_args, effect_instance(registry, old_name), options, registry);
    Value issues = prop(prediction, u"issues");
    if (jsv::strict_equals(prop(options, u"preflight"), Value(true)) && jsv::to_number(jsv::member(issues, u"length")) > 0) {
        Array messages;
        for (const auto& item : jsv::iterate(issues, u"prediction.issues")) messages.push_back(prop(item, u"message"));
        return object({{u"success", false}, {u"error", u"Replacement preflight failed: " + jsv::join(messages, u"; ")}, {u"prediction", prediction}});
    }
    if (jsv::truthy(ns)) {
        Value namespaces = jsv::get(program, u"searchNamespaces");
        bool includes = false;
        if (namespaces.is_array()) includes = in_set(namespaces.as_array(), ns);
        else if (namespaces.is_string()) includes = namespaces.as_string().find(string(ns)) != JsString::npos;
        else if (namespaces.is_null() || namespaces.is_undefined()) throw jsv::cannot_read(namespaces, u"includes");
        else throw jsv::not_a_function(u"newProgram.searchNamespaces.includes");
        if (!includes) {
            Array extended = jsv::iterate(namespaces, u"newProgram.searchNamespaces");
            extended.push_back(ns);
            program.as_object().set(u"searchNamespaces", Value(std::move(extended)));
        }
    }
    Value* target = &program;
    if (!target->is_object()) return object({{u"success", true}, {u"program", program}, {u"prediction", prediction}});
    target = &((*target).as_object()[u"plans"]);
    if (!target->is_array() || location->plan >= target->as_array().size()) return object({{u"success", true}, {u"program", program}, {u"prediction", prediction}});
    target = &target->as_array()[location->plan];
    if (!target->is_object()) return object({{u"success", true}, {u"program", program}, {u"prediction", prediction}});
    target = &target->as_object()[u"chain"];
    if (!target->is_array() || location->chain >= target->as_array().size()) return object({{u"success", true}, {u"program", program}, {u"prediction", prediction}});
    target = &target->as_array()[location->chain];
    if (target->is_object()) {
        target->as_object().set(u"op", resolved);
        target->as_object().set(u"args", Value(std::move(final_args)));
        target->as_object().set(u"namespace", jsv::truthy(ns) ? object({{u"resolved", ns}}) : Value::null());
    }
    return object({{u"success", true}, {u"program", program}, {u"prediction", prediction}});
}

Value get_compatible_replacements(const Value& compiled, const Value& step_index,
                                  const Value& options_source, const EffectRegistry& registry) {
    Value options = default_object(options_source);
    if (!jsv::truthy(prop(compiled, u"plans"))) return failure(u"Invalid compiled program: missing plans");
    Value order = search_order(options, compiled);
    auto location = locate(compiled, step_index);
    if (!location) return failure(u"Step with index " + string(step_index) + u" not found");
    bool position = starter_position(*location, registry, order);
    Value old_instance = effect_instance(registry, prop(location->step, u"op"));
    Array starters, non_starters, blocked;
    Object predictions;
    for (const auto& [key, spec] : jsv::entries(ops(registry))) {
        Value name(key);
        bool is_starter = check_starter(registry, name, order);
        Value prediction = predict_replacement(name, spec, Value(Object{}), old_instance, options, registry);
        jsv::set_plain(predictions, key, prediction);
        (is_starter ? starters : non_starters).push_back(name);
    }
    if (jsv::strict_equals(prop(options, u"preflight"), Value(true))) {
        auto filter = [&](Array& names) {
            Array kept;
            for (const auto& name : names) {
                Value issues = prop(jsv::member(Value(predictions), name.as_string()), u"issues");
                if (jsv::to_number(jsv::member(issues, u"length")) > 0)
                    blocked.push_back(object({{u"effect", name}, {u"issues", issues}}));
                else kept.push_back(name);
            }
            names = std::move(kept);
        };
        if (position) filter(starters); else filter(non_starters);
        return object({{u"success", true}, {u"compatible", position ? starters : non_starters},
                       {u"incompatible", position ? non_starters : starters}, {u"blocked", blocked},
                       {u"predictions", predictions}});
    }
    return object({{u"success", true}, {u"compatible", position ? starters : non_starters},
                   {u"incompatible", position ? non_starters : starters}, {u"predictions", predictions}});
}
} // namespace nm
