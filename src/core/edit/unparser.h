#pragma once
#include "core/edit/jsv.h"
#include "core/lang/effect_registry.h"
#include <functional>

namespace nm {
struct UnparseOptions {
    Value enums;
    Value omit_search_directive;
    Value multiline_kwargs;
    Value indent;
    Value specs;
    Value schema_specs;
    Value lookup_spec;
    Value custom_formatter_spec;
    const EffectRegistry* registry = nullptr;
    std::function<Value(const Value&, const Value&)> get_effect_def;
    std::function<Value(const Value&, const Value&)> custom_formatter;
    std::function<Value(const Value&)> format_temp;
    static UnparseOptions from_value(const Value& source, const EffectRegistry* registry = nullptr);
};
JsString format_lossless_number(double value);
Value format_value(const Value& value, const Value& spec = Value(),
                   const UnparseOptions& options = {}, const Value& source_form = Value());
JsString unparse_call(const Value& call, const UnparseOptions& options = {});
JsString unparse_chain(const Value& chain, const UnparseOptions& options = {});
Value format_let_expr(const Value& expr, const UnparseOptions& options = {});
JsString unparse(const Value& compiled, const Value& overrides,
                 const UnparseOptions& options, const EffectRegistry& registry);
JsString apply_parameter_updates(const JsString& dsl, const Value& compiled,
                                  const Value& updates, const EffectRegistry& registry);
}
