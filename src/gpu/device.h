#pragma once

#include <webgpu/webgpu.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace nm::gpu {

struct DeviceOptions {
    bool allow_fallback = false;
    WGPUBackendType backend = WGPUBackendType_Undefined;
};

struct ErrorSink {
    std::mutex mutex;
    std::vector<std::string> errors;
    bool lost = false;
    std::string lost_message;
};

struct Device {
    WGPUInstance instance = nullptr;
    WGPUAdapter adapter = nullptr;
    WGPUDevice device = nullptr;
    WGPUQueue queue = nullptr;
    std::string adapter_name;
    WGPUAdapterType adapter_type = WGPUAdapterType_Unknown;
    uint32_t vendor_id = 0;
    uint32_t device_id = 0;
    bool is_fallback = false;
    std::unique_ptr<ErrorSink> errors = std::make_unique<ErrorSink>();

    Device() = default;
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;
    Device(Device&& other) noexcept;
    Device& operator=(Device&& other) noexcept;
    ~Device();
};

enum class Status { Ok, NoAdapter, DeviceFailed, MapFailed, Timeout };

struct DeviceResult {
    Status status = Status::DeviceFailed;
    std::string message;
    Device device;
};

DeviceResult create_device(const DeviceOptions& options);
// True for software rasterizers: any CPU adapter, and WARP (the Microsoft
// Basic Render Driver), which Dawn can report as a GPU.
bool adapter_is_software(WGPUAdapterType type, uint32_t vendor_id, uint32_t device_id);
// Hardware GPUs are always allowed. Software and unknown adapters need allow_fallback.
bool adapter_allowed(WGPUAdapterType type, uint32_t vendor_id, uint32_t device_id, bool allow_fallback);
bool wait(WGPUInstance instance, WGPUFuture future);
Status read_rgba8(Device& device, WGPUTexture texture, uint32_t width, uint32_t height,
                  std::vector<uint8_t>& out, std::string& message);

}  // namespace nm::gpu
