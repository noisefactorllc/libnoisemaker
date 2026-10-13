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
void reduceQuadrant(double x, DoubleDouble& r, int& quadrant);

// 2 / pi in 32-bit words, most significant first: the sum of
// kTwoOverPi[w] * 2^(-32 (w + 1)).
constexpr uint32_t kTwoOverPi[40] = {
    0xa2f9836e, 0x4e441529, 0xfc2757d1, 0xf534ddc0, 0xdb629599, 0x3c439041,
    0xfe5163ab, 0xdebbc561, 0xb7246e3a, 0x424dd2e0, 0x06492eea, 0x09d1921c,
    0xfe1deb1c, 0xb129a73e, 0xe88235f5, 0x2ebb4484, 0xe99c7026, 0xb45f7e41,
    0x3991d639, 0x835339f4, 0x9c845f8b, 0xbdf9283b, 0x1ff897ff, 0xde05980f,
    0xef2f118b, 0x5a0a6d1f, 0x6d367ecf, 0x27cb09b7, 0x4f463f66, 0x9e5fea2d,
    0x7527bac7, 0xebe5f17b, 0x3d0739f7, 0x8a5292ea, 0x6bfb5fb1, 0x1f8d5d08,
    0x56033046, 0xfc7b6bab, 0xf0cfbc20, 0x9af4361d,
};

// Bits [top - 52, top] of the little-endian 32-bit limbs, as an integer;
// bits below 0 are zero.
uint64_t bitsBelow(const uint32_t* limbs, int top) {
    uint64_t out = 0;
    for (int bit = top; bit > top - 53; --bit) {
        out <<= 1;
        if (bit >= 0) out |= (limbs[bit / 32] >> (bit % 32)) & 1u;
    }
    return out;
}

// x = k * pi / 2 + r for finite x >= 2^20 (Payne and Hanek): x * 2 / pi
// mod 4 with 255 or more fraction bits, from exact integer products with
// 288 bits of 2 / pi; the fraction to 106 bits times pi / 2 in double-double.
void reduceLarge(double x, DoubleDouble& r, int& quadrant) {
    int exponent = 0;
    const double mantissa = std::frexp(x, &exponent);
    const auto m = static_cast<uint64_t>(std::ldexp(mantissa, 53));
    const int e = exponent - 53;  // x = m * 2^e, 2^52 <= m < 2^53
    // Words before wStart contribute multiples of 4 to x * 2 / pi.
    const int wStart = e >= 2 ? (e - 2) / 32 : 0;
    constexpr int kWords = 9;
    uint32_t product[kWords + 2] = {};
    const uint64_t mLimbs[2] = {m & 0xffffffffu, m >> 32};
    for (int i = 0; i < kWords; ++i) {
        const uint64_t word = kTwoOverPi[wStart + kWords - 1 - i];
        uint64_t carry = 0;
        for (int j = 0; j < 2; ++j) {
            const uint64_t sum = uint64_t(product[i + j]) + word * mLimbs[j] + carry;
            product[i + j] = uint32_t(sum);
            carry = sum >> 32;
        }
        for (int k = i + 2; carry != 0; ++k) {
            const uint64_t sum = uint64_t(product[k]) + carry;
            product[k] = uint32_t(sum);
            carry = sum >> 32;
        }
    }
    const int fractionBits = 32 * (wStart + kWords) - e;
    const auto bit = [&](int index) { return (product[index / 32] >> (index % 32)) & 1u; };
    int q = int(bit(fractionBits) | (bit(fractionBits + 1) << 1));
    // The fraction, or 1 minus it when it is at least 1/2.
    uint32_t fraction[kWords + 2] = {};
    for (int i = 0; i < kWords + 2; ++i) {
        const int low = 32 * i;
        if (low >= fractionBits) break;
        const int keep = fractionBits - low;
        fraction[i] = keep >= 32 ? product[i] : product[i] & ((1u << keep) - 1u);
    }
    const bool negative = bit(fractionBits - 1) != 0;
    if (negative) {
        // 2^fractionBits - fraction
        uint64_t borrow = 0;
        for (int i = 0; i < kWords + 2; ++i) {
            const uint64_t limb = (uint64_t(0) - uint64_t(fraction[i]) - borrow) & 0xffffffffu;
            borrow = (fraction[i] != 0 || borrow != 0) ? 1 : 0;
            fraction[i] = uint32_t(limb);
        }
        for (int i = 0; i < kWords + 2; ++i) {
            const int low = 32 * i;
            if (low >= fractionBits) fraction[i] = 0;
            else if (fractionBits - low < 32) fraction[i] &= (1u << (fractionBits - low)) - 1u;
        }
        q = (q + 1) % 4;
    }
    int top = fractionBits - 1;
    while (top >= 0 && ((fraction[top / 32] >> (top % 32)) & 1u) == 0) --top;
    if (top < 0) {
        r = DoubleDouble{0.0, 0.0};
        quadrant = q;
        return;
    }
    const double hi = std::ldexp(double(bitsBelow(fraction, top)), top - 52 - fractionBits);
    const double lo = std::ldexp(double(bitsBelow(fraction, top - 53)), top - 105 - fractionBits);
    DoubleDouble f = quickTwoSum(hi, lo);
    if (negative) f = ddNeg(f);
    r = ddMul(f, DoubleDouble{1.57079632679489655800e+00, 6.12323399573676588613e-17});
    quadrant = q;
}

void reduceQuadrant(double x, DoubleDouble& r, int& quadrant) {
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

// Arguments at or beyond 2^20 reduce as their magnitude, sin being odd and
// cos even.
double sin(double x) {
    if (x == 0.0) return x;
    if (!std::isfinite(x)) return std::numeric_limits<double>::quiet_NaN();
    DoubleDouble r{};
    int quadrant = 0;
    if (!(std::fabs(x) < 1048576.0)) {
        reduceLarge(std::fabs(x), r, quadrant);
        if (x < 0) r = ddNeg(r);
        if (x < 0) quadrant = (4 - quadrant) % 4;
    } else {
        reduceQuadrant(x, r, quadrant);
    }
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
    if (!(std::fabs(x) < 1048576.0)) {
        reduceLarge(std::fabs(x), r, quadrant);
        if (x < 0) r = ddNeg(r);
        if (x < 0) quadrant = (4 - quadrant) % 4;
    } else {
        reduceQuadrant(x, r, quadrant);
    }
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
