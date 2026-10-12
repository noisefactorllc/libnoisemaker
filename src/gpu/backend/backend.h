#pragma once

// The reference WebGPU backend (shaders/src/runtime/backends/webgpu.js,
// WebGPUBackend) over a Device: samplers, textures, programs, bind groups and
// the per-frame uniform buffers. Methods keep the reference's names and its
// order of device calls, which parity/check_backend.mjs compares call by call
// with the reference running on a recording mock device.
//
// Outside the port: a program source that is not a string (the compiler never
// makes one; the reference runs part of compileProgram on it before a string
// method throws), which compileProgram refuses before any device call, and
// texture ids other than strings, which the pipeline never uses.

#include "gpu/backend/device.h"
#include "gpu/backend/uniforms.h"
#include "gpu/wgsl/wgsl.h"

#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace libnoisemaker::backend {

// One record of this.textures. Members the reference leaves undefined are
// empty or false.
struct TextureRecord {
    Handle handle = 0;
    Handle view = 0;
    std::optional<Handle> renderView;
    std::vector<Handle> mipViews;
    Value width;
    Value height;
    Value depth;
    Value format;  // the spec's format, as given
    Value gpuFormat;
    uint32_t usage = 0;
    bool mipmaps = false;
    double mipLevels = 1;
    bool persistent = false;
    bool is3D = false;
    Value filter;
    bool cube = false;
    bool isExternal = false;
};

// What compileProgram keeps in this.programs.
struct ProgramInfo {
    bool isCompute = false;
    Handle module = 0;
    Handle pipeline = 0;
    // Compute programs.
    JsString entryPoint;
    std::vector<JsString> entryPoints;
    std::vector<std::pair<JsString, Handle>> pipelines;
    wgsl::OrderedMap<JsString, std::vector<double>> entryPointBindings;
    // Render programs.
    Handle vertexModule = 0;
    Handle fragmentModule = 0;
    JsString vertexEntryPoint;
    JsString fragmentEntryPoint;
    Value outputFormat;
    std::vector<std::pair<JsString, Handle>> pipelineCache;
    // Both.
    std::vector<wgsl::Binding> bindings;
    wgsl::OrderedMap<JsString, std::optional<JsString>> samplerTextureNames;
    bool sourceHasBindings = false;
    ProgramUniforms uniforms;
};

// The pipeline's frame state (getFrameState). A surface names the texture id
// of its current read texture, a write surface that of its write texture.
struct FrameState {
    Value globalUniforms;
    std::vector<std::pair<JsString, JsString>> surfaces;
    std::vector<std::pair<JsString, Value>> writeSurfaces;
    Value renderSurface;
    Value screenWidth;
    Value screenHeight;
};

class WebGPUBackend {
public:
    explicit WebGPUBackend(Device& device);

    void init();
    static std::optional<JsString> parseGlobalName(const Value& texId);

    void createTexture(const JsString& id, const Value& spec);
    void createTexture3D(const JsString& id, const Value& spec);
    void destroyTexture(const JsString& id);
    void createCubeTexture(const JsString& id, const Value& size);
    void uploadDataTexture(const JsString& id, const std::vector<float>& data, const Value& width, const Value& height);
    const TextureRecord* texture(const JsString& id) const;

    // compileProgram(id, spec), spec being the graph's program spec.
    const ProgramInfo& compileProgram(const JsString& id, const Value& spec);
    const ProgramInfo* program(const JsString& id) const;

    void beginFrame();
    void endFrame();
    // createBindGroup(pass, program, state, pipeline)
    Handle createBindGroup(const Value& pass, const ProgramInfo& program, const FrameState& state,
                           std::optional<Handle> pipeline = std::nullopt);

private:
    Value resolveFormat(const Value& format) const;
    uint32_t resolveUsage(const Value& usageArray) const;
    std::optional<BlendState> resolveBlendState(const Value& blend) const;
    Handle defaultVertexModule();
    const ProgramInfo& compileComputeProgram(const JsString& id, const JsString& source, const Value& spec);
    const ProgramInfo& compileRenderProgram(const JsString& id, const JsString& source, const Value& spec);

    const TextureRecord* textureFor(const Value& texId) const;
    const TextureRecord* textureOrUnscoped(const Value& texId) const;
    std::optional<Handle> uniformBuffer(const Value& pass, const FrameState& state, const ProgramUniforms* program);
    struct Buffer {
        Handle handle = 0;
        double size = 0;
    };
    std::optional<Buffer> singleUniformBuffer(const Value& value, const JsString& typeDecl);
    Buffer bufferFor(double size);
    Handle storageBuffer(const wgsl::Binding& binding, const FrameState& state);
    std::optional<Handle> storageTextureView(const wgsl::Binding& binding, const Value& pass, const FrameState& state);
    std::optional<Handle> outputStorageView(const FrameState& state);
    Handle legacyBindGroup(const Value& pass, const ProgramInfo& program, const FrameState& state);
    Handle createBindGroupFromEntries(Handle layout, const std::vector<BindGroupEntry>& entries);
    Handle sampler(const Value& type) const;
    const TextureRecord* surfaceTexture(const FrameState& state, const JsString& name) const;

    Device& device_;
    Value capabilities_;
    std::unordered_map<JsString, TextureRecord> textures_;
    std::unordered_map<JsString, ProgramInfo> programs_;
    std::vector<std::pair<JsString, Handle>> samplers_;
    Handle dummyTextureView_ = 0;
    Handle defaultVertexModule_ = 0;
    Handle commandEncoder_ = 0;
    std::vector<Buffer> uniformBufferPool_;
    std::vector<Buffer> activeUniformBuffers_;
    std::unordered_map<JsString, Buffer> storageBuffers_;
    std::vector<Buffer> retiredStorageBuffers_;
    std::unordered_map<JsString, Handle> fallbackStorageViews_;
    UniformPacker packer_;
};

}  // namespace libnoisemaker::backend
