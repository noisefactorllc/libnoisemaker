#include "obj_parser.h"


#include <algorithm>
#include <charconv>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string_view>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <unordered_map>

namespace nm {

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kInfinity = std::numeric_limits<double>::infinity();

// ECMAScript WhiteSpace and LineTerminator code points: the set shared by
// String.prototype.trim, the regular-expression class \s, parseFloat and
// parseInt. The Zs members are listed explicitly (U+0020, U+00A0, U+1680,
// U+2000..U+200A, U+202F, U+205F, U+3000).
bool isJsWhitespace(char16_t c) {
    switch (c) {
    case 0x0009: case 0x000A: case 0x000B: case 0x000C: case 0x000D:
    case 0x0020: case 0x00A0: case 0x1680: case 0x2028: case 0x2029:
    case 0x202F: case 0x205F: case 0x3000: case 0xFEFF:
        return true;
    default:
        return c >= 0x2000 && c <= 0x200A;
    }
}

bool isAsciiDigit(char16_t c) {
    return c >= u'0' && c <= u'9';
}

// JavaScript `value || 0` for a Number: NaN, +0 and -0 all become +0.
double orZero(double value) {
    return (std::isnan(value) || value == 0.0) ? 0.0 : value;
}

// Decimal digits -> value, saturating far above any array length or
// exponent that matters (2^53 is exact; larger values only need to stay
// larger).
double accumulateDigits(std::u16string_view digits) {
    double value = 0.0;
    for (const char16_t c : digits) {
        if (value < 9007199254740992.0) {
            value = value * 10.0 + static_cast<double>(c - u'0');
        } else {
            value = 9007199254740992.0 * 10.0;
        }
    }
    return value;
}

// parseFloat(token) for a token with no whitespace: the value of the longest
// prefix that is a StrDecimalLiteral ([+-] Infinity | digits [. [digits]]
// [exponent] | . digits [exponent]), or NaN when no prefix is one.
double jsParseFloat(std::u16string_view token) {
    const std::size_t n = token.size();
    std::size_t i = 0;
    bool negative = false;
    if (i < n && (token[i] == u'+' || token[i] == u'-')) {
        negative = token[i] == u'-';
        ++i;
    }
    if (token.substr(i).starts_with(u"Infinity")) {
        return negative ? -kInfinity : kInfinity;
    }

    const std::size_t intStart = i;
    while (i < n && isAsciiDigit(token[i])) ++i;
    const std::size_t intEnd = i;

    std::size_t fracStart = i;
    std::size_t fracEnd = i;
    if (i < n && token[i] == u'.') {
        std::size_t j = i + 1;
        while (j < n && isAsciiDigit(token[j])) ++j;
        if (intEnd > intStart || j > i + 1) {
            fracStart = i + 1;
            fracEnd = j;
            i = j;
        }
    }
    if (intEnd == intStart && fracEnd == fracStart) {
        return kNaN;
    }

    bool hasExponent = false;
    bool exponentNegative = false;
    std::size_t expStart = 0;
    std::size_t expEnd = 0;
    if (i < n && (token[i] == u'e' || token[i] == u'E')) {
        std::size_t k = i + 1;
        bool expSignNegative = false;
        if (k < n && (token[k] == u'+' || token[k] == u'-')) {
            expSignNegative = token[k] == u'-';
            ++k;
        }
        const std::size_t digitsStart = k;
        while (k < n && isAsciiDigit(token[k])) ++k;
        if (k > digitsStart) {
            hasExponent = true;
            exponentNegative = expSignNegative;
            expStart = digitsStart;
            expEnd = k;
        }
    }

    const std::u16string_view intDigits = token.substr(intStart, intEnd - intStart);
    const std::u16string_view fracDigits = token.substr(fracStart, fracEnd - fracStart);
    const std::u16string_view expDigits = token.substr(expStart, expEnd - expStart);

    // Canonical ASCII form for a correctly rounded, locale-independent
    // conversion: [-]digits[.digits][e[-]digits].
    std::string text;
    text.reserve(intDigits.size() + fracDigits.size() + expDigits.size() + 4);
    if (negative) text += '-';
    const auto appendAscii = [&](std::u16string_view digits) {
        for (char16_t digit : digits) text.push_back(static_cast<char>(digit));
    };
    if (intDigits.empty()) text += '0';
    else appendAscii(intDigits);
    if (!fracDigits.empty()) {
        text += '.';
        appendAscii(fracDigits);
    }
    if (hasExponent) {
        text += 'e';
        if (exponentNegative) text += '-';
        appendAscii(expDigits);
    }

    double value = 0.0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size()) {
        return value;
    }

