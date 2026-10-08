#include <noisemaker/noisemaker.h>

#include "capi/error.h"
#include "gpu/device.h"

#include <utility>

struct nm_context {
    nm::gpu::Device device;
};

extern "C" nm_status nm_context_create(const nm_context_desc* desc, nm_context** out) {
    nm::capi::clear_error();
    if (out == nullptr) {
        nm::capi::set_error("nm_context_create: out is NULL");
        return NM_ERR_INVALID_ARGUMENT;
    }
    *out = nullptr;
    nm::gpu::DeviceOptions options;
    if (desc != nullptr) {
        if (desc->struct_size != sizeof(nm_context_desc)) {
            nm::capi::set_error("nm_context_create: desc->struct_size does not match this library's nm_context_desc");
            return NM_ERR_INVALID_ARGUMENT;
        }
        options.allow_fallback = desc->allow_fallback_adapter != 0;
    }
    auto result = nm::gpu::create_device(options);
    switch (result.status) {
        case nm::gpu::Status::Ok: break;
        case nm::gpu::Status::NoAdapter:
            nm::capi::set_error("no WebGPU adapter: " + result.message);
            return NM_ERR_NO_ADAPTER;
        default:
            nm::capi::set_error("device creation failed: " + result.message);
            return NM_ERR_DEVICE_FAILED;
    }
    *out = new nm_context{std::move(result.device)};
    return NM_OK;
}

extern "C" void nm_context_destroy(nm_context* ctx) { delete ctx; }

extern "C" const char* nm_context_adapter_name(const nm_context* ctx) {
    return ctx ? ctx->device.adapter_name.c_str() : "";
}
