#include "stroke_canvas.h"

#include "stroke_raster.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

// This file and stroke_fill.cpp port parts of Skia (https://skia.org) at
// commit 9d07e5bad9e3e21da2426946e589daa647218271: src/core/SkDraw.cpp,
// SkScan_Hairline.cpp, SkScan_Antihair.cpp, SkScan_AntiPath.cpp,
// SkScan_AAAPath.cpp, SkAnalyticEdge.cpp, SkEdgeBuilder.cpp,
// SkEdgeClipper.cpp, SkLineClipper.cpp, SkStroke.cpp, SkStrokerPriv.cpp,
// SkGeometry.cpp, SkPathPriv.cpp, SkPoint.cpp, SkBlitter.cpp,
// SkBlitter_ARGB32.cpp, SkTSort.h, SkColorData.h, SkColorPriv.h and
// src/opts/SkBlitRow_opts.h and SkBlitMask_opts.h, copyright Google Inc.,
// Google LLC and The Android Open Source Project. Skia's license:
//
// Copyright (c) 2011 Google Inc. All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//   * Redistributions of source code must retain the above copyright
//     notice, this list of conditions and the following disclaimer.
//
//   * Redistributions in binary form must reproduce the above copyright
//     notice, this list of conditions and the following disclaimer in
//     the documentation and/or other materials provided with the
//     distribution.
//
//   * Neither the name of the copyright holder nor the names of its
//     contributors may be used to endorse or promote products derived
//     from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

namespace nm {

using namespace raster;

namespace {

// ---------------------------------------------------------------- pixel math

// An SkPMColor: premultiplied, alpha in bits 24..31 and red, green, blue in
// bits 16, 8 and 0. The blend arithmetic treats every channel alike, so the
// order only matters for alpha.
using PmColor = std::uint32_t;

constexpr std::uint32_t kAShift = 24;

std::uint32_t packedA(PmColor c) { return c >> kAShift; }

PmColor packArgb(std::uint32_t a, std::uint32_t r, std::uint32_t g, std::uint32_t b) {
    return (a << 24) | (r << 16) | (g << 8) | b;
}

// SkAlpha255To256.
std::uint32_t alpha255To256(std::uint32_t alpha) { return alpha + 1; }

// SkMulDiv255Round.
std::uint32_t mulDiv255Round(std::uint32_t a, std::uint32_t b) {
    const std::uint32_t prod = a * b + 128;
    return (prod + (prod >> 8)) >> 8;
}

// SkPreMultiplyColor of a straight 8-bit colour.
PmColor premultiply(const std::uint8_t color[4]) {
    const std::uint32_t r = color[0], g = color[1], b = color[2], a = color[3];
    if (a == 255) return packArgb(a, r, g, b);
    return packArgb(a, mulDiv255Round(r, a), mulDiv255Round(g, a), mulDiv255Round(b, a));
}

// SkAlphaMulQ: every channel times `scale` (0..256), shifted down 8.
PmColor alphaMulQ(PmColor c, std::uint32_t scale) {
    constexpr std::uint32_t kMask = 0x00FF00FFu;
    const std::uint32_t rb = ((c & kMask) * scale) >> 8;
    const std::uint32_t ag = ((c >> 8) & kMask) * scale;
    return (rb & kMask) | (ag & ~kMask);
}

// SkAlphaMulInv256.
std::uint32_t alphaMulInv256(std::uint32_t value, std::uint32_t alpha256) {
    const std::uint32_t prod = 0xFFFFu - value * alpha256;
    return (prod + (prod >> 8)) >> 8;
}

// SkBlendARGB32(src, dst, aa).
PmColor blendArgb32(PmColor src, PmColor dst, std::uint32_t aa) {
    constexpr std::uint32_t kMask = 0x00FF00FFu;
    const std::uint32_t srcScale = alpha255To256(aa);
    const std::uint32_t dstScale = alphaMulInv256(packedA(src), srcScale);
    const std::uint32_t srcRb = (src & kMask) * srcScale;
    const std::uint32_t srcAg = ((src >> 8) & kMask) * srcScale;
    const std::uint32_t dstRb = (dst & kMask) * dstScale;
    const std::uint32_t dstAg = ((dst >> 8) & kMask) * dstScale;
    return (((srcRb + dstRb) >> 8) & kMask) | ((srcAg + dstAg) & ~kMask);
}

// SkFastFourByteInterp(src, dst, srcWeight) (the 64-bit form).
PmColor fastFourByteInterp(PmColor src, PmColor dst, std::uint32_t weight) {
    const std::uint64_t scale = weight + (weight >> 7);
    auto splay = [](std::uint32_t c) {
        return (static_cast<std::uint64_t>((c >> 8) & 0x00FF00FFu) << 32) | (c & 0x00FF00FFu);
    };
    const std::uint64_t agrb = splay(src) * scale + (256 - scale) * splay(dst);
    constexpr std::uint64_t kMask = 0xFF00FF00ull;
    return static_cast<PmColor>(((agrb & kMask) >> 8) | ((agrb >> 32) & kMask));
}

// SkBlitRow::Color32 on one pixel: memset for an opaque colour, nothing for
// a transparent one, else blit_row_color32's (d * (256 - a)) >> 8 + c per
// channel.
void color32(PmColor& dst, PmColor color) {
    const std::uint32_t a = packedA(color);
    if (a == 0) return;
    if (a == 255) {
        dst = color;
        return;
    }
    const std::uint32_t inv = 256 - a;
    std::uint32_t out = 0;
    for (std::uint32_t shift : {0u, 8u, 16u, 24u}) {
        const std::uint32_t d = (dst >> shift) & 0xFFu;
        const std::uint32_t c = (color >> shift) & 0xFFu;
        out |= ((((d * inv) >> 8) + c) & 0xFFu) << shift;
    }
    dst = out;
}

// ---------------------------------------------------------------- blitters

// Which legacy N32 blitter SkBlitter::Choose returns for the paint.
enum class BlitterKind {
    Translucent, // SkARGB32_Blitter: alpha below 255
    Opaque,      // SkARGB32_Opaque_Blitter
    Black,       // SkARGB32_Black_Blitter: opaque black
};

// The device blitter: a solid colour over the canvas pixels.
class ArgbBlitter final : public Blitter {
public:
    ArgbBlitter(std::vector<PmColor>& pixels, std::size_t width, const std::uint8_t color[4])
        : m_pixels(pixels), m_width(width), m_pm(premultiply(color)), m_srcA(color[3]) {
        if (color[0] == 0 && color[1] == 0 && color[2] == 0 && color[3] == 255) {
            m_kind = BlitterKind::Black;
        } else if (color[3] == 255) {
            m_kind = BlitterKind::Opaque;
        } else {
            m_kind = BlitterKind::Translucent;
        }
    }

