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
bool adapter_type_allowed(WGPUAdapterType type, bool allow_fallback);
bool wait(WGPUInstance instance, WGPUFuture future);
Status read_rgba8(Device& device, WGPUTexture texture, uint32_t width, uint32_t height,
                  std::vector<uint8_t>& out, std::string& message);

}  // namespace nm::gpu
