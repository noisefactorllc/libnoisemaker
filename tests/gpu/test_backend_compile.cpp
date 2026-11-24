#include "check.h"
#include "core/edit/jsv.h"
#include "gpu/backend/backend.h"
#include "gpu/backend/recording_device.h"

#include <array>
#include <cstdio>
#include <vector>

namespace {

using libnoisemaker::JsError;
using libnoisemaker::JsString;
using libnoisemaker::backend::Handle;
using libnoisemaker::backend::RecordingDevice;
using libnoisemaker::backend::WebGPUBackend;
using libnoisemaker::Object;
using libnoisemaker::Value;

// The first failAfter_ modules compile; later ones report a diagnostic, so
// each failure point in compileProgram is reachable one case at a time. The
// device records every module it creates and every release, which the base
// class does not log.
class DiagnosticDevice final : public RecordingDevice {
public:
    void failAfter(size_t calls) { failAfter_ = calls; }

    Handle createShaderModule(const JsString& code) override {
        created_.push_back(RecordingDevice::createShaderModule(code));
        failing_ = created_.size() > failAfter_;
        return created_.back();
    }

    std::vector<JsString> compilationErrors(Handle) override {
        if (failing_) return {u"boom"};
        return {};
    }

    void release(Handle object) noexcept override {
        if (releasedCount_ < released_.size()) released_[releasedCount_++] = object;
        RecordingDevice::release(object);
    }

