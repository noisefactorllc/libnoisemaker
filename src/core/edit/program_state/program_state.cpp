#include "core/edit/program_state/program_state.h"

#include "core/edit/program_state/dsl_utils.h"
#include "core/edit/program_state/palette.h"
#include "core/lang/diagnostics.h"
#include "core/edit/unparser.h"
#include "core/value/json.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <exception>
#include <limits>

namespace nm {
namespace {
Value field(const Value& value, const JsString& key) { return jsv::member(value, key); }
Value registry_effect(EffectRegistry& registry, const JsString& key) {
    if (!registry.hasEffect(JsText(key))) return Value();
    Value def = registry.getEffectValue(key);
    if (def.is_object()) {
        const Value* source = def.as_object().find(u"sourceNamespace");
        if (source) {
            if (source->is_null() || source->is_undefined()) def.as_object().erase(u"namespace");
            else def.as_object().set(u"namespace", *source);
            def.as_object().erase(u"sourceNamespace");
        }
    }
    return def;
}
Value own(const Object& object, const JsString& key) {
    if (const Value* value = object.find(key)) return *value;
    return jsv::member(Value(Object{}), key);
}
bool nullish(const Value& value) { return value.is_null() || value.is_undefined(); }
double js_math_max(double a, double b) {
    if (std::isnan(a) || std::isnan(b)) return std::numeric_limits<double>::quiet_NaN();
    if (a == 0.0 && b == 0.0) return std::signbit(a) ? b : a;
    return a > b ? a : b;
}
double js_math_min(double a, double b) {
    if (std::isnan(a) || std::isnan(b)) return std::numeric_limits<double>::quiet_NaN();
    if (a == 0.0 && b == 0.0) return std::signbit(a) ? a : b;
    return a < b ? a : b;
}
bool var_ref(const Value& value) {
    return (value.is_object() || value.is_array()) && jsv::truthy(field(value, u"_varRef"));
}
bool automation(const Value& value) {
    const Value kind = field(value, u"type");
    if (kind.is_string() && (kind.as_string() == u"Oscillator" || kind.as_string() == u"Midi" || kind.as_string() == u"Audio")) return true;
    const Value ast_kind = field(field(value, u"_ast"), u"type");
    return ast_kind.is_string() && (ast_kind.as_string() == u"Oscillator" || ast_kind.as_string() == u"Midi" || ast_kind.as_string() == u"Audio");
}
Value validate_value(Value value, const Value& spec) {
    if (!jsv::truthy(spec) || automation(value)) return value;
    const Value kind = field(spec, u"type");
    if (!kind.is_string()) return value;
    const JsString& type = kind.as_string();
    if (type == u"float" || type == u"int") {
        double number = type == u"float" ? parse_js_float_value(value) : parse_js_int_value(value);
        if (std::isnan(number)) {
            value = field(spec, u"default");
            if (nullish(value)) value = Value(0);
        } else value = Value(number);
        const Value min = field(spec, u"min");
        const Value max = field(spec, u"max");
        if (!min.is_undefined()) value = Value(js_math_max(jsv::to_number(min), jsv::to_number(value)));
        if (!max.is_undefined()) value = Value(js_math_min(jsv::to_number(max), jsv::to_number(value)));
        return value;
    }
    if (type == u"boolean") return Value(jsv::truthy(value));
    int count = type == u"vec2" ? 2 : type == u"vec3" || type == u"color" ? 3 : type == u"vec4" ? 4 : 0;
    if (!count) return value;
    if (type == u"color" && value.is_string() && !value.as_string().empty() && value.as_string()[0] == u'#') {
        Array rgb;
        const JsString& hex = value.as_string();
        for (std::size_t i : {std::size_t(1), std::size_t(3), std::size_t(5)}) {
            const std::string part = i >= hex.size() ? std::string() : js_to_utf8(hex.substr(i, 2));
            char* end = nullptr;
            const long parsed = std::strtol(part.c_str(), &end, 16);
            rgb.emplace_back(end == part.c_str() ? std::numeric_limits<double>::quiet_NaN() : parsed / 255.0);
        }
        return Value(std::move(rgb));
    }
    if (!value.is_array()) {
        const Value fallback = field(spec, u"default");
        if (jsv::truthy(fallback)) return fallback;
        return Value(Array(static_cast<std::size_t>(count), Value(0)));
    }
    Array result;
    for (const Value& component : value.as_array()) {
        if (static_cast<int>(result.size()) >= count) break;
        const double number = parse_js_float_value(component);
        result.emplace_back(std::isnan(number) ? 0.0 : number);
    }
    return Value(std::move(result));
}
bool values_equal(const Value& a, const Value& b) {
    if (jsv::strict_equals(a, b)) return true;
    if (!a.is_array() || !b.is_array() || a.as_array().size() != b.as_array().size()) return false;
    for (std::size_t i = 0; i < a.as_array().size(); ++i)
        if (!jsv::strict_equals(a.as_array()[i], b.as_array()[i])) return false;
    return true;
}
JsString step_key(std::size_t index) { return u"step_" + utf8_to_js(std::to_string(index)); }
double step_index(const JsString& key) {
    if (key.size() < 6 || key.substr(0, 5) != u"step_") return std::numeric_limits<double>::quiet_NaN();
    const JsString digits = key.substr(5);
    if (digits.empty() || !std::all_of(digits.begin(), digits.end(), [](char16_t c) { return c >= u'0' && c <= u'9'; })) return std::numeric_limits<double>::quiet_NaN();
    return std::strtod(js_to_utf8(digits).c_str(), nullptr);
}
JsString number_key(double number) { return format_lossless_number(number); }
Value map_entries(const std::vector<std::pair<Value, Value>>& entries) {
    Array out;
    for (const auto& [key, value] : entries) out.emplace_back(Array{key, value});
    return Value(std::move(out));
}
Value map_object(const std::vector<std::pair<Value, Value>>& entries) {
    Object out;
    for (const auto& [key, value] : entries) jsv::set_plain(out, jsv::to_property_key(key), value);
    return Value(std::move(out));
}
template<class Map> void map_set(Map& map, const Value& key, const Value& value) {
    for (auto& [existing, current] : map.entries) if (jsv::same_value_zero(existing, key)) { current = value; return; }
    map.entries.push_back({key, value});
}
template<class Map> Value map_get(const Map& map, const Value& key) {
    for (const auto& [existing, current] : map.entries) if (jsv::same_value_zero(existing, key)) return current;
    return Value();
}
template<class Map> void map_erase(Map& map, const Value& key) {
    map.entries.erase(std::remove_if(map.entries.begin(), map.entries.end(), [&](const auto& item) { return jsv::same_value_zero(item.first, key); }), map.entries.end());
}
Value tagged_error(const JsError& error) {
    Object out;
    out.set(u"$js", Value(u"error"));
    out.set(u"name", Value(error.name()));
    out.set(u"message", Value(error.message()));
    return Value(std::move(out));
}
}  // namespace

Value Change::to_value() const {
    Object out;
    out.set(u"stepKey", Value(step_key));
    out.set(u"paramName", Value(param_name));
    out.set(u"value", value);
    out.set(u"previousValue", previous_value);
    return Value(std::move(out));
}

ProgramStateCore::ProgramStateCore(EffectRegistry& registry) : registry_(&registry) {}

ProgramStateCore::ListenerId ProgramStateCore::on(const JsString& event, const Listener& callback) {
    auto it = std::find_if(listeners_.begin(), listeners_.end(), [&](const auto& entry) { return entry.first == event; });
    if (it == listeners_.end()) { listeners_.push_back({event, std::make_shared<std::vector<EventListener>>()}); it = std::prev(listeners_.end()); }
    const ListenerId id = next_listener_id_++;
    it->second->push_back({id, callback, false});
    return id;
}
ProgramStateCore::ListenerId ProgramStateCore::once(const JsString& event, const Listener& callback) {
    auto it = std::find_if(listeners_.begin(), listeners_.end(), [&](const auto& entry) { return entry.first == event; });
    if (it == listeners_.end()) { listeners_.push_back({event, std::make_shared<std::vector<EventListener>>()}); it = std::prev(listeners_.end()); }
    const ListenerId id = next_listener_id_++;
    it->second->push_back({id, callback, true});
    return id;
}
void ProgramStateCore::off(const JsString& event, ListenerId id) {
    for (auto& [name, entries] : listeners_) if (name == event)
        entries->erase(std::remove_if(entries->begin(), entries->end(), [&](const EventListener& listener) { return listener.id == id; }), entries->end());
}
void ProgramStateCore::remove_all_listeners(const std::optional<JsString>& event) {
    if (!event || event->empty()) listeners_.clear();
    else listeners_.erase(std::remove_if(listeners_.begin(), listeners_.end(), [&](const auto& entry) { return entry.first == *event; }), listeners_.end());
}
void ProgramStateCore::emit(const JsString& event, const Value& data) {
    auto it = std::find_if(listeners_.begin(), listeners_.end(), [&](const auto& entry) { return entry.first == event; });
    if (it == listeners_.end()) return;
    const auto listeners = it->second;
    ListenerId last = 0;
    while (true) {
        auto next = std::find_if(listeners->begin(), listeners->end(), [&](const EventListener& item) { return item.id > last; });
        if (next == listeners->end()) break;
        const EventListener listener = *next;
        last = listener.id;
        if (listener.once) {
            listeners->erase(next);
        }
        try { listener.callback(*this, data); }
        catch (const JsError& error) { program_console_error({Value(u"[Emitter] Error in " + event + u" handler:"), tagged_error(error)}); }
        catch (const std::exception& error) {
            program_console_error({Value(u"[Emitter] Error in " + event + u" handler:"),
                                   tagged_error(JsError(u"Error", utf8_to_js(error.what())))});
        }
    }
}

Value ProgramStateCore::get_value(const JsString& key, const JsString& param) const {
    for (const auto& [name, state] : step_states_) if (name == key) {
        const Value value = own(state.values, param);
        return var_ref(value) ? field(value, u"value") : value;
    }
    return Value();
}
void ProgramStateCore::set_value(const JsString& key, const JsString& param, const Value& input) {
    auto it = std::find_if(step_states_.begin(), step_states_.end(), [&](const auto& item) { return item.first == key; });
    if (it == step_states_.end()) {
        StepState state;
        const double index = step_index(key);
        state.step_index = std::isnan(index) ? 0 : index;
        step_states_.push_back({key, std::move(state)});
        it = std::prev(step_states_.end());
    }
    const Value previous = get_value(key, param);
    const Value spec = field(field(it->second.effect_def, u"globals"), param);
    Value value = jsv::truthy(spec) ? validate_value(input, spec) : input;
    const Value current = own(it->second.values, param);
    if (var_ref(current)) {
        Object binding;
        jsv::spread_into(binding, current);
        jsv::set_plain(binding, u"value", value);
        jsv::set_plain(it->second.values, param, Value(std::move(binding)));
    } else jsv::set_plain(it->second.values, param, value);
    if (jsv::truthy(field(spec, u"define")) && !values_equal(previous, value)) recompile_pending_ = true;
    apply_to_pipeline();
    Change change{key, param, value, previous};
    if (batch_depth_) batched_changes_.push_back(change);
    else {
        emit(u"change", change.to_value());
        if (recompile_pending_) { recompile_pending_ = false; emit(u"recompileNeeded"); }
    }
}
Object ProgramStateCore::get_step_values(const JsString& key) const {
    Object result;
    for (const auto& [name, state] : step_states_) if (name == key) {
        for (const JsString& param : state.values.keys()) {
            const Value value = own(state.values, param);
            jsv::set_plain(result, param, var_ref(value) ? field(value, u"value") : value);
        }
        break;
    }
    return result;
}
void ProgramStateCore::set_step_values(const JsString& key, const Value& values) {
    batch([&](ProgramStateCore& state) {
        for (const auto& [param, value] : jsv::entries_strict(values)) state.set_value(key, param, value);
    });
}
void ProgramStateCore::batch(const std::function<void(ProgramStateCore&)>& action) {
    ++batch_depth_;
    std::exception_ptr error;
    try { action(*this); } catch (...) { error = std::current_exception(); }
    --batch_depth_;
    if (batch_depth_ || batched_changes_.empty()) {
        if (error) std::rethrow_exception(error);
        return;
    }
    std::vector<std::pair<JsString, std::pair<Object, Object>>> grouped;
    for (const Change& change : batched_changes_) {
        auto it = std::find_if(grouped.begin(), grouped.end(), [&](const auto& item) { return item.first == change.step_key; });
        if (it == grouped.end()) { grouped.push_back({change.step_key, {Object{}, Object{}}}); it = std::prev(grouped.end()); }
        jsv::set_plain(it->second.first, change.param_name, change.value);
        jsv::set_plain(it->second.second, change.param_name, change.previous_value);
    }
    for (auto& [key, pair] : grouped) {
        Object data;
        data.set(u"stepKey", Value(key));
        data.set(u"values", Value(std::move(pair.first)));
        data.set(u"previousValues", Value(std::move(pair.second)));
        emit(u"stepchange", Value(std::move(data)));
    }
    batched_changes_.clear();
    if (recompile_pending_) { recompile_pending_ = false; emit(u"recompileNeeded"); }
    if (error) std::rethrow_exception(error);
}

void ProgramStateCore::from_dsl(const JsString& dsl) {
    const Value previous = structure_;
    try { compiled_ = compile_state_dsl(dsl, *registry_); }
    catch (...) { compiled_ = Value::null(); }
    const Value effects = extract_effects_from_dsl(dsl, *registry_);
    bool changed = !previous.is_array() || !effects.is_array() || previous.as_array().size() != effects.as_array().size();
    if (!changed) for (std::size_t i = 0; i < effects.as_array().size(); ++i)
        if (!jsv::strict_equals(field(previous.as_array()[i], u"effectKey"), field(effects.as_array()[i], u"effectKey"))) { changed = true; break; }
    std::vector<std::pair<JsString, std::vector<Object>>> preserved;
    for (const auto& [key, state] : step_states_) {
        auto it = std::find_if(preserved.begin(), preserved.end(), [&](const auto& item) { return item.first == state.effect_key; });
        if (it == preserved.end()) { preserved.push_back({state.effect_key, {}}); it = std::prev(preserved.end()); }
        it->second.push_back(state.values);
    }
    structure_ = effects;
    if (changed) {
        std::vector<std::pair<JsString, StepState>> rebuilt;
        std::vector<std::pair<JsString, std::size_t>> occurrences;
        for (std::size_t i = 0; i < effects.as_array().size(); ++i) {
            const Value& effect = effects.as_array()[i];
            const Value effect_key_value = field(effect, u"effectKey");
            const JsString effect_key = effect_key_value.is_string() ? effect_key_value.as_string() : JsString();
            auto occurrence = std::find_if(occurrences.begin(), occurrences.end(), [&](const auto& item) { return item.first == effect_key; });
            if (occurrence == occurrences.end()) { occurrences.push_back({effect_key, 0}); occurrence = std::prev(occurrences.end()); }
            const std::size_t count = occurrence->second++;
            const Value def = registry_effect(*registry_, effect_key);
            const Value globals = field(def, u"globals");
            Object values;
            if (jsv::truthy(globals)) for (const auto& [param, spec] : jsv::entries(globals)) {
                const Value default_value = field(spec, u"default");
                if (!default_value.is_undefined()) jsv::set_plain(values, param, default_value);
            }
            const Value args = field(effect, u"args");
            for (const auto& [param, value] : jsv::entries(args)) jsv::set_plain(values, param, value);
            auto old = std::find_if(preserved.begin(), preserved.end(), [&](const auto& item) { return item.first == effect_key; });
            if (old != preserved.end() && count < old->second.size()) {
                for (const JsString& param : old->second[count].keys()) {
                    if (args.is_object() && args.as_object().has(param)) continue;
                    if (param == u"_skip") continue;
                    const Value value = own(old->second[count], param);
                    if (automation(field(args, param))) continue;
                    if ((!param.empty() && param[0] == u'_') || !value.is_undefined()) jsv::set_plain(values, param, value);
                }
            }
            rebuilt.push_back({step_key(i), StepState{effect_key, def, static_cast<double>(i), std::move(values)}});
        }
        step_states_ = std::move(rebuilt);
        Object event;
        event.set(u"structure", structure_);
        event.set(u"previousStructure", previous);
        emit(u"structurechange", Value(std::move(event)));
    } else {
        for (std::size_t i = 0; i < effects.as_array().size(); ++i) {
            auto it = std::find_if(step_states_.begin(), step_states_.end(), [&](const auto& item) { return item.first == step_key(i); });
            if (it == step_states_.end()) continue;
            for (const auto& [param, value] : jsv::entries(field(effects.as_array()[i], u"args"))) {
                if (json::stringify(own(it->second.values, param)) != json::stringify(value)) jsv::set_plain(it->second.values, param, value);
            }
        }
    }
    apply_to_pipeline();
    Object event;
    event.set(u"structure", structure_);
    emit(u"load", Value(std::move(event)));
}

void ProgramStateCore::from_dsl_value(const Value& dsl) {
    if (dsl.is_string()) { from_dsl(dsl.as_string()); return; }
    const Value previous = structure_;
    compiled_ = Value::null();
    structure_ = Value(Array{});
    if (previous.is_array() && !previous.as_array().empty()) {
        step_states_.clear();
        Object event;
        event.set(u"structure", structure_);
        event.set(u"previousStructure", previous);
        emit(u"structurechange", Value(std::move(event)));
    }
    apply_to_pipeline();
    Object event;
    event.set(u"structure", structure_);
    emit(u"load", Value(std::move(event)));
}

bool ProgramStateCore::would_change_structure(const JsString& dsl) const {
    const Value effects = extract_effects_from_dsl(dsl, *registry_);
    if (!structure_.is_array() || !effects.is_array() || structure_.as_array().size() != effects.as_array().size()) return true;
    for (std::size_t i = 0; i < effects.as_array().size(); ++i)
        if (!jsv::strict_equals(field(structure_.as_array()[i], u"effectKey"), field(effects.as_array()[i], u"effectKey"))) return true;
    return false;
}

void ProgramStateCore::reset_step(const JsString& key) {
    auto it = std::find_if(step_states_.begin(), step_states_.end(), [&](const auto& item) { return item.first == key; });
    if (it == step_states_.end()) return;
    const Value globals = field(it->second.effect_def, u"globals");
    if (!jsv::truthy(globals)) return;
    const Value skip = own(it->second.values, u"_skip");
    Object values;
    for (const auto& [param, spec] : jsv::entries(globals)) {
        const Value default_value = jsv::get(spec, u"default");
        if (!default_value.is_undefined()) jsv::set_plain(values, param, default_value);
    }
    if (jsv::truthy(skip)) values.set(u"_skip", Value(true));
    it->second.values = std::move(values);
    apply_to_pipeline();
    Object event;
    event.set(u"stepKey", Value(key));
    emit(u"reset", Value(std::move(event)));
}
void ProgramStateCore::set_skip(const JsString& key, const Value& skip) {
    auto it = std::find_if(step_states_.begin(), step_states_.end(), [&](const auto& item) { return item.first == key; });
    if (it == step_states_.end()) return;
    const Value previous = own(it->second.values, u"_skip");
    it->second.values.set(u"_skip", skip);
    apply_to_pipeline();
    emit(u"change", Change{key, u"_skip", skip, previous}.to_value());
}
bool ProgramStateCore::is_skipped(const JsString& key) const {
    for (const auto& [name, state] : step_states_) if (name == key) return jsv::strict_equals(own(state.values, u"_skip"), Value(true));
    return false;
}
Value ProgramStateCore::get_structure() const { return structure_; }
Value ProgramStateCore::get_effect_def(const JsString& key) const {
    for (const auto& [name, state] : step_states_) if (name == key) return jsv::truthy(state.effect_def) ? state.effect_def : Value::null();
    return Value::null();
}
Value ProgramStateCore::get_step_keys() const {
    Array keys;
    for (const auto& [key, state] : step_states_) keys.emplace_back(key);
    return Value(std::move(keys));
}
Object ProgramStateCore::get_all_step_values() const {
    Object all;
    for (const auto& [key, state] : step_states_) jsv::set_plain(all, key, Value(state.values));
    return all;
}

void ProgramStateCore::set_write_target(const Value& index, const Value& target) {
    map_set(routing_[0], index, target);
    Object event;
    event.set(u"type", Value(u"routing")); event.set(u"key", Value(u"writeTarget"));
    event.set(u"planIndex", index); event.set(u"value", target);
    emit(u"change", Value(std::move(event)));
}
Value ProgramStateCore::get_write_target(const Value& index) const { return map_get(routing_[0], index); }
void ProgramStateCore::set_write_step_target(const Value& index, const Value& target) {
    map_set(routing_[1], index, target);
    Object event;
    event.set(u"type", Value(u"routing")); event.set(u"key", Value(u"writeStepTarget"));
    event.set(u"stepIndex", index); event.set(u"value", target);
    emit(u"change", Value(std::move(event)));
}
Value ProgramStateCore::get_write_step_target(const Value& index) const { return map_get(routing_[1], index); }
void ProgramStateCore::set_read_source(const Value& index, const Value& source) {
    map_set(routing_[2], index, source);
    Object event;
    event.set(u"type", Value(u"routing")); event.set(u"key", Value(u"readSource"));
    event.set(u"stepIndex", index); event.set(u"value", source);
    emit(u"change", Value(std::move(event)));
}
Value ProgramStateCore::get_read_source(const Value& index) const { return map_get(routing_[2], index); }
void ProgramStateCore::set_read3d_volume(const Value& index, const Value& volume) { map_set(routing_[3], index, volume); }
void ProgramStateCore::set_read3d_geometry(const Value& index, const Value& geometry) { map_set(routing_[4], index, geometry); }
void ProgramStateCore::set_write3d_volume(const Value& index, const Value& volume) { map_set(routing_[5], index, volume); }
void ProgramStateCore::set_write3d_geometry(const Value& index, const Value& geometry) { map_set(routing_[6], index, geometry); }
void ProgramStateCore::set_render_target(const Value& target) {
    render_target_override_ = target;
    Object event;
    event.set(u"type", Value(u"routing")); event.set(u"key", Value(u"renderTarget")); event.set(u"value", target);
    emit(u"change", Value(std::move(event)));
}
void ProgramStateCore::clear_routing_overrides() {
    for (auto& map : routing_) map.entries.clear();
    render_target_override_ = Value::null();
}
Value ProgramStateCore::routing_overrides() const {
    Object out;
    const JsString names[] = {u"writeTargets", u"writeStepTargets", u"readSources", u"read3dVol", u"read3dGeo", u"write3dVol", u"write3dGeo"};
    for (std::size_t i = 0; i < routing_.size(); ++i) out.set(names[i], map_entries(routing_[i].entries));
    out.set(u"renderTarget", render_target_override_);
    return Value(std::move(out));
}
void ProgramStateCore::set_media_input(const Value& index, const Value& metadata) {
    map_set(media_inputs_, index, metadata);
    Object event; event.set(u"stepIndex", index); event.set(u"metadata", metadata);
    emit(u"mediachange", Value(std::move(event)));
}
Value ProgramStateCore::get_media_input(const Value& index) const { return map_get(media_inputs_, index); }
void ProgramStateCore::remove_media_input(const Value& index) {
    map_erase(media_inputs_, index);
    Object event; event.set(u"stepIndex", index); event.set(u"metadata", Value::null());
    emit(u"mediachange", Value(std::move(event)));
}
Value ProgramStateCore::get_all_media_inputs() const { return map_entries(media_inputs_.entries); }
void ProgramStateCore::set_text_input(const Value& index, const Value& metadata) {
    map_set(text_inputs_, index, metadata);
    Object event; event.set(u"stepIndex", index); event.set(u"metadata", metadata);
    emit(u"textchange", Value(std::move(event)));
}
Value ProgramStateCore::get_text_input(const Value& index) const { return map_get(text_inputs_, index); }
void ProgramStateCore::remove_text_input(const Value& index) {
    map_erase(text_inputs_, index);
    Object event; event.set(u"stepIndex", index); event.set(u"metadata", Value::null());
    emit(u"textchange", Value(std::move(event)));
}
Value ProgramStateCore::get_all_text_inputs() const { return map_entries(text_inputs_.entries); }

Value ProgramStateCore::snapshot() const {
    Array steps;
    for (const auto& [key, state] : step_states_) {
        Object item;
        item.set(u"effectKey", Value(state.effect_key));
        Value def = Value::null();
        if (jsv::truthy(state.effect_def)) {
            Object shallow;
            shallow.set(u"func", field(state.effect_def, u"func"));
            shallow.set(u"namespace", field(state.effect_def, u"namespace"));
            def = Value(std::move(shallow));
        }
        item.set(u"def", def);
        item.set(u"stepIndex", Value(state.step_index));
        item.set(u"values", Value(state.values));
        steps.emplace_back(Array{Value(key), Value(std::move(item))});
    }
    Object batch_state;
    batch_state.set(u"depth", Value(static_cast<double>(batch_depth_)));
    Array changes;
    for (const Change& change : batched_changes_) changes.push_back(change.to_value());
    batch_state.set(u"changes", Value(std::move(changes)));
    batch_state.set(u"recompilePending", Value(recompile_pending_));
    Object out;
    out.set(u"steps", Value(std::move(steps)));
    out.set(u"structure", structure_);
    out.set(u"compiled", compiled_);
    out.set(u"routing", routing_overrides());
    out.set(u"media", get_all_media_inputs());
    out.set(u"text", get_all_text_inputs());
    out.set(u"batch", Value(std::move(batch_state)));
    out.set(u"dsl", host_ ? Value(host_->dsl()) : Value::null());
    return Value(std::move(out));
}

Value ProgramStateCore::build_parameter_overrides() const {
    Object overrides;
    for (const auto& [key, state] : step_states_) {
        const double index = step_index(key);
        if (std::isnan(index)) continue;
        const Value info = structure_.is_array() && index >= 0 && index < structure_.as_array().size()
            ? structure_.as_array()[static_cast<std::size_t>(index)] : Value();
        const Value globals = field(state.effect_def, u"globals");
        Object values;
        for (const JsString& param : state.values.keys()) {
            const Value value = own(state.values, param);
            if (!param.empty() && param[0] == u'_' && param != u"_skip") continue;
            const Value spec = field(globals, param);
            const Value control = field(field(spec, u"ui"), u"control");
            if (jsv::strict_equals(control, Value(false)) || jsv::strict_equals(control, Value(u"button"))) continue;
            const Value raw = field(field(info, u"rawKwargs"), param);
            const Value raw_type = field(raw, u"type");
            if (raw_type.is_string() && (raw_type.as_string() == u"Oscillator" || raw_type.as_string() == u"Midi" || raw_type.as_string() == u"Audio")) continue;
            if (var_ref(value)) {
                Object binding;
                binding.set(u"_varRef", field(value, u"_varRef"));
                jsv::set_plain(values, param, Value(std::move(binding)));
            } else jsv::set_plain(values, param, value);
        }
        jsv::set_plain(overrides, number_key(index), Value(std::move(values)));
    }
    return Value(std::move(overrides));
}

void ProgramStateCore::apply_routing_overrides(Value& compiled) const {
    if (!compiled.is_object()) return;
    Value& plans = compiled.as_object()[u"plans"];
    if (!plans.is_array()) return;
    auto make_surface = [](const Value& name) -> Value {
        if (!name.is_string()) {
            if (nullish(name)) throw jsv::cannot_read(name, u"startsWith");
            throw jsv::not_a_function(u"target.startsWith");
        }
        Object ref;
        ref.set(u"kind", Value(!name.as_string().empty() && name.as_string()[0] == u'o' ? u"output" : u"feedback"));
        ref.set(u"name", name);
        return Value(std::move(ref));
    };
    auto make_kind = [](const JsString& kind, const Value& name) -> Value {
        Object ref; ref.set(u"kind", Value(kind)); ref.set(u"name", name); return Value(std::move(ref));
    };
    for (const auto& [index, target] : routing_[0].entries) {
        if (index.is_string() && index.as_string() == u"length")
            throw JsError(u"TypeError", u"Cannot create property 'write' on number '" + number_key(static_cast<double>(plans.as_array().size())) + u"'");
        const double number = jsv::to_number(index);
        if (!std::isfinite(number) || number < 0 || std::floor(number) != number || number >= plans.as_array().size()) continue;
        Value& plan = plans.as_array()[static_cast<std::size_t>(number)];
        if (!jsv::truthy(plan) || !plan.is_object()) continue;
        const Value surface = make_surface(target);
        plan.as_object().set(u"write", surface);
        Value& chain = plan.as_object()[u"chain"];
        if (chain.is_array() && !chain.as_array().empty()) {
            Value& last = chain.as_array().back();
            if (last.is_object() && jsv::truthy(field(last, u"builtin")) && jsv::strict_equals(field(last, u"op"), Value(u"_write"))) {
                Value& args = last.as_object()[u"args"];
                if (args.is_object()) args.as_object().set(u"tex", surface);
            }
        }
    }
    std::size_t global_index = 0;
    for (Value& plan : plans.as_array()) {
        if (!plan.is_object()) continue;
        Value& chain = plan.as_object()[u"chain"];
        if (!chain.is_array()) continue;
        for (Value& step : chain.as_array()) {
            const Value index(static_cast<double>(global_index++));
            if (!step.is_object() || !jsv::truthy(field(step, u"builtin"))) continue;
            const Value op = field(step, u"op");
            Value& args = step.as_object()[u"args"];
            if (!args.is_object()) continue;
            if (jsv::strict_equals(op, Value(u"_write"))) {
                const Value target = map_get(routing_[1], index);
                if (!target.is_undefined()) args.as_object().set(u"tex", make_surface(target));
            } else if (jsv::strict_equals(op, Value(u"_read"))) {
                const Value source = map_get(routing_[2], index);
                if (!source.is_undefined()) args.as_object().set(u"tex", make_surface(source));
            } else if (jsv::strict_equals(op, Value(u"_read3d")) || jsv::strict_equals(op, Value(u"_write3d"))) {
                const std::size_t offset = jsv::strict_equals(op, Value(u"_read3d")) ? 3 : 5;
                const Value vol = map_get(routing_[offset], index);
                const Value geo = map_get(routing_[offset + 1], index);
                if (!vol.is_undefined()) args.as_object().set(u"tex3d", make_kind(u"vol", vol));
                if (!geo.is_undefined()) args.as_object().set(u"geo", make_kind(u"geo", geo));
            }
        }
    }
    if (jsv::truthy(render_target_override_)) {
        Value& render = compiled.as_object()[u"render"];
        if (render.is_string()) render = render_target_override_;
        else if (render.is_object()) render.as_object().set(u"target", render_target_override_);
    }
}

JsString ProgramStateCore::to_dsl() const {
    const JsString current = host_ ? host_->dsl() : JsString();
    if (current.empty()) return {};
    try {
        Value compiled = compile_state_dsl(current, *registry_);
        if (!jsv::truthy(field(compiled, u"plans"))) return current;
        const Value overrides = build_parameter_overrides();
        apply_routing_overrides(compiled);
        UnparseOptions options;
        options.enums = host_ && jsv::truthy(host_->enum_tree()) ? host_->enum_tree() : Value(Object{});
        const Value enums = options.enums;
        options.custom_formatter = [enums](const Value& value, const Value& spec) {
            UnparseOptions format_options;
            format_options.enums = enums;
            return format_value(value, spec, format_options, Value());
        };
        EffectRegistry* registry = registry_;
        options.get_effect_def = [registry](const Value& name, const Value& ns) {
            if (!name.is_string()) {
                if (nullish(name)) throw jsv::cannot_read(name, u"includes");
                throw jsv::not_a_function(u"effectName.includes");
            }
            const JsString& effect = name.as_string();
            Value def = registry_effect(*registry, effect);
            if (jsv::truthy(def)) return def;
            const std::size_t dot = effect.find(u'.');
            if (dot != JsString::npos) {
                def = registry_effect(*registry, effect.substr(0, dot) + u"/" + effect.substr(dot + 1));
                if (jsv::truthy(def)) return def;
            }
            if (jsv::truthy(ns)) {
                const JsString prefix = jsv::to_string(ns);
                def = registry_effect(*registry, prefix + u"/" + effect);
                if (jsv::truthy(def)) return def;
                def = registry_effect(*registry, prefix + u"." + effect);
                if (jsv::truthy(def)) return def;
            }
            return Value::null();
        };
        return unparse(compiled, overrides, options, *registry_);
    } catch (const DslSyntaxError& error) {
        Object tagged;
        tagged.set(u"$js", Value(u"error")); tagged.set(u"name", Value(u"SyntaxError")); tagged.set(u"message", Value(error.message()));
        program_console_warn({Value(u"[ProgramState] Failed to generate DSL:"), Value(std::move(tagged))});
        return current;
    } catch (const JsError& error) {
        program_console_warn({Value(u"[ProgramState] Failed to generate DSL:"), tagged_error(error)});
        return current;
    } catch (const std::exception& error) {
        const JsString message = utf8_to_js(error.what());
        const JsString name = message.substr(0, 23) == u"Cannot read properties " ? u"TypeError" : u"Error";
        program_console_warn({Value(u"[ProgramState] Failed to generate DSL:"), tagged_error(JsError(name, message))});
        return current;
    }
}


void ProgramStateCore::apply_to_pipeline() {
    if (!host_) return;
    std::vector<Value>* passes = host_->graph_passes();
    if (!passes) return;
    const MockMethods* methods = host_->method_presence();
    bool scoped_changed = false;
    for (const auto& [key, state] : step_states_) {
        const double index = step_index(key);
        if (std::isnan(index)) continue;
        std::vector<std::size_t> matching;
        for (std::size_t i = 0; i < passes->size(); ++i) {
            const Value& pass = (*passes)[i];
            if (nullish(pass)) throw jsv::cannot_read(pass, u"id");
            const Value id = field(pass, u"id");
            if (!jsv::truthy(id)) continue;
            if (!id.is_string()) throw jsv::not_a_function(u"pass.id.match");
            const JsString& name = id.as_string();
            if (name.substr(0, 5) != u"node_") continue;
            const std::size_t pos = name.find(u'_', 5);
            if (pos == JsString::npos || pos == 5) continue;
            const JsString digits = name.substr(5, pos - 5);
            if (!std::all_of(digits.begin(), digits.end(), [](char16_t c) { return c >= u'0' && c <= u'9'; })) continue;
            if (std::strtod(js_to_utf8(digits).c_str(), nullptr) == index) matching.push_back(i);
        }
        if (matching.empty()) continue;
        const Value globals = field(state.effect_def, u"globals");
        for (const std::size_t pass_index : matching) {
            Value& pass = (*passes)[pass_index];
            if (!jsv::truthy(field(pass, u"uniforms"))) continue;
            Value palette = Value::null();
            for (const JsString& param : state.values.keys()) {
                const Value value = own(state.values, param);
                if (nullish(value) || (!param.empty() && param[0] == u'_') || var_ref(value) || automation(value)) continue;
                const Value spec = field(globals, param);
                const Value configured_name = field(spec, u"uniform");
                const Value uniform_name = jsv::truthy(configured_name) ? configured_name : Value(param);
                const Value converted = host_->has_converter() ? host_->convert_parameter_for_uniform(value, spec) : value;
                write_uniform_aliases(pass, param, uniform_name, converted);
                const JsString uniform_key = jsv::to_property_key(uniform_name);
                const Value uniforms = field(pass, u"uniforms");
                if (jsv::in_operator(Value(uniform_key), uniforms)) {
                    if (!(jsv::strict_equals(uniform_name, Value(u"volumeSize")) && jsv::truthy(field(pass, u"inheritsVolumeSize")))) {
                        jsv::set_plain(pass.as_object()[u"uniforms"].as_object(), uniform_key, converted);
                        const Value scoped = field(field(pass, u"scopedParams"), uniform_key);
                        if (jsv::truthy(scoped)) {
                            const JsString scoped_key = jsv::to_property_key(scoped);
                            const Value current = field(field(pass, u"uniforms"), uniform_key);
                            jsv::set_plain(pass.as_object()[u"uniforms"].as_object(), scoped_key, current);
                            scoped_changed = true;
                            if (!methods || !methods->broadcast_chain_scoped_param) throw jsv::not_a_function(u"pipeline.broadcastChainScopedParam");
                            host_->broadcast_chain_scoped_param(pass_index, uniform_key, scoped_key);
                        }
                    }
                }
                if (jsv::strict_equals(field(spec, u"type"), Value(u"palette"))) palette = expand_palette_value(converted);
            }
            if (palette.is_object()) for (const auto& [name, value] : jsv::entries(palette)) {
                const Value uniforms = field(pass, u"uniforms");
                if (jsv::in_operator(Value(name), uniforms)) jsv::set_plain(pass.as_object()[u"uniforms"].as_object(), name, value);
            }
        }
        if (methods && methods->check_async_regen) {
            const Value& first = (*passes)[matching[0]];
            const Value node_id = field(first, u"nodeId");
            const Value effect_key = field(first, u"effectKey");
            if (jsv::truthy(node_id) && jsv::truthy(effect_key)) host_->check_async_regen(node_id, effect_key, state.values);
        }
    }
    if (scoped_changed && methods && methods->recreate_textures) {
        if (!methods->collect_default_uniforms) throw jsv::not_a_function(u"pipeline.collectDefaultUniforms");
        const Object uniforms = host_->collect_default_uniforms();
        host_->recreate_textures(uniforms);
    }
    if (methods && methods->set_uniform) for (const auto& [key, state] : step_states_) {
        const double index = step_index(key);
        if (std::isnan(index)) continue;
        if (state.values.has(u"stateSize")) host_->set_uniform(u"stateSize_node_" + key.substr(5), own(state.values, u"stateSize"));
    }
}

}  // namespace nm