    BlitterKind kind() const { return m_kind; }
    std::uint32_t srcA() const { return m_srcA; }

    void blitH(std::int32_t x, std::int32_t y, std::int32_t width) override {
        for (std::int32_t i = 0; i < width; ++i) color32(px(x + i, y), m_pm);
    }

    void blitAntiH(std::int32_t x, std::int32_t y, const std::uint8_t* aa, std::int32_t count) override {
        if (m_kind == BlitterKind::Translucent && m_srcA == 0) return;
        for (std::int32_t i = 0; i < count; ++i) {
            const std::uint32_t a = aa[i];
            if (a == 0) continue;
            PmColor& p = px(x + i, y);
            if (m_kind == BlitterKind::Black) {
                if (a == 255) {
                    p = 0xFF000000u;
                } else {
                    p = (a << kAShift) + alphaMulQ(p, alpha255To256(255 - a));
                }
            } else if (m_kind == BlitterKind::Opaque && a == 255) {
                p = m_pm;
            } else {
                color32(p, alphaMulQ(m_pm, alpha255To256(a)));
            }
        }
    }

    void blitV(std::int32_t x, std::int32_t y, std::int32_t height, std::uint8_t alpha) override {
        if (alpha == 0 || m_srcA == 0) return;
        PmColor color = m_pm;
        if (alpha != 255) color = alphaMulQ(color, alpha255To256(alpha));
        const std::uint32_t dstScale = alpha255To256(255 - packedA(color));
        for (std::int32_t row = 0; row < height; ++row) {
            PmColor& p = px(x, y + row);
            p = color + alphaMulQ(p, dstScale);
        }
    }

    void blitRect(std::int32_t x, std::int32_t y, std::int32_t width, std::int32_t height) override {
        if (m_srcA == 0) return;
        for (std::int32_t row = 0; row < height; ++row) blitH(x, y + row, width);
    }

    void blitAntiH2(std::int32_t x, std::int32_t y, std::uint8_t a0, std::uint8_t a1) override {
        blendPixel(x, y, a0);
        blendPixel(x + 1, y, a1);
    }

    void blitAntiV2(std::int32_t x, std::int32_t y, std::uint8_t a0, std::uint8_t a1) override {
        blendPixel(x, y, a0);
        blendPixel(x, y + 1, a1);
    }

    void blitMask(const Mask& mask, const IRect& clip) override {
        // SkARGB32_Blitter::blitMask returns for a transparent colour; every
        // variant then blits through blit_color -> SkOpts::blit_mask_d32_a8.
        if (m_kind == BlitterKind::Translucent && m_srcA == 0) return;
        const std::uint32_t colorAlpha = packedA(m_pm);
        for (std::int32_t y = clip.top; y < clip.bottom; ++y) {
            for (std::int32_t x = clip.left; x < clip.right; ++x) {
                const std::uint32_t m = mask.get(x, y);
                PmColor& p = px(x, y);
                if (m_kind == BlitterKind::Black) {
                    p = alphaMulQ(p, 256 - m) + (m << kAShift);
                    continue;
                }
                const std::uint32_t m256 = alpha255To256(m);
                const std::uint32_t scale =
                    m_kind == BlitterKind::Translucent ? 256 - ((colorAlpha * m256) >> 8) : 256 - m;
                // Per channel in the NEON form: two u8 products, added with
                // u8 wrap-around.
                std::uint32_t out = 0;
                for (std::uint32_t shift : {0u, 8u, 16u, 24u}) {
                    const std::uint32_t c = (m_pm >> shift) & 0xFFu;
                    const std::uint32_t d = (p >> shift) & 0xFFu;
                    out |= ((((c * m256) >> 8) + ((d * scale) >> 8)) & 0xFFu) << shift;
                }
                p = out;
            }
        }
    }

private:
    PmColor& px(std::int32_t x, std::int32_t y) {
        return m_pixels[static_cast<std::size_t>(y) * m_width + static_cast<std::size_t>(x)];
    }

