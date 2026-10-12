#pragma once

// The GPU device the backend drives: the part of WebGPU that the reference
// WebGPU backend calls (shaders/src/runtime/backends/webgpu.js), with each
// descriptor in canonical form: WebGPU's static defaults applied, and view
// members whose default comes from the texture left empty. RecordingDevice logs
// every call for parity/check_backend.mjs, which compares the log with the
// reference backend's calls on a recording mock device; the Dawn device
// implements the same interface for rendering.

#include "core/edit/jsv.h"
#include "core/value/value.h"

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace libnoisemaker::backend {

// An object the device created, numbered in creation order from 1; 0 is none.
using Handle = uint32_t;

// A call the device rejected, with the device's message, thrown as the
// browser throws an Error. createBindGroupFromEntries reads a binding index
// from "binding index N not present" and retries without that entry.
class DeviceError : public JsError {
public:
    explicit DeviceError(JsString message) : JsError(u"Error", std::move(message)) {}
};

namespace TextureUsage {
constexpr uint32_t CopySrc = 1, CopyDst = 2, TextureBinding = 4, StorageBinding = 8, RenderAttachment = 16;
}
namespace BufferUsage {
constexpr uint32_t MapRead = 1, MapWrite = 2, CopySrc = 4, CopyDst = 8, Index = 16, Vertex = 32, Uniform = 64,
                   Storage = 128, Indirect = 256, QueryResolve = 512;
}

struct Extent3D {
    double width = 0;
    double height = 1;
    double depthOrArrayLayers = 1;
};

struct TextureDescriptor {
    Extent3D size;
    double mipLevelCount = 1;
    double sampleCount = 1;
    JsString dimension = u"2d";
    JsString format;
    uint32_t usage = 0;
};

// Members left out take their defaults from the texture.
struct TextureViewDescriptor {
    std::optional<JsString> format;
    std::optional<JsString> dimension;
    JsString aspect = u"all";
    double baseMipLevel = 0;
    std::optional<double> mipLevelCount;
    double baseArrayLayer = 0;
    std::optional<double> arrayLayerCount;
};

struct SamplerDescriptor {
    JsString addressModeU = u"clamp-to-edge";
    JsString addressModeV = u"clamp-to-edge";
    JsString addressModeW = u"clamp-to-edge";
    JsString magFilter = u"nearest";
    JsString minFilter = u"nearest";
    JsString mipmapFilter = u"nearest";
    double lodMinClamp = 0;
    double lodMaxClamp = 32;
    std::optional<JsString> compare;
    double maxAnisotropy = 1;
};

struct BufferDescriptor {
    double size = 0;
    uint32_t usage = 0;
    bool mappedAtCreation = false;
};

struct BlendComponent {
    JsString operation = u"add";
    JsString srcFactor = u"one";
    JsString dstFactor = u"zero";
};
struct BlendState {
    BlendComponent color;
    BlendComponent alpha;
};
struct ColorTargetState {
    JsString format;
    std::optional<BlendState> blend;
    uint32_t writeMask = 0xF;
};

struct DepthStencilState {
    JsString format;
    bool depthWriteEnabled = false;
    std::optional<JsString> depthCompare;
};

struct RenderPipelineDescriptor {
    Handle vertexModule = 0;
    JsString vertexEntryPoint;
    Handle fragmentModule = 0;
    JsString fragmentEntryPoint;
    std::vector<ColorTargetState> targets;
    JsString topology = u"triangle-list";
    JsString frontFace = u"ccw";
    JsString cullMode = u"none";
    std::optional<DepthStencilState> depthStencil;
};

struct Color {
    double r = 0, g = 0, b = 0, a = 0;
};
struct RenderPassColorAttachment {
    Handle view = 0;
    Color clearValue;
    JsString loadOp;
    JsString storeOp;
};
struct RenderPassDepthStencilAttachment {
    Handle view = 0;
    std::optional<double> depthClearValue;
    std::optional<JsString> depthLoadOp;
    std::optional<JsString> depthStoreOp;
};
struct RenderPassDescriptor {
    std::vector<RenderPassColorAttachment> colorAttachments;
    std::optional<RenderPassDepthStencilAttachment> depthStencilAttachment;
};

struct ComputePipelineDescriptor {
    Handle module = 0;
    JsString entryPoint;
};

struct BindGroupEntry {
    enum class Kind { TextureView, Sampler, Buffer };
    double binding = 0;
    Kind kind = Kind::TextureView;
    Handle resource = 0;
};

struct TexelCopyTextureInfo {
    Handle texture = 0;
    double mipLevel = 0;
    std::array<double, 3> origin{0, 0, 0};
    JsString aspect = u"all";
};
struct TexelCopyBufferLayout {
    double offset = 0;
    std::optional<double> bytesPerRow;
    std::optional<double> rowsPerImage;
};

class Device {
public:
    virtual ~Device() = default;
    virtual double maxTextureDimension2D() const = 0;
    virtual double maxColorAttachmentBytesPerSample() const = 0;

    virtual Handle createTexture(const TextureDescriptor& descriptor) = 0;
    virtual Handle createView(Handle texture, const TextureViewDescriptor& descriptor) = 0;
    virtual void destroyTexture(Handle texture) = 0;
    virtual Handle createSampler(const SamplerDescriptor& descriptor) = 0;
    virtual Handle createBuffer(const BufferDescriptor& descriptor) = 0;
    virtual void destroyBuffer(Handle buffer) = 0;
    virtual Handle createShaderModule(const JsString& code) = 0;
    // getCompilationInfo: the error messages, empty when the module compiled.
    virtual std::vector<JsString> compilationErrors(Handle module) = 0;
    virtual Handle createRenderPipeline(const RenderPipelineDescriptor& descriptor) = 0;
    virtual Handle createComputePipeline(const ComputePipelineDescriptor& descriptor) = 0;
    virtual Handle getBindGroupLayout(Handle pipeline, double index) = 0;
    // May throw DeviceError.
    virtual Handle createBindGroup(Handle layout, const std::vector<BindGroupEntry>& entries) = 0;

    virtual void writeBuffer(Handle buffer, double offset, const std::vector<uint8_t>& bytes) = 0;
    virtual void writeTexture(const TexelCopyTextureInfo& destination, const std::vector<uint8_t>& bytes,
                              const TexelCopyBufferLayout& layout, const Extent3D& size) = 0;

    virtual Handle createCommandEncoder() = 0;
    virtual Handle beginRenderPass(Handle encoder, const RenderPassDescriptor& descriptor) = 0;
    virtual Handle beginComputePass(Handle encoder) = 0;
    virtual void setPipeline(Handle pass, Handle pipeline) = 0;
    virtual void setBindGroup(Handle pass, double index, Handle group) = 0;
    virtual void setViewport(Handle pass, double x, double y, double width, double height, double minDepth, double maxDepth) = 0;
    virtual void draw(Handle pass, double vertexCount, double instanceCount, double firstVertex, double firstInstance) = 0;
    virtual void dispatchWorkgroups(Handle pass, double x, double y, double z) = 0;
    virtual void end(Handle pass) = 0;
    virtual void copyTextureToTexture(Handle encoder, const TexelCopyTextureInfo& source,
                                      const TexelCopyTextureInfo& destination, const Extent3D& size) = 0;
    virtual Handle finish(Handle encoder) = 0;
    virtual void submit(const std::vector<Handle>& commandBuffers) = 0;
    // queue.onSubmittedWorkDone().then(done)
    virtual void onSubmittedWorkDone(std::function<void()> done) = 0;
};

}  // namespace libnoisemaker::backend
