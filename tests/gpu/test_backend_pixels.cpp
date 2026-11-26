#include "check.h"
#include "gpu/backend/backend.h"
#include "gpu/dawn_device.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <mutex>
#include <utility>

namespace {

using libnoisemaker::Array;
using libnoisemaker::JsString;
using libnoisemaker::Object;
using libnoisemaker::Value;
using libnoisemaker::backend::FrameState;
using libnoisemaker::backend::WebGPUBackend;

void compile_color(WebGPUBackend& backend, const JsString& name, const JsString& rgba, bool triangles = false) {
    Object spec;
    spec.set(u"wgsl", Value(u"struct Params { gain: f32, }\n"
        u"@group(0) @binding(0) var<uniform> params: Params;\n"
        u"@fragment fn main() -> @location(0) vec4<f32> { return vec4<f32>(" + rgba + u") * params.gain; }"));
    spec.set(u"outputFormat", Value(u"rgba16f"));
    if (triangles) {
        spec.set(u"vertexWGSL", Value(u"@vertex fn screen_vertex(@builtin(vertex_index) index: u32) -> @builtin(position) vec4<f32> {\n"
            u"  let positions = array<vec2<f32>, 3>(vec2<f32>(-1.0, -1.0), vec2<f32>(-1.0, 3.0), vec2<f32>(3.0, -1.0));\n"
            u"  return vec4<f32>(positions[index], 0.0, 1.0);\n} "));
        spec.set(u"vertexEntryPoint", Value(u"screen_vertex"));
    }
    backend.compileProgram(name, Value(std::move(spec)));
}

Value color_pass(const JsString& program, Value blend = Value(), bool triangles = false) {
    Object outputs;
    outputs.set(u"fragColor", Value(u"work"));
    Object uniforms;
    uniforms.set(u"gain", Value(1.0));
    Object pass;
    pass.set(u"program", Value(program));
    pass.set(u"outputs", Value(std::move(outputs)));
    pass.set(u"uniforms", Value(std::move(uniforms)));
    pass.set(u"blend", std::move(blend));
    if (triangles) {
        pass.set(u"drawMode", Value(u"triangles"));
        pass.set(u"count", Value(u"screen"));
    }
    return Value(std::move(pass));
}

void check_pixels(WebGPUBackend& backend, const std::array<double, 4>& expected, const char* label = "blend") {
    const auto pixels = backend.readPixels(u"work");
    NM_CHECK(pixels.data.size() == 8 * 8 * 4);
    for (size_t i = 0; i < pixels.data.size(); ++i) {
        if (std::abs(double(pixels.data[i]) - expected[i % 4] * 255) > 1) {
            std::fprintf(stderr, "%s channel %zu: %u, expected %.3f\n", label, i,
                unsigned(pixels.data[i]), expected[i % 4] * 255);
            NM_CHECK(false);
        }
    }
}

void test_rendered_blend_factors_and_screen_triangles() {
    libnoisemaker::gpu::DeviceOptions options;
    options.allow_fallback = std::getenv("NM_ALLOW_FALLBACK_ADAPTER") != nullptr;
    auto native = libnoisemaker::gpu::create_device(options);
    if (native.status != libnoisemaker::gpu::Status::Ok) std::fprintf(stderr, "%s\n", native.message.c_str());
    NM_CHECK(native.status == libnoisemaker::gpu::Status::Ok);
    std::printf("adapter: %s%s\n", native.device.adapter_name.c_str(), native.device.is_fallback ? " (fallback)" : "");
    libnoisemaker::gpu::DawnDevice device(native.device);
    WebGPUBackend backend(device);
    backend.init();
    Object texture;
    texture.set(u"width", Value(8.0));
    texture.set(u"height", Value(8.0));
    texture.set(u"format", Value(u"rgba16f"));
    backend.createTexture(u"work", Value(std::move(texture)));
    compile_color(backend, u"backdrop", u"0.25, 0.5, 1.0, 1.0");
    compile_color(backend, u"source", u"0.5, 0.5, 0.5, 0.5");
    compile_color(backend, u"triangles", u"0.5, 0.5, 0.5, 0.5", true);

    struct Case { const char16_t* source; const char16_t* destination; std::array<double, 4> expected; };
    const Case cases[] = {
        {u"ONE", u"ZERO", {0.5, 0.5, 0.5, 0.5}},
        {u"ZERO", u"ONE", {0.25, 0.5, 1.0, 1.0}},
        {u"SRC_COLOR", u"ZERO", {0.25, 0.25, 0.25, 0.25}},
        {u"DST_COLOR", u"ZERO", {0.125, 0.25, 0.5, 0.5}},
        {u"ONE_MINUS_SRC_COLOR", u"ZERO", {0.25, 0.25, 0.25, 0.25}},
        {u"ONE_MINUS_DST_COLOR", u"ZERO", {0.375, 0.25, 0, 0}},
        {u"SRC_ALPHA", u"ONE_MINUS_SRC_ALPHA", {0.375, 0.5, 0.75, 0.75}},
    };
    for (const auto& item : cases) {
        backend.beginFrame();
        backend.executePass(color_pass(u"backdrop"), FrameState{});
        backend.executePass(color_pass(u"source", Value(Array{Value(item.source), Value(item.destination)})), FrameState{});
        backend.endFrame();
        check_pixels(backend, item.expected);
    }
    backend.beginFrame();
    backend.executePass(color_pass(u"triangles", Value(), true), FrameState{});
    backend.endFrame();
    check_pixels(backend, {0.5, 0.5, 0.5, 0.5}, "screen triangles");
    device.waitIdle();
    std::lock_guard<std::mutex> lock(native.device.errors->mutex);
    for (const auto& error : native.device.errors->errors) std::fprintf(stderr, "%s\n", error.c_str());
    NM_CHECK(native.device.errors->errors.empty());
    NM_CHECK(!native.device.errors->lost);
}

}  // namespace

int main() {
    try {
        test_rendered_blend_factors_and_screen_triangles();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
    std::printf("test_backend_pixels: ok\n");
    return 0;
}