namespace nm {
namespace {
Value edit_result(bool success, const JsString& error = {}) {
    Object out; out.set(u"success", Value(success));
    if (!success) out.set(u"error", Value(error));
    return Value(std::move(out));
}
void preserve_search_directive(Value& compiled, const JsString& current) {
    if (!compiled.is_object()) return;
    std::size_t cursor = 0;
    while (cursor <= current.size()) {
        const std::size_t end = current.find_first_of(u"\r\n", cursor);
        const JsString line = current.substr(cursor, end == JsString::npos ? JsString::npos : end - cursor);
        if (line.substr(0, 6) == u"search" && line.size() > 6) {
            std::size_t content = 6;
            while (content < line.size() && (line[content] == u' ' || line[content] == u'\t')) ++content;
            if (content == 6 || content >= line.size()) goto next_line;
            const JsString capture = line.substr(content);
            Array namespaces;
            std::size_t last = 0;
            for (std::size_t i = 0; i < capture.size();) {
                std::size_t comma = i;
                while (comma < capture.size() && (capture[comma] == u' ' || capture[comma] == u'\t')) ++comma;
                if (comma < capture.size() && capture[comma] == u',') {
                    namespaces.emplace_back(capture.substr(last, i - last));
                    std::size_t next = comma + 1;
                    while (next < capture.size() && (capture[next] == u' ' || capture[next] == u'\t')) ++next;
                    last = next;
                    i = next;
                } else ++i;
            }
            namespaces.emplace_back(capture.substr(last));
            compiled.as_object().set(u"searchNamespaces", Value(std::move(namespaces)));
            return;
        }
next_line:
        if (end == JsString::npos) return;
        cursor = end + 1;
        if (current[end] == u'\r' && cursor < current.size() && current[cursor] == u'\n') ++cursor;
    }
}
UnparseOptions edit_options(EffectRegistry& registry) {
    UnparseOptions options;
    options.get_effect_def = [&registry](const Value& name, const Value& ns) {
        Value def = name.is_string() ? registry_effect(registry, name.as_string()) : Value();
        if (jsv::truthy(def)) return def;
        if (jsv::truthy(ns)) {
            const JsString prefix = jsv::to_string(ns);
            const JsString effect = jsv::to_string(name);
            def = registry_effect(registry, prefix + u"/" + effect);
            if (jsv::truthy(def)) return def;
            def = registry_effect(registry, prefix + u"." + effect);
            if (jsv::truthy(def)) return def;
        }
        return Value();
    };
    return options;
}
Value surface_name(const Value& write) {
    return write.is_object() || write.is_array() ? field(write, u"name") : write;
}
}

Value ProgramStateCore::delete_step(double index) {
    const JsString current = host_ ? host_->dsl() : JsString();
    if (current.empty()) return edit_result(false, u"no DSL available");
    Value compiled;
    try { compiled = compile_state_dsl(current, *registry_); }
    catch (const DslSyntaxError& error) { return edit_result(false, u"DSL syntax error: " + error.message()); }
    catch (const JsError& error) { return edit_result(false, u"DSL syntax error: " + error.message()); }
    if (!jsv::truthy(field(compiled, u"plans"))) return edit_result(false, u"compilation failed");
    preserve_search_directive(compiled, current);
    Value& plans_value = compiled.as_object()[u"plans"];
    if (!plans_value.is_array()) return edit_result(false, u"step not found");
    Array& plans = plans_value.as_array();
    double global = 0;
    bool found = false;
    Value deleted_surface = Value::null();
    for (std::size_t p = 0; p < plans.size() && !found; ++p) {
        if (!plans[p].is_object()) continue;
        Value& chain_value = plans[p].as_object()[u"chain"];
        if (!chain_value.is_array()) continue;
        Array& chain = chain_value.as_array();
        for (std::size_t s = 0; s < chain.size(); ++s, ++global) {
            if (global != index) continue;
            const Value deleted = chain[s];
            bool remove_plan = false;
            if (s == 0 && jsv::truthy(deleted) && !jsv::truthy(field(deleted, u"builtin"))) {
                const Value op = field(deleted, u"op");
                const Value ns = field(deleted, u"namespace");
                Value namespace_value = field(ns, u"namespace");
                if (!jsv::truthy(namespace_value)) namespace_value = field(ns, u"resolved");
                Value def = op.is_string() ? registry_effect(*registry_, op.as_string()) : Value();
                if (!jsv::truthy(def) && jsv::truthy(namespace_value)) def = registry_effect(*registry_, jsv::to_string(namespace_value) + u"/" + jsv::to_string(op));
                if (jsv::truthy(def)) remove_plan = registry_->isStarterOp(jsv::to_string(op)) ||
                    (jsv::truthy(namespace_value) && registry_->isStarterOp(jsv::to_string(namespace_value) + u"." + jsv::to_string(op)));
            }
            if (!remove_plan) {
                chain.erase(chain.begin() + static_cast<std::ptrdiff_t>(s));
                remove_plan = chain.empty();
                if (!remove_plan) remove_plan = std::none_of(chain.begin(), chain.end(), [](const Value& step) {
                    return !(jsv::truthy(field(step, u"builtin")) && jsv::strict_equals(field(step, u"op"), Value(u"_write")));
                });
            }
            if (remove_plan) {
                const Value write = field(plans[p], u"write");
                if (jsv::truthy(write)) deleted_surface = surface_name(write);
                plans.erase(plans.begin() + static_cast<std::ptrdiff_t>(p));
            }
            found = true;
            break;
        }
    }
    if (!found) return edit_result(false, u"step not found");
    const JsString regenerated = unparse(compiled, Value(Object{}), edit_options(*registry_), *registry_);
    from_dsl(regenerated);
    Object out; out.set(u"success", Value(true)); out.set(u"newDsl", Value(regenerated)); out.set(u"deletedSurfaceName", deleted_surface);
    return Value(std::move(out));
}

Value ProgramStateCore::insert_step(double after, const JsString& effect_id, const Value&) {
    const JsString current = host_ ? host_->dsl() : JsString();
    if (current.empty()) return edit_result(false, u"no DSL available");
    Value compiled;
    try { compiled = compile_state_dsl(current, *registry_); }
    catch (const DslSyntaxError& error) { return edit_result(false, u"DSL syntax error: " + error.message()); }
    catch (const JsError& error) { return edit_result(false, u"DSL syntax error: " + error.message()); }
    if (!jsv::truthy(field(compiled, u"plans"))) return edit_result(false, u"compilation failed");
    preserve_search_directive(compiled, current);
    const std::size_t slash = effect_id.find(u'/');
    const JsString ns = slash == JsString::npos ? JsString() : effect_id.substr(0, slash);
    const JsString effect_name = slash == JsString::npos ? effect_id : effect_id.substr(slash + 1);
    Value def = registry_effect(*registry_, effect_id);
    if (!jsv::truthy(def)) def = registry_effect(*registry_, effect_name);
    if (!jsv::truthy(def) && !ns.empty()) def = registry_effect(*registry_, ns + u"." + effect_name);
    if (jsv::truthy(def) && (registry_->isStarterOp(effect_id) || registry_->isStarterOp(ns.empty() ? effect_name : ns + u"." + effect_name)))
        return edit_result(false, u"Cannot insert starter effect '" + effect_id + u"' mid-chain");
    if (std::isnan(after)) return edit_result(false, u"Target chain not found");
    if (!ns.empty()) {
        Value& search = compiled.as_object()[u"searchNamespaces"];
        if (!search.is_array()) search = Value(Array{});
        const bool present = std::any_of(search.as_array().begin(), search.as_array().end(), [&](const Value& value) { return jsv::same_value_zero(value, Value(ns)); });
        if (!present) search.as_array().emplace_back(ns);
    }
    Value& plans_value = compiled.as_object()[u"plans"];
    if (!plans_value.is_array()) return edit_result(false, u"Target chain not found");
    Array& plans = plans_value.as_array();
    double global = 0;
    std::size_t target_plan = 0;
    std::size_t target_step = 0;
    bool found = after < 0;
    if (!found) for (std::size_t p = 0; p < plans.size() && !found; ++p) {
        const Value chain = field(plans[p], u"chain");
        if (!chain.is_array()) continue;
        for (std::size_t s = 0; s < chain.as_array().size(); ++s, ++global) if (global == after) {
            target_plan = p; target_step = s + 1; found = true; break;
        }
    }
    if (!found) return edit_result(false, u"Step index " + number_key(after) + u" not found");
    if (target_plan >= plans.size() || !plans[target_plan].is_object() || !field(plans[target_plan], u"chain").is_array())
        return edit_result(false, u"Target chain not found");
    double max_temp = 0;
    for (const Value& plan : plans) {
        const Value chain = field(plan, u"chain");
        if (!chain.is_array()) continue;
        for (const Value& step : chain.as_array()) {
            const Value temp = field(step, u"temp");
            if (temp.is_number() && temp.as_number() > max_temp) max_temp = temp.as_number();
        }
    }
    Object step;
    step.set(u"op", Value(effect_name)); step.set(u"args", Value(Object{})); step.set(u"temp", Value(max_temp + 1));
    if (!ns.empty()) { Object namespace_value; namespace_value.set(u"namespace", Value(ns)); step.set(u"namespace", Value(std::move(namespace_value))); }
    Array& target_chain = plans[target_plan].as_object()[u"chain"].as_array();
    if (target_step > 0 && target_step <= target_chain.size()) {
        const Value& previous = target_chain[target_step - 1];
        if (jsv::truthy(field(previous, u"builtin")) && jsv::strict_equals(field(previous, u"op"), Value(u"_write"))) --target_step;
    }
    target_step = std::min(target_step, target_chain.size());
    target_chain.insert(target_chain.begin() + static_cast<std::ptrdiff_t>(target_step), Value(std::move(step)));
    const JsString regenerated = unparse(compiled, Value(Object{}), edit_options(*registry_), *registry_);
    from_dsl(regenerated);
    double new_index = target_step;
    for (std::size_t p = 0; p < target_plan; ++p) {
        const Value chain = field(plans[p], u"chain");
        if (chain.is_array()) new_index += chain.as_array().size();
    }
    Object out; out.set(u"success", Value(true)); out.set(u"newDsl", Value(regenerated)); out.set(u"newStepIndex", Value(new_index));
    return Value(std::move(out));
}

}  // namespace nm


