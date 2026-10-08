#include "core/edit/program_state/console.h"
#include "core/edit/program_state/program_state.h"
#include "core/graph/dsl_compiler.h"
#include "core/lang/diagnostics.h"
#include "core/value/json.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace nm_dump {
namespace {
using nm::Array;
using nm::JsString;
using nm::Object;
using nm::Value;
using State = nm::ProgramState<nm::MockHost>;

Value field(const Value& object, const char16_t* key) {
    if (!object.is_object()) return {};
    const Value* value = object.as_object().find(key);
    return value ? *value : Value();
}
Value object(std::initializer_list<std::pair<const char16_t*, Value>> values) {
    Object out;
    for (const auto& [key, value] : values) out.set(key, value);
    return Value(out);
}
Value tag(const char16_t* name) { return object({{u"$js", Value(name)}}); }
Value encode(const Value& value) {
    if (value.is_undefined()) return tag(u"undefined");
    if (value.is_function()) return tag(u"function");
    if (value.is_number()) {
        const double n = value.as_number();
        if (std::isnan(n)) return tag(u"NaN");
        if (std::isinf(n)) return tag(n < 0 ? u"-Infinity" : u"Infinity");
        if (n == 0 && std::signbit(n)) return tag(u"-0");
    }
    if (value.is_array()) {
        Array out;
        for (const auto& item : value.as_array()) out.push_back(encode(item));
        return Value(std::move(out));
    }
    if (value.is_object()) {
        const Value* kind = value.as_object().find(u"$js");
        if (kind && kind->is_string() && kind->as_string() == u"error" &&
            value.as_object().has(u"name") && value.as_object().has(u"message")) return value;
        Object out;
        for (const auto& key : value.as_object().keys())
            out.set(key, encode(*value.as_object().find(key)));
        if (out.has(u"$js")) return object({{u"$js", Value(u"object")}, {u"members", Value(out)}});
        return Value(out);
    }
    return value;
}
Value decode_members(const Value& value);
Value decode(const Value& value) {
    if (value.is_array()) {
        Array out;
        for (const auto& item : value.as_array()) out.push_back(decode(item));
        return Value(std::move(out));
    }
    if (!value.is_object()) return value;
    const Value kind = field(value, u"$js");
    if (kind.is_string()) {
        const JsString& name = kind.as_string();
        if (name == u"undefined") return {};
        if (name == u"NaN") return Value(std::numeric_limits<double>::quiet_NaN());
        if (name == u"Infinity") return Value(std::numeric_limits<double>::infinity());
        if (name == u"-Infinity") return Value(-std::numeric_limits<double>::infinity());
        if (name == u"-0") return Value(-0.0);
        if (name == u"function") return Value::function(u"function () {}");
        if (name == u"object") return decode_members(field(value, u"members"));
        if (name == u"error") return object({{u"name", field(value, u"name")}, {u"message", field(value, u"message")}});
    }
    return decode_members(value);
}
Value decode_members(const Value& value) {
    if (!value.is_object()) return Value(Object{});
    Object out;
    for (const auto& key : value.as_object().keys()) out.set(key, decode(*value.as_object().find(key)));
    return Value(out);
}
JsString string_field(const Value& value, const char16_t* key) {
    const Value item = field(value, key);
    return item.is_string() ? item.as_string() : JsString();
}
std::string json(const Value& value) { return nm::json::stringify(value); }
void merge(Object& target, const Value& source) {
    if (!source.is_object()) return;
    for (const auto& key : source.as_object().keys()) {
        if (key == u"__proto__" || key == u"constructor" || key == u"prototype") continue;
        const Value item = *source.as_object().find(key);
        const Value* existing = target.find(key);
        if (item.is_object() && existing && existing->is_object() && !item.as_object().has(u"type")) {
            Object child = existing->as_object();
            merge(child, item);
            target.set(key, Value(child));
        } else target.set(key, item);
    }
}
Value error_record(const nm::JsError& error) {
    return object({{u"name", Value(error.name())}, {u"message", Value(error.message())}});
}
Value error_record(const nm::DslSyntaxError& error) {
    return object({{u"name", Value(u"SyntaxError")}, {u"message", Value(error.message())}});
}
Value error_record(const nm::CompilationError& error) {
    Object thrown;
    thrown.set(u"code", Value(error.code()));
    if (error.code() == u"ERR_COMPILATION_FAILED")
        thrown.set(u"diagnostics", error.diagnostics().raw());
    else if (error.code() == u"ERR_EXPANSION_FAILED")
        thrown.set(u"errors", error.errors().raw());
    return object({{u"thrown", encode(Value(std::move(thrown)))}});
}
Value error_record(const std::exception& error) {
    const std::string message = error.what();
    const bool type_error = message.rfind("Cannot read properties of ", 0) == 0 ||
        message == "plan.chain is not iterable";
    return object({{u"name", Value(type_error ? u"TypeError" : u"Error")},
        {u"message", Value(nm::utf8_to_js(message))}});
}

struct Scenario {
    nm::EffectRegistry& registry;
    JsString name;
    std::unique_ptr<State> state;
    std::optional<nm::MockHost> detached;
    nm::MockMethods methods = nm::MockMethods::all();
    std::vector<std::string> pass_json;
    std::unordered_map<std::string, std::string> last;
    std::unordered_map<JsString, State::Listener> listeners;
    struct Registration {
        JsString event;
        State::ListenerId handle;
        std::size_t generation;
        bool once;
    };
    std::unordered_map<JsString, std::vector<Registration>> listener_ids;
    std::unordered_map<JsString, std::size_t> listener_generations;
    std::size_t next_listener_generation = 1;
    Array events;
    Array logs;
    std::size_t index = 0;

