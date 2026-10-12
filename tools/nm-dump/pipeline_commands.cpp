#include "pipeline_commands.h"

#include "core/edit/jsv.h"
#include "core/value/js_number.h"
#include "core/value/json.h"
#include "gpu/backend/pipeline.h"
#include "gpu/backend/recording_device.h"

#include <array>
#include <cmath>
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

double number(const nm::Value& value, const char16_t* name) {
    if (!value.is_number()) throw nm::JsError(u"Error", nm::JsString(name) + u" must be a number");
    return value.as_number();
}

nm::Value optionalId(const std::optional<nm::JsString>& id) { return id ? nm::Value(*id) : nm::Value(); }

nm::Value surfaceRecord(const be::Surface& surface) {
    nm::Object record;
    if (surface.mesh) {
        record.set(u"positions", nm::Value(surface.positions));
        record.set(u"normals", nm::Value(surface.normals));
        record.set(u"uvs", nm::Value(surface.uvs));
        record.set(u"width", nm::Value(surface.width));
        record.set(u"height", nm::Value(surface.height));
        if (surface.meshCurrentFrame) record.set(u"currentFrame", nm::Value(surface.currentFrame));
    } else {
        record.set(u"read", optionalId(surface.read));
        record.set(u"write", optionalId(surface.write));
        record.set(u"currentFrame", nm::Value(surface.currentFrame));
    }
    return nm::Value(std::move(record));
}

nm::Array strings(const std::vector<nm::JsString>& items) {
    nm::Array out;
    for (const nm::JsString& item : items) out.emplace_back(item);
    return out;
}

// The state tools/reference-pipeline.mjs records after each op.
nm::Value state(const be::Pipeline& pipeline, const be::WebGPUBackend& backend) {
    nm::Object out;
    out.set(u"width", nm::Value(pipeline.width()));
    out.set(u"height", nm::Value(pipeline.height()));
    nm::Array surfaces;
    for (const auto& [name, surface] : pipeline.surfaces()) surfaces.emplace_back(nm::Array{nm::Value(name), surfaceRecord(surface)});
    out.set(u"surfaces", nm::Value(std::move(surfaces)));
    nm::Array mipTargets;
    for (const auto& id : pipeline.mipTargets()) mipTargets.push_back(id ? nm::Value(*id) : nm::Value::null());
    out.set(u"mipTargets", nm::Value(std::move(mipTargets)));
    out.set(u"feedbackSurfaces", pipeline.feedbackSurfaces() ? nm::Value(strings(*pipeline.feedbackSurfaces())) : nm::Value::null());
    out.set(u"needsMidiNoteGrid", pipeline.needsMidiNoteGrid() ? nm::Value(*pipeline.needsMidiNoteGrid()) : nm::Value::null());
    nm::Array aliases;
    for (const auto& [member, storage] : pipeline.textureAliases()) aliases.emplace_back(nm::Array{nm::Value(member), nm::Value(storage)});
    out.set(u"textureAliases", nm::Value(std::move(aliases)));
    out.set(u"diagnostics", nm::Value(nm::Array(pipeline.diagnostics().begin(), pipeline.diagnostics().end())));
    out.set(u"lifecycleEffects", nm::Value(strings(pipeline.lifecycleEffects())));
    nm::Array textures;
    for (const nm::JsString& id : backend.textureIds()) {
        const be::TextureRecord* tex = backend.texture(id);
        nm::Object record;
        record.set(u"handle", nm::Value(static_cast<double>(tex->handle)));
        record.set(u"width", tex->width);
        record.set(u"height", tex->height);
        record.set(u"depth", tex->depth);
        record.set(u"format", tex->format);
        record.set(u"persistent", nm::Value(tex->persistent));
        record.set(u"is3D", nm::Value(tex->is3D));
        textures.emplace_back(nm::Array{nm::Value(id), nm::Value(std::move(record))});
    }
    out.set(u"textures", nm::Value(std::move(textures)));
    const nm::Value& graph = pipeline.graph();
    nm::Array passUniforms;
    const nm::Value passes = field(graph, u"passes");
    if (passes.is_array()) {
        for (const nm::Value& pass : passes.as_array()) {
            const nm::Value uniforms = pass.is_object() ? field(pass, u"uniforms") : nm::Value();
            passUniforms.push_back(uniforms.is_undefined() || uniforms.is_null() ? nm::Value::null() : uniforms);
        }
    }
    out.set(u"passUniforms", nm::Value(std::move(passUniforms)));
    const nm::Value textureSpecs = field(graph, u"textures");
    out.set(u"textureSpecs", textureSpecs.is_undefined() ? nm::Value::null() : textureSpecs);
    out.set(u"resourcePlan", pipeline.getResourcePlan());
    out.set(u"globalUniforms", pipeline.globalUniforms());
    out.set(u"frameIndex", nm::Value(pipeline.frameIndex()));
    out.set(u"lastTime", nm::Value(pipeline.lastTime()));
    out.set(u"lastPassCount", nm::Value(pipeline.lastPassCount()));
    const auto bindings = [](const be::Pipeline::FrameBindings& map) {
        nm::Array out;
        for (const auto& [name, id] : map) out.emplace_back(nm::Array{nm::Value(name), id ? nm::Value(*id) : nm::Value::null()});
        return nm::Value(std::move(out));
    };
    out.set(u"frameReadTextures", bindings(pipeline.frameReadTextures()));
    out.set(u"frameWriteTextures", bindings(pipeline.frameWriteTextures()));
    nm::Array viewports;
    if (passes.is_array()) {
        for (const nm::Value& pass : passes.as_array()) {
            const nm::Value resolved = pass.is_object() ? field(pass, u"viewportResolved") : nm::Value();
            viewports.push_back(resolved.is_undefined() || resolved.is_null() ? nm::Value::null() : resolved);
        }
    }
    out.set(u"passViewports", nm::Value(std::move(viewports)));
    return nm::Value(std::move(out));
}

