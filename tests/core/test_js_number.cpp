#include "check.h"
#include "core/value/js_number.h"
#include "core/value/js_string.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>

// std::to_chars for double is the reference for the shortest digits. Apple
// declares it unavailable below macOS 13.3, where only the library's own path
// is checked.
#if !defined(__APPLE__) || __ENVIRONMENT_MAC_OS_X_VERSION_MIN_REQUIRED__ >= 130300
#include <charconv>
#define NM_HAVE_FLOAT_TO_CHARS 1
#endif

namespace {
void check(double value, const char* expected) {
    const auto actual = nm::js_to_utf8(nm::js::numberToString(value));
    if (actual != expected) std::fprintf(stderr, "expected %s, got %s\n", expected, actual.c_str());
    NM_CHECK(actual == expected);
}

#ifdef NM_HAVE_FLOAT_TO_CHARS
std::string to_chars_scientific(double magnitude) {
    char buffer[40];
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), magnitude, std::chars_format::scientific);
    return std::string(buffer, result.ptr);
}

// Same digits and exponent, ignoring how the exponent is padded ("e-07" or "e-7").
bool same_scientific(const std::string& a, const std::string& b) {
    const auto ea = a.find('e'), eb = b.find('e');
    return a.substr(0, ea) == b.substr(0, eb) && std::stoi(a.substr(ea + 1)) == std::stoi(b.substr(eb + 1));
}

void check_shortest_against_to_chars() {
    uint64_t state = 0x9E3779B97F4A7C15ull;
    auto next = [&state]() {
        uint64_t z = (state += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    };
    int compared = 0;
    auto compare = [&compared](double magnitude) {
        if (!(magnitude > 0) || std::isinf(magnitude)) return;
        const std::string ours = nm::js::detail::shortest_scientific(magnitude);
        const std::string reference = to_chars_scientific(magnitude);
        if (!same_scientific(ours, reference))
            std::fprintf(stderr, "%a: expected %s, got %s\n", magnitude, reference.c_str(), ours.c_str());
        NM_CHECK(same_scientific(ours, reference));
        ++compared;
    };
    for (int i = 0; i < 200000; ++i) {
        const uint64_t bits = next() & 0x7FFFFFFFFFFFFFFFull;
        double magnitude;
        std::memcpy(&magnitude, &bits, sizeof magnitude);
        compare(magnitude);
    }
    // Short decimals, powers of ten and the subnormal and normal limits.
    for (int i = 1; i < 100000; ++i) compare(i / 1000.0);
    for (int e = -323; e <= 308; ++e) compare(std::pow(10.0, e));
    compare(std::numeric_limits<double>::denorm_min());
    compare(std::numeric_limits<double>::min());
    compare(std::numeric_limits<double>::max());
    compare(0.1 + 0.2);
    std::printf("shortest digits agree with std::to_chars on %d doubles\n", compared);
}
#endif
}

int main() {
    check(0.0, "0");
    check(-0.0, "0");
    check(1.0, "1");
    check(-1.0, "-1");
    check(0.1, "0.1");
    check(-0.5, "-0.5");
    check(1.5, "1.5");
    check(100.0, "100");
    check(123.456, "123.456");
    check(1.0 / 3.0, "0.3333333333333333");
    check(2.0 / 3.0, "0.6666666666666666");
    check(1e21, "1e+21");
    check(1e20, "100000000000000000000");
    check(123456789012345680000.0, "123456789012345680000");
    check(1.2345678901234568e+21, "1.2345678901234568e+21");
    check(9.999999999999999e20, "999999999999999900000");
    check(12345678901234567890.0, "12345678901234567000");
    check(1e-6, "0.000001");
    check(0.0000012345, "0.0000012345");
    check(1e-7, "1e-7");
    check(1.5e-7, "1.5e-7");
    check(-2.5e-10, "-2.5e-10");
    check(1e-5 * 3, "0.000030000000000000004");
    check(5e-324, "5e-324");
    check(1.7976931348623157e308, "1.7976931348623157e+308");
    check(1e300, "1e+300");
    check(-1e-300, "-1e-300");
    check(9007199254740992.0, "9007199254740992");
    check(9007199254740994.0, "9007199254740994");
    check(0.1 + 0.2, "0.30000000000000004");
    check(255.0 / 256.0, "0.99609375");
    check(3.141592653589793, "3.141592653589793");
    check(2.718281828459045, "2.718281828459045");
    check(4.35, "4.35");
    check(0.3, "0.3");
    check(std::numeric_limits<double>::quiet_NaN(), "NaN");
    check(std::numeric_limits<double>::infinity(), "Infinity");
    check(-std::numeric_limits<double>::infinity(), "-Infinity");
#ifdef NM_HAVE_FLOAT_TO_CHARS
    check_shortest_against_to_chars();
#endif
    return 0;
}
