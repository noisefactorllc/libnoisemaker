#include "core/value/js_number.h"

#include <charconv>
#include <cmath>
#include <cstdlib>
#include <string>

namespace nm::js {
JsString numberToString(double value) {
    if (std::isnan(value)) return u"NaN";
    if (value == 0.0) return u"0";
    if (std::isinf(value)) return value < 0 ? u"-Infinity" : u"Infinity";

    char buffer[64];
    const auto [end, error] = std::to_chars(buffer, buffer + sizeof(buffer), std::fabs(value),
                                            std::chars_format::general);
    if (error != std::errc{}) std::abort();
    std::string decimal(buffer, end);
    const auto epos = decimal.find_first_of("eE");
    int exponent = epos == std::string::npos ? 0 : std::stoi(decimal.substr(epos + 1));
    const std::string mantissa = decimal.substr(0, epos);
    const auto dot = mantissa.find('.');
    int n = static_cast<int>(dot == std::string::npos ? mantissa.size() : dot) + exponent;
    std::string digits;
    for (char ch : mantissa) if (ch != '.') digits.push_back(ch);
    const auto leading = digits.find_first_not_of('0');
    n -= static_cast<int>(leading);
    digits.erase(0, leading);
    while (digits.size() > 1 && digits.back() == '0') digits.pop_back();

    std::string out = value < 0 ? "-" : "";
    const int k = static_cast<int>(digits.size());
    if (k <= n && n <= 21) {
        out += digits + std::string(n - k, '0');
    } else if (0 < n && n <= 21) {
        out += digits.substr(0, n) + "." + digits.substr(n);
    } else if (-6 < n && n <= 0) {
        out += "0." + std::string(-n, '0') + digits;
    } else {
        out.push_back(digits[0]);
        if (k > 1) out += "." + digits.substr(1);
        const int printed_exponent = n - 1;
        out += "e";
        if (printed_exponent >= 0) out += "+";
        out += std::to_string(printed_exponent);
    }
    return utf8_to_js(out);
}
}  // namespace nm::js

namespace nm {
JsString js_number_to_string(double value) { return js::numberToString(value); }
}