    // Out-of-range results here have valid syntax:
    // overflow, which JavaScript rounds to +/-Infinity, and total underflow,
    // which JavaScript rounds to +/-0. Decide from the decimal order of the
    // leading non-zero digit.
    double order = 0.0;
    bool nonZero = false;
    for (std::size_t p = 0; p < intDigits.size(); ++p) {
        if (intDigits[p] != u'0') {
            order = static_cast<double>(intDigits.size() - p - 1);
            nonZero = true;
            break;
        }
    }
    if (!nonZero) {
        for (std::size_t q = 0; q < fracDigits.size(); ++q) {
            if (fracDigits[q] != u'0') {
                order = -static_cast<double>(q + 1);
                nonZero = true;
                break;
            }
        }
    }
    if (hasExponent) {
        const double exponent = accumulateDigits(expDigits);
        order += exponentNegative ? -exponent : exponent;
    }
    const double magnitude = (nonZero && order > 0.0) ? kInfinity : 0.0;
    return negative ? -magnitude : magnitude;
}

// parseInt(token, 10) for a token with no whitespace: optional sign, then
// the longest run of ASCII digits; NaN when there are none.
double jsParseInt(std::u16string_view token) {
    std::size_t i = 0;
    bool negative = false;
    if (i < token.size() && (token[i] == u'+' || token[i] == u'-')) {
        negative = token[i] == u'-';
        ++i;
    }
    const std::size_t start = i;
    while (i < token.size() && isAsciiDigit(token[i])) ++i;
    if (i == start) {
        return kNaN;
    }
    const double value = accumulateDigits(token.substr(start, i - start));
    return negative ? -value : value;
}

// reference `idx >= 0 && idx < array.length` (false for NaN).
bool indexInRange(double index, std::size_t size) {
    return index >= 0.0 && index < static_cast<double>(size);
}

// Math.round: nearest integer, ties toward +Infinity. x - floor(x) is exact
// for every finite double, so the tie test is exact too.
double jsRound(double x) {
    const double floored = std::floor(x);
    const double fraction = x - floored;
    return fraction >= 0.5 ? floored + 1.0 : floored;
}

struct FaceVertex {
    double vIdx;
    double vtIdx;
    double vnIdx;
};

// Position key of computeFaceNormals: `${round(x)},${round(y)},${round(z)}`
// with round(v) = Math.round(v * 10000) / 10000. Number-to-string is
// injective except that -0 and +0 both print "0", and no key component can
// be NaN, so the bit patterns with -0 folded into +0 are an equivalent key.
struct PositionKey {
    std::array<std::uint64_t, 3> bits;
    bool operator==(const PositionKey& other) const { return bits == other.bits; }
};

struct PositionKeyHash {
    std::size_t operator()(const PositionKey& key) const {
        std::uint64_t h = 1469598103934665603ULL;
        for (const std::uint64_t b : key.bits) {
            h ^= b;
            h *= 1099511628211ULL;
        }
        return static_cast<std::size_t>(h);
    }
};

std::uint64_t keyComponentBits(double v) {
    double rounded = jsRound(v * 10000.0) / 10000.0;
    if (rounded == 0.0) rounded = 0.0; // -0 prints as "0"
    std::uint64_t bits = 0;
    std::memcpy(&bits, &rounded, sizeof(bits));
    return bits;
}

