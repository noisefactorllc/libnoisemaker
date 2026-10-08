#include "check.h"
#include "core/value/js_number.h"
#include "core/value/js_string.h"

#include <cmath>
#include <cstdio>
#include <limits>

namespace {
void check(double value, const char* expected) {
    const auto actual = nm::js_to_utf8(nm::js::numberToString(value));
    if (actual != expected) std::fprintf(stderr, "expected %s, got %s\n", expected, actual.c_str());
    NM_CHECK(actual == expected);
}
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
    return 0;
}
