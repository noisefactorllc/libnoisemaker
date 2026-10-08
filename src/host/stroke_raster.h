#pragma once

// stroke_raster.h -- internal to stroke_canvas.cpp and stroke_fill.cpp: the
// pieces of Skia's CPU raster back end the overlay canvas uses (blitters,
// coverage masks, line clipping), and the conversions they rely on. Not
// installed; see stroke_canvas.h for the model.
//
// A port of Skia (https://skia.org) at commit
// 9d07e5bad9e3e21da2426946e589daa647218271; the license notice is in
// stroke_canvas.cpp and skia-LICENSE.txt.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace nm::raster {

// ------------------------------------------------------------ conversions
// Float to integer conversions that saturate, NaN giving 0: the arithmetic
// this file ports was measured against Chromium's Skia through a model
// that converts this way, and C++ casts leave out-of-range values undefined.

inline std::int32_t toI32(double v) {
    if (std::isnan(v)) return 0;
    if (v >= 2147483647.0) return std::numeric_limits<std::int32_t>::max();
    if (v <= -2147483648.0) return std::numeric_limits<std::int32_t>::min();
    return static_cast<std::int32_t>(v);
}

inline std::uint32_t toU32(double v) {
    if (std::isnan(v) || v <= 0.0) return 0;
    if (v >= 4294967295.0) return std::numeric_limits<std::uint32_t>::max();
    return static_cast<std::uint32_t>(v);
}

inline std::uint8_t toU8(double v) {
    if (std::isnan(v) || v <= 0.0) return 0;
    if (v >= 255.0) return 255;
    return static_cast<std::uint8_t>(v);
}

// Two's-complement wrapping arithmetic on 32-bit integers.
inline std::int32_t wadd(std::int32_t a, std::int32_t b) {
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(a) + static_cast<std::uint32_t>(b));
}
inline std::int32_t wsub(std::int32_t a, std::int32_t b) {
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(a) - static_cast<std::uint32_t>(b));
}
inline std::int32_t wmul(std::int32_t a, std::int32_t b) {
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(a) * static_cast<std::uint32_t>(b));
}
inline std::int32_t wabs(std::int32_t a) {
    return a < 0 ? wsub(0, a) : a;
}
inline std::int32_t shl(std::int32_t a, int n) {
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(a) << n);
}

// Leading zero bits of a 32- or 64-bit value (32 or 64 for zero).
inline int clz32(std::uint32_t v) {
    int n = 0;
    for (std::uint32_t bit = 0x80000000u; bit != 0 && (v & bit) == 0; bit >>= 1) ++n;
    return n;
}
inline int clz64(std::uint64_t v) {
    int n = 0;
    for (std::uint64_t bit = 0x8000000000000000ull; bit != 0 && (v & bit) == 0; bit >>= 1) ++n;
    return n;
}

// ------------------------------------------------------------ geometry

struct Pt {
    float x = 0.0f;
    float y = 0.0f;
};

inline bool operator==(Pt a, Pt b) { return a.x == b.x && a.y == b.y; }
inline bool operator!=(Pt a, Pt b) { return !(a == b); }

// An integer rectangle, left/top inclusive, right/bottom exclusive (SkIRect).
struct IRect {
    std::int32_t left = 0;
    std::int32_t top = 0;
    std::int32_t right = 0;
    std::int32_t bottom = 0;

    bool isEmpty() const { return left >= right || top >= bottom; }
    bool intersects(const IRect& o) const {
        return !isEmpty() && !o.isEmpty() && left < o.right && o.left < right && top < o.bottom && o.top < bottom;
    }
    bool contains(const IRect& o) const {
        return !o.isEmpty() && !isEmpty() && left <= o.left && top <= o.top && right >= o.right
            && bottom >= o.bottom;
    }
    // The intersection; false (and `out` unchanged) when it is empty.
    bool intersect(const IRect& o, IRect& out) const {
        const IRect r{left > o.left ? left : o.left, top > o.top ? top : o.top, right < o.right ? right : o.right,
                      bottom < o.bottom ? bottom : o.bottom};
        if (r.isEmpty()) return false;
        out = r;
        return true;
    }
};

// An SkRect.
struct Rect {
    float left = 0.0f;
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;
};

// SkLineClipper's sect_with_horizontal / sect_with_vertical, pin_unsorted.
float sectWithHorizontal(const Pt src[2], float y);
float sectWithVertical(const Pt src[2], float x);
double pinUnsorted(double value, double lo, double hi);

