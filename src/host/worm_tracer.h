#pragma once

// worm_tracer.h -- port of the reference CPU worm tracer
// (shaders/src/cpu/wormTracer.js) used by the asyncInit overlays of
// filter/fibers, filter/scratches and filter/strayHair.
//
// The reference runs in JavaScript, so every value here is an IEEE double
// evaluated in the reference's operation order. SeededRng reproduces the
// reference generator bit for bit, including the rounding of its
// double-precision multiplications. The flow field is stored as float32,
// as the reference's Float32Array is, and read with typed-array semantics
// (an index outside the array reads NaN). The strokes go to a StrokeCanvas,
// which models the browser canvas that receives them.

#include <cstdint>
#include <functional>

namespace nm {

class StrokeCanvas;

// Reference SeededRNG.
class SeededRng {
public:
    explicit SeededRng(double seed);
    std::uint32_t next();
    double nextFloat();                         // next() / 4294967295
    double normal(double mean, double stdDev);  // Box-Muller, as the reference

private:
    std::uint32_t m_state = 0;
};

enum class WormBehavior { Obedient, Unruly, Chaotic };

// Straight-alpha stroke colour. The reference's strokeStyle is
// `rgba(${r}, ${g}, ${b}, ${a * exposure})`.
struct WormColor {
    double r = 0.0;
    double g = 0.0;
    double b = 0.0;
    double a = 1.0;
};

// Reference traceWorms opts. width and height are the canvas size.
struct WormTraceOptions {
    int width = 0;
    int height = 0;
    double seed = 1.0;
    double density = 0.5;
    double kink = 1.0;
    double stride = 1.0;
    double strideDeviation = 0.0;
    double duration = 1.0;
    WormBehavior behavior = WormBehavior::Chaotic;
    double flowFreq = 1.0;
    double lineWidth = 1.0;
    // Called once per worm, in worm order, after its position, stride and
    // rotation have been drawn from the generator.
    std::function<WormColor(SeededRng&, int)> colorFn;
};

enum class TraceResult {
    Completed,
    Cancelled,
    // The reference throws (flow-field allocation) or exhausts the page
    // (more than kMaxTraceWorms worms or kMaxTraceSegments segments).
    // Nothing is drawn; the caller stops as the reference's asyncInit does.
    Failed,
};

constexpr double kMaxTraceWorms = 16777216.0;
constexpr double kMaxTraceSegments = 4294967296.0;

// Reference traceWorms. Draws on `canvas`, which must be opts.width x
// opts.height. `isCancelled` is polled before each worm; `onProgress` is
// called where the reference calls onProgress: after worms 0, 3, 6, ...
// and once at the end.
TraceResult traceWorms(StrokeCanvas& canvas, const WormTraceOptions& opts,
                       const std::function<bool()>& isCancelled = {},
                       const std::function<void()>& onProgress = {});

namespace detail {
// Math.sin, Math.cos and Math.log as V8 returns them: correctly rounded
// (worm_tracer.cpp explains the evaluation). Exposed for the tests.
double jsSin(double x);
double jsCos(double x);
double jsLog(double x);
} // namespace detail

} // namespace nm