    Scenario(nm::EffectRegistry& registry, JsString name, const Value& host)
        : registry(registry), name(std::move(name)) {
        nm::set_program_state_console([this](const JsString& level, const std::vector<Value>& args) {
            Array encoded;
            for (const auto& argument : args) encoded.push_back(encode(argument));
            logs.push_back(object({{u"level", Value(level)}, {u"args", Value(encoded)}}));
        });
        new_state(host);
    }

    Value same(const std::string& key, const Value& encoded) {
        const std::string current = json(encoded);
        auto [it, inserted] = last.try_emplace(key, current);
        if (!inserted && it->second == current) return object({{u"$same", Value(true)}});
        it->second = current;
        return encoded;
    }
    nm::MockHost& mock() {
        if (state->renderer_mut()) return *state->renderer_mut();
        if (!detached) throw std::runtime_error("mock renderer missing");
        return *detached;
    }
    const nm::MockHost& mock_ref() const {
        if (state->renderer()) return *state->renderer();
        if (!detached) throw std::runtime_error("mock renderer missing");
        return *detached;
    }
    void new_state(const Value& host) {
        const Value renderer = field(host, u"renderer");
        const bool attach = renderer.is_undefined() || nm::jsv::truthy(renderer);
        const JsString enums_name = string_field(host, u"enums");
        const JsString convert_name = string_field(host, u"convert");
        const Value method_names = field(host, u"methods");
        std::vector<JsString> names;
        if (method_names.is_array()) {
            for (const auto& item : method_names.as_array()) if (item.is_string()) names.push_back(item.as_string());
            methods = nm::MockMethods::from_names(names);
        } else methods = nm::MockMethods::all();
        nm::MockHost host_value;
        if (enums_name == u"none") host_value.enums = Value();
        else if (enums_name == u"empty") host_value.enums = Value(Object{});
        else if (enums_name == u"std") host_value.enums = registry.enums().std().raw();
        else {
            Object combined = registry.enums().std().raw().as_object();
            merge(combined, registry.enums().project().raw());
            host_value.enums = Value(combined);
        }
        host_value.convert = convert_name == u"passthrough" ? nm::MockConvert::Passthrough
            : convert_name == u"absent" ? nm::MockConvert::Absent : nm::MockConvert::Canvas;
        state = std::make_unique<State>(registry);
        if (attach) { state->set_renderer(std::move(host_value)); detached.reset(); }
        else { detached = std::move(host_value); state->set_renderer(std::nullopt); }
        pass_json.clear();
        events.clear();
        listeners.clear();
        listener_ids.clear();
        listener_generations.clear();
        for (const char16_t* event : {u"change", u"stepchange", u"structurechange", u"reset",
                                      u"load", u"recompileNeeded", u"mediachange", u"textchange"}) {
            const JsString label(event);
            state->on(label, [this, label](nm::ProgramStateCore&, const Value& data) {
                const std::string key = "ev." + nm::js_to_utf8(label);
                events.push_back(object({{u"l", Value(u"*")}, {u"e", Value(label)},
                                         {u"d", same(key, encode(data))}}));
            });
        }
    }

