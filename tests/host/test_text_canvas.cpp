// The filter/text canvas port (host/text_canvas.h) on a TrueType font built
// here: one square glyph for "A", 1000 units per em. Malformed font data
// fails with std::runtime_error.

#include "host/text_canvas.h"

#include "check.h"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using Bytes = std::vector<std::uint8_t>;

void u16(Bytes& b, unsigned v) {
    b.push_back(std::uint8_t(v >> 8));
    b.push_back(std::uint8_t(v));
}
void u32(Bytes& b, std::uint32_t v) {
    u16(b, v >> 16);
    u16(b, v & 0xFFFF);
}

// A font with .notdef (empty) and a square from (100, 0) to (600, 500),
// advance 700, mapped from U+0041; OS/2 typo ascender 800, descender -200.
Bytes squareFont() {
    std::vector<std::pair<std::string, Bytes>> tables;
    Bytes head(54, 0);
    head[18] = 0x03, head[19] = 0xE8; // unitsPerEm 1000
    tables.emplace_back("head", head);
    Bytes hhea(36, 0);
    hhea[4] = 0x03, hhea[5] = 0x20; // ascender 800
    hhea[6] = 0xFF, hhea[7] = 0x38; // descender -200
    hhea[35] = 2;                   // numberOfHMetrics
    tables.emplace_back("hhea", hhea);
    Bytes maxp;
    u32(maxp, 0x00005000);
    u16(maxp, 2);
    tables.emplace_back("maxp", maxp);
    Bytes hmtx;
    u16(hmtx, 500), u16(hmtx, 0), u16(hmtx, 700), u16(hmtx, 100);
    tables.emplace_back("hmtx", hmtx);
    Bytes glyf;
    u16(glyf, 1);                                       // one contour
    u16(glyf, 100), u16(glyf, 0), u16(glyf, 600), u16(glyf, 500);
    u16(glyf, 3);                                       // end point
    u16(glyf, 0);                                       // no instructions
    for (int i = 0; i < 4; ++i) glyf.push_back(0x01);   // on curve, 16-bit deltas
    for (const int x : {100, 0, 500, 0}) u16(glyf, unsigned(std::uint16_t(std::int16_t(x))));
    for (const int y : {0, 500, 0, -500}) u16(glyf, unsigned(std::uint16_t(std::int16_t(y))));
    if (glyf.size() % 2) glyf.push_back(0);
    Bytes loca;
    u16(loca, 0), u16(loca, 0), u16(loca, unsigned(glyf.size() / 2));
    tables.emplace_back("loca", loca);
    tables.emplace_back("glyf", glyf);
    Bytes cmap;
    u16(cmap, 0), u16(cmap, 1);
    u16(cmap, 3), u16(cmap, 1), u32(cmap, 12);
    // format 4: segments 'A' -> glyph 1 and the 0xFFFF terminator
    u16(cmap, 4), u16(cmap, 32), u16(cmap, 0), u16(cmap, 4), u16(cmap, 4), u16(cmap, 1), u16(cmap, 0);
    u16(cmap, 0x41), u16(cmap, 0xFFFF);                 // endCode
    u16(cmap, 0);                                       // reservedPad
    u16(cmap, 0x41), u16(cmap, 0xFFFF);                 // startCode
    u16(cmap, unsigned(std::uint16_t(1 - 0x41))), u16(cmap, 1); // idDelta
    u16(cmap, 0), u16(cmap, 0);                         // idRangeOffset
    tables.emplace_back("cmap", cmap);
    Bytes os2(78, 0);
    os2[68] = 0x03, os2[69] = 0x20; // sTypoAscender 800
    os2[70] = 0xFF, os2[71] = 0x38; // sTypoDescender -200
    tables.emplace_back("OS/2", os2);

    Bytes font;
    u32(font, 0x00010000);
    u16(font, unsigned(tables.size())), u16(font, 0), u16(font, 0), u16(font, 0);
    std::uint32_t offset = std::uint32_t(12 + 16 * tables.size());
    for (const auto& [tag, data] : tables) {
        for (const char c : tag) font.push_back(std::uint8_t(c));
        u32(font, 0), u32(font, offset), u32(font, std::uint32_t(data.size()));
        offset += std::uint32_t((data.size() + 3) & ~std::size_t(3));
    }
    for (const auto& [tag, data] : tables) {
        font.insert(font.end(), data.begin(), data.end());
        while (font.size() % 4) font.push_back(0);
    }
    return font;
}

