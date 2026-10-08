#include "worm_tracer.h"

#include "stroke_canvas.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <vector>

namespace nm {

namespace {

constexpr double kTau = 3.141592653589793 * 2.0; // Math.PI * 2
constexpr double kTwoTo32 = 4294967296.0;

// ---------------------------------------------------------------- Math.*
// V8 in Chromium returns correctly rounded Math.sin, Math.cos and Math.log;
// the platform libm need not (Apple's libm differs from V8 in about 3 % of
// sin and cos results and 0.07 % of log results at 1 ulp). These evaluate
// the functions in double-double arithmetic (about 104 bits) and round
// once. They reproduced Chromium 153's V8 on 1.8 million random arguments
// and on every call the three traces make at 256x256;
// parity/check_async_overlay.mjs re-checks 300000 arguments on each run.

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

// ECMAScript ToUint32.
std::uint32_t toUint32(double value) {
    if (!std::isfinite(value)) return 0;
    double m = std::fmod(std::trunc(value), kTwoTo32);
    if (m < 0.0) m += kTwoTo32;
    return static_cast<std::uint32_t>(m);
}

// ECMAScript ToInt32.
std::int32_t toInt32(std::uint32_t value) {
    return static_cast<std::int32_t>(value);
}

// Math.max(a, b) for the values the tracer passes (NaN propagates).
double jsMax(double a, double b) {
    if (std::isnan(a) || std::isnan(b)) return std::numeric_limits<double>::quiet_NaN();
    return std::max(a, b);
}

// A Float32Array read by a double index: NaN (undefined) off the array.
class Float32Field {
public:
    explicit Float32Field(size_t size) : m_values(size, 0.0f) {}
    size_t size() const { return m_values.size(); }
    void set(size_t index, double value) { m_values[index] = static_cast<float>(value); }
    double at(double index) const {
        if (!(index >= 0.0) || index >= static_cast<double>(m_values.size()) || std::floor(index) != index) {
            return std::numeric_limits<double>::quiet_NaN();
        }
        return static_cast<double>(m_values[static_cast<size_t>(index)]);
    }

private:
    std::vector<float> m_values;
};

// Reference valueNoiseField. Empty where the reference throws.
std::optional<Float32Field> valueNoiseField(int w, int h, double freq, SeededRng& rng) {
    const double gw = std::ceil(freq) + 2.0;
    const double gh = std::ceil(freq) + 2.0;
    double length = gw * gh;
    if (std::isnan(length)) length = 0.0;                                     // ToIndex(NaN) = 0
    if (!std::isfinite(length) || length > 268435456.0) return std::nullopt;  // RangeError
    Float32Field grid(static_cast<size_t>(length));
    for (size_t i = 0; i < grid.size(); ++i) grid.set(i, rng.nextFloat());

    Float32Field field(static_cast<size_t>(w) * static_cast<size_t>(h));
    const double wd = static_cast<double>(w);
    const double hd = static_cast<double>(h);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const double fx = (static_cast<double>(x) / wd) * freq;
            const double fy = (static_cast<double>(y) / hd) * freq;
            const double ix = std::floor(fx);
            const double iy = std::floor(fy);
            const double dx = fx - ix;
            const double dy = fy - iy;
            const double sx = dx * dx * (3.0 - 2.0 * dx);
            const double sy = dy * dy * (3.0 - 2.0 * dy);
            const double tl = grid.at(iy * gw + ix);
            const double tr = grid.at(iy * gw + ix + 1.0);
            const double bl = grid.at((iy + 1.0) * gw + ix);
            const double br = grid.at((iy + 1.0) * gw + ix + 1.0);
            field.set(static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x),
                      (tl * (1.0 - sx) + tr * sx) * (1.0 - sy) + (bl * (1.0 - sx) + br * sx) * sy);
        }
    }
    return field;
}

struct Worm {
    double x;
    double y;
    double stride;
    double rot;
    WormColor color;
};

} // namespace

