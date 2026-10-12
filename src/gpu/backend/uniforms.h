#pragma once

// Uniform buffer contents, as the reference WebGPU backend writes them.
//
// Ports WebGPUBackend.createUniformBuffer, packUniforms, packUniformsWithLayout,
// packUniformsWithByteLayout, _resolveUniformAlias and createSingleUniformBuffer
// (shaders/src/runtime/backends/webgpu.js). The packer keeps the backend's
// state across calls, because the bytes depend on it: the merged uniform object
// keeps every key it ever held (cleared keys stay, set to undefined), so
// packUniforms lays uniforms out in the order keys first appeared, and
// packUniforms reuses one scratch buffer without clearing it, so bytes it does
// not write this time keep earlier values. Reads follow JavaScript property
// access, inherited Object.prototype names included, and errors carry V8's
// names and messages. A Float32Array uniform (the pipeline's audio waveform and
// spectrum) is written by createSingleUniformBuffer and skipped by the struct
// packers, which take only numbers, booleans and arrays. Out of scope: a uniform named __proto__ with an object or null
// value (the reference would replace the merged object's prototype), which
// throws, and buffers over 1 GiB, which throw "Array buffer allocation failed"
// where V8 would try to allocate them. tools/nm-dump's `uniforms` command and
// parity/check_uniforms.mjs compare the bytes and errors with the reference.

#include "core/value/value.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace libnoisemaker::backend {

// One queue.writeBuffer: the size of the buffer the call asks for (before any
// pooling) and the bytes it writes at offset 0.
struct UniformWrite {
    double bufferSize = 0;
    std::vector<uint8_t> bytes;
};

// What createUniformBuffer reads from a compiled program.
struct ProgramUniforms {
    // packedUniformLayout: the definition's uniformLayout when it has one,
    // otherwise parsePackedUniformLayout of the source: null, slot entries, or
    // {type: 'byte', layout} with its structSize in byteStructSize. A
    // definition's layout is an array of {name, slot, components} or an object
    // {name: {slot, components}}.
    Value packedUniformLayout;
    // The structSize parseWgslStructByteLayout attaches to its layout array;
    // a definition's byte layout has none.
    std::optional<double> byteStructSize;
    double declaredUniformBufferSize = 0;

    // compileProgram's analysis of a source after injectDefines.
    static ProgramUniforms fromSource(const JsString& processedSource, const Value& definitionUniformLayout);
};

class UniformPacker {
public:
    UniformPacker();

    // createUniformBuffer(pass, state, program): pass uniforms over global
    // uniforms, packed with the program's layout; nullopt when nothing is set.
    std::optional<UniformWrite> createUniformBuffer(const Value& passUniforms, const Value& globalUniforms,
                                                    const ProgramUniforms& program);
    // createSingleUniformBuffer(value, typeDecl); nullopt for a value it cannot write.
    std::optional<UniformWrite> createSingleUniformBuffer(const Value& value, const JsString& typeDecl);

private:
    std::vector<uint8_t> packUniforms();
    std::vector<uint8_t> packUniformsWithLayout(const Value& layout, std::optional<double> byteStructSize);
    std::vector<uint8_t> packUniformsWithByteLayout(const Value& layout, std::optional<double> structSize);
    Value resolveAlias(const Value& name, bool includeChannelCount) const;
    void setMerged(const JsString& key, const Value& value);

    // this._mergedUniforms and this._mergedUniformKeys
    Value merged_;
    std::vector<JsString> mergedKeys_;
    // this._uniformBufferData
    std::vector<uint8_t> scratch_;
};

}  // namespace libnoisemaker::backend
