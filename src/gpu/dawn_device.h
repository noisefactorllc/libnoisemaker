#pragma once

// The backend's Device (gpu/backend/device.h) on Dawn: each call is the
// WebGPU call the reference backend makes in the browser, on a Dawn device
// that gpu/device.h created. Handles number the objects in creation order,
// as the recording device's do.
//
// Where the browser throws, this throws the same JavaScript error type: a
// TypeError for an enum string WebGPU does not define and for a number
// outside its IDL type ([EnforceRange] integers, finite floats), an
// OperationError for a failed buffer map. Validation errors do not throw,
// as in the browser: Dawn reports them to the device's ErrorSink.
//
// Objects the browser would let the garbage collector take are released
// when WebGPU makes them unusable (a pass encoder at end(), a command
// encoder at finish(), a command buffer at submit()) and when the backend
// releases them (each pass's bind group and depth view). getBindGroupLayout
// returns one handle per pipeline and group. The others, such as a resized
// texture's old views, live until the DawnDevice is destroyed.
// destroyTexture and destroyBuffer destroy the GPU resource, as destroy()
// does, and keep the handle valid.

#include "gpu/backend/device.h"
#include "gpu/device.h"

#include <webgpu/webgpu.h>

#include <cstdint>
#include <functional>
#include <map>
#include <utility>
#include <vector>

namespace libnoisemaker::gpu {

class DawnDevice : public backend::Device {
public:
    // The device must outlive this DawnDevice.
    explicit DawnDevice(gpu::Device& device);
    ~DawnDevice() override;
    DawnDevice(const DawnDevice&) = delete;
    DawnDevice& operator=(const DawnDevice&) = delete;

    double maxTextureDimension2D() const override { return maxTextureDimension2D_; }
    double maxColorAttachmentBytesPerSample() const override { return maxColorAttachmentBytesPerSample_; }

    backend::Handle createTexture(const backend::TextureDescriptor& descriptor) override;
    backend::Handle createView(backend::Handle texture, const backend::TextureViewDescriptor& descriptor) override;
    void destroyTexture(backend::Handle texture) override;
    backend::Handle createSampler(const backend::SamplerDescriptor& descriptor) override;
    backend::Handle createBuffer(const backend::BufferDescriptor& descriptor) override;
    void destroyBuffer(backend::Handle buffer) override;
    backend::Handle createShaderModule(const JsString& code) override;
    std::vector<JsString> compilationErrors(backend::Handle module) override;
    backend::Handle createRenderPipeline(const backend::RenderPipelineDescriptor& descriptor) override;
    backend::Handle createComputePipeline(const backend::ComputePipelineDescriptor& descriptor) override;
    backend::Handle getBindGroupLayout(backend::Handle pipeline, double index) override;
    backend::Handle createBindGroup(backend::Handle layout, const std::vector<backend::BindGroupEntry>& entries) override;
    void writeBuffer(backend::Handle buffer, double offset, const std::vector<uint8_t>& bytes) override;
    void writeTexture(const backend::TexelCopyTextureInfo& destination, const std::vector<uint8_t>& bytes,
                      const backend::TexelCopyBufferLayout& layout, const backend::Extent3D& size) override;
    backend::Handle createCommandEncoder() override;
    backend::Handle beginRenderPass(backend::Handle encoder, const backend::RenderPassDescriptor& descriptor) override;
    backend::Handle beginComputePass(backend::Handle encoder) override;
    void setPipeline(backend::Handle pass, backend::Handle pipeline) override;
    void setBindGroup(backend::Handle pass, double index, backend::Handle group) override;
    void setViewport(backend::Handle pass, double x, double y, double width, double height, double minDepth,
                     double maxDepth) override;
    void draw(backend::Handle pass, double vertexCount, double instanceCount, double firstVertex,
              double firstInstance) override;
    void dispatchWorkgroups(backend::Handle pass, double x, double y, double z) override;
    void end(backend::Handle pass) override;
    void copyTextureToTexture(backend::Handle encoder, const backend::TexelCopyTextureInfo& source,
                              const backend::TexelCopyTextureInfo& destination, const backend::Extent3D& size) override;
    void copyTextureToBuffer(backend::Handle encoder, const backend::TexelCopyTextureInfo& source, backend::Handle buffer,
                             const backend::TexelCopyBufferLayout& layout, const backend::Extent3D& size) override;
    backend::Handle finish(backend::Handle encoder) override;
    void submit(const std::vector<backend::Handle>& commandBuffers) override;
    // done runs from processEvents() once the work submitted before it is
    // done, as the browser resolves the promise in a later task.
    void onSubmittedWorkDone(std::function<void()> done) override;
    std::vector<uint8_t> mapRead(backend::Handle buffer) override;
    void unmap(backend::Handle buffer) override;
    void release(backend::Handle object) override;

    // Runs the callbacks of finished work.
    void processEvents();
    // Waits until the work submitted so far is done, then runs the callbacks.
    void waitIdle();

private:
    enum class Kind : uint8_t {
        Released,
        Texture,
        View,
        Sampler,
        Buffer,
        ShaderModule,
        RenderPipeline,
        ComputePipeline,
        BindGroupLayout,
        BindGroup,
        CommandEncoder,
        RenderPass,
        ComputePass,
        CommandBuffer,
    };
    struct Object {
        Kind kind = Kind::Released;
        void* handle = nullptr;
        uint64_t size = 0;  // a buffer's size
    };

    backend::Handle add(Kind kind, void* handle, uint64_t size = 0);
    void* get(backend::Handle handle, Kind kind, const char* what) const;
    template <typename T>
    T as(backend::Handle handle, Kind kind, const char* what) const {
        return static_cast<T>(get(handle, kind, what));
    }
    static void release(Object& object);

    gpu::Device& device_;
    std::vector<Object> objects_;
    std::map<std::pair<backend::Handle, uint32_t>, backend::Handle> layouts_;
    double maxTextureDimension2D_ = 0;
    double maxColorAttachmentBytesPerSample_ = 0;
};

}  // namespace libnoisemaker::gpu
