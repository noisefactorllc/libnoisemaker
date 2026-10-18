#include "host/text_canvas.h"

#include "host/stroke_canvas.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace libnoisemaker {

namespace {

constexpr double kPi = 3.14159265358979323846;

// The demo page's computed style on its text canvases (see the header).
constexpr double kWeight = 580;
constexpr double kLetterSpacing = 0.16;

// Coverage samples per pixel side.
constexpr int kSamples = 16;
// Line segments per quadratic curve.
constexpr int kCurveSegments = 16;

[[noreturn]] void bad(const char* what) { throw std::runtime_error(std::string("TrueType font: ") + what); }

class Reader {
public:
    explicit Reader(const std::vector<std::uint8_t>& data) : m_data(data) {}
    std::uint8_t u8(std::size_t at) const {
        if (at >= m_data.size()) bad("read past the end");
        return m_data[at];
    }
    std::uint16_t u16(std::size_t at) const { return std::uint16_t((u8(at) << 8) | u8(at + 1)); }
    std::int16_t i16(std::size_t at) const { return std::int16_t(u16(at)); }
    std::uint32_t u32(std::size_t at) const { return (std::uint32_t(u16(at)) << 16) | u16(at + 2); }
    std::int32_t i32(std::size_t at) const { return std::int32_t(u32(at)); }
    double f2dot14(std::size_t at) const { return i16(at) / 16384.0; }
    double fixed(std::size_t at) const { return i32(at) / 65536.0; }
    std::string tag(std::size_t at) const { return {char(u8(at)), char(u8(at + 1)), char(u8(at + 2)), char(u8(at + 3))}; }

private:
    const std::vector<std::uint8_t>& m_data;
};

double toF2Dot14(double v) { return std::round(v * 16384.0) / 16384.0; }

// A variation region's (or tuple's) scalar for one axis.
double axisScalar(double v, double start, double peak, double end, bool intermediate) {
    if (peak == 0) return 1;
    if (v == peak) return 1;
    if (!intermediate) {
        if (v < std::min(0.0, peak) || v > std::max(0.0, peak)) return 0;
        return v / peak;
    }
    if (start > peak || peak > end) return 1;
    if (start < 0 && end > 0) return 1;
    if (v < start || v > end) return 0;
    return v < peak ? (v - start) / (peak - start) : (end - v) / (end - peak);
}

// Packed point numbers: the points, or empty for all of them.
std::vector<std::uint16_t> packedPoints(const Reader& r, std::size_t& at) {
    unsigned count = r.u8(at++);
    if (count & 0x80) count = ((count & 0x7F) << 8) | r.u8(at++);
    std::vector<std::uint16_t> points;
    std::uint16_t point = 0;
    while (points.size() < count) {
        const std::uint8_t control = r.u8(at++);
        const unsigned run = (control & 0x7F) + 1u;
        for (unsigned i = 0; i < run && points.size() < count; ++i) {
            const std::uint16_t delta = (control & 0x80) ? r.u16((at += 2) - 2) : r.u8(at++);
            point = std::uint16_t(point + delta);
            points.push_back(point);
        }
    }
    return points;
}

std::vector<double> packedDeltas(const Reader& r, std::size_t& at, std::size_t count) {
    std::vector<double> deltas;
    while (deltas.size() < count) {
        const std::uint8_t control = r.u8(at++);
        const unsigned run = (control & 0x3F) + 1u;
        for (unsigned i = 0; i < run && deltas.size() < count; ++i) {
            if (control & 0x80) deltas.push_back(0);
            else if (control & 0x40) deltas.push_back(r.i16((at += 2) - 2));
            else deltas.push_back(std::int8_t(r.u8(at++)));
        }
    }
    return deltas;
}

// The coverage index of a glyph in a Coverage table, or -1.
int coverageIndex(const Reader& r, std::size_t table, std::uint16_t glyph) {
    const std::uint16_t format = r.u16(table);
    const std::uint16_t count = r.u16(table + 2);
    if (format == 1) {
        for (unsigned i = 0; i < count; ++i) {
            if (r.u16(table + 4 + 2 * i) == glyph) return int(i);
        }
    } else if (format == 2) {
        for (unsigned i = 0; i < count; ++i) {
            const std::size_t rec = table + 4 + 6 * i;
            if (glyph >= r.u16(rec) && glyph <= r.u16(rec + 2)) return int(r.u16(rec + 4) + (glyph - r.u16(rec)));
        }
    }
    return -1;
}

std::uint16_t glyphClass(const Reader& r, std::size_t table, std::uint16_t glyph) {
    const std::uint16_t format = r.u16(table);
    if (format == 1) {
        const std::uint16_t start = r.u16(table + 2);
        const std::uint16_t count = r.u16(table + 4);
        if (glyph >= start && glyph < start + count) return r.u16(table + 6 + 2 * (glyph - start));
    } else if (format == 2) {
        const std::uint16_t count = r.u16(table + 2);
        for (unsigned i = 0; i < count; ++i) {
            const std::size_t rec = table + 4 + 6 * i;
            if (glyph >= r.u16(rec) && glyph <= r.u16(rec + 2)) return r.u16(rec + 4);
        }
    }
    return 0;
}

int popcount16(std::uint16_t v) {
    int n = 0;
    for (; v; v = std::uint16_t(v & (v - 1))) ++n;
    return n;
}

} // namespace