    Value host_load(const JsString& dsl, bool set_dsl) {
        nm::MockHost& host = mock();
        if (set_dsl) host.current_dsl = dsl;
        try {
            Value graph(nm::compileGraphJson(dsl, registry));
            const Value passes = field(graph, u"passes");
            nm::MockPipeline pipeline;
            pipeline.graph = std::move(graph);
            pipeline.methods = methods;
            host.pipeline = std::move(pipeline);
            pass_json.clear();
            Array summaries;
            if (passes.is_array()) {
                for (const auto& pass : passes.as_array()) {
                    pass_json.push_back(json(encode(field(pass, u"uniforms"))));
                    summaries.push_back(object({{u"id", field(pass, u"id")},
                        {u"nodeId", field(pass, u"nodeId")}, {u"effectKey", field(pass, u"effectKey")},
                        {u"stepIndex", field(pass, u"stepIndex")}, {u"uniforms", field(pass, u"uniforms")},
                        {u"scopedParams", field(pass, u"scopedParams")},
                        {u"inheritsVolumeSize", field(pass, u"inheritsVolumeSize")},
                        {u"uniformAliases", field(pass, u"uniformAliases")}}));
                }
            }
            return object({{u"passes", Value(std::move(summaries))}});
        } catch (const nm::JsError& error) {
            host.pipeline.reset();
            pass_json.clear();
            return object({{u"error", error_record(error)}});
        } catch (const nm::DslSyntaxError& error) {
            host.pipeline.reset();
            pass_json.clear();
            return object({{u"error", error_record(error)}});
        } catch (const nm::CompilationError& error) {
            host.pipeline.reset();
            pass_json.clear();
            return object({{u"error", error_record(error)}});
        } catch (const std::exception& error) {
            host.pipeline.reset();
            pass_json.clear();
            return object({{u"error", error_record(error)}});
        }
    }