struct NormalAccumulator {
    double nx = 0.0;
    double ny = 0.0;
    double nz = 0.0;
};

// reference computeFaceNormals: per-triangle normal (b - a) x (c - a) of the
// stored (v0, v2, v1) order, rounded to Float32 as the reference's
// Float32Array faceNormals, then averaged over every vertex that shares a
// rounded position.
void computeFaceNormals(const std::vector<double>& positions, std::vector<double>& normals) {
    const std::size_t vertexCount = positions.size() / 3;
    const std::size_t triangleCount = vertexCount / 3;

    std::vector<float> faceNormals(triangleCount * 3);
    for (std::size_t tri = 0; tri < triangleCount; ++tri) {
        const std::size_t i0 = tri * 9;
        const std::size_t i1 = i0 + 3;
        const std::size_t i2 = i0 + 6;

        const double ax = positions[i0], ay = positions[i0 + 1], az = positions[i0 + 2];
        const double bx = positions[i1], by = positions[i1 + 1], bz = positions[i1 + 2];
        const double cx = positions[i2], cy = positions[i2 + 1], cz = positions[i2 + 2];

        const double e1x = bx - ax, e1y = by - ay, e1z = bz - az;
        const double e2x = cx - ax, e2y = cy - ay, e2z = cz - az;

        double nx = e1y * e2z - e1z * e2y;
        double ny = e1z * e2x - e1x * e2z;
        double nz = e1x * e2y - e1y * e2x;

        const double len = std::sqrt(nx * nx + ny * ny + nz * nz);
        if (len > 0.0001) {
            nx /= len;
            ny /= len;
            nz /= len;
        } else {
            nx = 0.0;
            ny = 0.0;
            nz = 1.0;
        }

        faceNormals[tri * 3] = static_cast<float>(nx);
        faceNormals[tri * 3 + 1] = static_cast<float>(ny);
        faceNormals[tri * 3 + 2] = static_cast<float>(nz);
    }

    std::unordered_map<PositionKey, std::size_t, PositionKeyHash> keyIndex;
    std::vector<NormalAccumulator> accumulators;
    std::vector<std::size_t> vertexKey(vertexCount);
    for (std::size_t v = 0; v < vertexCount; ++v) {
        const PositionKey key{{keyComponentBits(positions[v * 3]),
                               keyComponentBits(positions[v * 3 + 1]),
                               keyComponentBits(positions[v * 3 + 2])}};
        const auto inserted = keyIndex.emplace(key, accumulators.size());
        if (inserted.second) {
            accumulators.push_back(NormalAccumulator{});
        }
        const std::size_t index = inserted.first->second;
        vertexKey[v] = index;

        // faceNormals holds Float32 values; triangles cover every vertex.
        const std::size_t triIdx = v / 3;
        NormalAccumulator& acc = accumulators[index];
        acc.nx += static_cast<double>(faceNormals[triIdx * 3]);
        acc.ny += static_cast<double>(faceNormals[triIdx * 3 + 1]);
        acc.nz += static_cast<double>(faceNormals[triIdx * 3 + 2]);
    }

    for (NormalAccumulator& acc : accumulators) {
        const double len = std::sqrt(acc.nx * acc.nx + acc.ny * acc.ny + acc.nz * acc.nz);
        if (len > 0.0001) {
            acc.nx /= len;
            acc.ny /= len;
            acc.nz /= len;
        } else {
            acc.nx = 0.0;
            acc.ny = 0.0;
            acc.nz = 1.0;
        }
    }

    for (std::size_t v = 0; v < vertexCount; ++v) {
        const NormalAccumulator& acc = accumulators[vertexKey[v]];
        normals[v * 3] = acc.nx;
        normals[v * 3 + 1] = acc.ny;
        normals[v * 3 + 2] = acc.nz;
    }
}