TrueTypeFont TrueTypeFont::parse(std::vector<std::uint8_t> bytes) {
    TrueTypeFont font;
    font.m_data = std::move(bytes);
    const Reader r(font.m_data);
    const std::uint32_t version = r.u32(0);
    if (version != 0x00010000u && version != 0x74727565u) bad("not a TrueType font");
    const std::uint16_t numTables = r.u16(4);
    for (unsigned i = 0; i < numTables; ++i) {
        const std::size_t rec = 12 + 16 * i;
        Table t{r.u32(rec + 8), r.u32(rec + 12)};
        if (std::uint64_t(t.offset) + t.length > font.m_data.size()) bad("a table lies past the end");
        font.m_tables.emplace_back(r.tag(rec), t);
    }
    for (const char* required : {"head", "maxp", "hhea", "hmtx", "loca", "glyf", "cmap"}) {
        if (!font.table(required)) bad("a required table is missing");
    }
    const std::uint32_t head = font.table("head")->offset;
    font.m_unitsPerEm = r.u16(head + 18);
    font.m_locaFormat = r.i16(head + 50);
    if (font.m_unitsPerEm == 0) bad("unitsPerEm is 0");
    font.m_numGlyphs = r.u16(font.table("maxp")->offset + 4);
    font.m_numHMetrics = r.u16(font.table("hhea")->offset + 34);
    if (font.m_numHMetrics == 0) bad("no horizontal metrics");
    const std::uint32_t hhea = font.table("hhea")->offset;
    font.m_typoAscender = r.i16(hhea + 4);
    font.m_typoDescender = r.i16(hhea + 6);
    if (const auto os2 = font.table("OS/2"); os2 && os2->length >= 72) {
        font.m_typoAscender = r.i16(os2->offset + 68);
        font.m_typoDescender = r.i16(os2->offset + 70);
    }
    if (const auto fvar = font.table("fvar")) {
        const std::size_t axes = fvar->offset + r.u16(fvar->offset + 4);
        const std::uint16_t count = r.u16(fvar->offset + 8);
        const std::uint16_t size = r.u16(fvar->offset + 10);
        for (unsigned i = 0; i < count; ++i) {
            const std::size_t rec = axes + std::size_t(size) * i;
            Axis axis{r.tag(rec), r.fixed(rec + 4), r.fixed(rec + 8), r.fixed(rec + 12), 0};
            axis.value = axis.def;
            font.m_axes.push_back(axis);
        }
    }
    // The lookups of the 'kern' feature for the Latin script (else the
    // default script), in lookup order.
    if (const auto gpos = font.table("GPOS")) {
        const std::size_t base = gpos->offset;
        const std::size_t scripts = base + r.u16(base + 4);
        const std::size_t features = base + r.u16(base + 6);
        const std::size_t lookups = base + r.u16(base + 8);
        std::size_t script = 0;
        for (const char* wanted : {"latn", "DFLT"}) {
            for (unsigned i = 0; i < r.u16(scripts) && !script; ++i) {
                if (r.tag(scripts + 2 + 6 * i) == wanted) script = scripts + r.u16(scripts + 2 + 6 * i + 4);
            }
            if (script) break;
        }
        std::vector<std::uint16_t> indices;
        if (script && r.u16(script)) {
            const std::size_t langSys = script + r.u16(script);
            for (unsigned i = 0; i < r.u16(langSys + 4); ++i) {
                const std::uint16_t feature = r.u16(langSys + 6 + 2 * i);
                if (feature >= r.u16(features) || r.tag(features + 2 + 6 * feature) != "kern") continue;
                const std::size_t table = features + r.u16(features + 2 + 6 * feature + 4);
                for (unsigned j = 0; j < r.u16(table + 2); ++j) indices.push_back(r.u16(table + 4 + 2 * j));
            }
        }
        std::sort(indices.begin(), indices.end());
        indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
        for (const std::uint16_t index : indices) {
            if (index >= r.u16(lookups)) continue;
            font.m_kernLookups.push_back(std::uint32_t(lookups + r.u16(lookups + 2 + 2 * index)));
        }
    }
    return font;
}

std::optional<TrueTypeFont::Table> TrueTypeFont::table(const char* tag) const {
    for (const auto& [name, t] : m_tables) {
        if (name == tag) return t;
    }
    return std::nullopt;
}

void TrueTypeFont::setVariation(std::string_view tag, double value) {
    for (Axis& axis : m_axes) {
        if (axis.tag == tag) axis.value = std::clamp(value, axis.min, axis.max);
    }
}

std::vector<double> TrueTypeFont::normalizedCoords() const {
    const Reader r(m_data);
    std::vector<double> coords;
    for (const Axis& axis : m_axes) {
        double v = 0;
        if (axis.value < axis.def && axis.def > axis.min) v = (axis.value - axis.def) / (axis.def - axis.min);
        else if (axis.value > axis.def && axis.max > axis.def) v = (axis.value - axis.def) / (axis.max - axis.def);
        coords.push_back(toF2Dot14(v));
    }
    if (const auto avar = table("avar")) {
        std::size_t at = avar->offset + 8;
        const std::uint16_t count = r.u16(avar->offset + 6);
        for (unsigned axis = 0; axis < count && axis < coords.size(); ++axis) {
            const std::uint16_t maps = r.u16(at);
            const std::size_t first = at + 2;
            at = first + 4 * std::size_t(maps);
            const double v = coords[axis];
            for (unsigned k = 0; k + 1 < maps; ++k) {
                const double from0 = r.f2dot14(first + 4 * k), to0 = r.f2dot14(first + 4 * k + 2);
                const double from1 = r.f2dot14(first + 4 * (k + 1)), to1 = r.f2dot14(first + 4 * (k + 1) + 2);
                if (v >= from0 && v <= from1) {
                    coords[axis] = toF2Dot14(from1 == from0 ? to0 : to0 + (to1 - to0) * (v - from0) / (from1 - from0));
                    break;
                }
            }
        }
    }
    return coords;
}

