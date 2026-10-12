#include "worm_tracer.h"

#include "stroke_canvas.h"
#include "core/value/js_math.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <vector>

namespace libnoisemaker {

namespace {

constexpr double kTau = 3.141592653589793 * 2.0; // Math.PI * 2
constexpr double kTwoTo32 = 4294967296.0;

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
    return mean + stdDev * std::sqrt(-2.0 * js::log(u1)) * js::cos(kTau * u2);
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

            const double newX = wx + js::sin(angle) * worm.stride;
            const double newY = wy + js::cos(angle) * worm.stride;

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

} // namespace libnoisemaker
