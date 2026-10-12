#include "core/value/js_number.h"

#include "core/js/js_unicode.h"
#include "core/value/parse_double.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <string>
#include <string_view>

#ifdef __APPLE__
#include <cstdio>
#include <xlocale.h>
#else
#include <charconv>
#endif

namespace libnoisemaker::js {
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
}  // namespace libnoisemaker::js

namespace libnoisemaker::js {
namespace {
// A 0x, 0o or 0b literal's digits, rounded to the nearest double, ties to even.
double non_decimal_integer(const std::string& digits, int radix, int bits_per_digit) {
    std::string bits;
    for (char ch : digits) {
        int value = -1;
        if (ch >= '0' && ch <= '9') value = ch - '0';
        else if (ch >= 'a' && ch <= 'f') value = ch - 'a' + 10;
        else if (ch >= 'A' && ch <= 'F') value = ch - 'A' + 10;
        if (value < 0 || value >= radix) return std::numeric_limits<double>::quiet_NaN();
        for (int bit = bits_per_digit - 1; bit >= 0; --bit) bits.push_back(((value >> bit) & 1) ? '1' : '0');
    }
    const size_t first_one = bits.find('1');
    if (first_one == std::string::npos) return 0;
    bits.erase(0, first_one);
    uint64_t mantissa = 0;
    for (size_t i = 0; i < bits.size() && i < 53; ++i) mantissa = mantissa * 2 + (bits[i] == '1');
    if (bits.size() <= 53) return static_cast<double>(mantissa);
    const bool round = bits[53] == '1';
    const bool sticky = bits.find('1', 54) != std::string::npos;
    if (round && (sticky || (mantissa & 1))) ++mantissa;
    return std::ldexp(static_cast<double>(mantissa), static_cast<int>(std::min<size_t>(bits.size() - 53, 4096)));
}

bool digit(char ch) { return ch >= '0' && ch <= '9'; }
}  // namespace

double stringToNumber(const JsString& text) {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    auto space = [](char16_t ch) { return isWhiteSpace(ch) || isLineTerminator(ch); };
    size_t begin = 0, end = text.size();
    while (begin < end && space(text[begin])) ++begin;
    while (end > begin && space(text[end - 1])) --end;
    if (begin == end) return 0;
    std::string s;
    for (size_t i = begin; i < end; ++i) {
        if (text[i] > 0x7F) return nan;
        s.push_back(static_cast<char>(text[i]));
    }
    if (s.size() > 2 && s[0] == '0') {
        switch (s[1]) {
            case 'x': case 'X': return non_decimal_integer(s.substr(2), 16, 4);
            case 'o': case 'O': return non_decimal_integer(s.substr(2), 8, 3);
            case 'b': case 'B': return non_decimal_integer(s.substr(2), 2, 1);
            default: break;
        }
    }
    size_t i = 0;
    const bool negative = s[0] == '-';
    if (s[0] == '+' || s[0] == '-') ++i;
    if (s.compare(i, std::string::npos, "Infinity") == 0) {
        return negative ? -std::numeric_limits<double>::infinity() : std::numeric_limits<double>::infinity();
    }
    // StrUnsignedDecimalLiteral: digits [. digits] [e [sign] digits], one digit at least.
    const size_t body = i;
    long long leading = 0;  // position of the first nonzero digit relative to the point
    bool seen_nonzero = false;
    size_t int_digits = 0, frac_digits = 0;
    while (i < s.size() && digit(s[i])) {
        if (!seen_nonzero && s[i] != '0') { seen_nonzero = true; leading = static_cast<long long>(int_digits); }
        ++int_digits; ++i;
    }
    if (i < s.size() && s[i] == '.') {
        ++i;
        while (i < s.size() && digit(s[i])) {
            if (!seen_nonzero && s[i] != '0') { seen_nonzero = true; leading = -static_cast<long long>(frac_digits) - 1; }
            ++frac_digits; ++i;
        }
    }
    if (int_digits + frac_digits == 0) return nan;
    long long exponent = 0;
    if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
        ++i;
        bool exponent_negative = false;
        if (i < s.size() && (s[i] == '+' || s[i] == '-')) { exponent_negative = s[i] == '-'; ++i; }
        if (i == s.size()) return nan;
        while (i < s.size() && digit(s[i])) {
            if (exponent < 1000000000LL) exponent = exponent * 10 + (s[i] - '0');
            ++i;
        }
        if (exponent_negative) exponent = -exponent;
    }
    if (i != s.size()) return nan;
    if (!seen_nonzero) return negative ? -0.0 : 0.0;

    double value = 0;
    const char* first = s.data() + body;
    const auto parsed = parse_double(first, s.data() + s.size(), value);
    if (parsed.ec == std::errc::result_out_of_range) {
        // Either side of the double range is hundreds of decades away.
        const long long magnitude = exponent + (leading >= 0 ? int_digits - leading : leading + 1);
        value = magnitude > 0 ? std::numeric_limits<double>::infinity() : 0.0;
    } else if (parsed.ec != std::errc() || parsed.ptr != s.data() + s.size()) {
        return nan;
    }
    return negative ? -value : value;
}
namespace {
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kInfinity = std::numeric_limits<double>::infinity();

bool isAsciiDigit(char16_t c) { return c >= u'0' && c <= u'9'; }

// Decimal digits -> value, saturating far above any exponent that matters
// (2^53 is exact; larger values only need to stay larger).
double accumulateDigits(std::u16string_view digits) {
    double value = 0.0;
    for (const char16_t c : digits) {
        if (value < 9007199254740992.0) {
            value = value * 10.0 + static_cast<double>(c - u'0');
        } else {
            value = 9007199254740992.0 * 10.0;
        }
    }
    return value;
}
}  // namespace