namespace detail {

// Arguments at or beyond 2^20 do not occur in the three effects (their
// angles stay below 400 rad); they fall back to the platform functions.
double jsSin(double x) {
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

double jsCos(double x) {
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
double jsLog(double x) {
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

} // namespace detail

SeededRng::SeededRng(double seed)
    : m_state(toUint32(static_cast<double>(toUint32(seed)) * 747796405.0 + 2891336453.0)) {}

std::uint32_t SeededRng::next() {
    m_state = toUint32(static_cast<double>(m_state) * 747796405.0 + 2891336453.0);
    const std::uint32_t shifted = m_state >> ((m_state >> 28) + 4);
    const std::int32_t mixed = toInt32(shifted ^ m_state);
    const std::uint32_t word = toUint32(static_cast<double>(mixed) * 277803737.0);
    return (word >> 22) ^ word;
}

double SeededRng::nextFloat() {
    return static_cast<double>(next()) / 4294967295.0;
}

double SeededRng::normal(double mean, double stdDev) {
    const double u1 = jsMax(nextFloat(), 1e-10);
    const double u2 = nextFloat();
    return mean + stdDev * std::sqrt(-2.0 * detail::jsLog(u1)) * detail::jsCos(kTau * u2);
}

TraceResult traceWorms(StrokeCanvas& canvas, const WormTraceOptions& opts,
                       const std::function<bool()>& isCancelled,
                       const std::function<void()>& onProgress) {
    const int width = opts.width;
    const int height = opts.height;
    const double wd = static_cast<double>(width);
    const double hd = static_cast<double>(height);

    SeededRng rng(opts.seed);
    const double minDim = std::min(wd, hd);
    const double maxDim = std::max(wd, hd);
    const double strideScale = maxDim / 1024.0;

    SeededRng fieldRng(opts.seed * 31337.0);
    const std::optional<Float32Field> flowField = valueNoiseField(width, height, opts.flowFreq, fieldRng);
    if (!flowField) return TraceResult::Failed;

    const double count = jsMax(1.0, std::floor(maxDim * opts.density));
    if (count > kMaxTraceWorms) return TraceResult::Failed;
    const double iterations = jsMax(1.0, std::floor(std::sqrt(minDim) * opts.duration));
    if (count * iterations > kMaxTraceSegments) return TraceResult::Failed;

    // For obedient behavior, all worms share one base rotation.
    const double sharedRot = rng.nextFloat() * kTau;
    const bool obedient = opts.behavior == WormBehavior::Obedient;

    // `i < count` is false for a NaN count, as in the reference.
    std::vector<Worm> worms;
    for (double i = 0.0; i < count; i += 1.0) {
        Worm worm;
        worm.x = rng.nextFloat() * wd;
        worm.y = rng.nextFloat() * hd;
        worm.stride = rng.normal(opts.stride, opts.strideDeviation) * strideScale;
        worm.rot = obedient ? sharedRot : rng.nextFloat() * kTau;
        worm.color = opts.colorFn ? opts.colorFn(rng, static_cast<int>(i)) : WormColor{};
        worms.push_back(worm);
    }

    canvas.setLineWidth(opts.lineWidth);

    for (size_t w = 0; w < worms.size(); ++w) {
        if (isCancelled && isCancelled()) return TraceResult::Cancelled;

        const Worm& worm = worms[w];
        double wx = worm.x;
        double wy = worm.y;
        for (double iter = 0.0; iter < iterations; iter += 1.0) {
            // Exposure ramp: 0 -> 1 -> 0 over the worm's lifetime.
            const double t = iterations > 1.0 ? iter / (iterations - 1.0) : 1.0;
            const double exposure = 1.0 - std::fabs(1.0 - t * 2.0);

            // Flow field lookup (wrap coordinates).
            const double fx = std::floor(std::fmod(std::fmod(wx, wd) + wd, wd));
            const double fy = std::floor(std::fmod(std::fmod(wy, hd) + hd, hd));
            const double fieldVal = flowField->at(fy * wd + fx);
            double angle = fieldVal * kTau * opts.kink;
            angle += obedient ? sharedRot : worm.rot;

            const double newX = wx + detail::jsSin(angle) * worm.stride;
            const double newY = wy + detail::jsCos(angle) * worm.stride;

            canvas.setStrokeColor(worm.color.r, worm.color.g, worm.color.b, worm.color.a * exposure);
            canvas.strokeLine(wx, wy, newX, newY);

            wx = newX;
            wy = newY;
        }

        // The reference yields every few worms and uploads the canvas.
        if (w % 3 == 0 && onProgress) onProgress();
    }

    if (onProgress) onProgress();
    return TraceResult::Completed;
}

} // namespace nm
