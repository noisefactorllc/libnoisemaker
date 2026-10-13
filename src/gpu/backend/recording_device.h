#pragma once

// A Device that creates nothing and logs every call, one JSON object per call,
// in the form tools/reference-backend.mjs gives the reference backend's calls
// on its mock device: objects numbered by creation order, descriptors with
// WebGPU's static defaults applied (members whose default comes from the
// texture stay null), extents and origins as arrays, written bytes in hex.

#include "gpu/backend/device.h"

#include <unordered_map>
#include <unordered_set>

namespace libnoisemaker::backend {

class RecordingDevice : public Device {
public:
    double maxTextureDimension2D() const override { return 8192; }
    double maxColorAttachmentBytesPerSample() const override { return 32; }

    Handle createTexture(const TextureDescriptor& descriptor) override;
    Handle createView(Handle texture, const TextureViewDescriptor& descriptor) override;
    void destroyTexture(Handle texture) override;
    Handle createSampler(const SamplerDescriptor& descriptor) override;
    Handle createBuffer(const BufferDescriptor& descriptor) override;
    void destroyBuffer(Handle buffer) override;
    Handle createShaderModule(const JsString& code) override;
    std::vector<JsString> compilationErrors(Handle) override { return {}; }
    Handle createRenderPipeline(const RenderPipelineDescriptor& descriptor) override;
    Handle createComputePipeline(const ComputePipelineDescriptor& descriptor) override;
    Handle getBindGroupLayout(Handle pipeline, double index) override;
    Handle createBindGroup(Handle layout, const std::vector<BindGroupEntry>& entries) override;
    void writeBuffer(Handle buffer, double offset, const std::vector<uint8_t>& bytes) override;
    void writeTexture(const TexelCopyTextureInfo& destination, const std::vector<uint8_t>& bytes,
                      const TexelCopyBufferLayout& layout, const Extent3D& size) override;
    Handle createCommandEncoder() override;
    Handle beginRenderPass(Handle encoder, const RenderPassDescriptor& descriptor) override;
    Handle beginComputePass(Handle encoder) override;
    void setPipeline(Handle pass, Handle pipeline) override;
    void setBindGroup(Handle pass, double index, Handle group) override;
    void setViewport(Handle pass, double x, double y, double width, double height, double minDepth, double maxDepth) override;
    void draw(Handle pass, double vertexCount, double instanceCount, double firstVertex, double firstInstance) override;
    void dispatchWorkgroups(Handle pass, double x, double y, double z) override;
    void end(Handle pass) override;
    void copyTextureToTexture(Handle encoder, const TexelCopyTextureInfo& source, const TexelCopyTextureInfo& destination,
                              const Extent3D& size) override;
    Handle finish(Handle encoder) override;
    void submit(const std::vector<Handle>& commandBuffers) override;
    void copyTextureToBuffer(Handle encoder, const TexelCopyTextureInfo& source, Handle buffer,
                             const TexelCopyBufferLayout& layout, const Extent3D& size) override;
    // The work is done when it is submitted: done runs at once.
    void onSubmittedWorkDone(std::function<void()> done) override { done(); }
    // A mapped buffer's 16-bit unit k is (k * 40503 + buffer * 11) & 65535,
    // little-endian, as the mock device's are: 65536 consecutive units hold
    // every value, so a large enough readback converts every half float.
    std::vector<uint8_t> mapRead(Handle buffer) override;
    void unmap(Handle buffer) override;
    // Not logged: the reference has no such call. A released handle passed
    // to any later call throws, so the gate fails on a premature release.
    void release(Handle object) override { released_.insert(object); }

    // The calls since the last take, in order.
    Array takeLog();

    // For the gate: createBindGroup rejects a group holding one of these
    // bindings ("binding index N not present"), or every group with
    // failMessage when it is set.
    void rejectBindings(std::vector<double> bindings, std::optional<JsString> failMessage);

private:
    Handle next();
    void log(Object call);

    Handle next_ = 1;
    std::unordered_map<Handle, double> bufferSizes_;
    std::unordered_set<Handle> released_;
    void live(Handle handle) const;
    Array log_;
    std::vector<double> rejected_;
    std::optional<JsString> failMessage_;
};

}  // namespace libnoisemaker::backend
