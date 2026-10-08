#pragma once

// obj_parser.h -- port of the reference Wavefront OBJ parser
// (shaders/src/runtime/obj-parser.js). Same accepted syntax, same fan
// triangulation with reversed winding, same placeholder values for missing
// or out-of-range indices, and the same smooth-normal computation when the
// file has no `vn` lines. Numbers follow JavaScript parseFloat/parseInt and
// String.prototype.trim/split(/\s+/) semantics on UTF-16 code units, so a
// given text produces bit-identical Float32 arrays to the reference.

#include "core/value/js_string.h"

#include <cstddef>
#include <string>

#include <vector>

namespace nm {

// reference parseOBJ() result: de-indexed triangle-list vertex data, three
// vertices per triangle, in the reference's (v0, v2, v1) winding order.
struct ObjMeshData {
    std::vector<float> positions; // xyz per vertex
    std::vector<float> normals;   // xyz per vertex
    std::vector<float> uvs;       // uv per vertex
    std::ptrdiff_t vertexCount = 0;
};

// reference packMeshDataForTextures() result: one RGBA32F texel per vertex,
// row-major, texWidth * texHeight texels. positionData w is 1 for a used
// vertex and 0 for every other texel.
struct PackedMeshData {
    std::vector<float> positionData;
    std::vector<float> normalData;
    std::vector<float> uvData;
    int vertexCount = 0; // vertices stored (truncated to texWidth * texHeight)
};

// Parses OBJ text. Never throws: malformed lines, numbers and indices take
// the reference's fallback values (0 for numbers, (0,0,0) positions,
// (0,0,1) normals and (0,0) UVs for unresolved indices).
ObjMeshData parseOBJ(const JsString& objText);

// Decodes OBJ file bytes as the reference's fetch().text() does (UTF-8,
// initial BOM dropped, invalid sequences replaced by U+FFFD) and parses
// them. Throws std::runtime_error("Failed to load OBJ: <reason>") when the
// file cannot be read.
ObjMeshData loadOBJ(const std::string& path);

// Packs parsed mesh data into texture-sized arrays. Warns and
// truncates when the mesh has more vertices than texWidth * texHeight.
// Throws std::invalid_argument for a negative texture dimension.
PackedMeshData packMeshDataForTextures(const ObjMeshData& mesh, int texWidth, int texHeight);

} // namespace nm