std::vector<float> toFloat32(const std::vector<double>& values) {
    std::vector<float> out(values.size());
    for (std::size_t i = 0; i < values.size(); ++i) {
        out[i] = static_cast<float>(values[i]);
    }
    return out;
}

} // namespace

ObjMeshData parseOBJ(const JsString& objText) {
    std::vector<std::array<double, 3>> rawPositions;
    std::vector<std::array<double, 3>> rawNormals;
    std::vector<std::array<double, 2>> rawUVs;

    std::vector<double> positions;
    std::vector<double> normals;
    std::vector<double> uvs;

    const auto addVertex = [&](const FaceVertex& v) {
        if (indexInRange(v.vIdx, rawPositions.size())) {
            const auto& p = rawPositions[static_cast<std::size_t>(v.vIdx)];
            positions.insert(positions.end(), p.begin(), p.end());
        } else {
            positions.insert(positions.end(), {0.0, 0.0, 0.0});
        }
        if (indexInRange(v.vnIdx, rawNormals.size())) {
            const auto& nrm = rawNormals[static_cast<std::size_t>(v.vnIdx)];
            normals.insert(normals.end(), nrm.begin(), nrm.end());
        } else {
            normals.insert(normals.end(), {0.0, 0.0, 1.0});
        }
        if (indexInRange(v.vtIdx, rawUVs.size())) {
            const auto& uv = rawUVs[static_cast<std::size_t>(v.vtIdx)];
            uvs.insert(uvs.end(), uv.begin(), uv.end());
        } else {
            uvs.insert(uvs.end(), {0.0, 0.0});
        }
    };

    const std::u16string_view text(objText);
    std::vector<std::u16string_view> parts;
    std::vector<FaceVertex> faceVerts;
    std::size_t lineStart = 0;
    while (lineStart <= text.size()) {
        // objText.split('\n'): only LF separates lines.
        std::size_t lineEnd = text.find(u'\n', lineStart);
        if (lineEnd == std::u16string_view::npos) lineEnd = text.size();

        // rawLine.trim()
        std::size_t b = lineStart;
        std::size_t e = lineEnd;
        while (b < e && isJsWhitespace(text[b])) ++b;
        while (e > b && isJsWhitespace(text[e - 1])) --e;
        const std::u16string_view line = text.substr(b, e - b);
        lineStart = lineEnd + 1;

        if (line.empty() || line.starts_with(u'#')) continue;

        // line.split(/\s+/) on a trimmed, non-empty line.
        parts.clear();
        std::size_t tokenStart = 0;
        for (std::size_t k = 0; k <= line.size(); ++k) {
            if (k == line.size() || isJsWhitespace(line[k])) {
                if (k > tokenStart) parts.push_back(line.substr(tokenStart, k - tokenStart));
                tokenStart = k + 1;
            }
        }

        const std::u16string_view cmd = parts[0];
        const auto numberAt = [&](std::size_t index) {
            return index < parts.size() ? orZero(jsParseFloat(parts[index])) : 0.0;
        };

        if (cmd == u"v") {
            rawPositions.push_back({numberAt(1), numberAt(2), numberAt(3)});
        } else if (cmd == u"vn") {
            rawNormals.push_back({numberAt(1), numberAt(2), numberAt(3)});
        } else if (cmd == u"vt") {
            rawUVs.push_back({numberAt(1), numberAt(2)});
        } else if (cmd == u"f") {
            faceVerts.clear();
            for (std::size_t i = 1; i < parts.size(); ++i) {
                // parts[i].split('/'): the first three fields.
                std::array<std::u16string_view, 3> fields;
                std::array<bool, 3> present{false, false, false};
                std::size_t fieldStart = 0;
                std::size_t fieldIndex = 0;
                const std::u16string_view part = parts[i];
                for (std::size_t k = 0; k <= part.size() && fieldIndex < 3; ++k) {
                    if (k == part.size() || part[k] == u'/') {
                        fields[fieldIndex] = part.substr(fieldStart, k - fieldStart);
                        present[fieldIndex] = true;
                        ++fieldIndex;
                        fieldStart = k + 1;
                    }
                }
                // OBJ indices are 1-based. An absent or empty uv/normal field
                // is -1 (the reference's `indices[n] ? ... : -1`).
                const double vIdx = jsParseInt(fields[0]) - 1.0;
                const double vtIdx = (present[1] && !fields[1].empty()) ? jsParseInt(fields[1]) - 1.0 : -1.0;
                const double vnIdx = (present[2] && !fields[2].empty()) ? jsParseInt(fields[2]) - 1.0 : -1.0;
                faceVerts.push_back({vIdx, vtIdx, vnIdx});
            }

            // Fan triangulation, reversed winding (OBJ CW to OpenGL CCW).
            for (std::size_t i = 1; i + 1 < faceVerts.size(); ++i) {
                addVertex(faceVerts[0]);
                addVertex(faceVerts[i + 1]);
                addVertex(faceVerts[i]);
            }
        }
    }

    const std::ptrdiff_t vertexCount = static_cast<std::ptrdiff_t>(positions.size() / 3);
    if (rawNormals.empty() && vertexCount > 0) {
        computeFaceNormals(positions, normals);
    }

    ObjMeshData mesh;
    mesh.positions = toFloat32(positions);
    mesh.normals = toFloat32(normals);
    mesh.uvs = toFloat32(uvs);
    mesh.vertexCount = vertexCount;
    return mesh;
}

