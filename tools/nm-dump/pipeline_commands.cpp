#include "pipeline_commands.h"

#include "core/edit/jsv.h"
#include "core/value/js_number.h"
#include "core/value/json.h"
#include "gpu/backend/pipeline.h"
#include "gpu/backend/recording_device.h"

#include <array>
#include <cmath>
#include <cstdint>
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

// passes[index]: the index as a property key, which names an element only
// when it is a canonical array index ("0", "12"; not "01", "0.5" or "-1").
size_t passIndex(const nm::Value& value) {
    const nm::JsString key = nm::jsv::to_property_key(value);
    if (key.empty() || key.size() > 10 || (key.size() > 1 && key[0] == u'0')) return static_cast<size_t>(-1);
    uint64_t n = 0;
    for (const char16_t c : key) {
        if (c < u'0' || c > u'9') return static_cast<size_t>(-1);
        n = n * 10 + static_cast<uint64_t>(c - u'0');
    }
    return n < 4294967295ull ? static_cast<size_t>(n) : static_cast<size_t>(-1);
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

// JSON has no -0, NaN or Infinity: they travel as {"$number": text}, as
// tools/reference-pipeline.mjs writes them.
void tagSpecialNumbers(nm::Value& value) {
    if (value.is_number()) {
        const double n = value.as_number();
        if (std::isfinite(n) && !(n == 0 && std::signbit(n))) return;
        nm::Object tag;
        tag.set(u"$number", nm::Value(n == 0 ? nm::JsString(u"-0") : nm::js_number_to_string(n)));
        value = nm::Value(std::move(tag));
    } else if (value.is_array()) {
        for (nm::Value& element : value.as_array()) tagSpecialNumbers(element);
    } else if (value.is_object()) {
        for (const nm::JsString& key : value.as_object().keys()) tagSpecialNumbers(value.as_object()[key]);
    }
}

// {"$number": text} stands for a number JSON cannot hold.
nm::Value decodeNumbers(const nm::Value& value) {
    if (value.is_array()) {
        nm::Array out;
        for (const nm::Value& element : value.as_array()) out.push_back(decodeNumbers(element));
        return nm::Value(std::move(out));
    }
    if (!value.is_object()) return value;
    const std::vector<nm::JsString> keys = value.as_object().keys();
    if (keys.size() == 1 && keys[0] == u"$number") {
        const nm::JsString text = value.as_object().find(u"$number")->as_string();
        return nm::Value(text == u"-0" ? -0.0 : nm::js::stringToNumber(text));
    }
    nm::Object out;
    for (const nm::JsString& key : keys) out.set(key, decodeNumbers(*value.as_object().find(key)));
    return nm::Value(std::move(out));
}

// An object whose undefined members are kept, as {"$undefined": true}.
nm::Value taggedUndefined(const nm::Value& object) {
    if (!object.is_object()) return object;
    nm::Object out;
    for (const nm::JsString& key : object.as_object().keys()) {
        const nm::Value& member = *object.as_object().find(key);
        if (member.is_undefined()) {
            nm::Object tag;
            tag.set(u"$undefined", nm::Value(true));
            out.set(key, nm::Value(std::move(tag)));
        } else {
            out.set(key, member);
        }
    }
    return nm::Value(std::move(out));
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
        for (const auto& [name, id] : map.entries()) out.emplace_back(nm::Array{nm::Value(name), id ? nm::Value(*id) : nm::Value::null()});
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
    out.set(u"proxyUniforms", taggedUndefined(pipeline.proxyUniforms()));
    out.set(u"resolvedUniforms", taggedUndefined(pipeline.resolvedUniforms()));
    nm::Value encoded(std::move(out));
    tagSpecialNumbers(encoded);
    return encoded;
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
                } else if (kind == u"init" || kind == u"resize") {
                    // Each read is its own statement: C++ leaves argument order unspecified.
                    const double width = number(field(op, u"width"), u"width");
                    const double height = number(field(op, u"height"), u"height");
                    if (kind == u"init") pipeline->init(width, height);
                    else pipeline->resize(width, height);
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
                    const std::array<double, 2> offset = pair(u"offset");
                    const std::array<double, 2> fullResolution = pair(u"fullResolution");
                    const nm::Value renderScale = field(op, u"renderScale");
                    const std::optional<double> scale = renderScale.is_undefined() || renderScale.is_null()
                        ? std::nullopt : std::optional<double>(number(renderScale, u"renderScale"));
                    pipeline->setTileRegion(offset, fullResolution, scale);
                } else if (kind == u"clearTileRegion") {
                    pipeline->clearTileRegion();
                } else if (kind == u"getFrameState") {
                    const be::FrameState& state = pipeline->getFrameState();
                    nm::Object result;
                    result.set(u"frameIndex", nm::Value(state.frameIndex));
                    result.set(u"time", nm::Value(state.time));
                    result.set(u"globalUniforms", state.globalUniforms);
                    nm::Object surfaces;
                    for (const auto& [name, id] : state.surfaces) {
                        surfaces.set(name, nm::Value(static_cast<double>(backend->texture(id)->handle)));
                    }
                    result.set(u"surfaces", nm::Value(std::move(surfaces)));
                    nm::Object writeSurfaces;
                    for (const auto& [name, id] : state.writeSurfaces) writeSurfaces.set(name, id.is_undefined() ? nm::Value::null() : id);
                    result.set(u"writeSurfaces", nm::Value(std::move(writeSurfaces)));
                    result.set(u"screenWidth", state.screenWidth);
                    result.set(u"screenHeight", state.screenHeight);
                    result.set(u"renderSurface", state.renderSurface.is_undefined() ? nm::Value::null() : state.renderSurface);
                    nm::Value encoded(std::move(result));
                    tagSpecialNumbers(encoded);
                    record.set(u"result", encoded);
                } else if (kind == u"setUniform") {
                    const nm::JsString name = field(op, u"name").as_string();
                    pipeline->setUniform(name, decodeNumbers(field(op, u"value")));
                } else if (kind == u"setPassUniform") {
                    const size_t pass = passIndex(field(op, u"pass"));
                    const nm::JsString name = field(op, u"name").as_string();
                    pipeline->setPassUniform(pass, name, decodeNumbers(field(op, u"value")));
                } else if (kind == u"broadcastChainScopedParam") {
                    const size_t pass = passIndex(field(op, u"pass"));
                    const nm::JsString uniformName = field(op, u"uniformName").as_string();
                    const nm::JsString scopedName = field(op, u"scopedName").as_string();
                    pipeline->broadcastChainScopedParam(pass, uniformName, scopedName);
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