nm::Value error_record(const nm::JsString& name, const nm::JsString& message) {
    nm::Object error;
    error.set(u"name", nm::Value(name));
    error.set(u"message", nm::Value(message));
    return nm::Value(std::move(error));
}

}  // namespace

// Replays JSON lines on a Pipeline over a WebGPUBackend and a RecordingDevice,
// as tools/reference-pipeline.mjs replays them on the reference Pipeline.
// Writes {key, calls, state}, with result where the op returns a value and
// error when it throws, for each op.
int pipeline(int, char**) {
    std::unique_ptr<be::RecordingDevice> device;
    std::unique_ptr<be::WebGPUBackend> backend;
    std::unique_ptr<be::Pipeline> pipeline;
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;
        nm::Object record;
        nm::Value error;
        try {
            const nm::Value op = nm::json::parse(line);
            const nm::JsString kind = field(op, u"op").as_string();
            record.set(u"key", field(op, u"key"));
            try {
                if (kind == u"create") {
                    pipeline.reset();
                    backend.reset();
                    device = std::make_unique<be::RecordingDevice>();
                    backend = std::make_unique<be::WebGPUBackend>(*device);
                    be::Pipeline::Options options;
                    options.texturePooling = nm::jsv::strict_equals(field(field(op, u"options"), u"texturePooling"), nm::Value(true));
                    pipeline = std::make_unique<be::Pipeline>(field(op, u"graph"), *backend, options);
                } else if (!pipeline) {
                    throw nm::JsError(u"TypeError", u"Cannot read properties of null (reading '" + kind + u"')");
                } else if (kind == u"init") {
                    pipeline->init(number(field(op, u"width"), u"width"), number(field(op, u"height"), u"height"));
                } else if (kind == u"resize") {
                    pipeline->resize(number(field(op, u"width"), u"width"), number(field(op, u"height"), u"height"));
                } else if (kind == u"recompile") {
                    pipeline->recompile(field(op, u"graph"));
                } else if (kind == u"updateParameterTextures") {
                    const nm::Value uniforms = field(op, u"uniforms");
                    pipeline->updateParameterTextures(uniforms.is_undefined() ? nm::Value(nm::Object{}) : uniforms);
                } else if (kind == u"render") {
                    pipeline->render(number(field(op, u"time"), u"time"));
                } else if (kind == u"syncTime") {
                    pipeline->syncTime(number(field(op, u"time"), u"time"));
                } else if (kind == u"setTileRegion") {
                    const auto pair = [&](const char16_t* key) {
                        const nm::Value value = field(op, key);
                        return std::array<double, 2>{number(nm::jsv::get(value, u"0"), key), number(nm::jsv::get(value, u"1"), key)};
                    };
                    const nm::Value renderScale = field(op, u"renderScale");
                    pipeline->setTileRegion(pair(u"offset"), pair(u"fullResolution"),
                                            renderScale.is_undefined() || renderScale.is_null() ? std::nullopt
                                                                                                : std::optional<double>(number(renderScale, u"renderScale")));
                } else if (kind == u"clearTileRegion") {
                    pipeline->clearTileRegion();
                } else if (kind == u"clearSurface") {
                    pipeline->clearSurface(field(op, u"name").as_string());
                } else if (kind == u"resolveDimension") {
                    const nm::Value uniforms = field(op, u"uniforms");
                    const double value = pipeline->resolveDimension(field(op, u"spec"), number(field(op, u"screenSize"), u"screenSize"),
                                                                    uniforms.is_undefined() ? nm::Value(nm::Object{}) : uniforms);
                    record.set(u"result", nm::Value(value == 0 && std::signbit(value) ? nm::JsString(u"-0") : nm::js_number_to_string(value)));
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
        record.set(u"calls", device ? nm::Value(device->takeLog()) : nm::Value(nm::Array{}));
        record.set(u"state", pipeline ? state(*pipeline, *backend) : nm::Value::null());
        if (!error.is_undefined()) record.set(u"error", error);
        std::cout << nm::json::stringify(nm::Value(std::move(record))) << '\n';
    }
    return 0;
}

}  // namespace nm_dump