double TrueTypeFont::itemVariationDelta(std::uint32_t store, std::uint16_t outer, std::uint16_t inner) const {
    const Reader r(m_data);
    if (outer >= r.u16(store + 6)) return 0;
    const std::size_t regions = store + r.u32(store + 2);
    const std::size_t data = store + r.u32(store + 8 + 4 * std::size_t(outer));
    const std::uint16_t itemCount = r.u16(data);
    if (inner >= itemCount) return 0;
    const std::uint16_t wordField = r.u16(data + 2);
    const bool longWords = wordField & 0x8000;
    const unsigned wordCount = wordField & 0x7FFF;
    const std::uint16_t regionCount = r.u16(data + 4);
    const unsigned wordSize = longWords ? 4 : 2;
    const unsigned rowSize = wordCount * wordSize + (regionCount - wordCount) * (wordSize / 2);
    const std::size_t row = data + 6 + 2 * std::size_t(regionCount) + std::size_t(rowSize) * inner;
    const std::vector<double> coords = normalizedCoords();
    const std::uint16_t axisCount = r.u16(regions);
    double delta = 0;
    std::size_t at = row;
    for (unsigned i = 0; i < regionCount; ++i) {
        double d = 0;
        if (i < wordCount) {
            d = longWords ? double(r.i32(at)) : double(r.i16(at));
            at += wordSize;
        } else {
            d = longWords ? double(r.i16(at)) : double(std::int8_t(r.u8(at)));
            at += wordSize / 2;
        }
        const std::uint16_t region = r.u16(data + 6 + 2 * i);
        if (region >= r.u16(regions + 2)) continue;
        double scalar = 1;
        for (unsigned a = 0; a < axisCount; ++a) {
            const std::size_t rec = regions + 4 + (std::size_t(region) * axisCount + a) * 6;
            const double v = a < coords.size() ? coords[a] : 0;
            scalar *= axisScalar(v, r.f2dot14(rec), r.f2dot14(rec + 2), r.f2dot14(rec + 4), true);
        }
        delta += scalar * d;
    }
    return delta;
}

std::uint16_t TrueTypeFont::glyphForCodePoint(std::uint32_t codePoint) const {
    const Reader r(m_data);
    const std::uint32_t cmap = table("cmap")->offset;
    std::size_t format4 = 0, format12 = 0;
    for (unsigned i = 0; i < r.u16(cmap + 2); ++i) {
        const std::size_t rec = cmap + 4 + 8 * i;
        const std::uint16_t platform = r.u16(rec), encoding = r.u16(rec + 2);
        const std::size_t sub = cmap + r.u32(rec + 4);
        const std::uint16_t format = r.u16(sub);
        const bool unicode = platform == 0 || (platform == 3 && (encoding == 1 || encoding == 10));
        if (!unicode) continue;
        if (format == 12 && !format12) format12 = sub;
        if (format == 4 && !format4) format4 = sub;
    }
    if (format12) {
        const std::uint32_t groups = r.u32(format12 + 12);
        for (std::uint32_t g = 0; g < groups; ++g) {
            const std::size_t rec = format12 + 16 + 12 * std::size_t(g);
            if (codePoint >= r.u32(rec) && codePoint <= r.u32(rec + 4)) {
                const std::uint32_t glyph = r.u32(rec + 8) + (codePoint - r.u32(rec));
                return glyph < std::uint32_t(m_numGlyphs) ? std::uint16_t(glyph) : 0;
            }
        }
        return 0;
    }
    if (!format4 || codePoint > 0xFFFF) return 0;
    const unsigned segCount = r.u16(format4 + 6) / 2;
    const std::size_t ends = format4 + 14, starts = ends + 2 * segCount + 2;
    const std::size_t deltas = starts + 2 * segCount, ranges = deltas + 2 * segCount;
    for (unsigned s = 0; s < segCount; ++s) {
        if (codePoint > r.u16(ends + 2 * s)) continue;
        const std::uint16_t start = r.u16(starts + 2 * s);
        if (codePoint < start) return 0;
        const std::uint16_t range = r.u16(ranges + 2 * s);
        std::uint16_t glyph;
        if (range == 0) {
            glyph = std::uint16_t(codePoint + r.u16(deltas + 2 * s));
        } else {
            glyph = r.u16(ranges + 2 * s + range + 2 * (codePoint - start));
            if (glyph) glyph = std::uint16_t(glyph + r.u16(deltas + 2 * s));
        }
        return glyph < m_numGlyphs ? glyph : 0;
    }
    return 0;
}

