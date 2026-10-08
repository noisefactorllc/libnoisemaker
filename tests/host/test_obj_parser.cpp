// Unit tests for nm::parseOBJ / nm::loadOBJ / nm::packMeshDataForTextures
// (qt/noisemaker/runtime/obj_parser.{h,cpp}). Plain assert-style checks.
// Every expected value is hand-traced against the reference
// shaders/src/runtime/obj-parser.js; parity/check_obj_parser.mjs compares
// the same functions against the reference itself under Node.

#include "host/obj_parser.h"

#include <chrono>
#include <filesystem>
#include <fstream>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void check(bool condition, const char* description) {
    if (condition) {
        std::printf("PASS: %s\n", description);
    } else {
        std::printf("FAIL: %s\n", description);
        ++g_failures;
    }
}

bool sameBits(float a, float b) {
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    std::memcpy(&x, &a, sizeof(x));
    std::memcpy(&y, &b, sizeof(y));
    return x == y;
}

bool equals(const std::vector<float>& actual, const std::vector<float>& expected) {
    if (actual.size() != expected.size()) return false;
    for (std::size_t i = 0; i < actual.size(); ++i) {
        if (!sameBits(actual[i], expected[i])) return false;
    }
    return true;
}

nm::ObjMeshData parse(const char16_t* text) {
    return nm::parseOBJ(text);
}

} // namespace

