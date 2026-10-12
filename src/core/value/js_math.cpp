#include "core/value/js_math.h"

#include <cmath>
#include <cstdint>
#include <limits>

namespace libnoisemaker::js {

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

struct DoubleDouble {
    double hi;
    double lo;
};

DoubleDouble twoSum(double a, double b) {
    const double s = a + b;
    const double bb = s - a;
    return {s, (a - (s - bb)) + (b - bb)};
}

DoubleDouble quickTwoSum(double a, double b) {
    const double s = a + b;
    return {s, b - (s - a)};
}

DoubleDouble twoProd(double a, double b) {
    const double p = a * b;
    return {p, std::fma(a, b, -p)};
}

DoubleDouble ddAdd(DoubleDouble a, DoubleDouble b) {
    DoubleDouble s = twoSum(a.hi, b.hi);
    const DoubleDouble t = twoSum(a.lo, b.lo);
    s.lo += t.hi;
    s = quickTwoSum(s.hi, s.lo);
    s.lo += t.lo;
    return quickTwoSum(s.hi, s.lo);
}

DoubleDouble ddNeg(DoubleDouble a) {
    return {-a.hi, -a.lo};
}

DoubleDouble ddMul(DoubleDouble a, DoubleDouble b) {
    DoubleDouble p = twoProd(a.hi, b.hi);
    p.lo += a.hi * b.lo + a.lo * b.hi;
    return quickTwoSum(p.hi, p.lo);
}

DoubleDouble ddMulD(DoubleDouble a, double b) {
    DoubleDouble p = twoProd(a.hi, b);
    p.lo += a.lo * b;
    return quickTwoSum(p.hi, p.lo);
}

DoubleDouble ddDivD(DoubleDouble a, double b) {
    const double q1 = a.hi / b;
    const DoubleDouble p = twoProd(q1, b);
    const double r = ((a.hi - p.hi) - p.lo + a.lo) / b;
    return quickTwoSum(q1, r);
}

DoubleDouble ddDiv(DoubleDouble a, DoubleDouble b) {
    const double q1 = a.hi / b.hi;
    DoubleDouble r = ddAdd(a, ddNeg(ddMulD(b, q1)));
    const double q2 = r.hi / b.hi;
    r = ddAdd(r, ddNeg(ddMulD(b, q2)));
    const double q3 = r.hi / b.hi;
    return ddAdd(quickTwoSum(q1, q2), DoubleDouble{q3, 0.0});
}

// Taylor series of sin and cos for |r| <= pi/4.
DoubleDouble sinSeries(DoubleDouble r) {
    const DoubleDouble r2 = ddMul(r, r);
    DoubleDouble term = r;
    DoubleDouble sum = r;
    for (int n = 3; n <= 33; n += 2) {
        term = ddDivD(ddMul(term, r2), -static_cast<double>((n - 1) * n));
        sum = ddAdd(sum, term);
    }
    return sum;
}

DoubleDouble cosSeries(DoubleDouble r) {
    const DoubleDouble r2 = ddMul(r, r);
    DoubleDouble term{1.0, 0.0};
    DoubleDouble sum{1.0, 0.0};
    for (int n = 2; n <= 32; n += 2) {
        term = ddDivD(ddMul(term, r2), -static_cast<double>((n - 1) * n));
        sum = ddAdd(sum, term);
    }
    return sum;
}

// x = k * pi / 2 + r for |x| < 2^20, with pi / 2 in four parts (the fdlibm
// pio2_1, pio2_2, pio2_3 and pio2_3t constants; the first three have 33
// significant bits, so k times each is exact).
bool reduceQuadrant(double x, DoubleDouble& r, int& quadrant) {
    if (!(std::fabs(x) < 1048576.0)) return false;
    const double k = std::nearbyint(x * 6.36619772367581382433e-01);
    DoubleDouble acc{x, 0.0};
    acc = ddAdd(acc, DoubleDouble{-k * 1.57079632673412561417e+00, 0.0});
    acc = ddAdd(acc, DoubleDouble{-k * 6.07710050630396597660e-11, 0.0});
    acc = ddAdd(acc, DoubleDouble{-k * 2.02226624871116645580e-21, 0.0});
    acc = ddAdd(acc, ddNeg(twoProd(k, 8.47842766036889956997e-32)));
    r = acc;
    long long q = static_cast<long long>(k) % 4;
    if (q < 0) q += 4;
    quadrant = static_cast<int>(q);
    return true;
}

}  // namespace