double TrueTypeFont::advance(std::uint16_t glyph) const {
    const Reader r(m_data);
    const std::uint32_t hmtx = table("hmtx")->offset;
    const int index = std::min<int>(glyph, m_numHMetrics - 1);
    double width = r.u16(hmtx + 4 * std::size_t(index));
    if (m_axes.empty()) return width;
    if (const auto hvar = table("HVAR")) {
        const std::uint32_t store = hvar->offset + r.u32(hvar->offset + 4);
        const std::uint32_t mapOffset = r.u32(hvar->offset + 8);
        std::uint16_t outer = 0, inner = glyph;
        if (mapOffset) {
            const std::size_t map = hvar->offset + mapOffset;
            const std::uint8_t format = r.u8(map);
            const std::uint8_t entryFormat = r.u8(map + 1);
            const std::uint32_t count = format == 1 ? r.u32(map + 2) : r.u16(map + 2);
            const std::size_t entries = map + (format == 1 ? 6 : 4);
            const unsigned size = ((entryFormat & 0x30) >> 4) + 1u;
            const unsigned innerBits = (entryFormat & 0x0F) + 1u;
            if (count) {
                const std::uint32_t i = std::min<std::uint32_t>(glyph, count - 1);
                std::uint32_t entry = 0;
                for (unsigned b = 0; b < size; ++b) entry = (entry << 8) | r.u8(entries + std::size_t(i) * size + b);
                outer = std::uint16_t(entry >> innerBits);
                inner = std::uint16_t(entry & ((1u << innerBits) - 1u));
            }
        }
        return width + itemVariationDelta(store, outer, inner);
    }
    return width;
}

// The glyph's points, its contours ending at `endPoints`, with the glyph
// variations applied, in font units.
std::vector<TrueTypeFont::Point> TrueTypeFont::glyphPoints(std::uint16_t glyph, std::vector<int>& endPoints, int depth) const {
    endPoints.clear();
    if (glyph >= m_numGlyphs || depth > 8) return {};
    const Reader r(m_data);
    const std::uint32_t loca = table("loca")->offset;
    const std::uint32_t glyf = table("glyf")->offset;
    const auto offset = [&](std::size_t i) {
        return m_locaFormat ? std::size_t(r.u32(loca + 4 * i)) : std::size_t(r.u16(loca + 2 * i)) * 2;
    };
    const std::size_t start = offset(glyph), end = offset(std::size_t(glyph) + 1);
    if (end <= start) return {};
    const std::size_t at = glyf + start;
    const std::int16_t contours = r.i16(at);
    if (contours >= 0) {
        std::size_t p = at + 10;
        int count = 0;
        for (int c = 0; c < contours; ++c) {
            const int last = r.u16(p);
            p += 2;
            if (last < count - 1 || last > 0xFFFF) bad("contour end points are not increasing");
            endPoints.push_back(last);
            count = last + 1;
        }
        p += 2 + r.u16(p);
        std::vector<std::uint8_t> flags;
        while (int(flags.size()) < count) {
            const std::uint8_t flag = r.u8(p++);
            flags.push_back(flag);
            if (flag & 0x08) {
                const unsigned repeat = r.u8(p++);
                for (unsigned i = 0; i < repeat && int(flags.size()) < count; ++i) flags.push_back(flag);
            }
        }
        std::vector<Point> points(static_cast<std::size_t>(count));
        double x = 0, y = 0;
        for (int i = 0; i < count; ++i) {
            const std::uint8_t f = flags[std::size_t(i)];
            if (f & 0x02) x += (f & 0x10) ? r.u8(p) : -r.u8(p), p += 1;
            else if (!(f & 0x10)) x += r.i16(p), p += 2;
            points[std::size_t(i)].x = x;
            points[std::size_t(i)].onCurve = f & 0x01;
        }
        for (int i = 0; i < count; ++i) {
            const std::uint8_t f = flags[std::size_t(i)];
            if (f & 0x04) y += (f & 0x20) ? r.u8(p) : -r.u8(p), p += 1;
            else if (!(f & 0x20)) y += r.i16(p), p += 2;
            points[std::size_t(i)].y = y;
        }
        applyGlyphVariations(glyph, points, endPoints);
        return points;
    }
    // A composite glyph: its components' offsets vary as points.
    struct Component {
        std::uint16_t glyph;
        double a = 1, b = 0, c = 0, d = 1;
    };
    std::vector<Component> components;
    std::vector<Point> offsets;
    std::size_t p = at + 10;
    for (;;) {
        const std::uint16_t flags = r.u16(p);
        Component component{r.u16(p + 2)};
        p += 4;
        double dx = 0, dy = 0;
        if (flags & 0x0001) {
            dx = r.i16(p), dy = r.i16(p + 2);
            p += 4;
        } else {
            dx = std::int8_t(r.u8(p)), dy = std::int8_t(r.u8(p + 1));
            p += 2;
        }
        if (!(flags & 0x0002)) dx = dy = 0; // point matching is not supported
        if (flags & 0x0008) {
            component.a = component.d = r.f2dot14(p);
            p += 2;
        } else if (flags & 0x0040) {
            component.a = r.f2dot14(p), component.d = r.f2dot14(p + 2);
            p += 4;
        } else if (flags & 0x0080) {
            component.a = r.f2dot14(p), component.b = r.f2dot14(p + 2);
            component.c = r.f2dot14(p + 4), component.d = r.f2dot14(p + 6);
            p += 8;
        }
        components.push_back(component);
        offsets.push_back(Point{dx, dy, true});
        if (!(flags & 0x0020)) break;
        if (components.size() > 1024) bad("too many components");
    }
    std::vector<int> componentEnds;
    for (std::size_t i = 0; i < offsets.size(); ++i) componentEnds.push_back(int(i));
    applyGlyphVariations(glyph, offsets, componentEnds);
    std::vector<Point> points;
    for (std::size_t i = 0; i < components.size(); ++i) {
        std::vector<int> ends;
        const std::vector<Point> part = glyphPoints(components[i].glyph, ends, depth + 1);
        const int base = int(points.size());
        for (const Point& q : part) {
            const Component& m = components[i];
            points.push_back(Point{m.a * q.x + m.c * q.y + offsets[i].x, m.b * q.x + m.d * q.y + offsets[i].y, q.onCurve});
        }
        for (const int e : ends) endPoints.push_back(base + e);
    }
    return points;
}

