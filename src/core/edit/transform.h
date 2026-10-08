#pragma once
#include "core/lang/effect_registry.h"
#include "core/value/value.h"
namespace nm {
Value list_steps(const Value& compiled, const Value& options, const EffectRegistry& registry);
Value replace_effect(const Value& compiled, const Value& step_index, const Value& new_effect_name,
                     const Value& new_args, const Value& options, const EffectRegistry& registry);
Value get_compatible_replacements(const Value& compiled, const Value& step_index,
                                  const Value& options, const EffectRegistry& registry);
Value predict_replacement(const Value& resolved_name, const Value& spec, const Value& new_args,
                          const Value& old_instance, const Value& options, const EffectRegistry& registry);
}
