#include "backend_commands.h"

#include "core/edit/jsv.h"
#include "core/value/json.h"
#include "gpu/backend/backend.h"
#include "gpu/backend/recording_device.h"

#include <exception>
#include <iostream>
#include <memory>
#include <string>

namespace nm_dump {

namespace {

namespace nm = libnoisemaker;
namespace be = libnoisemaker::backend;

nm::Value field(const nm::Value& object, const char16_t* key) {
    const nm::Value* found = object.is_object() ? object.as_object().find(key) : nullptr;
    return found ? *found : nm::Value();
}

// {"$float32": [numbers]} stands for a Float32Array.
nm::Value decode(const nm::Value& value) {
    if (!value.is_object() || value.as_object().size() != 1) return value;
    const nm::Value* elements = value.as_object().find(u"$float32");
    if (!elements || !elements->is_array()) return value;
    std::vector<float> floats;
    for (const nm::Value& element : elements->as_array()) floats.push_back(static_cast<float>(element.as_number()));
    return nm::Value::float32_array(std::move(floats));
}

nm::Value decode_members(const nm::Value& encoded) {
    const nm::Value value = decode(encoded);
    if (!value.is_object()) return value;
    nm::Object out;
    for (const auto& key : value.as_object().keys()) out.set(key, decode(*value.as_object().find(key)));
    return nm::Value(std::move(out));
}

be::FrameState frame_state(const nm::Value& state) {
    be::FrameState out;
    out.globalUniforms = decode_members(field(state, u"globalUniforms"));
    for (const auto& [name, id] : nm::jsv::entries(field(state, u"surfaces"))) {
        if (id.is_string()) out.surfaces.emplace_back(name, id.as_string());
    }
    for (const auto& [name, id] : nm::jsv::entries(field(state, u"writeSurfaces"))) out.writeSurfaces.emplace_back(name, id);
    out.renderSurface = field(state, u"renderSurface");
    out.screenWidth = field(state, u"screenWidth");
    out.screenHeight = field(state, u"screenHeight");
    return out;
}

nm::Value error_record(const nm::JsString& name, const nm::JsString& message) {
    nm::Object error;
    error.set(u"name", nm::Value(name));
    error.set(u"message", nm::Value(message));
    return nm::Value(std::move(error));
}

}  // namespace

// Replays JSON lines on one RecordingDevice and WebGPUBackend, as
// tools/reference-backend.mjs replays them on the reference backend. Writes
// {key, calls} or {key, calls, error: {name, message}} for each op but reset.
int backend(int, char**) {
    auto device = std::make_unique<be::RecordingDevice>();
    auto backend = std::make_unique<be::WebGPUBackend>(*device);
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;
        nm::Object record;
        nm::Value error;
        try {
            const nm::Value op = nm::json::parse(line);
            const nm::JsString kind = field(op, u"op").as_string();
            if (kind == u"reset") {
                backend.reset();
                device = std::make_unique<be::RecordingDevice>();
                backend = std::make_unique<be::WebGPUBackend>(*device);
                continue;
            }
            record.set(u"key", field(op, u"key"));
            try {
                const nm::Value id = field(op, u"id");
                if (kind == u"init") backend->init();
                else if (kind == u"rejectBindings") {
                    std::vector<double> bindings;
                    for (const nm::Value& b : nm::jsv::iterate_anon(field(op, u"bindings").is_undefined() ? nm::Value(nm::Array{}) : field(op, u"bindings"))) {
                        bindings.push_back(b.as_number());
                    }
                    const nm::Value message = field(op, u"message");
                    device->rejectBindings(std::move(bindings), message.is_string() ? std::optional<nm::JsString>(message.as_string()) : std::nullopt);
                }
                else if (kind == u"createTexture") backend->createTexture(id.as_string(), field(op, u"spec"));
                else if (kind == u"createTexture3D") backend->createTexture3D(id.as_string(), field(op, u"spec"));
                else if (kind == u"createCubeTexture") backend->createCubeTexture(id.as_string(), field(op, u"size"));
                else if (kind == u"uploadDataTexture") {
                    const nm::Value data = decode(field(op, u"data"));
                    backend->uploadDataTexture(id.as_string(), data.as_float32_array(), field(op, u"width"), field(op, u"height"));
                } else if (kind == u"destroyTexture") backend->destroyTexture(id.as_string());
                else if (kind == u"compileProgram") backend->compileProgram(id.as_string(), field(op, u"spec"));
                else if (kind == u"beginFrame") backend->beginFrame();
                else if (kind == u"endFrame") backend->endFrame();
                else if (kind == u"createBindGroup") {
                    const be::ProgramInfo* program = backend->program(field(op, u"program").as_string());
                    if (!program) throw nm::JsError(u"TypeError", u"Cannot read properties of undefined (reading 'bindings')");
                    backend->createBindGroup(field(op, u"pass"), *program, frame_state(field(op, u"state")));
                } else if (kind == u"executePass") {
                    backend->executePass(field(op, u"pass"), frame_state(field(op, u"state")));
                } else {
                    throw nm::JsError(u"Error", u"unknown op " + kind);
                }
            } catch (const be::ThrownValue& e) {
                // JSON.parse(JSON.stringify(value)), as the oracle records it.
                nm::Object thrown;
                thrown.set(u"thrown", nm::json::parse(nm::json::stringify(e.value())));
                error = nm::Value(std::move(thrown));
            } catch (const nm::JsError& e) {
                error = error_record(e.name(), e.message());
            } catch (const std::exception& e) {
                error = error_record(u"InternalError", nm::utf8_to_js(e.what()));
            }
        } catch (const std::exception& e) {
            error = error_record(u"InternalError", nm::utf8_to_js(e.what()));
        }
        record.set(u"calls", nm::Value(device->takeLog()));
        if (!error.is_undefined()) record.set(u"error", error);
        std::cout << nm::json::stringify(nm::Value(std::move(record))) << '\n';
    }
    return 0;
}

}  // namespace nm_dump