bool isWhiteSpaceOrLineTerminator(char16_t c) {
    switch (c) {
    case 0x0009: case 0x000A: case 0x000B: case 0x000C: case 0x000D:
    case 0x0020: case 0x00A0: case 0x1680: case 0x2028: case 0x2029:
    case 0x202F: case 0x205F: case 0x3000: case 0xFEFF:
        return true;
    default:
        return c >= 0x2000 && c <= 0x200A;
    }
}

// The value of the longest prefix of token that is a StrDecimalLiteral ([+-] Infinity | digits [. [digits]]
// [exponent] | . digits [exponent]), or NaN when no prefix is one.
double parseFloatPrefix(std::u16string_view token) {
    const std::size_t n = token.size();
    std::size_t i = 0;
    bool negative = false;
    if (i < n && (token[i] == u'+' || token[i] == u'-')) {
        negative = token[i] == u'-';
        ++i;
    }
    if (token.substr(i).starts_with(u"Infinity")) {
        return negative ? -kInfinity : kInfinity;
    }

    const std::size_t intStart = i;
    while (i < n && isAsciiDigit(token[i])) ++i;
    const std::size_t intEnd = i;

    std::size_t fracStart = i;
    std::size_t fracEnd = i;
    if (i < n && token[i] == u'.') {
        std::size_t j = i + 1;
        while (j < n && isAsciiDigit(token[j])) ++j;
        if (intEnd > intStart || j > i + 1) {
            fracStart = i + 1;
            fracEnd = j;
            i = j;
        }
    }
    if (intEnd == intStart && fracEnd == fracStart) {
        return kNaN;
    }

    bool hasExponent = false;
    bool exponentNegative = false;
    std::size_t expStart = 0;
    std::size_t expEnd = 0;
    if (i < n && (token[i] == u'e' || token[i] == u'E')) {
        std::size_t k = i + 1;
        bool expSignNegative = false;
        if (k < n && (token[k] == u'+' || token[k] == u'-')) {
            expSignNegative = token[k] == u'-';
            ++k;
        }
        const std::size_t digitsStart = k;
        while (k < n && isAsciiDigit(token[k])) ++k;
        if (k > digitsStart) {
            hasExponent = true;
            exponentNegative = expSignNegative;
            expStart = digitsStart;
            expEnd = k;
        }
    }

    const std::u16string_view intDigits = token.substr(intStart, intEnd - intStart);
    const std::u16string_view fracDigits = token.substr(fracStart, fracEnd - fracStart);
    const std::u16string_view expDigits = token.substr(expStart, expEnd - expStart);

    // Canonical ASCII form for a correctly rounded, locale-independent
    // conversion: [-]digits[.digits][e[-]digits].
    std::string text;
    text.reserve(intDigits.size() + fracDigits.size() + expDigits.size() + 4);
    if (negative) text += '-';
    const auto appendAscii = [&](std::u16string_view digits) {
        for (char16_t digit : digits) text.push_back(static_cast<char>(digit));
    };
    if (intDigits.empty()) text += '0';
    else appendAscii(intDigits);
    if (!fracDigits.empty()) {
        text += '.';
        appendAscii(fracDigits);
    }
    if (hasExponent) {
        text += 'e';
        if (exponentNegative) text += '-';
        appendAscii(expDigits);
    }

    double value = 0.0;
    const auto parsed = parse_double(text.data(), text.data() + text.size(), value);
    if (parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size()) {
        return value;
    }

    // Out-of-range results here have valid syntax:
    // overflow, which JavaScript rounds to +/-Infinity, and total underflow,
    // which JavaScript rounds to +/-0. Decide from the decimal order of the
    // leading non-zero digit.
    double order = 0.0;
    bool nonZero = false;
    for (std::size_t p = 0; p < intDigits.size(); ++p) {
        if (intDigits[p] != u'0') {
            order = static_cast<double>(intDigits.size() - p - 1);
            nonZero = true;
            break;
        }
    }
    if (!nonZero) {
        for (std::size_t q = 0; q < fracDigits.size(); ++q) {
            if (fracDigits[q] != u'0') {
                order = -static_cast<double>(q + 1);
                nonZero = true;
                break;
            }
        }
    }
    if (hasExponent) {
        const double exponent = accumulateDigits(expDigits);
        order += exponentNegative ? -exponent : exponent;
    }
    const double magnitude = (nonZero && order > 0.0) ? kInfinity : 0.0;
    return negative ? -magnitude : magnitude;
}


double parseFloat(const JsString& text) {
    size_t i = 0;
    while (i < text.size() && isWhiteSpaceOrLineTerminator(text[i])) ++i;
    return parseFloatPrefix(std::u16string_view(text).substr(i));
}
}  // namespace libnoisemaker::js

namespace libnoisemaker {
JsString js_number_to_string(double value) { return js::numberToString(value); }
}