int main() {
    const float inf = std::numeric_limits<float>::infinity();

    // Quad with v/vt/vn: fan triangulation, reversed winding (v0, v2, v1).
    {
        const auto mesh = parse(u"v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\n"
                                u"vt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\nvn 0 0 1\n"
                                u"f 1/1/1 2/2/1 3/3/1 4/4/1\n");
        check(mesh.vertexCount == 6, "quad: two triangles, six vertices");
        check(equals(mesh.positions, {0, 0, 0, 1, 1, 0, 1, 0, 0, 0, 0, 0, 0, 1, 0, 1, 1, 0}),
              "quad: positions in (v0, v2, v1), (v0, v3, v2) order");
        check(equals(mesh.uvs, {0, 0, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1}), "quad: uvs follow the same order");
        check(equals(mesh.normals, {0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1}),
              "quad: file normals are used as given");
    }

    // Pentagon: three fan triangles.
    {
        const auto mesh = parse(u"v 0 0 0\nv 1 0 0\nv 2 1 0\nv 1 2 0\nv 0 1 0\nf 1 2 3 4 5\n");
        check(mesh.vertexCount == 9, "pentagon: three triangles");
        check(equals(std::vector<float>(mesh.positions.begin() + 18, mesh.positions.end()), {0, 0, 0, 0, 1, 0, 1, 2, 0}),
              "pentagon: last triangle is (v0, v4, v3)");
    }

    // Missing normals: face normal of the stored (v0, v2, v1) triangle.
    {
        const auto mesh = parse(u"v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");
        check(equals(mesh.normals, {0, 0, -1, 0, 0, -1, 0, 0, -1}),
              "computed normal: (v2 - v0) x (v1 - v0) of the stored order is (0, 0, -1)");
    }

    // Smooth normals: vertices that share a rounded position average the
    // face normals of every triangle that uses them.
    {
        const auto mesh = parse(u"v 0 0 0\nv 0 1 0\nv 1 0 0\nv 0 0 1\nf 1 2 3\nf 1 4 2\n");
        const float h = static_cast<float>(1.0 / std::sqrt(2.0));
        check(equals(mesh.normals, {h, 0, h, 0, 0, 1, h, 0, h, h, 0, h, h, 0, h, 1, 0, 0}),
              "computed normals: shared edge vertices average (0,0,1) and (1,0,0)");
    }

    // Position keys round to 1e-4 with Math.round, and a -0 key equals +0:
    // vertex 3 (-0.00001, 0.00004, 0) shares vertex 0's averaged normal.
    {
        const auto mesh = parse(u"v 0 0 0\nv 1 0 0\nv 0 1 0\nv -0.00001 0.00004 0\nv 0 0 1\nv 0 2 0\n"
                                u"f 1 2 3\nf 4 5 6\n");
        const bool shared = sameBits(mesh.normals[0], mesh.normals[9]) && sameBits(mesh.normals[1], mesh.normals[10])
            && sameBits(mesh.normals[2], mesh.normals[11]);
        check(shared && !sameBits(mesh.normals[2], -1.0f),
              "computed normals: (-0.00001, 0.00004, 0) shares the key of (0, 0, 0)");
    }
    // 0.00005 rounds up to 0.0001: a separate key, so vertex 0 keeps its own
    // face normal.
    {
        const auto mesh = parse(u"v 0 0 0\nv 1 0 0\nv 0 1 0\nv 0.00005 0 0\nv 0 0 1\nv 0 2 0\n"
                                u"f 1 2 3\nf 4 5 6\n");
        check(equals(std::vector<float>(mesh.normals.begin(), mesh.normals.begin() + 3), {0, 0, -1})
                  && !sameBits(mesh.normals[9], 0.0f),
              "computed normals: 0.00005 rounds up to 0.0001, a separate key");
    }

    // Normals present in the file but not referenced: placeholders, not
    // computed normals.
    {
        const auto mesh = parse(u"v 0 0 0\nv 1 0 0\nv 0 1 0\nvn 1 0 0\nf 1 2 3\n");
        check(equals(mesh.normals, {0, 0, 1, 0, 0, 1, 0, 0, 1}), "unreferenced vn: (0, 0, 1) placeholders");
    }

    // Unresolved indices take placeholders.
    {
        const auto mesh = parse(u"v 1 2 3\nv 4 5 6\nvn 0 1 0\nf 1/9/1 2/1/7 9 0 -1\n");
        check(mesh.vertexCount == 9, "placeholders: five face vertices make three triangles");
        check(equals(std::vector<float>(mesh.positions.begin(), mesh.positions.begin() + 9), {1, 2, 3, 0, 0, 0, 4, 5, 6}),
              "placeholders: index 9 (out of range) is (0, 0, 0)");
        check(equals(std::vector<float>(mesh.normals.begin(), mesh.normals.begin() + 9), {0, 1, 0, 0, 0, 1, 0, 0, 1}),
              "placeholders: missing or out-of-range normal is (0, 0, 1)");
        check(equals(std::vector<float>(mesh.positions.begin() + 9, mesh.positions.end()),
                     {1, 2, 3, 0, 0, 0, 0, 0, 0, 1, 2, 3, 0, 0, 0, 0, 0, 0}),
              "placeholders: indices 0 and -1 are invalid (no relative indices)");
    }

    // parseInt field semantics: "1.9" -> 1, "+2" -> 2, empty uv field -> -1.
    {
        const auto mesh = parse(u"v 1 0 0\nv 2 0 0\nv 3 0 0\nvt 0.5 0.25\nf 1.9/1 +2//x 3/abc\n");
        check(equals(mesh.positions, {1, 0, 0, 3, 0, 0, 2, 0, 0}), "parseInt: '1.9' and '+2' resolve");
        check(equals(mesh.uvs, {0.5f, 0.25f, 0, 0, 0, 0}), "parseInt: empty or non-numeric uv field is a placeholder");
    }

    // parseFloat prefixes and `|| 0`.
    {
        const auto mesh = parse(u"v 1e400 -0 abc\nv .5 5. -.5e1\nv 0x10 1_000 Infinityx\nf 1 2 3\n");
        check(mesh.vertexCount == 3, "numbers: one triangle");
        check(equals(mesh.positions, {inf, 0, 0, 0, 1, inf, 0.5f, 5, -5}), "numbers: overflow, prefixes, hex and separators");
        check(sameBits(mesh.positions[1], 0.0f), "numbers: -0 from `-0 || 0` is +0");
        const auto more = parse(u"v -Infinity 1e 2e+\nv\nv 5e-324 1.7976931348623159e308 -1e-400\nf 1 2 3\n");
        check(equals(more.positions, {-inf, 1, 2, 0, inf, 0, 0, 0, 0}),
              "numbers: -Infinity, dangling exponents, overflow to Infinity, missing values");
        check(sameBits(more.positions[3], 0.0f) && sameBits(more.positions[5], 0.0f),
              "numbers: a Float32-underflowing denormal and -1e-400 store +0");
    }

    // Whitespace: JS trim and \s include NBSP, U+2003, U+3000, BOM; CR is
    // whitespace but only LF splits lines.
    {
        const auto mesh = parse(u"﻿ v 1\t2　3\r\n"
                                u"v 4\u000B5\u000C6 v 7 8 9\rv 1 1 1\n"
                                u"   # v 9 9 9\n\n"
                                u"v 0 1 0\n"
                                u"f 1 2 3\n");
        check(mesh.vertexCount == 3, "whitespace: three vertices parsed");
        check(equals(mesh.positions, {1, 2, 3, 0, 1, 0, 4, 5, 6}),
              "whitespace: unicode spaces separate tokens, CR does not end a line");
    }

    // Unknown commands are ignored; a two-vertex face makes no triangle.
    {
        const auto mesh = parse(u"o obj\nV 1 1 1\nvp 1 1\nv 1 1 1\nv 2 2 2\nf 1 2\ns 1\n");
        check(mesh.vertexCount == 0 && mesh.positions.empty(), "no triangles: vertexCount 0 and empty arrays");
    }

    // Packing: w = 1 marks used vertices; truncation to the texture size.
    {
        const auto mesh = parse(u"v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\nvt 0.25 0.75\nf 1/1 2/1 3/1 4/1\n");
        const nm::PackedMeshData grid4x4 = nm::packMeshDataForTextures(mesh, 4, 4);
        check(grid4x4.vertexCount == 6 && grid4x4.positionData.size() == 64, "pack 4x4: six vertices in 16 texels");
        check(sameBits(grid4x4.positionData[5 * 4 + 3], 1.0f) && sameBits(grid4x4.positionData[6 * 4 + 3], 0.0f),
              "pack 4x4: w is 1 for used texels and 0 after them");
        check(sameBits(grid4x4.uvData[4], 0.25f) && sameBits(grid4x4.uvData[5], 0.75f) && sameBits(grid4x4.uvData[6], 0.0f),
              "pack 4x4: uv in xy, zw zero");
        check(sameBits(grid4x4.normalData[3], 0.0f), "pack 4x4: normal w is zero");
        const nm::PackedMeshData grid2x2 = nm::packMeshDataForTextures(mesh, 2, 2);
        check(grid2x2.vertexCount == 4 && sameBits(grid2x2.positionData[3 * 4 + 3], 1.0f),
              "pack 2x2: truncated to four vertices");
        bool threw = false;
        try {
            nm::packMeshDataForTextures(mesh, -1, 4);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        check(threw, "pack: negative dimension throws std::invalid_argument");
    }

    // loadOBJ: UTF-8 file with BOM and CRLF; missing file error message.
    {
        const auto path = std::filesystem::temp_directory_path() /
            ("nm-obj-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".obj");
        {
            std::ofstream file(path, std::ios::binary);
            file << "\xEF\xBB\xBFv 0 0 0\r\nv 1 0 0\r\nv 0 1 0\r\nf 1 2 3\r\n";
        }
        const nm::ObjMeshData mesh = nm::loadOBJ(path.string());
        check(mesh.vertexCount == 3 && equals(mesh.positions, {0, 0, 0, 0, 1, 0, 1, 0, 0}),
              "loadOBJ: BOM and CRLF file parses");
        std::filesystem::remove(path);
        std::string message;
        try {
            nm::loadOBJ(path.string());
        } catch (const std::runtime_error& e) {
            message = e.what();
        }
        check(message.rfind("Failed to load OBJ: ", 0) == 0,
              "loadOBJ: missing file throws 'Failed to load OBJ: ...'");
    }

    if (g_failures == 0) {
        std::printf("ALL PASS\n");
        return 0;
    }
    std::printf("%d FAILURE(S)\n", g_failures);
    return 1;
}