double round(double x) {
    if (!std::isfinite(x) || std::trunc(x) == x) return x;
    if (x < 0 && x >= -0.5) return -0.0;
    const double floor = std::floor(x);
    return x - floor >= 0.5 ? floor + 1 : floor;
}

double max(double a, double b) {
    if (std::isnan(a) || std::isnan(b)) return kNaN;
    if (a == b) return std::signbit(a) ? b : a;
    return a > b ? a : b;
}

double min(double a, double b) {
    if (std::isnan(a) || std::isnan(b)) return kNaN;
    if (a == b) return std::signbit(a) ? a : b;
    return a < b ? a : b;
}

double pow(double base, double exponent) {
    if (std::isnan(exponent)) return kNaN;
    if (exponent == 0) return 1;
    if (std::isnan(base)) return kNaN;
    if (std::fabs(base) == 1 && std::isinf(exponent)) return kNaN;
    return std::pow(base, exponent);
}

// Arguments at or beyond 2^20 do not occur in the three effects (their
// angles stay below 400 rad); they fall back to the platform functions.
double sin(double x) {
    if (x == 0.0) return x;
    if (!std::isfinite(x)) return std::numeric_limits<double>::quiet_NaN();
    DoubleDouble r{};
    int quadrant = 0;
    if (!reduceQuadrant(x, r, quadrant)) return std::sin(x);
    switch (quadrant) {
    case 0: return sinSeries(r).hi;
    case 1: return cosSeries(r).hi;
    case 2: return -sinSeries(r).hi;
    default: return -cosSeries(r).hi;
    }
}

double cos(double x) {
    if (!std::isfinite(x)) return std::numeric_limits<double>::quiet_NaN();
    if (x == 0.0) return 1.0;
    DoubleDouble r{};
    int quadrant = 0;
    if (!reduceQuadrant(x, r, quadrant)) return std::cos(x);
    switch (quadrant) {
    case 0: return cosSeries(r).hi;
    case 1: return -sinSeries(r).hi;
    case 2: return -cosSeries(r).hi;
    default: return sinSeries(r).hi;
    }
}

// log(x) = e * ln 2 + 2 atanh((m - 1) / (m + 1)), x = m * 2^e.
double log(double x) {
    if (std::isnan(x) || x < 0.0) return std::numeric_limits<double>::quiet_NaN();
    if (x == 0.0) return -std::numeric_limits<double>::infinity();
    if (std::isinf(x)) return x;
    if (x == 1.0) return 0.0;
    int e = 0;
    double m = std::frexp(x, &e);
    if (m < 0.70710678118654752440) {
        m *= 2.0;
        e -= 1;
    }
    const DoubleDouble s = ddDiv(DoubleDouble{m - 1.0, 0.0}, twoSum(m, 1.0));
    const DoubleDouble s2 = ddMul(s, s);
    DoubleDouble power = s;
    DoubleDouble sum = s;
    for (int n = 3; n <= 61; n += 2) {
        power = ddMul(power, s2);
        sum = ddAdd(sum, ddDivD(power, static_cast<double>(n)));
    }
    DoubleDouble result = ddMulD(sum, 2.0);
    if (e != 0) {
        const DoubleDouble ln2{6.93147180559945286227e-01, 2.31904681384629955842e-17};
        result = ddAdd(ddMulD(ln2, static_cast<double>(e)), result);
    }
    return result.hi;
}

}  // namespace libnoisemaker::js