    // One pixel of blitAntiH2 / blitAntiV2.
    void blendPixel(std::int32_t x, std::int32_t y, std::uint32_t a) {
        PmColor& p = px(x, y);
        switch (m_kind) {
        case BlitterKind::Translucent: p = blendArgb32(m_pm, p, a); break;
        case BlitterKind::Opaque: p = fastFourByteInterp(m_pm, p, a); break;
        case BlitterKind::Black: p = (a << kAShift) + alphaMulQ(p, 256 - a); break;
        }
    }

    std::vector<PmColor>& m_pixels;
    std::size_t m_width;
    BlitterKind m_kind = BlitterKind::Translucent;
    PmColor m_pm;    // fPMColor
    std::uint32_t m_srcA; // fSrcA
};

// ------------------------------------------------- anti-aliased hairlines

using FDot6 = std::int32_t; // 26.6 fixed point (SkFDot6)
using Fixed = std::int32_t; // 16.16 fixed point (SkFixed)

constexpr std::int32_t kFDot6One = 64;
constexpr std::int32_t kFDot6Half = 32;
constexpr std::int32_t kFixedHalf = 1 << 15;

std::int32_t fdot6Floor(FDot6 x) { return x >> 6; }
std::int32_t fdot6Ceil(FDot6 x) { return wadd(x, 63) >> 6; }
Fixed fdot6ToFixed(FDot6 x) { return shl(x, 10); }
std::int32_t fixedFloor(Fixed x) { return x >> 16; }
std::int32_t fixedCeil(Fixed x) { return wadd(x, 0xFFFF) >> 16; }

// fastfixdiv: (a << 16) / b.
Fixed fastFixDiv(FDot6 a, FDot6 b) { return shl(a, 16) / b; }

std::int32_t fd6Frac(FDot6 x) { return x & (kFDot6One - 1); }

// partial_pixel_coverage.
std::int32_t partialPixelCoverage(FDot6 pos) { return fd6Frac(pos - 1) + 1; }

// scale_alpha_by_coverage.
std::uint8_t scaleAlpha(std::uint32_t value, std::int32_t coverage) {
    return static_cast<std::uint8_t>((value * static_cast<std::uint32_t>(coverage)) >> 6);
}

// fixed_to_alpha.
std::uint32_t fixedToAlpha(Fixed f) { return static_cast<std::uint32_t>((f >> 8) & 0xFF); }

// call_hline_blitter: `count` pixels of one coverage.
void hline(Blitter& blitter, std::int32_t x, std::int32_t y, std::int32_t count, std::uint8_t alpha) {
    const std::vector<std::uint8_t> aa(static_cast<std::size_t>(std::max(count, 0)), alpha);
    blitter.blitAntiH(x, y, aa.data(), count);
}

// The four SkAntiHairBlitter strategies by slope.
enum class HairKind { HLine, Horish, VLine, Vertish };

// drawCap(x, fy, slope, coverage).
Fixed drawCap(HairKind kind, Blitter& blitter, std::int32_t x, Fixed f, Fixed slope, std::int32_t coverage) {
    f = wadd(f, kFixedHalf);
    const std::int32_t i = fixedFloor(f);
    const std::uint32_t a = fixedToAlpha(f);
    switch (kind) {
    case HairKind::HLine: {
        std::uint8_t ma = scaleAlpha(a, coverage);
        if (ma != 0) hline(blitter, x, i, 1, ma);
        ma = scaleAlpha(255 - a, coverage);
        if (ma != 0) hline(blitter, x, i - 1, 1, ma);
        return wsub(f, kFixedHalf);
    }
    case HairKind::Horish:
        blitter.blitAntiV2(x, i - 1, scaleAlpha(255 - a, coverage), scaleAlpha(a, coverage));
        return wsub(wadd(f, slope), kFixedHalf);
    case HairKind::VLine: {
        std::uint8_t ma = scaleAlpha(a, coverage);
        if (ma != 0) blitter.blitV(i, x, 1, ma);
        ma = scaleAlpha(255 - a, coverage);
        if (ma != 0) blitter.blitV(i - 1, x, 1, ma);
        return wsub(f, kFixedHalf);
    }
    case HairKind::Vertish:
        blitter.blitAntiH2(i - 1, x, scaleAlpha(255 - a, coverage), scaleAlpha(a, coverage));
        return wsub(wadd(f, slope), kFixedHalf);
    }
    return f;
}

// drawLine(x, stopx, fy, slope).
Fixed drawLine(HairKind kind, Blitter& blitter, std::int32_t start, std::int32_t stop, Fixed f, Fixed slope) {
    f = wadd(f, kFixedHalf);
    switch (kind) {
    case HairKind::HLine: {
        const std::int32_t y = fixedFloor(f);
        std::uint32_t a = fixedToAlpha(f);
        if (a != 0) hline(blitter, start, y, stop - start, static_cast<std::uint8_t>(a));
        a = 255 - a;
        if (a != 0) hline(blitter, start, y - 1, stop - start, static_cast<std::uint8_t>(a));
        break;
    }
    case HairKind::Horish:
        for (std::int32_t x = start; x < stop; ++x) {
            const std::int32_t lower = fixedFloor(f);
            const std::uint32_t a = fixedToAlpha(f);
            blitter.blitAntiV2(x, lower - 1, static_cast<std::uint8_t>(255 - a), static_cast<std::uint8_t>(a));
            f = wadd(f, slope);
        }
        break;
    case HairKind::VLine: {
        const std::int32_t x = fixedFloor(f);
        std::uint32_t a = fixedToAlpha(f);
        if (a != 0) blitter.blitV(x, start, stop - start, static_cast<std::uint8_t>(a));
        a = 255 - a;
        if (a != 0) blitter.blitV(x - 1, start, stop - start, static_cast<std::uint8_t>(a));
        break;
    }
    case HairKind::Vertish:
        for (std::int32_t y = start; y < stop; ++y) {
            const std::int32_t x = fixedFloor(f);
            const std::uint32_t a = fixedToAlpha(f);
            blitter.blitAntiH2(x - 1, y, static_cast<std::uint8_t>(255 - a), static_cast<std::uint8_t>(a));
            f = wadd(f, slope);
        }
        break;
    }
    return wsub(f, kFixedHalf);
}

// The tail of do_anti_hairline: the start cap, the full spans and the stop
// cap.
void drawHair(HairKind kind, Blitter& blitter, std::int32_t istart, std::int32_t istop, Fixed fstart, Fixed slope,
              std::int32_t startCoverage, std::int32_t stopCoverage) {
    Fixed f = drawCap(kind, blitter, istart, fstart, slope, startCoverage);
    istart += 1;
    const std::int32_t fullSpans = istop - istart - (stopCoverage > 0 ? 1 : 0);
    if (fullSpans > 0) f = drawLine(kind, blitter, istart, istart + fullSpans, f, slope);
    if (stopCoverage > 0) drawCap(kind, blitter, istop - 1, f, slope, stopCoverage);
}

// do_anti_hairline(x0, y0, x1, y1, clip, blitter); `clip` null for none.
void doAntiHairline(FDot6 x0, FDot6 y0, FDot6 x1, FDot6 y1, const IRect* clipIn, Blitter& blitter) {
    // Integer NaN (0x80000000): a huge or non-finite coordinate.
    constexpr std::int32_t kNaN = std::numeric_limits<std::int32_t>::min();
    if (x0 == kNaN || y0 == kNaN || x1 == kNaN || y1 == kNaN) return;
    if (wabs(wsub(x1, x0)) > 511 * kFDot6One || wabs(wsub(y1, y0)) > 511 * kFDot6One) {
        const FDot6 hx = (x0 >> 1) + (x1 >> 1);
        const FDot6 hy = (y0 >> 1) + (y1 >> 1);
        doAntiHairline(x0, y0, hx, hy, clipIn, blitter);
        doAntiHairline(hx, hy, x1, y1, clipIn, blitter);
        return;
    }

    bool clipped = clipIn != nullptr;
    const IRect clip = clipped ? *clipIn : IRect{};
    std::int32_t istart = 0;
    std::int32_t istop = 0;
    Fixed fstart = 0;
    Fixed slope = 0;
    HairKind kind = HairKind::HLine;
    std::int32_t startCoverage = 0;
    std::int32_t stopCoverage = 0;

    if (std::abs(x1 - x0) > std::abs(y1 - y0)) {
        // Mostly horizontal: left to right.
        if (x0 > x1) {
            std::swap(x0, x1);
            std::swap(y0, y1);
        }
        istart = fdot6Floor(x0);
        istop = fdot6Ceil(x1);
        if (y0 == y1) {
            slope = 0;
            kind = HairKind::HLine;
            fstart = fdot6ToFixed(y0);
        } else {
            slope = fastFixDiv(y1 - y0, x1 - x0);
            const std::int32_t dxToCenter = kFDot6Half - fd6Frac(x0);
            fstart = wadd(fdot6ToFixed(y0), wadd(wmul(slope, dxToCenter), kFDot6Half) >> 6);
            kind = HairKind::Horish;
        }
        if (istop - istart == 1) {
            startCoverage = x1 - x0;
            stopCoverage = 0;
        } else {
            startCoverage = kFDot6One - fd6Frac(x0);
            stopCoverage = fd6Frac(x1);
        }
        if (clipped) {
            if (istart >= clip.right || istop <= clip.left) return;
            if (istart < clip.left) {
                fstart = wadd(fstart, wmul(slope, clip.left - istart));
                istart = clip.left;
                startCoverage = kFDot6One;
                if (istop - istart == 1) {
                    startCoverage = partialPixelCoverage(x1);
                    stopCoverage = 0;
                }
            }
            if (istop > clip.right) {
                istop = clip.right;
                stopCoverage = 0;
            }
            if (istart == istop) return;
            const Fixed span = wmul(slope, istop - istart - 1);
            std::int32_t top = 0;
            std::int32_t bottom = 0;
            if (slope >= 0) {
                top = fixedFloor(wsub(fstart, kFixedHalf));
                bottom = fixedCeil(wadd(wadd(fstart, span), kFixedHalf));
            } else {
                bottom = fixedCeil(wadd(fstart, kFixedHalf));
                top = fixedFloor(wsub(wadd(fstart, span), kFixedHalf));
            }
            top -= 1;
            bottom += 1;
            if (top >= clip.bottom || bottom <= clip.top) return;
            if (clip.top <= top && clip.bottom >= bottom) clipped = false;
        }
    } else {
        // Mostly vertical: top to bottom.
        if (y0 > y1) {
            std::swap(x0, x1);
            std::swap(y0, y1);
        }
        istart = fdot6Floor(y0);
        istop = fdot6Ceil(y1);
        if (x0 == x1) {
            if (y0 == y1) return;
            slope = 0;
            kind = HairKind::VLine;
            fstart = fdot6ToFixed(x0);
        } else {
            slope = fastFixDiv(x1 - x0, y1 - y0);
            const std::int32_t dyToCenter = kFDot6Half - fd6Frac(y0);
            fstart = wadd(fdot6ToFixed(x0), wadd(wmul(slope, dyToCenter), kFDot6Half) >> 6);
            kind = HairKind::Vertish;
        }
        if (istop - istart == 1) {
            startCoverage = y1 - y0;
            stopCoverage = 0;
        } else {
            startCoverage = kFDot6One - fd6Frac(y0);
            stopCoverage = fd6Frac(y1);
        }
        if (clipped) {
            if (istart >= clip.bottom || istop <= clip.top) return;
            if (istart < clip.top) {
                fstart = wadd(fstart, wmul(slope, clip.top - istart));
                istart = clip.top;
                startCoverage = kFDot6One;
                if (istop - istart == 1) {
                    startCoverage = partialPixelCoverage(y1);
                    stopCoverage = 0;
                }
            }
            if (istop > clip.bottom) {
                istop = clip.bottom;
                stopCoverage = 0;
            }
            if (istart == istop) return;
            const Fixed span = wmul(slope, istop - istart - 1);
            std::int32_t left = 0;
            std::int32_t right = 0;
            if (slope >= 0) {
                left = fixedFloor(wsub(fstart, kFixedHalf));
                right = fixedCeil(wadd(wadd(fstart, span), kFixedHalf));
            } else {
                right = fixedCeil(wadd(fstart, kFixedHalf));
                left = fixedFloor(wsub(wadd(fstart, span), kFixedHalf));
            }
            left -= 1;
            right += 1;
            if (left >= clip.right || right <= clip.left) return;
            if (clip.left <= left && clip.right >= right) clipped = false;
        }
    }

    if (clipped) {
        RectClipBlitter clippedBlitter(blitter, clip);
        drawHair(kind, clippedBlitter, istart, istop, fstart, slope, startCoverage, stopCoverage);
    } else {
        drawHair(kind, blitter, istart, istop, fstart, slope, startCoverage, stopCoverage);
    }
}

// ------------------------------------------------------------ line clipper

constexpr float kScalarNearlyZero = 1.0f / 4096.0f;

// sk_float_midpoint.
float midpoint(float a, float b) {
    return static_cast<float>((static_cast<double>(a) + static_cast<double>(b)) * 0.5);
}

bool nestedLt(float a, float b, float dim) { return a <= b && (a < b || dim > 0.0f); }

// SkLineClipper::IntersectLine(src, clip, dst).
bool intersectLine(const Pt src[2], const Rect& clip, Pt dst[2]) {
    const Rect bounds{std::fmin(src[0].x, src[1].x), std::fmin(src[0].y, src[1].y), std::fmax(src[0].x, src[1].x),
                      std::fmax(src[0].y, src[1].y)};
    if (clip.left <= bounds.left && clip.top <= bounds.top && clip.right >= bounds.right
        && clip.bottom >= bounds.bottom) {
        dst[0] = src[0];
        dst[1] = src[1];
        return true;
    }
    const float bw = bounds.right - bounds.left;
    const float bh = bounds.bottom - bounds.top;
    if (nestedLt(bounds.right, clip.left, bw) || nestedLt(clip.right, bounds.left, bw)
        || nestedLt(bounds.bottom, clip.top, bh) || nestedLt(clip.bottom, bounds.top, bh)) {
        return false;
    }
    int i0 = src[0].y < src[1].y ? 0 : 1;
    int i1 = 1 - i0;
    Pt tmp[2] = {src[0], src[1]};
    if (tmp[i0].y < clip.top) tmp[i0] = {sectWithHorizontal(src, clip.top), clip.top};
    if (tmp[i1].y > clip.bottom) tmp[i1] = {sectWithHorizontal(src, clip.bottom), clip.bottom};
    i0 = tmp[0].x < tmp[1].x ? 0 : 1;
    i1 = 1 - i0;
    if ((tmp[i1].x <= clip.left || tmp[i0].x >= clip.right)
        && (tmp[0].x != tmp[1].x || tmp[0].x < clip.left || tmp[0].x > clip.right)) {
        return false;
    }
    if (tmp[i0].x < clip.left) tmp[i0] = {clip.left, sectWithVertical(tmp, clip.left)};
    if (tmp[i1].x > clip.right) tmp[i1] = {clip.right, sectWithVertical(tmp, clip.right)};
    dst[0] = tmp[0];
    dst[1] = tmp[1];
    return true;
}

// SkScan::AntiHairLineRgn for one segment, `clip` the device clip (the
// canvas) or null when the segment's bounds lie inside it.
void antiHairLine(Pt pts[2], const IRect* clip, Blitter& blitter) {
    constexpr float kMax = 32767.0f;
    const Rect fixedBounds{-kMax, -kMax, kMax, kMax};
    Pt clippedPts[2];
    if (!intersectLine(pts, fixedBounds, clippedPts)) return;
    if (clip) {
        const Rect clipBounds{static_cast<float>(clip->left) - 1.0f, static_cast<float>(clip->top) - 1.0f,
                              static_cast<float>(clip->right) + 1.0f, static_cast<float>(clip->bottom) + 1.0f};
        Pt next[2];
        if (!intersectLine(clippedPts, clipBounds, next)) return;
        clippedPts[0] = next[0];
        clippedPts[1] = next[1];
    }
    const FDot6 x0 = toFDot6(clippedPts[0].x);
    const FDot6 y0 = toFDot6(clippedPts[0].y);
    const FDot6 x1 = toFDot6(clippedPts[1].x);
    const FDot6 y1 = toFDot6(clippedPts[1].y);
    if (clip) {
        const IRect ir{fdot6Floor(std::min(x0, x1)) - 1, fdot6Floor(std::min(y0, y1)) - 1,
                       fdot6Ceil(std::max(x0, x1)) + 1, fdot6Ceil(std::max(y0, y1)) + 1};
        if (!clip->intersects(ir)) return;
        if (!clip->contains(ir)) {
            // SkRegion::Cliperator over a rectangular region: the clip
            // intersected with the segment's bounds.
            IRect r;
            if (clip->intersect(ir, r)) doAntiHairline(x0, y0, x1, y1, &r, blitter);
            return;
        }
    }
    doAntiHairline(x0, y0, x1, y1, nullptr, blitter);
}

// SkScan::AntiHairRoundPath of a moveTo/lineTo path: hair_path with
// extend_pts<kRound_Cap> at both ends.
void antiHairRoundLine(Pt p0, Pt p1, const IRect& canvas, Blitter& blitter) {
    // The path's bounds, rounded out and outset by 2 for the caps.
    const IRect ibounds{wsub(toI32(std::floor(std::fmin(p0.x, p1.x))), 2),
                        wsub(toI32(std::floor(std::fmin(p0.y, p1.y))), 2),
                        wadd(toI32(std::ceil(std::fmax(p0.x, p1.x))), 2),
                        wadd(toI32(std::ceil(std::fmax(p0.y, p1.y))), 2)};
    if (!canvas.intersects(ibounds)) return;
    const IRect* clip = canvas.contains(ibounds) ? nullptr : &canvas;

    // extend_pts: each end moves out along its tangent by the area of a half
    // disc of radius 1/2 (pi/8). The end is extended after the start has
    // moved, from the moved start.
    const float capOutset = static_cast<float>(3.14159265358979323846) / 8.0f;
    Pt pts[2] = {p0, p1};
    Pt firstTangent;
    if (!normalize(pts[0].x - pts[1].x, pts[0].y - pts[1].y, firstTangent)) firstTangent = {1.0f, 0.0f};
    pts[0].x += firstTangent.x * capOutset;
    pts[0].y += firstTangent.y * capOutset;
    Pt lastTangent;
    if (!normalize(pts[1].x - pts[0].x, pts[1].y - pts[0].y, lastTangent)) lastTangent = {-1.0f, 0.0f};
    pts[1].x += lastTangent.x * capOutset;
    pts[1].y += lastTangent.y * capOutset;

    antiHairLine(pts, clip, blitter);
}

// One texel of the WebGL upload of a canvas created with willReadFrequently,
// which Chromium unpremultiplies as getImageData does (Skia's raster
// pipeline): each channel read as c * (1 / 255.0f), multiplied by the float
// reciprocal of a * (1 / 255.0f) (0 where that reciprocal is infinite),
// clamped to [0, 1], and stored as the product with 255 rounded to nearest,
// ties to even.
void readBackTexel(const std::uint8_t* in, std::uint8_t* out) {
    const auto fromByte = [](std::uint8_t b) { return static_cast<float>(b) * (1.0f / 255.0f); };
    const float reciprocal = 1.0f / fromByte(in[3]);
    const float scale = reciprocal < std::numeric_limits<float>::infinity() ? reciprocal : 0.0f;
    const auto unorm = [](float v) {
        const float clamped = std::min(std::max(v, 0.0f), 1.0f);
        return static_cast<std::uint8_t>(std::nearbyint(clamped * 255.0f));
    };
    out[0] = unorm(fromByte(in[0]) * scale);
    out[1] = unorm(fromByte(in[1]) * scale);
    out[2] = unorm(fromByte(in[2]) * scale);
    out[3] = unorm(fromByte(in[3]));
}

} // namespace

// ------------------------------------------------------- shared helpers

namespace raster {

float sectWithHorizontal(const Pt src[2], float y) {
    const float dy = src[1].y - src[0].y;
    if (std::fabs(dy) <= kScalarNearlyZero) return midpoint(src[0].x, src[1].x);
    const double x0 = src[0].x, y0 = src[0].y, x1 = src[1].x, y1 = src[1].y;
    const double result = x0 + (static_cast<double>(y) - y0) * (x1 - x0) / (y1 - y0);
    return static_cast<float>(pinUnsorted(result, x0, x1));
}

float sectWithVertical(const Pt src[2], float x) {
    const float dx = src[1].x - src[0].x;
    if (std::fabs(dx) <= kScalarNearlyZero) return midpoint(src[0].y, src[1].y);
    const double x0 = src[0].x, y0 = src[0].y, x1 = src[1].x, y1 = src[1].y;
    return static_cast<float>(y0 + (static_cast<double>(x) - x0) * (y1 - y0) / (x1 - x0));
}

double pinUnsorted(double value, double lo, double hi) {
    if (hi < lo) std::swap(lo, hi);
    if (value < lo) return lo;
    if (value > hi) return hi;
    return value;
}

bool normalize(float x, float y, Pt& out) {
    const double xx = x, yy = y;
    const double dmag = std::sqrt(xx * xx + yy * yy);
    const double dscale = 1.0 / dmag;
    const float nx = static_cast<float>(static_cast<double>(x) * dscale);
    const float ny = static_cast<float>(static_cast<double>(y) * dscale);
    if (!std::isfinite(nx) || !std::isfinite(ny) || (nx == 0.0f && ny == 0.0f)) return false;
    out = {nx, ny};
    return true;
}

void RectClipBlitter::blitH(std::int32_t x, std::int32_t y, std::int32_t width) {
    if (y < m_clip.top || y >= m_clip.bottom) return;
    const std::int32_t left = std::max(x, m_clip.left);
    const std::int32_t right = std::min(x + width, m_clip.right);
    if (right > left) m_inner.blitH(left, y, right - left);
}

void RectClipBlitter::blitAntiH(std::int32_t x, std::int32_t y, const std::uint8_t* aa, std::int32_t count) {
    if (y < m_clip.top || y >= m_clip.bottom || x >= m_clip.right) return;
    const std::int32_t x1 = x + count;
    if (x1 <= m_clip.left) return;
    const std::int32_t x0 = std::max(x, m_clip.left);
    const std::int32_t end = std::min(x1, m_clip.right);
    m_inner.blitAntiH(x0, y, aa + (x0 - x), end - x0);
}

void RectClipBlitter::blitV(std::int32_t x, std::int32_t y, std::int32_t height, std::uint8_t alpha) {
    if (x < m_clip.left || x >= m_clip.right) return;
    const std::int32_t y0 = std::max(y, m_clip.top);
    const std::int32_t y1 = std::min(y + height, m_clip.bottom);
    if (y0 < y1) m_inner.blitV(x, y0, y1 - y0, alpha);
}

void RectClipBlitter::blitRect(std::int32_t x, std::int32_t y, std::int32_t width, std::int32_t height) {
    IRect r;
    if (IRect{x, y, x + width, y + height}.intersect(m_clip, r)) {
        m_inner.blitRect(r.left, r.top, r.right - r.left, r.bottom - r.top);
    }
}

void RectClipBlitter::blitAntiRect(std::int32_t x, std::int32_t y, std::int32_t width, std::int32_t height,
                                   std::uint8_t left, std::uint8_t right) {
    // The true width of the rectangle is width + 2.
    IRect r;
    if (!IRect{x, y, x + width + 2, y + height}.intersect(m_clip, r)) return;
    if (r.left != x) left = 255;
    if (r.right != x + width + 2) right = 255;
    const std::int32_t rw = r.right - r.left;
    const std::int32_t rh = r.bottom - r.top;
    if (left == 255 && right == 255) {
        m_inner.blitRect(r.left, r.top, rw, rh);
    } else if (rw == 1) {
        m_inner.blitV(r.left, r.top, rh, r.left == x ? left : right);
    } else {
        m_inner.blitAntiRect(r.left, r.top, rw - 2, rh, left, right);
    }
}

void RectClipBlitter::blitMask(const Mask& mask, const IRect& clip) {
    IRect r;
    if (clip.intersect(m_clip, r)) m_inner.blitMask(mask, r);
}

} // namespace raster

// ---------------------------------------------------------------- canvas

StrokeCanvas::StrokeCanvas(int width, int height) : m_width(width), m_height(height) {
    if (width <= 0 || height <= 0) {
        throw std::invalid_argument("nm::StrokeCanvas: width and height must be positive");
    }
    m_pixels.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height), 0u);
}

