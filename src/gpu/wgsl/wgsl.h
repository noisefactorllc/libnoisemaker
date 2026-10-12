#pragma once

// WGSL source analysis of the reference WebGPU backend.
//
// The reference does not reflect shaders: it reads them with regular
// expressions and small scanners (shaders/src/runtime/backends/webgpu.js,
// WebGPUBackend methods of the same names). Their results decide which
// bindings reach a bind group, which entry point a pipeline uses, how large
// the uniform buffer is and where each uniform is packed. Each function here
// ports one method line for line, with the reference's own patterns run by
// jsre. tools/nm-dump's `wgsl` command and parity/check_wgsl.mjs compare them
// with the reference over every catalog shader.

#include "core/value/value.h"

#include <optional>
#include <utility>
#include <vector>

namespace libnoisemaker::wgsl {

// detectEntryPoints
struct EntryPoints {
    std::optional<JsString> vertex;
    std::optional<JsString> fragment;
    std::optional<JsString> compute;
};

// One element of parseShaderBindings' result.
struct Binding {
    double group = 0;
    double binding = 0;
    JsString type;     // storage_texture, texture, sampler, uniform, storage or unknown
    JsString name;
    JsString storage;  // the var<...> address space and access, or empty
    JsString typeDecl;
};

// parsePackedUniformLayout's slot-based entries: {name, slot, components}.
struct SlotField {
    JsString name;
    double slot = 0;
    JsString components;
};

// parseWgslStructByteLayout's entries: {name, offset, size, type, components}.
struct ByteField {
    JsString name;
    double offset = 0;
    double size = 0;
    JsString type;
    double components = 0;
};

// parsePackedUniformLayout: null, {type: 'byte', layout} (the layout array
// also carries structSize), or an array of slot fields.
struct PackedLayout {
    enum class Kind { None, Byte, Slots };
    Kind kind = Kind::None;
    std::vector<ByteField> bytes;
    double structSize = 0;
    std::vector<SlotField> slots;
};

// An insertion-ordered map, as the reference's Map and Set iterate.
template <class K, class V>
using OrderedMap = std::vector<std::pair<K, V>>;

JsString stripComments(const JsString& source);
EntryPoints detectEntryPoints(const JsString& source);
// The names /@compute[^f]*fn\s+(\w+)/g finds, in order (compileComputeProgram).
std::vector<JsString> computeEntryPoints(const JsString& source);
// /@compute\s/, /@fragment\s/ and /@vertex\s/ (compileProgram, compileRenderProgram).
bool hasComputeEntry(const JsString& source);
bool hasFragmentEntry(const JsString& source);
bool hasVertexEntry(const JsString& source);
std::vector<Binding> parseShaderBindings(const JsString& source);
// entry point -> binding indices it reaches, both in insertion order.
OrderedMap<JsString, std::vector<double>> parseEntryPointBindings(const JsString& source,
                                                                  const std::vector<Binding>& bindings);
double parseDeclaredUniformBufferSize(const JsString& source);
PackedLayout parsePackedUniformLayout(const JsString& source);
// sampler name -> the one texture it samples, or nullopt when it samples several.
OrderedMap<JsString, std::optional<JsString>> parseSamplerTextureUses(const JsString& source);
bool hasShaderBindings(const JsString& source);
// injectDefines: `const NAME: type = value;` lines before the source.
JsString injectDefines(const JsString& source, const Value& defines);

// The results above as the reference's values serialize to JSON: optional
// strings and missing layouts become null, maps become [key, value] pairs,
// and a byte layout keeps its structSize as {type, layout, structSize}.
Value toValue(const EntryPoints& entryPoints);
Value toValue(const std::vector<Binding>& bindings);
Value toValue(const OrderedMap<JsString, std::vector<double>>& entryPointBindings);
Value toValue(const PackedLayout& layout);
Value toValue(const OrderedMap<JsString, std::optional<JsString>>& samplerTextures);

}  // namespace libnoisemaker::wgsl