    double number_arg(const Value& op, const char16_t* key) const {
        const Value number = decode(field(op, key));
        if (!number.is_number()) throw std::runtime_error(nm::js_to_utf8(JsString(key)) + " must be a number");
        return number.as_number();
    }
    nm::JsError make_error(const Value& spec) const {
        const Value name = field(spec, u"name");
        const Value message = field(spec, u"message");
        return nm::JsError(name.is_string() ? name.as_string() : JsString(u"Error"),
                           message.is_string() ? message.as_string() : JsString());
    }
    void on_listener(const JsString& event, const JsString& id, bool once = false) {
        auto it = listeners.find(id);
        if (it == listeners.end()) throw std::runtime_error("undefined listener " + nm::js_to_utf8(id));
        const std::size_t generation = listener_generations[id];
        auto& registrations = listener_ids[id];
        if (!once && std::any_of(registrations.begin(), registrations.end(), [&](const Registration& item) {
            return !item.once && item.event == event && item.generation == generation;
        })) return;
        const auto listener_id = once ? state->once(event, it->second) : state->on(event, it->second);
        registrations.push_back({event, listener_id, generation, once});
    }
    void off_listener(const JsString& event, const JsString& id) {
        auto it = listener_ids.find(id);
        if (it == listener_ids.end()) return;
        const std::size_t generation = listener_generations[id];
        for (const auto& entry : it->second)
            if (entry.event == event && entry.generation == generation && !entry.once)
                state->off(event, entry.handle);
        auto& entries = it->second;
        entries.erase(std::remove_if(entries.begin(), entries.end(),
            [&](const Registration& entry) {
                return entry.event == event && entry.generation == generation && !entry.once;
            }), entries.end());
    }
    void remove_all_listeners(const std::optional<JsString>& event) {
        state->remove_all_listeners(event);
        for (auto& [id, entries] : listener_ids) {
            entries.erase(std::remove_if(entries.begin(), entries.end(), [&](const Registration& entry) {
                return !event || event->empty() || entry.event == *event;
            }), entries.end());
        }
    }
    Value run_state_op(const Value& op) {
        const JsString name = string_field(op, u"op");
        const auto s = [&](const char16_t* key) { return string_field(op, key); };
        const auto a = [&](const char16_t* key) { return decode(field(op, key)); };
        if (name == u"fromDsl") { state->from_dsl(s(u"dsl")); return {}; }
        if (name == u"toDsl") return Value(state->to_dsl());
        if (name == u"wouldChangeStructure") return Value(state->would_change_structure(s(u"dsl")));
        if (name == u"getValue") return state->get_value(s(u"stepKey"), s(u"paramName"));
        if (name == u"setValue") { state->set_value(s(u"stepKey"), s(u"paramName"), a(u"value")); return {}; }
        if (name == u"getStepValues") return Value(state->get_step_values(s(u"stepKey")));
        if (name == u"setStepValues") { state->set_step_values(s(u"stepKey"), a(u"values")); return {}; }
        if (name == u"batch") {
            Array results;
            const Value nested = field(op, u"ops");
            state->batch([&](nm::ProgramStateCore&) {
                if (nested.is_array()) for (const auto& action : nested.as_array()) results.push_back(run_state_op(action));
                const Value thrown = field(op, u"throw");
                if (nm::jsv::truthy(thrown)) throw make_error(thrown);
            });
            return Value(results);
        }
        if (name == u"resetStep") { state->reset_step(s(u"stepKey")); return {}; }
        if (name == u"setSkip") { state->set_skip(s(u"stepKey"), a(u"skip")); return {}; }
        if (name == u"isSkipped") return Value(state->is_skipped(s(u"stepKey")));
        if (name == u"deleteStep") return state->delete_step(number_arg(op, u"stepIndex"));
        if (name == u"insertStep") return state->insert_step(number_arg(op, u"afterStepIndex"), s(u"effectId"));
        if (name == u"getStructure") return state->get_structure();
        if (name == u"getCompiled") return state->get_compiled();
        if (name == u"getEffectDef") {
            const Value def = state->get_effect_def(s(u"stepKey"));
            if (!nm::jsv::truthy(def)) return Value::null();
            return object({{u"func", field(def, u"func")}, {u"namespace", field(def, u"namespace")}});
        }
        if (name == u"stepCount") return Value(static_cast<double>(state->step_count()));
        if (name == u"getStepKeys") return state->get_step_keys();
        if (name == u"getAllStepValues") return Value(state->get_all_step_values());
        if (name == u"setWriteTarget") { state->set_write_target(a(u"planIndex"), a(u"target")); return {}; }
        if (name == u"getWriteTarget") return state->get_write_target(a(u"planIndex"));
        if (name == u"setWriteStepTarget") { state->set_write_step_target(a(u"stepIndex"), a(u"target")); return {}; }
        if (name == u"getWriteStepTarget") return state->get_write_step_target(a(u"stepIndex"));
        if (name == u"setReadSource") { state->set_read_source(a(u"stepIndex"), a(u"source")); return {}; }
        if (name == u"getReadSource") return state->get_read_source(a(u"stepIndex"));
        if (name == u"setRead3dVolume") { state->set_read3d_volume(a(u"stepIndex"), a(u"volume")); return {}; }
        if (name == u"setRead3dGeometry") { state->set_read3d_geometry(a(u"stepIndex"), a(u"geometry")); return {}; }
        if (name == u"setWrite3dVolume") { state->set_write3d_volume(a(u"stepIndex"), a(u"volume")); return {}; }
        if (name == u"setWrite3dGeometry") { state->set_write3d_geometry(a(u"stepIndex"), a(u"geometry")); return {}; }
        if (name == u"setRenderTarget") { state->set_render_target(a(u"target")); return {}; }
        if (name == u"getRenderTarget") return state->get_render_target();
        if (name == u"clearRoutingOverrides") { state->clear_routing_overrides(); return {}; }
        if (name == u"setMediaInput") { state->set_media_input(a(u"stepIndex"), a(u"metadata")); return {}; }
        if (name == u"getMediaInput") return state->get_media_input(a(u"stepIndex"));
        if (name == u"removeMediaInput") { state->remove_media_input(a(u"stepIndex")); return {}; }
        if (name == u"getAllMediaInputs") return state->get_all_media_inputs();
        if (name == u"setTextInput") { state->set_text_input(a(u"stepIndex"), a(u"metadata")); return {}; }
        if (name == u"getTextInput") return state->get_text_input(a(u"stepIndex"));
        if (name == u"removeTextInput") { state->remove_text_input(a(u"stepIndex")); return {}; }
        if (name == u"getAllTextInputs") return state->get_all_text_inputs();
        if (name == u"applyToPipeline") { state->apply_to_pipeline(); return {}; }
        if (name == u"serialize") return state->serialize();
        if (name == u"deserialize") { state->deserialize(a(u"data")); return {}; }
        if (name == u"emit") { state->emit(s(u"event"), a(u"data")); return {}; }
        if (name == u"defineListener") {
            const JsString id = s(u"id");
            const Value actions = field(op, u"actions");
            listener_generations[id] = next_listener_generation++;
            listeners[id] = [this, id, actions](nm::ProgramStateCore&, const Value& data) {
                if (!actions.is_array()) return;
                for (const auto& action : actions.as_array()) {
                    const JsString doing = string_field(action, u"do");
                    const JsString event = string_field(action, u"event");
                    if (doing == u"record") events.push_back(object({{u"l", Value(id)}, {u"d", encode(data)}}));
                    else if (doing == u"throw") throw make_error(action);
                    else if (doing == u"on") on_listener(event, string_field(action, u"id"));
                    else if (doing == u"off") off_listener(event, string_field(action, u"id"));
                    else if (doing == u"once") on_listener(event, string_field(action, u"id"), true);
                    else if (doing == u"removeAllListeners") {
                        const Value target = field(action, u"event");
                        remove_all_listeners(target.is_string()
                            ? std::optional<JsString>(target.as_string()) : std::nullopt);
                    } else if (doing == u"op") {
                        const Value nested = field(action, u"op");
                        const Value nested_name = field(nested, u"op");
                        try {
                            events.push_back(object({{u"l", Value(id)}, {u"n", nested_name},
                                                     {u"r", encode(run_state_op(nested))}}));
                        } catch (const nm::JsError& error) {
                            events.push_back(object({{u"l", Value(id)}, {u"n", nested_name},
                                                     {u"x", error_record(error)}}));
                            if (nm::jsv::truthy(field(action, u"rethrow"))) throw;
                        }
                    }
                }
            };
            return {};
        }
        if (name == u"on") { on_listener(s(u"event"), s(u"id")); return {}; }
        if (name == u"off") { off_listener(s(u"event"), s(u"id")); return {}; }
        if (name == u"once") { on_listener(s(u"event"), s(u"id"), true); return {}; }
        if (name == u"removeAllListeners") {
            const Value event = field(op, u"event");
            remove_all_listeners(event.is_string() ? std::optional<JsString>(event.as_string()) : std::nullopt);
            return {};
        }
        if (name == u"extractEffectsFromDsl") return nm::extract_effects_from_dsl(s(u"dsl"), registry);
        throw nm::JsError(u"Error", u"unknown op " + name);
    }

