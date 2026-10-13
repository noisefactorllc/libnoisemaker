#include "host/async_overlays.h"

#include "host/stroke_canvas.h"

#include <cmath>
#include <limits>

namespace libnoisemaker {

namespace {

// JavaScript's Math.max of two numbers: NaN when either is NaN.
double jsMax(double a, double b) {
    if (std::isnan(a) || std::isnan(b)) return std::numeric_limits<double>::quiet_NaN();
    return a > b ? a : b;
}

WormTraceOptions baseOptions(const StrokeCanvas& canvas) {
    WormTraceOptions opts;
    opts.width = canvas.width();
    opts.height = canvas.height();
    return opts;
}

// Reference Fibers.asyncInit: four layers of chaotic worms.
TraceResult drawFibers(StrokeCanvas& canvas, double seed, double density) {
    const double width = canvas.width();
    const double baseDensity = 0.5 + density * 2.0;
    for (int layer = 0; layer < 4; ++layer) {
        const double layerSeed = seed * 1000 + layer * 137.0;
        WormTraceOptions opts = baseOptions(canvas);
        opts.seed = layerSeed;
        opts.density = baseDensity;
        opts.kink = 5 + std::fmod(layerSeed, 5.0);
        opts.stride = 0.75;
        opts.strideDeviation = 0.125;
        opts.duration = 1;
        opts.behavior = WormBehavior::Chaotic;
        opts.flowFreq = 4;
        opts.lineWidth = jsMax(1.5, width / 384);
        opts.colorFn = [](SeededRng& rng, int) {
            WormColor color;
            color.r = std::floor(rng.nextFloat() * 200 + 55);
            color.g = std::floor(rng.nextFloat() * 200 + 55);
            color.b = std::floor(rng.nextFloat() * 200 + 55);
            color.a = 0.5;
            return color;
        };
        const TraceResult result = traceWorms(canvas, opts);
        if (result != TraceResult::Completed) return result;
    }
    return TraceResult::Completed;
}

// Reference Scratches.asyncInit: four layers, obedient or unruly by the
// layer seed's parity.
TraceResult drawScratches(StrokeCanvas& canvas, double seed, double density) {
    const double width = canvas.width();
    for (int layer = 0; layer < 4; ++layer) {
        const double layerSeed = seed * 1000 + layer * 251.0;
        const bool isObedient = std::fmod(layerSeed, 2.0) == 0;
        WormTraceOptions opts = baseOptions(canvas);
        opts.seed = layerSeed;
        opts.density = 0.1 + density * 0.4;
        opts.kink = 0.125 + std::fmod(layerSeed, 50.0) / 400;
        opts.stride = 0.75;
        opts.strideDeviation = 0.5;
        opts.duration = 2 + std::fmod(layerSeed, 3.0);
        opts.behavior = isObedient ? WormBehavior::Obedient : WormBehavior::Unruly;
        opts.flowFreq = 2 + std::fmod(layerSeed, 3.0);
        opts.lineWidth = jsMax(0.5, width / 1024);
        opts.colorFn = [](SeededRng&, int) {
            WormColor color;
            color.r = 255;
            color.g = 255;
            color.b = 255;
            color.a = 1.0;
            return color;
        };
        const TraceResult result = traceWorms(canvas, opts);
        if (result != TraceResult::Completed) return result;
    }
    return TraceResult::Completed;
}

// Reference StrayHair.asyncInit: one layer of sparse unruly worms.
TraceResult drawStrayHair(StrokeCanvas& canvas, double seed, double density) {
    const double width = canvas.width();
    const double layerSeed = seed * 1000 + 42;
    WormTraceOptions opts = baseOptions(canvas);
    opts.seed = layerSeed;
    opts.density = 0.001 + density * 0.004;
    opts.kink = 5 + std::fmod(layerSeed, 45.0);
    opts.stride = 0.5;
    opts.strideDeviation = 0.25;
    opts.duration = 8 + std::fmod(layerSeed, 8.0);
    opts.behavior = WormBehavior::Unruly;
    opts.flowFreq = 4;
    opts.lineWidth = jsMax(1, width / 400);
    opts.colorFn = [](SeededRng& rng, int) {
        WormColor color;
        color.r = std::floor(rng.nextFloat() * 30);
        color.g = std::floor(rng.nextFloat() * 30);
        color.b = std::floor(rng.nextFloat() * 30);
        color.a = 0.666;
        return color;
    };
    return traceWorms(canvas, opts);
}

} // namespace

std::optional<AsyncOverlay> asyncOverlayForEffect(std::u16string_view effectKey) {
    if (effectKey == u"filter.fibers") return AsyncOverlay::Fibers;
    if (effectKey == u"filter.scratches") return AsyncOverlay::Scratches;
    if (effectKey == u"filter.strayHair") return AsyncOverlay::StrayHair;
    return std::nullopt;
}

TraceResult drawAsyncOverlay(AsyncOverlay overlay, StrokeCanvas& canvas, double seed, std::optional<double> density) {
    canvas.clear();
    switch (overlay) {
    case AsyncOverlay::Fibers:
        return drawFibers(canvas, seed, density.value_or(0.5));
    case AsyncOverlay::Scratches:
        return drawScratches(canvas, seed, density.value_or(0.3));
    case AsyncOverlay::StrayHair:
        return drawStrayHair(canvas, seed, density.value_or(0.5));
    }
    return TraceResult::Failed;
}

} // namespace libnoisemaker
