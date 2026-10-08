#pragma once

#include "core/value/value.h"

namespace nm {

// Expands the classic Noisedeck palette index into its five shader uniforms.
// Returns null outside the 1..55 range, as expandPalette does in the reference.
Value expand_palette_value(const Value& index);

}  // namespace nm
