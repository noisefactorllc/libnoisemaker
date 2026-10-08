#pragma once

#include <charconv>

#ifdef __APPLE__
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <string>
#include <xlocale.h>
#endif

namespace nm {

inline std::from_chars_result parse_double(const char* first, const char* last, double& value) {
#ifdef __APPLE__
    // The libc++ shipped with Xcode 16.4 declares floating from_chars but
    // does not implement it. strtod_l keeps decimal parsing in the C locale.
    if (first == last || *first == '+') return {first, std::errc::invalid_argument};
    struct CLocale {
        locale_t value = newlocale(LC_NUMERIC_MASK, "C", nullptr);
        ~CLocale() { if (value) freelocale(value); }
    };
    static const CLocale c_locale;
    if (!c_locale.value) return {first, std::errc::invalid_argument};

    std::string input(first, last);
    const size_t sign = input[0] == '-' ? 1 : 0;
    if (input.size() > sign + 1 && input[sign] == '0'
        && (input[sign + 1] == 'x' || input[sign + 1] == 'X')) {
        input.resize(sign + 1);
    }
    if (input[0] == ' ' || (input[0] >= '\t' && input[0] <= '\r'))
        return {first, std::errc::invalid_argument};

    char* end = nullptr;
    const int saved_errno = errno;
    errno = 0;
    const double parsed = strtod_l(input.c_str(), &end, c_locale.value);
    const int parse_errno = errno;
    errno = saved_errno;
    const size_t consumed = static_cast<size_t>(end - input.c_str());
    if (consumed == 0) return {first, std::errc::invalid_argument};
    if (parse_errno == ERANGE && (parsed == 0.0 || std::isinf(parsed)))
        return {first + consumed, std::errc::result_out_of_range};
    value = parsed;
    return {first + consumed, std::errc{}};
#else
    return std::from_chars(first, last, value);
#endif
}

} // namespace nm
