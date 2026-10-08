#pragma once

#include "core/edit/program_state/host.h"
#include "core/edit/program_state/console.h"
#include "core/lang/effect_registry.h"

#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <utility>

namespace nm {

struct StepState {
    JsString effect_key;
    Value effect_def;
    double step_index = 0;
    Object values;
};

struct Change {
    JsString step_key;
    JsString param_name;
    Value value;
    Value previous_value;
    Value to_value() const;
};

class ProgramStateCore {
public:
    using Listener = std::function<void(ProgramStateCore&, const Value&)>;
    using ListenerId = std::size_t;

    explicit ProgramStateCore(EffectRegistry& registry);
    virtual ~ProgramStateCore() = default;
    ProgramStateCore(const ProgramStateCore&) = delete;
    ProgramStateCore& operator=(const ProgramStateCore&) = delete;

    EffectRegistry& registry() const { return *registry_; }
    void set_registry(EffectRegistry& registry) { registry_ = &registry; }

    ListenerId on(const JsString& event, const Listener& callback);
    ListenerId once(const JsString& event, const Listener& callback);
    void off(const JsString& event, ListenerId id);
    void remove_all_listeners(const std::optional<JsString>& event = std::nullopt);
    void emit(const JsString& event, const Value& data = Value());

    Value get_value(const JsString& step_key, const JsString& param_name) const;
    void set_value(const JsString& step_key, const JsString& param_name, const Value& value);
    Object get_step_values(const JsString& step_key) const;
    void set_step_values(const JsString& step_key, const Value& values);
    void batch(const std::function<void(ProgramStateCore&)>& action);
    void from_dsl(const JsString& dsl);
    void from_dsl_value(const Value& dsl);
    JsString to_dsl() const;
    bool would_change_structure(const JsString& dsl) const;
    void reset_step(const JsString& step_key);
    void set_skip(const JsString& step_key, const Value& skip);
    bool is_skipped(const JsString& step_key) const;
    Value delete_step(double step_index);
    Value insert_step(double after_step_index, const JsString& effect_name,
                      const Value& args = Value());

    Value get_structure() const;
    Value get_compiled() const { return compiled_; }
    Value get_effect_def(const JsString& step_key) const;
    std::size_t step_count() const { return step_states_.size(); }
    Value get_step_keys() const;
    Object get_all_step_values() const;
    const std::vector<std::pair<JsString, StepState>>& step_states() const { return step_states_; }

    void set_write_target(const Value& plan_index, const Value& target);
    Value get_write_target(const Value& plan_index) const;
    void set_write_step_target(const Value& step_index, const Value& target);
    Value get_write_step_target(const Value& step_index) const;
    void set_read_source(const Value& step_index, const Value& source);
    Value get_read_source(const Value& step_index) const;
    void set_read3d_volume(const Value& step_index, const Value& volume);
    void set_read3d_geometry(const Value& step_index, const Value& geometry);
    void set_write3d_volume(const Value& step_index, const Value& volume);
    void set_write3d_geometry(const Value& step_index, const Value& geometry);
    void set_render_target(const Value& target);
    Value get_render_target() const { return render_target_override_; }
    void clear_routing_overrides();
    Value routing_overrides() const;

    void set_media_input(const Value& step_index, const Value& metadata);
    Value get_media_input(const Value& step_index) const;
    void remove_media_input(const Value& step_index);
    Value get_all_media_inputs() const;
    void set_text_input(const Value& step_index, const Value& metadata);
    Value get_text_input(const Value& step_index) const;
    void remove_text_input(const Value& step_index);
    Value get_all_text_inputs() const;

    void apply_to_pipeline();
    Value serialize() const;
    void deserialize(const Value& data);
    std::size_t batch_depth() const { return batch_depth_; }
    const std::vector<Change>& batched_changes() const { return batched_changes_; }
    bool recompile_pending() const { return recompile_pending_; }
    Value snapshot() const;

protected:
    void set_host(ProgramHost* host) { host_ = host; }
    ProgramHost* host() const { return host_; }

private:
    Value build_parameter_overrides() const;
    void apply_routing_overrides(Value& compiled) const;
    struct EventListener { ListenerId id; Listener callback; bool once; };
    struct KeyMap { std::vector<std::pair<Value, Value>> entries; };
    EffectRegistry* registry_;
    ProgramHost* host_ = nullptr;
    std::vector<std::pair<JsString, StepState>> step_states_;
    Value structure_ = Value(Array{});
    Value compiled_ = Value::null();
    std::array<KeyMap, 7> routing_;
    Value render_target_override_ = Value::null();
    KeyMap media_inputs_;
    KeyMap text_inputs_;
    std::vector<std::pair<JsString, std::shared_ptr<std::vector<EventListener>>>> listeners_;
    ListenerId next_listener_id_ = 1;
    std::size_t batch_depth_ = 0;
    std::vector<Change> batched_changes_;
    bool recompile_pending_ = false;
};

template <class Host = MockHost>
class ProgramState : public ProgramStateCore {
public:
    explicit ProgramState(EffectRegistry& registry) : ProgramStateCore(registry) {}
    ProgramState(EffectRegistry& registry, Host renderer) : ProgramStateCore(registry) {
        set_renderer(std::move(renderer));
    }
    void set_renderer(std::optional<Host> renderer) {
        renderer_ = std::move(renderer);
        set_host(renderer_ ? static_cast<ProgramHost*>(&*renderer_) : nullptr);
    }
    Host* renderer_mut() { return renderer_ ? &*renderer_ : nullptr; }
    const Host* renderer() const { return renderer_ ? &*renderer_ : nullptr; }
    std::optional<Host> take_renderer() {
        std::optional<Host> result = std::move(renderer_);
        renderer_.reset();
        set_host(nullptr);
        return result;
    }

private:
    std::optional<Host> renderer_;
};

Value extract_effects_from_dsl(const JsString& dsl, EffectRegistry& registry);

}  // namespace nm
