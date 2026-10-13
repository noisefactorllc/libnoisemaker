#pragma once
// async_overlays.h -- the asyncInit overlays of filter/fibers,
// filter/scratches and filter/strayHair (shaders/effects/filter/<name>/
// definition.js), the only catalog effects with an asyncInit. Each draws
// layers of worms with the reference CPU worm tracer on a canvas the size of
// the pipeline, from its seed and density parameters.
//
// The reference uploads the canvas as the node's overlayTex with
// updateTextureFromSource(flipY true). The canvas is created with
// willReadFrequently, so the WebGPU backend writes its getImageData bytes
// (StrokeCanvas::unpremultipliedRgba8) with the rows reversed.

#include "host/worm_tracer.h"

#include <optional>
#include <string_view>

namespace libnoisemaker {

class StrokeCanvas;

enum class AsyncOverlay { Fibers, Scratches, StrayHair };

// The overlay of an effect key ("filter.fibers", "filter.scratches",
// "filter.strayHair"), or none.
std::optional<AsyncOverlay> asyncOverlayForEffect(std::u16string_view effectKey);

// The effect's asyncInit drawn on `canvas`, which is the pipeline's width and
// height and is cleared first. `seed` is `params.seed || 1` and `density`
// `params.density` as numbers, the caller applying JavaScript's conversions;
// nullopt is an undefined density, which takes the effect's default.
// Completed, or Failed when a layer's trace fails: the reference's asyncInit
// then rejects, leaving the layers drawn before it on the canvas.
TraceResult drawAsyncOverlay(AsyncOverlay overlay, StrokeCanvas& canvas, double seed, std::optional<double> density);

} // namespace libnoisemaker
