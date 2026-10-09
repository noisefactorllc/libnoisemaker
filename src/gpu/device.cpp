#include "gpu/device.h"

#include <cstring>
#include <utility>

namespace nm::gpu {
namespace {

std::string to_string(WGPUStringView view) {
    if (view.data == nullptr) return {};
    if (view.length == WGPU_STRLEN) return std::string(view.data);
    return std::string(view.data, view.length);
}

struct AdapterReply {
    WGPURequestAdapterStatus status = WGPURequestAdapterStatus_Error;
    WGPUAdapter adapter = nullptr;
    std::string message;
};

struct DeviceReply {
    WGPURequestDeviceStatus status = WGPURequestDeviceStatus_Error;
    WGPUDevice device = nullptr;
    std::string message;
};

struct MapReply {
    WGPUMapAsyncStatus status = WGPUMapAsyncStatus_Error;
    std::string message;
};

WGPUAdapter request_adapter(WGPUInstance instance, WGPUBackendType backend, bool fallback,
                            std::string& message) {
    WGPURequestAdapterOptions options = {};
    options.powerPreference = WGPUPowerPreference_HighPerformance;
    options.backendType = backend;
    options.forceFallbackAdapter = fallback ? WGPU_TRUE : WGPU_FALSE;
    AdapterReply reply;
    WGPURequestAdapterCallbackInfo info = {};
    info.mode = WGPUCallbackMode_WaitAnyOnly;
    info.callback = [](WGPURequestAdapterStatus status, WGPUAdapter adapter, WGPUStringView msg,
                       void* userdata1, void*) {
        auto* r = static_cast<AdapterReply*>(userdata1);
        r->status = status;
        r->adapter = adapter;
        r->message = to_string(msg);
    };
    info.userdata1 = &reply;
    if (!wait(instance, wgpuInstanceRequestAdapter(instance, &options, info))) {
        message = "timed out waiting for an adapter";
        return nullptr;
    }
    if (reply.status != WGPURequestAdapterStatus_Success || reply.adapter == nullptr) {
        message = reply.message.empty() ? "no WebGPU adapter is available" : reply.message;
        return nullptr;
    }
    return reply.adapter;
}

}  // namespace

Device::Device(Device&& other) noexcept { *this = std::move(other); }

Device& Device::operator=(Device&& other) noexcept {
    if (this != &other) {
        std::swap(instance, other.instance);
        std::swap(adapter, other.adapter);
        std::swap(device, other.device);
        std::swap(queue, other.queue);
        std::swap(adapter_name, other.adapter_name);
        std::swap(adapter_type, other.adapter_type);
        std::swap(vendor_id, other.vendor_id);
        std::swap(device_id, other.device_id);
        std::swap(is_fallback, other.is_fallback);
        std::swap(errors, other.errors);
    }
    return *this;
}

Device::~Device() {
    if (queue) wgpuQueueRelease(queue);
    if (device) wgpuDeviceRelease(device);
    if (adapter) wgpuAdapterRelease(adapter);
    if (instance) wgpuInstanceRelease(instance);
}

bool wait(WGPUInstance instance, WGPUFuture future) {
    WGPUFutureWaitInfo info = {};
    info.future = future;
    WGPUWaitStatus status = wgpuInstanceWaitAny(instance, 1, &info, UINT64_MAX);
    return status == WGPUWaitStatus_Success && info.completed;
}

bool adapter_is_software(WGPUAdapterType type, uint32_t vendor_id, uint32_t device_id) {
    if (type == WGPUAdapterType_CPU) return true;
    // Dawn reports CPU only when DXGI sets DXGI_ADAPTER_FLAG_SOFTWARE. Hosted
    // Windows runners expose WARP without that flag, and Dawn calls it a GPU.
    constexpr uint32_t kMicrosoftVendor = 0x1414;
    constexpr uint32_t kBasicRenderDriver = 0x8c;
    return vendor_id == kMicrosoftVendor && device_id == kBasicRenderDriver;
}

bool adapter_allowed(WGPUAdapterType type, uint32_t vendor_id, uint32_t device_id, bool allow_fallback) {
    if (allow_fallback) return true;
    const bool gpu = type == WGPUAdapterType_DiscreteGPU || type == WGPUAdapterType_IntegratedGPU;
    return gpu && !adapter_is_software(type, vendor_id, device_id);
}

DeviceResult create_device(const DeviceOptions& options) {
    DeviceResult result;
    Device& d = result.device;

    WGPUInstanceFeatureName features[] = {WGPUInstanceFeatureName_TimedWaitAny};
    WGPUInstanceDescriptor instance_desc = {};
    instance_desc.requiredFeatureCount = 1;
    instance_desc.requiredFeatures = features;
    d.instance = wgpuCreateInstance(&instance_desc);
    if (d.instance == nullptr) {
        result.message = "wgpuCreateInstance failed";
        return result;
    }

    std::string message;
    d.adapter = request_adapter(d.instance, options.backend, false, message);
    if (d.adapter == nullptr && options.allow_fallback) {
        d.adapter = request_adapter(d.instance, options.backend, true, message);
    }
    if (d.adapter == nullptr) {
        result.status = Status::NoAdapter;
        result.message = message;
        return result;
    }

    WGPUAdapterInfo info = {};
    if (wgpuAdapterGetInfo(d.adapter, &info) != WGPUStatus_Success) {
        result.message = "could not read WebGPU adapter information";
        return result;
    }
    d.adapter_name = to_string(info.device);
    if (d.adapter_name.empty()) d.adapter_name = to_string(info.description);
    if (d.adapter_name.empty()) d.adapter_name = to_string(info.vendor);
    d.adapter_type = info.adapterType;
    d.vendor_id = info.vendorID;
    d.device_id = info.deviceID;
    d.is_fallback = adapter_is_software(info.adapterType, info.vendorID, info.deviceID);
    const bool allowed = adapter_allowed(info.adapterType, info.vendorID, info.deviceID, options.allow_fallback);
    wgpuAdapterInfoFreeMembers(info);
    if (!allowed) {
        result.status = Status::NoAdapter;
        result.message = "no hardware WebGPU adapter is available; software or unknown adapter requires explicit fallback";
        return result;
    }

    WGPUDeviceDescriptor device_desc = {};
    device_desc.uncapturedErrorCallbackInfo.callback =
        [](WGPUDevice const*, WGPUErrorType, WGPUStringView msg, void* userdata1, void*) {
            auto* sink = static_cast<ErrorSink*>(userdata1);
            std::lock_guard<std::mutex> lock(sink->mutex);
            sink->errors.push_back(to_string(msg));
        };
    device_desc.uncapturedErrorCallbackInfo.userdata1 = d.errors.get();
    device_desc.deviceLostCallbackInfo.mode = WGPUCallbackMode_AllowSpontaneous;
    device_desc.deviceLostCallbackInfo.callback =
        [](WGPUDevice const*, WGPUDeviceLostReason reason, WGPUStringView msg, void* userdata1, void*) {
            if (reason == WGPUDeviceLostReason_Destroyed || reason == WGPUDeviceLostReason_CallbackCancelled) return;
            auto* sink = static_cast<ErrorSink*>(userdata1);
            std::lock_guard<std::mutex> lock(sink->mutex);
            sink->lost = true;
            sink->lost_message = to_string(msg);
        };
    device_desc.deviceLostCallbackInfo.userdata1 = d.errors.get();

    DeviceReply reply;
    WGPURequestDeviceCallbackInfo device_cb = {};
    device_cb.mode = WGPUCallbackMode_WaitAnyOnly;
    device_cb.callback = [](WGPURequestDeviceStatus status, WGPUDevice device, WGPUStringView msg,
                            void* userdata1, void*) {
        auto* r = static_cast<DeviceReply*>(userdata1);
        r->status = status;
        r->device = device;
        r->message = to_string(msg);
    };
    device_cb.userdata1 = &reply;
    if (!wait(d.instance, wgpuAdapterRequestDevice(d.adapter, &device_desc, device_cb)) ||
        reply.status != WGPURequestDeviceStatus_Success || reply.device == nullptr) {
        result.status = Status::DeviceFailed;
        result.message = reply.message.empty() ? "device request failed" : reply.message;
        return result;
    }
    d.device = reply.device;
    d.queue = wgpuDeviceGetQueue(d.device);
    result.status = Status::Ok;
    return result;
}

Status read_rgba8(Device& d, WGPUTexture texture, uint32_t width, uint32_t height,
                  std::vector<uint8_t>& out, std::string& message) {
    const uint32_t row = width * 4;
    const uint32_t padded = (row + 255u) & ~255u;
    const uint64_t size = uint64_t(padded) * height;

    WGPUBufferDescriptor buffer_desc = {};
    buffer_desc.usage = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst;
    buffer_desc.size = size;
    WGPUBuffer buffer = wgpuDeviceCreateBuffer(d.device, &buffer_desc);

    WGPUTexelCopyTextureInfo src = {};
    src.texture = texture;
    src.aspect = WGPUTextureAspect_All;
    WGPUTexelCopyBufferInfo dst = {};
    dst.buffer = buffer;
    dst.layout.bytesPerRow = padded;
    dst.layout.rowsPerImage = height;
    WGPUExtent3D extent = {width, height, 1};

    WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(d.device, nullptr);
    wgpuCommandEncoderCopyTextureToBuffer(enc, &src, &dst, &extent);
    WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(enc, nullptr);
    wgpuQueueSubmit(d.queue, 1, &cmd);
    wgpuCommandBufferRelease(cmd);
    wgpuCommandEncoderRelease(enc);

    MapReply reply;
    WGPUBufferMapCallbackInfo map_cb = {};
    map_cb.mode = WGPUCallbackMode_WaitAnyOnly;
    map_cb.callback = [](WGPUMapAsyncStatus status, WGPUStringView msg, void* userdata1, void*) {
        auto* r = static_cast<MapReply*>(userdata1);
        r->status = status;
        r->message = to_string(msg);
    };
    map_cb.userdata1 = &reply;
    if (!wait(d.instance, wgpuBufferMapAsync(buffer, WGPUMapMode_Read, 0, size_t(size), map_cb))) {
        wgpuBufferRelease(buffer);
        message = "timed out mapping the readback buffer";
        return Status::Timeout;
    }
    if (reply.status != WGPUMapAsyncStatus_Success) {
        wgpuBufferRelease(buffer);
        message = reply.message;
        return Status::MapFailed;
    }
    const auto* mapped = static_cast<const uint8_t*>(wgpuBufferGetConstMappedRange(buffer, 0, size_t(size)));
    out.resize(size_t(row) * height);
    for (uint32_t y = 0; y < height; ++y) {
        std::memcpy(out.data() + size_t(y) * row, mapped + size_t(y) * padded, row);
    }
    wgpuBufferUnmap(buffer);
    wgpuBufferRelease(buffer);
    return Status::Ok;
}

}  // namespace nm::gpu
