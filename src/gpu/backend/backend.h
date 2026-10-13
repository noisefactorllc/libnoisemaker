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
// texture ids and compute entry points other than strings, which the pipeline
// never uses (the reference keys its Maps by the value itself).

#include "gpu/backend/device.h"
#include "gpu/backend/uniforms.h"
#include "gpu/wgsl/wgsl.h"

#include <memory>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace libnoisemaker::backend {

// A value the reference throws that is not an Error, such as
// `throw { code: 'ERR_TEXTURE_NOT_FOUND', pass, texture }`.
class ThrownValue : public JsError {
public:
    explicit ThrownValue(Value value) : JsError(u"object", u"[object Object]"), value_(std::move(value)) {}
    const Value& value() const { return value_; }

private:
    Value value_;
};

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
    // generateMipmaps' bind group per level, made on first use.
    std::vector<Handle> mipBindGroups;
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
// of its current read texture, a write surface that of its write texture; the
// graph appears as the one member the backend reads, renderSurface.
struct FrameState {
    double frameIndex = 0;
    double time = 0;
    Value globalUniforms;
    std::vector<std::pair<JsString, JsString>> surfaces;
    std::vector<std::pair<JsString, Value>> writeSurfaces;
    Value renderSurface;
    Value screenWidth;
    Value screenHeight;
};

// readPixels' result: RGBA8 rows, row 0 first.
struct Pixels {
    Value width;
    Value height;
    std::vector<uint8_t> data;
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
    // uploadCubeFace(id, face, { width, height, data }) with RGBA8 data.
    void uploadCubeFace(const JsString& id, const Value& face, const Value& width, const Value& height,
                        const std::vector<uint8_t>& data);
    // uploadMeshData(meshId, positions, normals, uvs, width, height, vertexCount):
    // { success: true, vertexCount }, which the canvas renderer passes on.
    Value uploadMeshData(const JsString& meshId, const std::vector<float>& positions, const std::vector<float>& normals,
                         const std::vector<float>& uvs, const Value& width, const Value& height, const Value& vertexCount);
    void copyTexture(const JsString& srcId, const JsString& dstId);
    void copyTextureScaled(const JsString& srcId, const JsString& dstId);
    void generateMipmaps(const std::vector<JsString>& ids);
    void clearTexture(const JsString& id);
    // updateTextureFromSource(id, canvas, { flipY }) for a 2D canvas created
    // with willReadFrequently, whose getImageData rows are rgba: the path
    // that writes CPU pixels. { width, height }, zero for an empty source.
    Value updateTextureFromPixels(const JsString& id, double width, double height, const std::vector<uint8_t>& rgba,
                                  bool flipY);
    // readPixels(textureId): the texture's texels as RGBA8, rgba16float and
    // rgba32float converted as round(v * 255) clamped to 0..255.
    Pixels readPixels(const JsString& textureId);
    // this.textures: get, set (texture pooling names one record by several
    // ids) and keys, in a Map's insertion order.
    const TextureRecord* texture(const JsString& id) const;
    std::shared_ptr<TextureRecord> textureRecord(const JsString& id) const;
    void setTexture(const JsString& id, std::shared_ptr<TextureRecord> record);
    const std::vector<JsString>& textureIds() const { return textureOrder_; }
    const Value& capabilities() const { return capabilities_; }
    static JsString getName() { return u"WebGPU"; }

    // compileProgram(id, spec), spec being the graph's program spec.
    const ProgramInfo& compileProgram(const JsString& id, const Value& spec);
    const ProgramInfo* program(const JsString& id) const;

    void beginFrame();
    void endFrame();
    // executePass(pass, state): the pass's render or compute pass on the
    // frame's command encoder.
    void executePass(const Value& pass, const FrameState& state);
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
    std::shared_ptr<TextureRecord> ownRecord(TextureRecord record);

    struct Viewport {
        Value x, y, w, h;
    };
    void executeRenderPass(const Value& pass, ProgramInfo& program, const FrameState& state);
    void executeMRTRenderPass(const Value& pass, ProgramInfo& program, const FrameState& state,
                              const std::vector<JsString>& outputKeys);
    void executeComputePass(const Value& pass, ProgramInfo& program, const FrameState& state);
    Handle resolveRenderPipeline(ProgramInfo& program, const Value& blend, const JsString& topology, const Value& format);
    Handle resolveMRTRenderPipeline(ProgramInfo& program, const Value& blend, const JsString& topology,
                                    const std::vector<Value>& formats);
    Handle resolve3DRenderPipeline(ProgramInfo& program, const Value& blend, const Value& format);
    Handle renderPipeline(ProgramInfo& program, const JsString& key, RenderPipelineDescriptor descriptor);
    Handle getDepthTexture(const Value& width, const Value& height);
    std::optional<Viewport> resolveViewport(const Value& pass, const TextureRecord* tex) const;
    Value resolvePointCount(const Value& pass, const FrameState& state, const Value& outputId, const TextureRecord* outputTex) const;
    Value resolveMeshVertexCount(const Value& pass, const FrameState& state, const TextureRecord* outputTex) const;
    Handle getComputePipeline(ProgramInfo& program, const Value& entryPoint);
    std::vector<Value> resolveWorkgroups(const Value& pass, const FrameState& state) const;
    void copyBufferToTexture(const FrameState& state, const Value& outputId, const JsString& bufferName);
    Handle getBufferToTextureRenderPipeline(const Value& format);
    Handle getResamplePipeline(const Value& gpuFormat, const JsString& entryPoint);
    void writeTextureRows(Handle texture, const std::vector<float>& data, const Value& width, const Value& height,
                          double bytesPerPixel);
    Handle encoder() const;
    void drawPass(Handle passEncoder, const Value& pass, const FrameState& state, const Value& outputId, const TextureRecord* tex);
    Handle sampler(const Value& type) const;
    const TextureRecord* surfaceTexture(const FrameState& state, const JsString& name) const;

    Device& device_;
    // Records' deleters release through this while the backend lives; a
    // record that outlives the backend leaves its objects to the device.
    std::shared_ptr<Device*> releaser_;
    Value capabilities_;
    std::unordered_map<JsString, std::shared_ptr<TextureRecord>> textures_;
    std::vector<JsString> textureOrder_;
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
    Handle depthTexture_ = 0;
    Value depthTextureWidth_;
    Value depthTextureHeight_;
    std::vector<std::pair<JsString, Handle>> bufferToTexturePipelines_;
    Handle resampleModule_ = 0;
    std::vector<std::pair<JsString, Handle>> resamplePipelines_;
    UniformPacker packer_;
};

}  // namespace libnoisemaker::backend
