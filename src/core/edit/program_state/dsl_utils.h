#pragma once

#include "core/lang/effect_registry.h"

namespace nm {

Value compile_state_dsl(const JsString& dsl, EffectRegistry& registry);
Value extract_effects_from_dsl(const JsString& dsl, EffectRegistry& registry);

}  // namespace nm