bool throwsRuntimeError(Bytes bytes) {
    try {
        libnoisemaker::TrueTypeFont::parse(std::move(bytes));
    } catch (const std::runtime_error&) {
        return true;
    }
    return false;
}

} // namespace

int main() {
    using libnoisemaker::textCanvasDrawsFamily;
    NM_CHECK(textCanvasDrawsFamily(u"Nunito"));
    NM_CHECK(textCanvasDrawsFamily(u"nunito"));
    NM_CHECK(textCanvasDrawsFamily(u" 'Nunito', sans-serif"));
    NM_CHECK(textCanvasDrawsFamily(u"\"NUNITO\""));
    NM_CHECK(!textCanvasDrawsFamily(u"sans-serif"));
    NM_CHECK(!textCanvasDrawsFamily(u"Nunito Blank"));
    NM_CHECK(!textCanvasDrawsFamily(u""));

    // Malformed data fails cleanly.
    NM_CHECK(throwsRuntimeError({}));
    NM_CHECK(throwsRuntimeError(Bytes(64, 0x5A)));
    Bytes truncated = squareFont();
    truncated.resize(40);
    NM_CHECK(throwsRuntimeError(truncated));
    Bytes noGlyf = squareFont();
    noGlyf[12 + 16 * 5] = 'x'; // rename the glyf table record
    NM_CHECK(throwsRuntimeError(noGlyf));

    libnoisemaker::TrueTypeFont font = libnoisemaker::TrueTypeFont::parse(squareFont());
    NM_CHECK(font.unitsPerEm() == 1000);
    NM_CHECK(font.glyphForCodePoint(0x41) == 1);
    NM_CHECK(font.glyphForCodePoint(0x42) == 0);
    NM_CHECK(font.advance(1) == 700);
    NM_CHECK(font.kerning(1, 1) == 0);
    NM_CHECK(font.outline(1).size() == 1 && font.outline(1)[0].size() == 4);

    // "A" at 32 px (size 0.5 of a 64 px canvas), centered: the line is
    // 700 * 0.032 + 0.16 = 22.56 px wide, so the glyph origin is
    // 32 - 11.28 = 20.72, placed at 20.75; the middle baseline is
    // (25.59375 - 6.40625) / 2 above the alphabetic one (the ascender
    // 25.6 rounded to 1/64), which lands at 41.59, rounded to 42. The square
    // covers x 23.95..39.95, y 26..42.
    libnoisemaker::TextCanvasParams params;
    params.text = u"A";
    params.font = u"Nunito";
    params.size = 0.5;
    params.color[0] = 1, params.color[1] = 0.5, params.color[2] = 0;
    const std::vector<std::uint8_t> rgba = libnoisemaker::drawTextCanvas(font, 64, params);
    NM_CHECK(rgba.size() == 64u * 64u * 4u);
    const auto at = [&](int x, int y, int c) { return int(rgba[(std::size_t(y) * 64 + std::size_t(x)) * 4 + std::size_t(c)]); };
    NM_CHECK(at(31, 34, 3) == 255 && at(31, 34, 0) == 255 && at(31, 34, 1) == 128 && at(31, 34, 2) == 0);
    NM_CHECK(at(24, 30, 3) == 255 && at(38, 30, 3) == 255);
    NM_CHECK(at(22, 30, 3) == 0 && at(41, 30, 3) == 0 && at(31, 24, 3) == 0 && at(31, 43, 3) == 0);
    NM_CHECK(at(23, 30, 3) == 16 && at(39, 30, 3) == 239); // 1 and 15 of each row's 16 samples
    long total = 0;
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x) total += at(x, y, 3);
    NM_CHECK(total > 255L * 254 && total < 255L * 258); // 16 x 16 px

    // A family this port does not draw leaves the canvas clear.
    params.font = u"serif";
    const std::vector<std::uint8_t> clear = libnoisemaker::drawTextCanvas(font, 64, params);
    NM_CHECK(clear.size() == rgba.size());
    bool empty = true;
    for (const std::uint8_t v : clear) empty = empty && v == 0;
    NM_CHECK(empty);
    return 0;
}
