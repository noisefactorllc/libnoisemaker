#pragma once

#include "core/value/js_string.h"

#include <string>

namespace nm::js {
JsString numberToString(double value);

namespace detail {
// The shortest round-trip digits of a finite double above zero, as "d.ddde±x".
std::string shortest_scientific(double magnitude);
}  // namespace detail
}  // namespace nm::js

namespace nm {
JsString js_number_to_string(double value);
}
