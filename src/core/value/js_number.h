#pragma once

#include "core/value/js_string.h"

namespace nm::js {
JsString numberToString(double value);
}  // namespace nm::js

namespace nm {
JsString js_number_to_string(double value);
}