    std::vector<Handle> created_;
    std::array<Handle, 16> released_{};
    size_t releasedCount_ = 0;

private:
    size_t failAfter_ = 0;
    bool failing_ = false;
};

bool released_a(const DiagnosticDevice& device, Handle handle) {
    for (size_t i = 0; i < device.releasedCount_; ++i) {
        if (device.released_[i] == handle) return true;
    }
    return false;
}

const char16_t* kFragment = u"@fragment fn main() -> @location(0) vec4<f32> { return vec4<f32>(0.0); }";
const char16_t* kVertex = u"@vertex fn vs() -> @builtin(position) vec4<f32> { return vec4<f32>(0.0); }";
const char16_t* kCompute = u"@compute @workgroup_size(1) fn main() {}";

// A failing compute module is released when compileProgram throws.
void test_releases_a_failed_compute_module() {
    DiagnosticDevice device;
    device.failAfter(0);
    WebGPUBackend backend(device);
    backend.init();
    Object spec;
    spec.set(u"wgsl", Value(JsString(kCompute)));
    bool threw = false;
    try {
        backend.compileProgram(u"p", Value(std::move(spec)));
    } catch (const JsError& error) {
        threw = error.name() == JsString(u"ShaderDiagnostic") && error.message() == JsString(u"boom");
    }
    NM_CHECK(threw);
    NM_CHECK(device.created_.size() == 1);
    NM_CHECK(device.releasedCount_ == 1);
    NM_CHECK(device.released_[0] == device.created_[0]);
}

// A failing render module is released when compileProgram throws.
void test_releases_a_failed_render_module() {
    DiagnosticDevice device;
    device.failAfter(0);
    WebGPUBackend backend(device);
    backend.init();
    Object spec;
    spec.set(u"wgsl", Value(JsString(kFragment)));
    bool threw = false;
    try {
        backend.compileProgram(u"p", Value(std::move(spec)));
    } catch (const JsError& error) {
        threw = error.name() == JsString(u"ShaderDiagnostic") && error.message() == JsString(u"boom");
    }
    NM_CHECK(threw);
    NM_CHECK(device.created_.size() == 1);
    NM_CHECK(device.releasedCount_ == 1);
    NM_CHECK(device.released_[0] == device.created_[0]);
}

// A failing vertex module is released with the main module it left behind.
void test_releases_a_failed_vertex_module() {
    DiagnosticDevice device;
    device.failAfter(1);
    WebGPUBackend backend(device);
    backend.init();
    Object spec;
    spec.set(u"wgsl", Value(JsString(kFragment)));
    spec.set(u"vertexWGSL", Value(JsString(kVertex)));
    bool threw = false;
    try {
        backend.compileProgram(u"p", Value(std::move(spec)));
    } catch (const JsError& error) {
        threw = error.name() == JsString(u"ShaderDiagnostic") && error.message() == JsString(u"boom");
    }
    NM_CHECK(threw);
    NM_CHECK(device.created_.size() == 2);
    NM_CHECK(device.releasedCount_ == 2);
    NM_CHECK(released_a(device, device.created_[0]));
    NM_CHECK(released_a(device, device.created_[1]));
}

// A compiling program releases nothing.
void test_releases_nothing_on_success() {
    DiagnosticDevice device;
    device.failAfter(64);
    WebGPUBackend backend(device);
    backend.init();
    Object spec;
    spec.set(u"wgsl", Value(JsString(kFragment)));
    spec.set(u"vertexWGSL", Value(JsString(kVertex)));
    backend.compileProgram(u"p", Value(std::move(spec)));
    NM_CHECK(device.created_.size() == 2);
    NM_CHECK(device.releasedCount_ == 0);
}

void test_mrt_pipeline_is_built_for_its_actual_attachments() {
    RecordingDevice device;
    WebGPUBackend backend(device);
    backend.init();
    Object spec;
    spec.set(u"wgsl", Value(uR"(
        struct Result {
            @location(0) first: vec4f,
            // } @location(2) is not a field.
            @location(1) second: vec4f,
        }
        @fragment fn main(@location(0) uv: vec2f) -> Result {
            return Result(vec4f(uv, 0.0, 1.0), vec4f(1.0));
        }
    )"));
    backend.compileProgram(u"mrt", Value(std::move(spec)));
    const auto log = device.takeLog();
    size_t eagerPipelines = 0;
    for (const auto& call : log) {
        if (libnoisemaker::jsv::strict_equals(libnoisemaker::jsv::get(call, u"call"), Value(u"createRenderPipeline"))) {
            ++eagerPipelines;
        }
    }
    NM_CHECK(eagerPipelines == 0);
    NM_CHECK(backend.programsSnapshot().at(u"mrt").fragmentOutputCount == 2);
}

void test_depth_eviction_waits_for_the_current_frame() {
    using libnoisemaker::backend::FrameState;
    RecordingDevice device;
    WebGPUBackend backend(device);
    backend.init();
    Object shader;
    shader.set(u"wgsl", Value(kFragment));
    backend.compileProgram(u"mesh", Value(std::move(shader)));
    backend.beginFrame();
    for (int width = 1; width <= 5; ++width) {
        Object spec;
        spec.set(u"width", Value(double(width)));
        spec.set(u"height", Value(1.0));
        const JsString id(1, static_cast<char16_t>(u'a' + width));
        backend.createTexture(id, Value(std::move(spec)));
        Object outputs;
        outputs.set(u"color", Value(id));
        Object pass;
        pass.set(u"program", Value(u"mesh"));
        pass.set(u"drawMode", Value(u"triangles"));
        pass.set(u"outputs", Value(std::move(outputs)));
        backend.executePass(Value(std::move(pass)), FrameState{});
    }
    for (const auto& call : device.takeLog()) {
        NM_CHECK(!libnoisemaker::jsv::strict_equals(libnoisemaker::jsv::get(call, u"call"), Value(u"destroyTexture")));
    }
    backend.endFrame();
    size_t destroyed = 0;
    for (const auto& call : device.takeLog()) {
        if (libnoisemaker::jsv::strict_equals(libnoisemaker::jsv::get(call, u"call"), Value(u"destroyTexture"))) ++destroyed;
    }
    NM_CHECK(destroyed == 1);
}

}  // namespace

int main() {
    test_releases_a_failed_compute_module();
    test_releases_a_failed_render_module();
    test_releases_a_failed_vertex_module();
    test_releases_nothing_on_success();
    test_mrt_pipeline_is_built_for_its_actual_attachments();
    test_depth_eviction_waits_for_the_current_frame();
    std::printf("test_backend_compile: ok\n");
    return 0;
}