void StrokeCanvas::clear() {
    std::fill(m_pixels.begin(), m_pixels.end(), 0u);
}

void StrokeCanvas::setLineWidth(double width) {
    if (!std::isfinite(width) || width <= 0.0) return;
    m_lineWidth = static_cast<float>(width);
}

void StrokeCanvas::setStrokeColor(double r, double g, double b, double alpha) {
    if (!std::isfinite(r) || !std::isfinite(g) || !std::isfinite(b) || !std::isfinite(alpha)) return;
    auto channel = [](double value) { return toU8(std::floor(std::clamp(value, 0.0, 255.0) + 0.5)); };
    m_color[0] = channel(r);
    m_color[1] = channel(g);
    m_color[2] = channel(b);
    m_color[3] = toU8(std::floor(std::clamp(alpha, 0.0, 1.0) * 255.0 + 0.5));
}

void StrokeCanvas::strokeLine(double x0d, double y0d, double x1d, double y1d) {
    if (!std::isfinite(x0d) || !std::isfinite(y0d) || !std::isfinite(x1d) || !std::isfinite(y1d)) return;
    // Blink keeps path points as floats.
    const float x0 = static_cast<float>(x0d);
    const float y0 = static_cast<float>(y0d);
    const float x1 = static_cast<float>(x1d);
    const float y1 = static_cast<float>(y1d);
    // A path whose points coincide is pruned before stroking.
    if (x0 == x1 && y0 == y1) return;
    // InflateStrokeRect + ComputeDirtyRect: the bounds outset by
    // lineWidth / 2, rounded out, must intersect the canvas.
    const float strokeRadius = 0.5f * m_lineWidth;
    {
        const float left = std::fmin(x0, x1) - strokeRadius;
        const float top = std::fmin(y0, y1) - strokeRadius;
        const float width = (std::fmax(x0, x1) - std::fmin(x0, x1)) + 2.0f * strokeRadius;
        const float height = (std::fmax(y0, y1) - std::fmin(y0, y1)) + 2.0f * strokeRadius;
        const double l = std::floor(static_cast<double>(left));
        const double t = std::floor(static_cast<double>(top));
        const double r = std::ceil(static_cast<double>(left + width));
        const double b = std::ceil(static_cast<double>(top + height));
        if (r <= 0.0 || b <= 0.0 || l >= static_cast<double>(m_width) || t >= static_cast<double>(m_height)) return;
    }
    if (m_color[3] == 0) return;
    const IRect canvas{0, 0, m_width, m_height};
    const std::size_t stride = static_cast<std::size_t>(m_width);
    // modifyPaintForHairlines: fast_len of (w, 0) is w.
    const float w = m_lineWidth;
    if (w <= 1.0f) {
        std::uint8_t color[4] = {m_color[0], m_color[1], m_color[2], m_color[3]};
        if (w != 1.0f) {
            const std::uint32_t scale = toU32(static_cast<double>(w * 256.0f));
            color[3] = static_cast<std::uint8_t>((static_cast<std::uint32_t>(color[3]) * scale) >> 8);
        }
        ArgbBlitter blitter(m_pixels, stride, color);
        if (blitter.kind() == BlitterKind::Translucent && blitter.srcA() == 0) return;
        antiHairRoundLine({x0, y0}, {x1, y1}, canvas, blitter);
    } else {
        ArgbBlitter blitter(m_pixels, stride, m_color);
        fillStroke({x0, y0}, {x1, y1}, w, canvas, blitter);
    }
}

std::vector<std::uint8_t> StrokeCanvas::premultiplied() const {
    std::vector<std::uint8_t> out(m_pixels.size() * 4);
    for (std::size_t i = 0; i < m_pixels.size(); ++i) {
        const std::uint32_t p = m_pixels[i];
        out[i * 4 + 0] = static_cast<std::uint8_t>(p >> 16);
        out[i * 4 + 1] = static_cast<std::uint8_t>(p >> 8);
        out[i * 4 + 2] = static_cast<std::uint8_t>(p);
        out[i * 4 + 3] = static_cast<std::uint8_t>(p >> 24);
    }
    return out;
}

std::vector<std::uint8_t> StrokeCanvas::unpremultipliedRgba8() const {
    return unpremultiplyForUpload(premultiplied());
}

std::vector<std::uint8_t> unpremultiplyForUpload(const std::vector<std::uint8_t>& premultiplied) {
    std::vector<std::uint8_t> out(premultiplied.size());
    for (std::size_t i = 0; i + 3 < premultiplied.size(); i += 4) readBackTexel(&premultiplied[i], &out[i]);
    return out;
}

} // namespace nm
