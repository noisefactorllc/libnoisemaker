#pragma once

#include "core/edit/jsv.h"

#include <optional>
#include <vector>

namespace nm {

enum class MockConvert { Canvas, Passthrough, Absent };

struct MockMethods {
    bool broadcast_chain_scoped_param = true;
    bool check_async_regen = true;
    bool recreate_textures = true;
    bool collect_default_uniforms = true;
    bool set_uniform = true;

    static MockMethods all() { return {}; }
    static MockMethods none() { return {false, false, false, false, false}; }
    static MockMethods from_names(const std::vector<JsString>& names);
};

struct MockPipeline {
    Value graph;
    MockMethods methods;
    std::vector<Value> calls;
};

class ProgramHost {
public:
    virtual ~ProgramHost() = default;
    virtual JsString dsl() const = 0;
    virtual Value enum_tree() const = 0;
    virtual bool has_converter() const = 0;
    virtual Value convert_parameter_for_uniform(const Value& value, const Value& spec) const = 0;
    virtual std::vector<Value>* graph_passes() = 0;
    virtual const MockMethods* method_presence() const = 0;
    virtual void broadcast_chain_scoped_param(std::size_t pass_index, const JsString& uniform,
                                              const JsString& scoped) = 0;
    virtual void check_async_regen(const Value& node_id, const Value& effect_key,
                                   const Object& step_values) = 0;
    virtual Object collect_default_uniforms() = 0;
    virtual void recreate_textures(const Object& uniforms) = 0;
    virtual void set_uniform(const JsString& name, const Value& value) = 0;
};

// A data-only renderer used by the parity runner and CPU consumers.
class MockHost final : public ProgramHost {
public:
    JsString current_dsl;
    Value enums;
    MockConvert convert = MockConvert::Canvas;
    std::optional<MockPipeline> pipeline;

    JsString dsl() const override { return current_dsl; }
    Value enum_tree() const override { return enums; }
    bool has_converter() const override { return convert != MockConvert::Absent; }
    Value convert_parameter_for_uniform(const Value& value, const Value& spec) const override;
    std::vector<Value>* graph_passes() override;
    const MockMethods* method_presence() const override;
    void broadcast_chain_scoped_param(std::size_t pass_index, const JsString& uniform,
                                      const JsString& scoped) override;
    void check_async_regen(const Value& node_id, const Value& effect_key,
                           const Object& step_values) override;
    Object collect_default_uniforms() override;
    void recreate_textures(const Object& uniforms) override;
    void set_uniform(const JsString& name, const Value& value) override;
    std::vector<Value> take_calls();
};

Value resolve_enum_value(const Value& path, const Value& enums);
double parse_js_float_value(const Value& value);
double parse_js_int_value(const Value& value, int radix = 10);
Value convert_parameter_for_uniform(const Value& value, const Value& spec, const Value& enums);
bool write_uniform_aliases(Value& pass, const JsString& param_name,
                           const Value& uniform_name, const Value& value);

}  // namespace nm