    Value run_op(const Value& op) {
        const JsString name = string_field(op, u"op");
        const JsString dsl = string_field(op, u"dsl");
        if (name == u"host.load") return host_load(dsl, true);
        if (name == u"host.loadGraph") return host_load(dsl, false);
        if (name == u"host.setDsl") { mock().current_dsl = dsl; return {}; }
        if (name == u"host.clearPipeline") { mock().pipeline.reset(); pass_json.clear(); return {}; }
        if (name == u"host.setMethods") {
            const Value values = field(op, u"methods");
            std::vector<JsString> names;
            if (values.is_array()) for (const auto& value : values.as_array())
                if (value.is_string()) names.push_back(value.as_string());
            methods = nm::MockMethods::from_names(names);
            return {};
        }
        if (name == u"host.setEnums") {
            const JsString kind = string_field(op, u"enums");
            if (kind == u"none") mock().enums = Value();
            else if (kind == u"empty") mock().enums = Value(Object{});
            else if (kind == u"std") mock().enums = registry.enums().std().raw();
            else {
                Object combined = registry.enums().std().raw().as_object();
                merge(combined, registry.enums().project().raw());
                mock().enums = Value(combined);
            }
            return {};
        }
        if (name == u"setRenderer") {
            if (nm::jsv::truthy(field(op, u"renderer"))) {
                if (!state->renderer()) { state->set_renderer(std::move(detached)); detached.reset(); }
            } else if (state->renderer()) detached = state->take_renderer();
            return {};
        }
        if (name == u"newState") { new_state(field(op, u"host")); return {}; }
        if (name == u"convertParameterForUniform") {
            const JsString kind = string_field(op, u"enums");
            Value enums;
            if (kind == u"empty") enums = Value(Object{});
            else if (kind == u"none") enums = Value();
            else if (kind == u"std") enums = registry.enums().std().raw();
            else { Object combined = registry.enums().std().raw().as_object();
                   merge(combined, registry.enums().project().raw()); enums = Value(combined); }
            return nm::convert_parameter_for_uniform(decode(field(op, u"value")),
                                                     decode(field(op, u"spec")), enums);
        }
        if (name == u"resolveEnumValue") {
            const JsString kind = string_field(op, u"enums");
            Value enums;
            if (kind == u"empty") enums = Value(Object{});
            else if (kind == u"none") enums = Value();
            else if (kind == u"std") enums = registry.enums().std().raw();
            else { Object combined = registry.enums().std().raw().as_object();
                   merge(combined, registry.enums().project().raw()); enums = Value(combined); }
            return nm::resolve_enum_value(decode(field(op, u"path")), enums);
        }
        return run_state_op(op);
    }