ObjMeshData loadOBJ(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("Failed to load OBJ: cannot open file");
    const std::string bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (file.bad()) throw std::runtime_error("Failed to load OBJ: cannot read file");
    JsString text = utf8_to_js(bytes);
    if (!text.empty() && text.front() == u'\ufeff') text.erase(text.begin());
    return parseOBJ(text);
}

PackedMeshData packMeshDataForTextures(const ObjMeshData& mesh, int texWidth, int texHeight) {
    if (texWidth < 0 || texHeight < 0) {
        throw std::invalid_argument("packMeshDataForTextures: texture dimensions must not be negative");
    }
    const std::ptrdiff_t maxVertices = static_cast<std::ptrdiff_t>(texWidth) * static_cast<std::ptrdiff_t>(texHeight);
    const std::ptrdiff_t vertexCount = mesh.vertexCount;

    if (vertexCount > maxVertices) {
        std::cerr << "[OBJ] Mesh has " << vertexCount << " vertices, but texture can only hold "
                  << maxVertices << ". Truncating.\n";
    }

    const std::ptrdiff_t usedVertices = std::min(vertexCount, maxVertices);
    const std::size_t pixelCount = static_cast<std::size_t>(maxVertices);

    PackedMeshData packed;
    packed.positionData.assign(pixelCount * 4, 0.0f);
    packed.normalData.assign(pixelCount * 4, 0.0f);
    packed.uvData.assign(pixelCount * 4, 0.0f);

    for (std::ptrdiff_t v = 0; v < usedVertices; ++v) {
        const std::size_t i = static_cast<std::size_t>(v);
        const std::size_t pi = i * 4;
        const std::size_t vi3 = i * 3;
        const std::size_t vi2 = i * 2;

        packed.positionData[pi] = mesh.positions[vi3];
        packed.positionData[pi + 1] = mesh.positions[vi3 + 1];
        packed.positionData[pi + 2] = mesh.positions[vi3 + 2];
        packed.positionData[pi + 3] = 1.0f;

        packed.normalData[pi] = mesh.normals[vi3];
        packed.normalData[pi + 1] = mesh.normals[vi3 + 1];
        packed.normalData[pi + 2] = mesh.normals[vi3 + 2];

        packed.uvData[pi] = mesh.uvs[vi2];
        packed.uvData[pi + 1] = mesh.uvs[vi2 + 1];
    }

    packed.vertexCount = static_cast<int>(usedVertices);
    return packed;
}

} // namespace nm