// gvar: each tuple's deltas, scaled by its scalar at the variation, with the
// deltas of the points a tuple leaves out inferred along each contour (IUP)
// for an outline (not for a composite's component offsets). The four phantom
// points after the outline points take no part here.
void TrueTypeFont::applyGlyphVariations(std::uint16_t glyph, std::vector<Point>& points, const std::vector<int>& endPoints) const {
    const auto gvar = table("gvar");
    if (!gvar || m_axes.empty()) return;
    const Reader r(m_data);
    const std::size_t base = gvar->offset;
    const std::uint16_t axisCount = r.u16(base + 4);
    const std::uint16_t sharedCount = r.u16(base + 6);
    const std::size_t shared = base + r.u32(base + 8);
    const std::uint16_t glyphCount = r.u16(base + 12);
    const bool longOffsets = r.u16(base + 14) & 1;
    const std::size_t array = base + r.u32(base + 16);
    if (glyph >= glyphCount) return;
    const auto dataOffset = [&](std::size_t i) {
        return longOffsets ? std::size_t(r.u32(base + 20 + 4 * i)) : std::size_t(r.u16(base + 20 + 2 * i)) * 2;
    };
    const std::size_t start = dataOffset(glyph), end = dataOffset(std::size_t(glyph) + 1);
    if (end <= start) return;
    const std::size_t data = array + start;
    const std::uint16_t header = r.u16(data);
    const unsigned tupleCount = header & 0x0FFF;
    std::size_t serialized = data + r.u16(data + 2);
    std::vector<std::uint16_t> sharedPoints;
    bool sharedAll = false;
    if (header & 0x8000) {
        sharedPoints = packedPoints(r, serialized);
        sharedAll = sharedPoints.empty();
    }
    const std::vector<double> coords = normalizedCoords();
    const std::size_t total = points.size() + 4; // with the phantom points
    std::vector<Point> original = points;
    std::size_t tuple = data + 4;
    for (unsigned t = 0; t < tupleCount; ++t) {
        const std::uint16_t size = r.u16(tuple);
        const std::uint16_t index = r.u16(tuple + 2);
        std::size_t at = tuple + 4;
        std::vector<double> peak(axisCount), lo(axisCount), hi(axisCount);
        for (unsigned a = 0; a < axisCount; ++a) {
            if (index & 0x8000) peak[a] = r.f2dot14(at + 2 * a);
            else if ((index & 0x0FFF) < sharedCount) peak[a] = r.f2dot14(shared + 2 * (std::size_t(index & 0x0FFF) * axisCount + a));
        }
        if (index & 0x8000) at += 2 * std::size_t(axisCount);
        const bool intermediate = index & 0x4000;
        if (intermediate) {
            for (unsigned a = 0; a < axisCount; ++a) lo[a] = r.f2dot14(at + 2 * a), hi[a] = r.f2dot14(at + 2 * (axisCount + a));
            at += 4 * std::size_t(axisCount);
        }
        tuple = at;
        double scalar = 1;
        for (unsigned a = 0; a < axisCount && scalar != 0; ++a) {
            scalar *= axisScalar(a < coords.size() ? coords[a] : 0, lo[a], peak[a], hi[a], intermediate);
        }
        std::size_t body = serialized;
        serialized += size;
        if (scalar == 0) continue;
        std::vector<std::uint16_t> which = sharedPoints;
        bool all = sharedAll;
        if (index & 0x2000) {
            which = packedPoints(r, body);
            all = which.empty();
        } else if (!(header & 0x8000)) {
            all = true;
        }
        const std::size_t count = all ? total : which.size();
        const std::vector<double> dx = packedDeltas(r, body, count);
        const std::vector<double> dy = packedDeltas(r, body, count);
        std::vector<double> deltaX(points.size(), 0), deltaY(points.size(), 0);
        std::vector<bool> touched(points.size(), all);
        for (std::size_t i = 0; i < count; ++i) {
            const std::size_t point = all ? i : which[i];
            if (point >= points.size()) continue;
            deltaX[point] = dx[i];
            deltaY[point] = dy[i];
            touched[point] = true;
        }
        // IUP along each contour of an outline.
        if (!all && endPoints.size() && endPoints.back() + 1 == int(points.size()) && endPoints.size() != points.size()) {
            int first = 0;
            for (const int last : endPoints) {
                std::vector<int> refs;
                for (int i = first; i <= last; ++i) {
                    if (touched[std::size_t(i)]) refs.push_back(i);
                }
                if (!refs.empty() && int(refs.size()) != last - first + 1) {
                    for (std::size_t k = 0; k < refs.size(); ++k) {
                        const int p1 = refs[k], p2 = refs[(k + 1) % refs.size()];
                        for (int i = p1 + 1;; ++i) {
                            if (i > last) i = first;
                            if (i == p2) break;
                            for (int axis = 0; axis < 2; ++axis) {
                                const auto coord = [&](int n) { return axis ? original[std::size_t(n)].y : original[std::size_t(n)].x; };
                                std::vector<double>& delta = axis ? deltaY : deltaX;
                                const double c = coord(i), c1 = coord(p1), c2 = coord(p2);
                                const double d1 = delta[std::size_t(p1)], d2 = delta[std::size_t(p2)];
                                double d;
                                if (c1 == c2) d = d1 == d2 ? d1 : 0;
                                else if (c <= std::min(c1, c2)) d = c1 < c2 ? d1 : d2;
                                else if (c >= std::max(c1, c2)) d = c1 > c2 ? d1 : d2;
                                else d = d1 + (c - c1) * (d2 - d1) / (c2 - c1);
                                delta[std::size_t(i)] = d;
                            }
                        }
                    }
                }
                first = last + 1;
            }
        }
        for (std::size_t i = 0; i < points.size(); ++i) {
            points[i].x += scalar * deltaX[i];
            points[i].y += scalar * deltaY[i];
        }
    }
}