    Value compress_call(const Value& call) {
        Value result = encode(call);
        if (!result.is_object()) return result;
        const JsString name = string_field(result, u"fn");
        if (name == u"checkAsyncRegen") {
            const Value node = field(result, u"nodeId");
            result.as_object().set(u"stepValues", same("call.checkAsyncRegen." + json(node),
                                                       field(result, u"stepValues")));
        } else if (name == u"recreateTextures") {
            result.as_object().set(u"uniforms", same("call.recreateTextures", field(result, u"uniforms")));
        }
        return result;
    }
    Value uniform_deltas() {
        const nm::MockHost& host = mock_ref();
        if (!host.pipeline) return Value(Array{});
        const Value passes = field(host.pipeline->graph, u"passes");
        if (!passes.is_array()) return Value(Array{});
        Array out;
        for (std::size_t i = 0; i < passes.as_array().size(); ++i) {
            const Value uniforms = encode(field(passes.as_array()[i], u"uniforms"));
            const std::string current = json(uniforms);
            if (i >= pass_json.size() || pass_json[i] != current) {
                out.push_back(object({{u"p", Value(static_cast<double>(i))}, {u"u", uniforms}}));
                if (i >= pass_json.size()) pass_json.resize(i + 1);
                pass_json[i] = current;
            }
        }
        return Value(out);
    }
    Value snapshot() {
        const Value raw = state->snapshot();
        Object out;
        for (const char16_t* key : {u"steps", u"structure", u"compiled", u"routing", u"media", u"text"}) {
            const JsString cache_key = u"st." + JsString(key);
            out.set(key, same(nm::js_to_utf8(cache_key), encode(field(raw, key))));
        }
        out.set(u"batch", encode(field(raw, u"batch")));
        out.set(u"dsl", state->renderer() ? encode(field(raw, u"dsl")) : Value::null());
        return Value(out);
    }
    void exec(const Value& op, std::ostream& output) {
        logs.clear();
        Object record;
        record.set(u"s", Value(name));
        record.set(u"i", Value(static_cast<double>(index++)));
        const JsString op_name = string_field(op, u"op");
        record.set(u"op", Value(op_name));
        try {
            Value result = encode(run_op(op));
            if (op_name == u"getStructure" || op_name == u"getCompiled" ||
                op_name == u"getAllStepValues" || op_name == u"serialize") {
                result = same("res." + nm::js_to_utf8(op_name), result);
            }
            record.set(u"r", result);
        } catch (const nm::JsError& error) {
            record.set(u"x", error_record(error));
        } catch (const nm::DslSyntaxError& error) {
            record.set(u"x", error_record(error));
        } catch (const nm::CompilationError& error) {
            record.set(u"x", error_record(error));
        } catch (const std::exception& error) {
            record.set(u"x", error_record(error));
        }
        record.set(u"ev", Value(std::move(events)));
        events.clear();
        record.set(u"log", Value(std::move(logs)));
        logs.clear();
        Array calls;
        for (const auto& call : mock().take_calls()) calls.push_back(compress_call(call));
        record.set(u"calls", Value(calls));
        record.set(u"u", uniform_deltas());
        record.set(u"st", snapshot());
        output << json(Value(record)) << '\n';
    }
};
}  // namespace

int run_program_state_full(int argc, char** argv) {
    if (argc < 3 || std::string(argv[1]) != "--out") {
        std::cerr << "usage: nm-dump run RESOLVED --out CANDIDATE\n";
        return 2;
    }
    std::ifstream input(argv[0], std::ios::binary);
    std::ofstream output(argv[2], std::ios::binary);
    if (!input || !output) {
        std::cerr << "nm-dump run: cannot open input/output\n";
        return 2;
    }
    nm::EffectRegistry registry;
    registry.loadEmbedded();
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty()) continue;
        const Value scenario = nm::json::parse(line);
        if (!field(scenario, u"suite").is_undefined()) continue;
        Scenario context(registry, string_field(scenario, u"name"), field(scenario, u"host"));
        const Value ops = field(scenario, u"ops");
        if (ops.is_array()) for (const auto& op : ops.as_array()) context.exec(op, output);
    }
    return 0;
}
}  // namespace nm_dump