// SkPoint::normalize (through doubles); false for a zero or non-finite vector.
bool normalize(float x, float y, Pt& out);

// SkScalarToFDot6: (int)(x * 64), truncating.
inline std::int32_t toFDot6(float x) {
    return toI32(static_cast<double>(x * 64.0f));
}

// ------------------------------------------------------------ blitters

// An A8 coverage mask (SkMask): image[offset + (y - top) * rowBytes +
// (x - left)] is the coverage at (x, y).
struct Mask {
    std::vector<std::uint8_t> image;
    std::size_t offset = 0;
    IRect bounds;
    std::size_t rowBytes = 0;

    std::uint8_t get(std::int32_t x, std::int32_t y) const {
        const std::ptrdiff_t i = static_cast<std::ptrdiff_t>(offset)
            + static_cast<std::ptrdiff_t>(y - bounds.top) * static_cast<std::ptrdiff_t>(rowBytes)
            + static_cast<std::ptrdiff_t>(x - bounds.left);
        return image[static_cast<std::size_t>(i)];
    }
};

// The SkBlitter calls the scan converters make. blitAntiH takes one
// coverage per pixel (the runs of SkBlitter::blitAntiH expanded; every
// blitter here computes each pixel independently, so run boundaries do not
// change the result). The non-pure methods are SkBlitter's defaults.
class Blitter {
public:
    virtual ~Blitter() = default;

    virtual void blitH(std::int32_t x, std::int32_t y, std::int32_t width) = 0;
    virtual void blitAntiH(std::int32_t x, std::int32_t y, const std::uint8_t* aa, std::int32_t count) = 0;

    virtual void blitV(std::int32_t x, std::int32_t y, std::int32_t height, std::uint8_t alpha) {
        if (alpha == 255) {
            blitRect(x, y, 1, height);
        } else {
            for (std::int32_t row = 0; row < height; ++row) blitAntiH(x, y + row, &alpha, 1);
        }
    }

    virtual void blitRect(std::int32_t x, std::int32_t y, std::int32_t width, std::int32_t height) {
        for (std::int32_t row = 0; row < height; ++row) blitH(x, y + row, width);
    }

    virtual void blitAntiH2(std::int32_t x, std::int32_t y, std::uint8_t a0, std::uint8_t a1) {
        const std::uint8_t aa[2] = {a0, a1};
        blitAntiH(x, y, aa, 2);
    }

    virtual void blitAntiV2(std::int32_t x, std::int32_t y, std::uint8_t a0, std::uint8_t a1) {
        blitAntiH(x, y, &a0, 1);
        blitAntiH(x, y + 1, &a1, 1);
    }

    virtual void blitAntiRect(std::int32_t x, std::int32_t y, std::int32_t width, std::int32_t height,
                              std::uint8_t left, std::uint8_t right) {
        if (left > 0) blitV(x, y, height, left);
        x += 1;
        if (width > 0) {
            blitRect(x, y, width, height);
            x += width;
        }
        if (right > 0) blitV(x, y, height, right);
    }

    // blitMask of an A8 mask restricted to `clip`.
    virtual void blitMask(const Mask& mask, const IRect& clip) = 0;
};

// SkRectClipBlitter: clips each call to a rectangle. It does not override
// blitAntiH2/blitAntiV2, so those take SkBlitter's defaults through the
// clipped blitAntiH.
class RectClipBlitter final : public Blitter {
public:
    RectClipBlitter(Blitter& inner, const IRect& clip) : m_inner(inner), m_clip(clip) {}

    void blitH(std::int32_t x, std::int32_t y, std::int32_t width) override;
    void blitAntiH(std::int32_t x, std::int32_t y, const std::uint8_t* aa, std::int32_t count) override;
    void blitV(std::int32_t x, std::int32_t y, std::int32_t height, std::uint8_t alpha) override;
    void blitRect(std::int32_t x, std::int32_t y, std::int32_t width, std::int32_t height) override;
    void blitAntiRect(std::int32_t x, std::int32_t y, std::int32_t width, std::int32_t height, std::uint8_t left,
                      std::uint8_t right) override;
    void blitMask(const Mask& mask, const IRect& clip) override;

private:
    Blitter& m_inner;
    IRect m_clip;
};

// Fills the stroke of p0 -> p1 (width > 1) with `blitter` on a canvas of
// `canvas` bounds (stroke_fill.cpp).
void fillStroke(Pt p0, Pt p1, float width, const IRect& canvas, Blitter& blitter);

} // namespace nm::raster