std::vector<TrueTypeFont::Contour> TrueTypeFont::outline(std::uint16_t glyph) const {
    std::vector<int> ends;
    const std::vector<Point> points = glyphPoints(glyph, ends, 0);
    std::vector<Contour> contours;
    int first = 0;
    for (const int last : ends) {
        if (last >= int(points.size())) break;
        contours.emplace_back(points.begin() + first, points.begin() + last + 1);
        first = last + 1;
    }
    return contours;
}

// One PairPos subtable's adjustment of the first glyph's advance.
double TrueTypeFont::pairValue(std::uint32_t subtable, std::uint16_t first, std::uint16_t second, bool& matched) const {
    const Reader r(m_data);
    matched = false;
    const std::uint16_t format = r.u16(subtable);
    if (coverageIndex(r, subtable + r.u16(subtable + 2), first) < 0) return 0;
    const std::uint16_t format1 = r.u16(subtable + 4), format2 = r.u16(subtable + 6);
    const std::size_t size1 = 2 * std::size_t(popcount16(format1)), size2 = 2 * std::size_t(popcount16(format2));
    std::size_t record = 0;
    if (format == 1) {
        const int index = coverageIndex(r, subtable + r.u16(subtable + 2), first);
        if (index >= r.u16(subtable + 8)) return 0;
        const std::size_t set = subtable + r.u16(subtable + 10 + 2 * std::size_t(index));
        const std::uint16_t count = r.u16(set);
        for (unsigned i = 0; i < count && !record; ++i) {
            const std::size_t rec = set + 2 + i * (2 + size1 + size2);
            if (r.u16(rec) == second) record = rec + 2;
        }
        if (!record) return 0;
    } else if (format == 2) {
        const std::uint16_t class1 = glyphClass(r, subtable + r.u16(subtable + 8), first);
        const std::uint16_t class2 = glyphClass(r, subtable + r.u16(subtable + 10), second);
        const std::uint16_t count1 = r.u16(subtable + 12), count2 = r.u16(subtable + 14);
        if (class1 >= count1 || class2 >= count2) return 0;
        record = subtable + 16 + (std::size_t(class1) * count2 + class2) * (size1 + size2);
    } else {
        return 0;
    }
    matched = true;
    // ValueRecord fields in bit order; XAdvance is 0x0004, XAdvDevice 0x0040.
    double value = 0;
    std::size_t at = record;
    for (std::uint16_t bit = 1; bit <= 0x0080; bit = std::uint16_t(bit << 1)) {
        if (!(format1 & bit)) continue;
        if (bit == 0x0004) value += r.i16(at);
        if (bit == 0x0040 && r.u16(at)) {
            const std::size_t device = subtable + r.u16(at);
            const auto gdef = table("GDEF");
            if (r.u16(device + 4) == 0x8000 && gdef && r.u16(gdef->offset + 2) >= 3 && r.u32(gdef->offset + 14)) {
                value += itemVariationDelta(gdef->offset + r.u32(gdef->offset + 14), r.u16(device), r.u16(device + 2));
            }
        }
        at += 2;
    }
    return value;
}

double TrueTypeFont::kerning(std::uint16_t first, std::uint16_t second) const {
    const Reader r(m_data);
    double total = 0;
    for (const std::uint32_t lookup : m_kernLookups) {
        std::uint16_t type = r.u16(lookup);
        const std::uint16_t count = r.u16(lookup + 4);
        for (unsigned i = 0; i < count; ++i) {
            std::uint32_t subtable = lookup + r.u16(lookup + 6 + 2 * i);
            std::uint16_t subtableType = type;
            if (type == 9) {
                subtableType = r.u16(subtable + 2);
                subtable += r.u32(subtable + 4);
            }
            if (subtableType != 2) continue;
            bool matched = false;
            const double value = pairValue(subtable, first, second, matched);
            if (matched) {
                total += value;
                break;
            }
        }
    }
    return total;
}