namespace nm {

Value ProgramStateCore::serialize() const {
    Object steps;
    for (const auto& [key, state] : step_states_) {
        Object saved;
        saved.set(u"effectKey", Value(state.effect_key));
        saved.set(u"values", Value(state.values));
        saved.set(u"_skip", own(state.values, u"_skip"));
        jsv::set_plain(steps, key, Value(std::move(saved)));
    }
    Object overrides;
    const JsString names[] = {u"writeTargets", u"writeStepTargets", u"readSources", u"read3dVol", u"read3dGeo", u"write3dVol", u"write3dGeo"};
    for (std::size_t i = 0; i < routing_.size(); ++i) overrides.set(names[i], map_object(routing_[i].entries));
    overrides.set(u"renderTarget", render_target_override_);
    Object out;
    out.set(u"version", Value(1));
    out.set(u"dsl", Value(host_ ? host_->dsl() : JsString()));
    out.set(u"stepStates", Value(std::move(steps)));
    out.set(u"overrides", Value(std::move(overrides)));
    out.set(u"mediaInputs", map_object(media_inputs_.entries));
    out.set(u"textInputs", map_object(text_inputs_.entries));
    return Value(std::move(out));
}

void ProgramStateCore::deserialize(const Value& data) {
    const Value version = jsv::get(data, u"version");
    if (!jsv::strict_equals(version, Value(1))) program_console_warn({Value(u"[ProgramState] Unknown serialization version:"), version});
    const Value overrides = field(data, u"overrides");
    const JsString names[] = {u"writeTargets", u"writeStepTargets", u"readSources", u"read3dVol", u"read3dGeo", u"write3dVol", u"write3dGeo"};
    for (std::size_t i = 0; i < routing_.size(); ++i) {
        routing_[i].entries.clear();
        const Value source = field(overrides, names[i]);
        if (jsv::truthy(source)) for (const auto& [key, value] : jsv::entries(source)) map_set(routing_[i], Value(key), value);
    }
    render_target_override_ = jsv::truthy(field(overrides, u"renderTarget")) ? field(overrides, u"renderTarget") : Value::null();
    media_inputs_.entries.clear();
    text_inputs_.entries.clear();
    const Value media = field(data, u"mediaInputs");
    const Value text = field(data, u"textInputs");
    if (jsv::truthy(media)) for (const auto& [key, value] : jsv::entries(media)) map_set(media_inputs_, Value(key), value);
    if (jsv::truthy(text)) for (const auto& [key, value] : jsv::entries(text)) map_set(text_inputs_, Value(key), value);
    const Value dsl = field(data, u"dsl");
    if (jsv::truthy(dsl)) from_dsl_value(dsl);
    const Value saved = field(data, u"stepStates");
    if (jsv::truthy(saved)) for (const auto& [key, saved_state] : jsv::entries(saved)) {
        auto it = std::find_if(step_states_.begin(), step_states_.end(), [&](const auto& item) { return item.first == key; });
        if (it == step_states_.end()) continue;
        const Value values = jsv::get(saved_state, u"values");
        jsv::spread_into(it->second.values, values);
    }
    apply_to_pipeline();
    Object event;
    event.set(u"structure", structure_);
    emit(u"load", Value(std::move(event)));
}

}  // namespace nm
