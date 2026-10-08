#pragma once

#include <string>
#include <string_view>

namespace nm {
using JsString = std::u16string;

JsString utf8_to_js(std::string_view text);
std::string js_to_utf8(const JsString& text);
}  // namespace nm
