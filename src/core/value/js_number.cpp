#include "core/value/js_number.h"

#include <cmath>
#include <cstdlib>
#include <string>

#ifdef __APPLE__
#include <cstdio>
#include <xlocale.h>
#else
#include <charconv>
#endif

namespace nm::js {
namespace detail {
#ifdef __APPLE__
// Apple's libc++ provides floating std::to_chars only from macOS 13.3, and
// the library must build for macOS 13.0. Apple's printf rounds correctly, so
// the first precision whose output reads back as the same double gives the
// fewest digits and, among those, the closest to the value, as
// Number::prototype.toString requires. The C locale keeps '.' as the point.
std::string shortest_scientific(double magnitude) {
    static const locale_t c_locale = newlocale(LC_NUMERIC_MASK, "C", nullptr);
    if (!c_locale) std::abort();
    char buffer[40];
    for (int precision = 0; precision < 17; ++precision) {
        snprintf_l(buffer, sizeof(buffer), c_locale, "%.*e", precision, magnitude);
        if (strtod_l(buffer, nullptr, c_locale) == magnitude) return buffer;
    }
    std::abort();  // 17 significant digits always round-trip.
}
#else
std::string shortest_scientific(double magnitude) {
    char buffer[40];
    const auto [end, error] = std::to_chars(buffer, buffer + sizeof(buffer), magnitude,
                                            std::chars_format::scientific);
    if (error != std::errc{}) std::abort();
    return std::string(buffer, end);
}
#endif
}  // namespace detail

JsString numberToString(double value) {
    if (std::isnan(value)) return u"NaN";
    if (value == 0.0) return u"0";
    if (std::isinf(value)) return value < 0 ? u"-Infinity" : u"Infinity";

    // value = 0.d1..dk x 10^n, from the shortest "d.ddde±x" form.
    const std::string scientific = detail::shortest_scientific(std::fabs(value));
    const auto epos = scientific.find('e');
    const int n = std::stoi(scientific.substr(epos + 1)) + 1;
    std::string digits;
    for (char ch : scientific.substr(0, epos)) if (ch != '.') digits.push_back(ch);
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
