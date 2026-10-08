#pragma once

// stroke_canvas.h -- CPU model of the browser 2D canvas that the reference
// asyncInit effects (filter/fibers, filter/scratches, filter/strayHair) draw
// on. The reference traces worms on the CPU and strokes each step as a
// round-capped line segment on a 2D context it creates with
// willReadFrequently (shaders/src/cpu/wormTracer.js and each effect's
// definition.js), which pins the canvas to Skia's CPU raster back end, then
// uploads the canvas as the overlay texture. This class reproduces that
// canvas pixel for pixel, as Chromium 153 (Skia 9d07e5ba) draws it:
//
//   - Blink culls a stroke whose bounding box, outset by lineWidth / 2,
//     does not intersect the canvas, and prunes a path whose points
//     coincide (Canvas2DRecorderContext::DrawPathInternal).
//   - "rgba(r, g, b, a)" is parsed to an 8-bit colour: channels clamp and
//     round, alpha becomes floor(a * 255 + 0.5).
//   - A stroke at most 1 px wide is a hairline (modifyPaintForHairlines: a
//     thinner width w scales alpha by (int)(w * 256) >> 8), drawn by
//     SkScan::AntiHairRoundPath in 26.6 and 16.16 fixed point. A wider
//     stroke becomes a fill path (SkStroke) filled by SkScan::AntiFillPath
//     with analytic anti-aliasing (stroke_fill.cpp).
//   - Pixels: the legacy N32 blitters (SkARGB32_Blitter and its opaque and
//     black variants) in their integer arithmetic.
//   - The upload (texImage2D with UNPACK_PREMULTIPLY_ALPHA_WEBGL false, and
//     the getImageData the reference's WebGPU path reads) divides out alpha
//     as Skia's raster pipeline does: c * (1 / 255.0f) times the float
//     reciprocal of a * (1 / 255.0f), times 255, rounded to nearest even.
//
// The rasterizer follows noisemaker-for-rust-gpu's crates/noisemaker-host
// raster.rs, which ports the same Skia code. Graded by
// parity/check_async_overlay.mjs against Chromium's canvas on any host.

#include <cstdint>
#include <vector>

namespace nm {

class StrokeCanvas {
public:
    // A transparent canvas. Width and height must be positive.
    StrokeCanvas(int width, int height);

    int width() const { return m_width; }
    int height() const { return m_height; }

    // clearRect(0, 0, width, height).
    void clear();

    // ctx.lineWidth = width. Ignores non-finite and non-positive values,
    // as the canvas does.
    void setLineWidth(double width);

    // ctx.strokeStyle = `rgba(${r}, ${g}, ${b}, ${alpha})`, the only style
    // form the reference tracer uses. Channels are clamped to 0..255 and
    // rounded; alpha is clamped to 0..1. A non-finite value makes the style
    // string invalid, so the canvas keeps its previous style.
    void setStrokeColor(double r, double g, double b, double alpha);

    // ctx.beginPath(); ctx.moveTo(x0, y0); ctx.lineTo(x1, y1); ctx.stroke()
    // with lineCap and lineJoin "round". Non-finite points draw nothing.
    void strokeLine(double x0, double y0, double x1, double y1);

    // Premultiplied RGBA8, row 0 at the top (the canvas backing store).
    std::vector<std::uint8_t> premultiplied() const;

    // Straight-alpha RGBA8, row 0 at the top: the bytes WebGL receives from
    // texImage2D(canvas) with UNPACK_PREMULTIPLY_ALPHA_WEBGL false.
    std::vector<std::uint8_t> unpremultipliedRgba8() const;

private:
    int m_width = 0;
    int m_height = 0;
    float m_lineWidth = 1.0f;
    std::uint8_t m_color[4] = {0, 0, 0, 255}; // straight RGBA8, as Blink stores the style
    std::vector<std::uint32_t> m_pixels;     // SkPMColor: premultiplied, alpha in bits 24..31
};

// The WebGL upload conversion of premultiplied canvas pixels (RGBA8) to
// straight alpha, as unpremultipliedRgba8() applies it.
std::vector<std::uint8_t> unpremultiplyForUpload(const std::vector<std::uint8_t>& premultiplied);

} // namespace nm
