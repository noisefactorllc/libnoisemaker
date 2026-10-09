#include "check.h"
#include "gpu/device.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

nm::gpu::DeviceOptions options_from_env() {
    nm::gpu::DeviceOptions options;
    options.allow_fallback = std::getenv("NM_ALLOW_FALLBACK_ADAPTER") != nullptr;
    return options;
}

WGPUTexture make_texture(nm::gpu::Device& d, uint32_t w, uint32_t h, WGPUTextureUsage usage) {
    WGPUTextureDescriptor desc = {};
    desc.usage = usage;
    desc.dimension = WGPUTextureDimension_2D;
    desc.size = {w, h, 1};
    desc.format = WGPUTextureFormat_RGBA8Unorm;
    desc.mipLevelCount = 1;
    desc.sampleCount = 1;
    return wgpuDeviceCreateTexture(d.device, &desc);
}

void test_creates_a_device() {
    auto result = nm::gpu::create_device(options_from_env());
    if (result.status != nm::gpu::Status::Ok) std::fprintf(stderr, "create_device: %s\n", result.message.c_str());
    NM_CHECK(result.status == nm::gpu::Status::Ok);
    NM_CHECK(result.device.device != nullptr);
    NM_CHECK(!result.device.adapter_name.empty());
    std::printf("adapter: %s%s (type %d, vendor 0x%04x, device 0x%04x)\n", result.device.adapter_name.c_str(),
                result.device.is_fallback ? " (fallback)" : "", static_cast<int>(result.device.adapter_type),
                static_cast<unsigned>(result.device.vendor_id), static_cast<unsigned>(result.device.device_id));
}

void test_reports_a_missing_adapter() {
    nm::gpu::DeviceOptions options;
    options.backend = WGPUBackendType_Null;
    auto result = nm::gpu::create_device(options);
    NM_CHECK(result.status == nm::gpu::Status::NoAdapter);
    NM_CHECK(!result.message.empty());
}

void test_cpu_adapter_requires_explicit_fallback() {
    using nm::gpu::adapter_allowed;
    using nm::gpu::adapter_is_software;
    NM_CHECK(!adapter_allowed(WGPUAdapterType_CPU, 0x10005, 0, false));
    NM_CHECK(adapter_allowed(WGPUAdapterType_CPU, 0x10005, 0, true));
    NM_CHECK(adapter_allowed(WGPUAdapterType_DiscreteGPU, 0x10de, 0x2684, false));
    NM_CHECK(adapter_allowed(WGPUAdapterType_IntegratedGPU, 0x106b, 0, false));
    NM_CHECK(!adapter_allowed(WGPUAdapterType_Unknown, 0, 0, false));
    // WARP reported as a GPU, as on hosted Windows runners.
    NM_CHECK(adapter_is_software(WGPUAdapterType_DiscreteGPU, 0x1414, 0x8c));
    NM_CHECK(adapter_is_software(WGPUAdapterType_IntegratedGPU, 0x1414, 0x8c));
    NM_CHECK(!adapter_allowed(WGPUAdapterType_DiscreteGPU, 0x1414, 0x8c, false));
    NM_CHECK(adapter_allowed(WGPUAdapterType_DiscreteGPU, 0x1414, 0x8c, true));
    // Other Microsoft adapters are not WARP.
    NM_CHECK(!adapter_is_software(WGPUAdapterType_DiscreteGPU, 0x1414, 0x8d));
}

void test_clears_and_reads_back() {
    auto result = nm::gpu::create_device(options_from_env());
    NM_CHECK(result.status == nm::gpu::Status::Ok);
    auto& d = result.device;
    const uint32_t w = 5, h = 3;
    WGPUTexture tex = make_texture(d, w, h, WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopySrc);
    WGPUTextureView view = wgpuTextureCreateView(tex, nullptr);

    WGPURenderPassColorAttachment color = {};
    color.view = view;
    color.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
    color.loadOp = WGPULoadOp_Clear;
    color.storeOp = WGPUStoreOp_Store;
    color.clearValue = {0.2, 0.4, 0.6, 1.0};
    WGPURenderPassDescriptor pass = {};
    pass.colorAttachmentCount = 1;
    pass.colorAttachments = &color;

    WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(d.device, nullptr);
    WGPURenderPassEncoder rp = wgpuCommandEncoderBeginRenderPass(enc, &pass);
    wgpuRenderPassEncoderEnd(rp);
    WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(enc, nullptr);
    wgpuQueueSubmit(d.queue, 1, &cmd);

    std::vector<uint8_t> pixels;
    std::string message;
    NM_CHECK(nm::gpu::read_rgba8(d, tex, w, h, pixels, message) == nm::gpu::Status::Ok);
    NM_CHECK(pixels.size() == size_t(w) * h * 4);
    for (size_t i = 0; i < pixels.size(); i += 4) {
        NM_CHECK(pixels[i + 0] == 51);
        NM_CHECK(pixels[i + 1] == 102);
        NM_CHECK(pixels[i + 2] == 153);
        NM_CHECK(pixels[i + 3] == 255);
    }
    wgpuCommandBufferRelease(cmd);
    wgpuRenderPassEncoderRelease(rp);
    wgpuCommandEncoderRelease(enc);
    wgpuTextureViewRelease(view);
    wgpuTextureRelease(tex);
}

void test_round_trips_an_unaligned_texture() {
    auto result = nm::gpu::create_device(options_from_env());
    NM_CHECK(result.status == nm::gpu::Status::Ok);
    auto& d = result.device;
    const uint32_t w = 5, h = 3;
    std::vector<uint8_t> data(size_t(w) * h * 4);
    for (size_t i = 0; i < data.size(); ++i) data[i] = uint8_t((i * 37 + 11) & 0xff);
    WGPUTexture tex = make_texture(d, w, h, WGPUTextureUsage_CopyDst | WGPUTextureUsage_CopySrc);

    WGPUTexelCopyTextureInfo dst = {};
    dst.texture = tex;
    dst.aspect = WGPUTextureAspect_All;
    WGPUTexelCopyBufferLayout layout = {};
    layout.bytesPerRow = w * 4;
    layout.rowsPerImage = h;
    WGPUExtent3D size = {w, h, 1};
    wgpuQueueWriteTexture(d.queue, &dst, data.data(), data.size(), &layout, &size);

    std::vector<uint8_t> pixels;
    std::string message;
    NM_CHECK(nm::gpu::read_rgba8(d, tex, w, h, pixels, message) == nm::gpu::Status::Ok);
    NM_CHECK(pixels == data);
    wgpuTextureRelease(tex);
}

}  // namespace

int main() {
    test_creates_a_device();
    test_reports_a_missing_adapter();
    test_cpu_adapter_requires_explicit_fallback();
    test_clears_and_reads_back();
    test_round_trips_an_unaligned_texture();
    std::printf("test_device: ok\n");
    return 0;
}