bool textCanvasDrawsFamily(std::u16string_view family) {
    std::u16string_view name = family.substr(0, family.find(u','));
    const auto space = [](char16_t c) { return c == u' ' || c == u'\t' || c == u'\n' || c == u'\r' || c == u'\f'; };
    while (!name.empty() && space(name.front())) name.remove_prefix(1);
    while (!name.empty() && space(name.back())) name.remove_suffix(1);
    if (name.size() >= 2 && (name.front() == u'"' || name.front() == u'\'') && name.back() == name.front()) {
        name = name.substr(1, name.size() - 2);
    }
    constexpr std::u16string_view nunito = u"nunito";
    if (name.size() != nunito.size()) return false;
    for (std::size_t i = 0; i < name.size(); ++i) {
        const char16_t c = name[i] >= u'A' && name[i] <= u'Z' ? char16_t(name[i] + 32) : name[i];
        if (c != nunito[i]) return false;
    }
    return true;
}

std::vector<std::uint8_t> drawTextCanvas(TrueTypeFont& font, int resolution, const TextCanvasParams& params) {
    const std::size_t side = std::size_t(std::max(resolution, 0));
    std::vector<std::uint8_t> premultiplied(side * side * 4, 0);
    // Math.round(size * canvas.height)
    const double fontSize = std::floor(params.size * double(resolution) + 0.5);
    if (side == 0 || !(fontSize > 0) || fontSize > 1e5 || !textCanvasDrawsFamily(params.font)) {
        return unpremultiplyForUpload(premultiplied);
    }
    font.setVariation("wght", kWeight);
    const double scale = fontSize / font.unitsPerEm();
    // fillStyle = `rgba(${r}, ${g}, ${b}, 1)` with each channel Math.round(c * 255): a
    // non-finite channel makes the string invalid and keeps the default, black.
    std::uint8_t rgb[3] = {0, 0, 0};
    if (std::isfinite(params.color[0] * 255) && std::isfinite(params.color[1] * 255) && std::isfinite(params.color[2] * 255)) {
        for (int i = 0; i < 3; ++i) rgb[i] = std::uint8_t(std::clamp(std::floor(params.color[i] * 255 + 0.5), 0.0, 255.0));
    }
    // The middle baseline above the alphabetic one.
    const double ascender = font.typoAscender(), descender = -double(font.typoDescender());
    double middle = 0;
    if (ascender >= 0 && ascender + descender > 0 && ascender <= ascender + descender) {
        const double a = std::floor(ascender * fontSize / (ascender + descender) * 64 + 0.5) / 64;
        middle = (a - (fontSize - a)) / 2;
    }
    const double radians = std::isfinite(params.rotation) ? params.rotation * kPi / 180 : 0;
    const double cosR = std::cos(radians), sinR = std::sin(radians);
    const bool axisAligned = radians == 0;
    const double originX = params.posX * resolution, originY = params.posY * resolution;
    // Lines, 1.2 font sizes apart, centered on the origin.
    std::vector<std::u16string> lines(1);
    for (const char16_t c : params.text) {
        if (c == u'\n') lines.emplace_back();
        else lines.back() += c;
    }
    const double lineHeight = fontSize * 1.2;
    const double startY = -double(lines.size() - 1) * lineHeight / 2;
    std::vector<std::uint16_t> coverage;
    for (std::size_t lineIndex = 0; lineIndex < lines.size(); ++lineIndex) {
        const std::u16string& line = lines[lineIndex];
        std::vector<std::uint16_t> glyphs;
        for (std::size_t i = 0; i < line.size(); ++i) {
            std::uint32_t cp = line[i];
            if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < line.size() && line[i + 1] >= 0xDC00 && line[i + 1] <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (line[i + 1] - 0xDC00);
                ++i;
            }
            glyphs.push_back(font.glyphForCodePoint(cp));
        }
        std::vector<double> advances;
        double width = 0;
        for (std::size_t i = 0; i < glyphs.size(); ++i) {
            double advance = font.advance(glyphs[i]);
            if (i + 1 < glyphs.size()) advance += font.kerning(glyphs[i], glyphs[i + 1]);
            advances.push_back(advance * scale + kLetterSpacing);
            width += advances.back();
        }
        double x = 0;
        if (params.justify == u"center") x = -width / 2;
        else if (params.justify == u"right" || params.justify == u"end") x = -width;
        const double baseline = startY + double(lineIndex) * lineHeight + middle;
        for (std::size_t g = 0; g < glyphs.size(); ++g) {
            const double localX = x;
            x += advances[g];
            const std::vector<TrueTypeFont::Contour> contours = font.outline(glyphs[g]);
            if (contours.empty()) continue;
            double ox = originX + localX * cosR - baseline * sinR;
            double oy = originY + localX * sinR + baseline * cosR;
            if (axisAligned) {
                ox = std::floor(ox * 4 + 0.5) / 4;
                oy = std::floor(oy + 0.5);
            }
            // Flatten the outline to device-space polygons.
            std::vector<std::vector<std::pair<double, double>>> polygons;
            const auto device = [&](double px, double py) {
                const double gx = px * scale, gy = -py * scale;
                return std::pair<double, double>(ox + gx * cosR - gy * sinR, oy + gx * sinR + gy * cosR);
            };
            for (const TrueTypeFont::Contour& contour : contours) {
                if (contour.size() < 2) continue;
                // Start at an on-curve point, or at the midpoint of the first two off-curve points.
                std::size_t begin = 0;
                while (begin < contour.size() && !contour[begin].onCurve) ++begin;
                TrueTypeFont::Point start;
                if (begin == contour.size()) {
                    start = {(contour[0].x + contour[1].x) / 2, (contour[0].y + contour[1].y) / 2, true};
                    begin = 0;
                } else {
                    start = contour[begin];
                }
                std::vector<std::pair<double, double>> polygon{device(start.x, start.y)};
                TrueTypeFont::Point current = start;
                std::optional<TrueTypeFont::Point> control;
                const auto curveTo = [&](const TrueTypeFont::Point& c, const TrueTypeFont::Point& to) {
                    for (int s = 1; s <= kCurveSegments; ++s) {
                        const double t = double(s) / kCurveSegments, u = 1 - t;
                        polygon.push_back(device(u * u * current.x + 2 * u * t * c.x + t * t * to.x,
                                                 u * u * current.y + 2 * u * t * c.y + t * t * to.y));
                    }
                    current = to;
                };
                for (std::size_t k = 1; k <= contour.size(); ++k) {
                    const TrueTypeFont::Point& p = k == contour.size() ? start : contour[(begin + k) % contour.size()];
                    const bool closing = k == contour.size();
                    if (p.onCurve || closing) {
                        if (control) curveTo(*control, p);
                        else {
                            polygon.push_back(device(p.x, p.y));
                            current = p;
                        }
                        control.reset();
                    } else if (control) {
                        const TrueTypeFont::Point mid{(control->x + p.x) / 2, (control->y + p.y) / 2, true};
                        curveTo(*control, mid);
                        control = p;
                    } else {
                        control = p;
                    }
                }
                polygons.push_back(std::move(polygon));
            }
            // Coverage: the 16 x 16 samples of each pixel inside (nonzero).
            double minX = 1e300, minY = 1e300, maxX = -1e300, maxY = -1e300;
            for (const auto& polygon : polygons) {
                for (const auto& [px, py] : polygon) {
                    minX = std::min(minX, px), maxX = std::max(maxX, px);
                    minY = std::min(minY, py), maxY = std::max(maxY, py);
                }
            }
            if (!(minX <= maxX) || !std::isfinite(minX + maxX + minY + maxY)) continue;
            const int x0 = std::max(0, int(std::floor(minX))), x1 = std::min(int(side), int(std::ceil(maxX)) + 1);
            const int y0 = std::max(0, int(std::floor(minY))), y1 = std::min(int(side), int(std::ceil(maxY)) + 1);
            if (x0 >= x1 || y0 >= y1) continue;
            const int w = x1 - x0;
            coverage.assign(std::size_t(w) * std::size_t(y1 - y0), 0);
            std::vector<std::pair<double, int>> crossings;
            for (int row = y0; row < y1; ++row) {
                for (int sub = 0; sub < kSamples; ++sub) {
                    const double sy = row + (sub + 0.5) / kSamples;
                    crossings.clear();
                    for (const auto& polygon : polygons) {
                        for (std::size_t i = 0; i < polygon.size(); ++i) {
                            const auto& [xa, ya] = polygon[i];
                            const auto& [xb, yb] = polygon[(i + 1) % polygon.size()];
                            if (ya == yb) continue;
                            if (sy < std::min(ya, yb) || sy >= std::max(ya, yb)) continue;
                            crossings.emplace_back(xa + (sy - ya) * (xb - xa) / (yb - ya), yb > ya ? 1 : -1);
                        }
                    }
                    std::sort(crossings.begin(), crossings.end());
                    int winding = 0;
                    for (std::size_t i = 0; i + 1 <= crossings.size(); ++i) {
                        winding += crossings[i].second;
                        if (winding == 0 || i + 1 == crossings.size()) continue;
                        // Samples whose centre lies in [crossing i, crossing i + 1).
                        const double from = (crossings[i].first - x0) * kSamples, to = (crossings[i + 1].first - x0) * kSamples;
                        long first = long(std::ceil(from - 0.5)), last = long(std::ceil(to - 0.5)) - 1;
                        first = std::max(first, 0L);
                        last = std::min(last, long(w) * kSamples - 1);
                        for (long s = first; s <= last; ++s) ++coverage[std::size_t(row - y0) * std::size_t(w) + std::size_t(s / kSamples)];
                    }
                }
            }
            // Source-over in premultiplied 8-bit: the colour scaled by the
            // coverage, then the destination scaled by the rest.
            for (int row = y0; row < y1; ++row) {
                for (int col = x0; col < x1; ++col) {
                    const std::uint16_t count = coverage[std::size_t(row - y0) * std::size_t(w) + std::size_t(col - x0)];
                    if (!count) continue;
                    const unsigned aa = unsigned(std::lrint(255.0 * count / (kSamples * kSamples)));
                    if (!aa) continue;
                    std::uint8_t* px = &premultiplied[(std::size_t(row) * side + std::size_t(col)) * 4];
                    const unsigned scaleSrc = aa + 1;
                    const unsigned srcA = (255 * scaleSrc) >> 8;
                    for (int c = 0; c < 4; ++c) {
                        const unsigned src = ((c < 3 ? rgb[c] : 255u) * scaleSrc) >> 8;
                        px[c] = std::uint8_t(std::min(255u, src + ((px[c] * (256 - srcA)) >> 8)));
                    }
                }
            }
        }
    }
    return unpremultiplyForUpload(premultiplied);
}

} // namespace libnoisemaker
