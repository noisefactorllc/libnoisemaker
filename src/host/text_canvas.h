#pragma once

// text_canvas.h -- the text of filter/text, drawn as the reference demo page
// draws it (demo/shaders/lib/demo-ui.js _renderTextToCanvas) on the hidden
// canvas it uploads as the step's textTex with updateTextureFromSource(flipY
// true).
//
// The page draws on a canvas of the pipeline's width by its width, cleared to
// transparent: ctx.font = `${round(size * height)}px ${font}`, textAlign the
// justify value, textBaseline 'middle', fillStyle the color as
// rgba(round(r * 255), round(g * 255), round(b * 255), 1); it translates to
// (posX * width, posY * height), rotates by rotation degrees and fills each
// line of the text, the lines 1.2 font sizes apart and centered on that point.
//
// The canvas is a child of the page's body, so its text takes the body's
// computed style where ctx.font leaves it: font-variation-settings "wght"
// 580 and letter-spacing 0.16px (the demo page's stylesheet). Nunito, the
// default font, is the page's web font, the variable TrueType font of the
// reference's demo/font/Nunito. This port reads that font (TrueTypeFont),
// shapes each line with its kerning, and draws the glyph outlines with
// anti-aliasing:
//
//   - each glyph at its advance (hmtx and HVAR at the variation) plus the
//     GPOS kerning of the 'kern' feature (pair adjustments and their
//     variation deltas) plus the letter spacing;
//   - textAlign moves the line by its width (the advances and the letter
//     spacing after each glyph); the middle baseline is (a - d) / 2 above
//     the alphabetic one, a being the OS/2 typo ascender scaled so that a
//     and d = size - a make one em, a rounded to 1/64 px (Blink's
//     NormalizedTypoAscentAndDescent);
//   - unrotated text has its baseline rounded to a whole pixel and its glyph
//     origins to a quarter pixel, as Skia's glyph cache places them;
//   - coverage is the share of 16 x 16 samples inside the outline (nonzero
//     winding), and the glyphs are blended source-over in premultiplied
//     8-bit color, which the upload then divides out.
//
// The page rasterizes with the platform's font engine, so edges differ by a
// few levels from it. Other fonts (the system families sans-serif, serif,
// monospace, cursive and fantasy) are not drawn: the canvas stays clear.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace libnoisemaker {

// A TrueType font with glyph outlines (glyf), optionally variable (fvar,
// avar, gvar, HVAR), with the GPOS kerning that shaping applies by default.
// parse() throws std::runtime_error on data it cannot read.
class TrueTypeFont {
public:
    static TrueTypeFont parse(std::vector<std::uint8_t> bytes);

    // Sets a variation axis ('wght') in user units, clamped to its range.
    void setVariation(std::string_view tag, double value);

    struct Point {
        double x = 0;
        double y = 0;
        bool onCurve = true;
    };
    using Contour = std::vector<Point>;

    int unitsPerEm() const { return m_unitsPerEm; }
    int typoAscender() const { return m_typoAscender; }
    int typoDescender() const { return m_typoDescender; }
    std::uint16_t glyphForCodePoint(std::uint32_t codePoint) const;
    // The glyph's advance and outline, in font units, at the variation.
    double advance(std::uint16_t glyph) const;
    std::vector<Contour> outline(std::uint16_t glyph) const;
    // The 'kern' feature's adjustment of the first glyph's advance before
    // the second, in font units, at the variation.
    double kerning(std::uint16_t first, std::uint16_t second) const;

private:
    struct Table {
        std::uint32_t offset = 0;
        std::uint32_t length = 0;
    };
    std::optional<Table> table(const char* tag) const;
    std::vector<double> normalizedCoords() const;
    double itemVariationDelta(std::uint32_t store, std::uint16_t outer, std::uint16_t inner) const;
    std::vector<Point> glyphPoints(std::uint16_t glyph, std::vector<int>& endPoints, int depth) const;
    void applyGlyphVariations(std::uint16_t glyph, std::vector<Point>& points, const std::vector<int>& endPoints) const;
    double pairValue(std::uint32_t subtable, std::uint16_t first, std::uint16_t second, bool& matched) const;

    std::vector<std::uint8_t> m_data;
    std::vector<std::pair<std::string, Table>> m_tables;
    int m_unitsPerEm = 1000;
    int m_numGlyphs = 0;
    int m_numHMetrics = 0;
    int m_locaFormat = 0;
    int m_typoAscender = 0;
    int m_typoDescender = 0;
    struct Axis {
        std::string tag;
        double min = 0;
        double def = 0;
        double max = 0;
        double value = 0;
    };
    std::vector<Axis> m_axes;
    std::vector<std::uint32_t> m_kernLookups; // GPOS lookup table offsets of 'kern'
};

struct TextCanvasParams {
    std::u16string text;
    std::u16string font;     // the CSS font family
    double size = 0.1;       // of the canvas height
    double posX = 0.5;
    double posY = 0.5;
    double rotation = 0;     // degrees
    double color[3] = {1, 1, 1};
    std::u16string justify = u"center";
};

// The font families this port draws with `font` ("Nunito", case-insensitive,
// quoted or not).
bool textCanvasDrawsFamily(std::u16string_view family);

// The canvas, `resolution` by `resolution` pixels, as straight-alpha RGBA8 in
// canvas row order (row 0 at the top): the bytes the page's upload reads.
// `font` is the page's Nunito; a family textCanvasDrawsFamily refuses leaves
// the canvas clear.
std::vector<std::uint8_t> drawTextCanvas(TrueTypeFont& font, int resolution, const TextCanvasParams& params);

} // namespace libnoisemaker
