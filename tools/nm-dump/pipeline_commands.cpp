#include "pipeline_commands.h"

#include "core/edit/jsv.h"
#include "core/value/js_number.h"
#include "core/value/json.h"
#include "gpu/backend/pipeline.h"
#include "gpu/backend/recording_device.h"
#include "host/audio_state.h"
#include "host/midi_state.h"

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

// An op's pass: a non-negative integer index into graph.passes; any other
// value names no pass (tools/reference-pipeline.mjs reads it the same way).
size_t passIndex(const nm::Value& value) {
    if (!value.is_number()) return static_cast<size_t>(-1);
    const double n = value.as_number();
    if (!(n >= 0) || n >= 4294967295.0 || std::floor(n) != n) return static_cast<size_t>(-1);
    return static_cast<size_t>(n);
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

// JSON has no -0, NaN or Infinity: they travel as {"$number": text}, and a
// Float32Array as {"$float32": [numbers]}, as tools/reference-pipeline.mjs
// writes them.
void tagSpecialNumbers(nm::Value& value) {
    if (value.is_float32_array()) {
        nm::Array numbers;
        for (const float f : value.as_float32_array()) numbers.emplace_back(static_cast<double>(f));
        nm::Object tag;
        tag.set(u"$float32", nm::Value(std::move(numbers)));
        value = nm::Value(std::move(tag));
    }
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

// {"$number": text} stands for a number JSON cannot hold, and
// {"$float32": [numbers]} for a Float32Array.
nm::Value decodeNumbers(const nm::Value& value) {
    if (value.is_array()) {
        nm::Array out;
        for (const nm::Value& element : value.as_array()) out.push_back(decodeNumbers(element));
        return nm::Value(std::move(out));
    }
    if (!value.is_object()) return value;
    const std::vector<nm::JsString> keys = value.as_object().keys();
    if (keys.size() == 1 && keys[0] == u"$float32") {
        std::vector<float> floats;
        for (const nm::Value& element : value.as_object().find(u"$float32")->as_array()) {
            floats.push_back(static_cast<float>(decodeNumbers(element).as_number()));
        }
        return nm::Value::float32_array(std::move(floats));
    }
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

// The global uniforms an op's record compares: audioWaveform and
// audioSpectrum only after a render (tools/reference-pipeline.mjs).
nm::Value globalsFor(const nm::Value& globals, const nm::JsString& kind) {
    if (kind == u"render" || !globals.is_object()) return globals;
    nm::Value out = globals;
    out.as_object().erase(u"audioWaveform");
    out.as_object().erase(u"audioSpectrum");
    return out;
}

// The state tools/reference-pipeline.mjs records after each op.
nm::Value state(const be::Pipeline& pipeline, const be::WebGPUBackend& backend, const nm::JsString& kind) {
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
    out.set(u"globalUniforms", globalsFor(pipeline.globalUniforms(), kind));
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
// The MIDI message bytes and the port of a midi op.
std::vector<uint8_t> bytes(const nm::Value& values) {
    std::vector<uint8_t> out;
    for (const nm::Value& v : values.as_array()) out.push_back(static_cast<uint8_t>(v.as_number()));
    return out;
}
nm::MidiPort midiPort(const nm::Value& port) {
    nm::MidiPort out;
    out.id = field(port, u"id").as_string();
    out.name = field(port, u"name").as_string();
    const nm::Value connected = field(port, u"connected");
    out.connected = !connected.is_bool() || connected.as_bool();
    return out;
}
nm::AudioDevice audioDevice(const nm::Value& device) {
    nm::AudioDevice out;
    out.id = field(device, u"id").as_string();
    out.name = field(device, u"name").as_string();
    const nm::Value count = field(device, u"channelCount");
    out.channelCount = count.is_number() ? static_cast<int>(count.as_number()) : 1;
    const nm::Value connected = field(device, u"connected");
    out.connected = !connected.is_bool() || connected.as_bool();
    return out;
}
double optionalNumber(const nm::Value& value) { return value.is_number() ? value.as_number() : std::nan(""); }

int pipeline(int, char**) {
    std::unique_ptr<be::RecordingDevice> device;
    std::unique_ptr<be::WebGPUBackend> backend;
    std::unique_ptr<be::Pipeline> pipeline;
    std::shared_ptr<nm::MidiState> midi;
    std::shared_ptr<nm::AudioState> audio;
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;
        nm::Object record;
        nm::Value error;
        nm::JsString kindOf;
        try {
            const nm::Value op = nm::json::parse(line);
            const nm::JsString kind = field(op, u"op").as_string();
            kindOf = kind;
            record.set(u"key", field(op, u"key"));
            try {
                if (kind == u"create") {
                    pipeline.reset();
                    midi.reset();
                    audio.reset();
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
                    const double time = number(field(op, u"time"), u"time");
                    const nm::Value wallTime = field(op, u"wallTime");
                    if (wallTime.is_number()) {
                        const double now = wallTime.as_number();
                        pipeline->setWallClock([now] { return now; });
                    }
                    pipeline->render(time);
                } else if (kind == u"attachMidi") {
                    midi = std::make_shared<nm::MidiState>(!nm::jsv::strict_equals(field(op, u"registry"), nm::Value(false)));
                    pipeline->setMidiState(midi);
                } else if (kind == u"detachMidi") {
                    pipeline->setMidiState(nullptr);
                } else if (kind == u"midiMessage") {
                    const std::vector<uint8_t> data = bytes(field(op, u"data"));
                    const nm::Value portValue = field(op, u"port");
                    const double time = number(field(op, u"time"), u"time");
                    if (portValue.is_object()) {
                        const nm::MidiPort port = midiPort(portValue);
                        midi->handleMessage(data.data(), static_cast<std::ptrdiff_t>(data.size()), &port, time);
                    } else {
                        midi->handleMessage(data.data(), static_cast<std::ptrdiff_t>(data.size()), nullptr, time);
                    }
                } else if (kind == u"midiRegisterPort") {
                    midi->registerPort(midiPort(field(op, u"port")));
                } else if (kind == u"midiDisconnectPort") {
                    midi->disconnectPort(field(op, u"id").as_string());
                } else if (kind == u"midiInventory") {
                    std::vector<nm::MidiPort> ports;
                    // Held in a variable: a range-for over a temporary's member dangles.
                    const nm::Value list = field(op, u"ports");
                    for (const nm::Value& port : list.as_array()) ports.push_back(midiPort(port));
                    midi->setPortInventory(ports);
                } else if (kind == u"attachAudio") {
                    audio = std::make_shared<nm::AudioState>(!nm::jsv::strict_equals(field(op, u"registry"), nm::Value(false)));
                    pipeline->setAudioState(audio);
                } else if (kind == u"detachAudio") {
                    pipeline->setAudioState(nullptr);
                } else if (kind == u"audioBands") {
                    const double low = number(field(op, u"low"), u"low");
                    const double mid = number(field(op, u"mid"), u"mid");
                    const double high = number(field(op, u"high"), u"high");
                    audio->setBands(low, mid, high);
                } else if (kind == u"audioRaw") {
                    audio->setRaw(number(field(op, u"value"), u"value"));
                } else if (kind == u"audioRawUnavailable") {
                    audio->setRawUnavailable();
                } else if (kind == u"audioSpectrum") {
                    const std::vector<uint8_t> data = bytes(field(op, u"bytes"));
                    audio->setSpectrum(data.data(), static_cast<int>(data.size()));
                } else if (kind == u"audioWaveform") {
                    const std::vector<uint8_t> data = bytes(field(op, u"bytes"));
                    audio->setWaveform(data.data(), static_cast<int>(data.size()));
                } else if (kind == u"audioRegisterDevice") {
                    audio->registerDevice(audioDevice(field(op, u"device")));
                } else if (kind == u"audioChannelValues") {
                    const nm::Value values = field(op, u"values");
                    const nm::JsString id = field(op, u"id").as_string();
                    const int channel = static_cast<int>(number(field(op, u"channel"), u"channel"));
                    audio->setChannelValues(id, channel, optionalNumber(field(values, u"low")), optionalNumber(field(values, u"mid")),
                                            optionalNumber(field(values, u"high")), optionalNumber(field(values, u"vol")),
                                            optionalNumber(field(values, u"raw")));
                } else if (kind == u"audioRegisterDefault") {
                    audio->registerDefaultChannels(static_cast<int>(number(field(op, u"count"), u"count")));
                } else if (kind == u"audioDefaultBands") {
                    const int channel = static_cast<int>(number(field(op, u"channel"), u"channel"));
                    const double low = number(field(op, u"low"), u"low");
                    const double mid = number(field(op, u"mid"), u"mid");
                    const double high = number(field(op, u"high"), u"high");
                    if (nm::AudioState* state = audio->defaultChannelState(channel)) state->setBands(low, mid, high);
                } else if (kind == u"audioInventory") {
                    std::vector<nm::AudioDevice> devices;
                    const nm::Value list = field(op, u"devices");
                    for (const nm::Value& entry : list.as_array()) devices.push_back(audioDevice(entry));
                    audio->setDeviceInventory(devices);
                } else if (kind == u"midiReset") {
                    midi->reset();
                } else if (kind == u"audioDisconnectDefault") {
                    audio->disconnectDefaultInput();
                } else if (kind == u"audioDeviceRawUnavailable") {
                    audio->setDeviceRawUnavailable(field(op, u"id").as_string());
                } else if (kind == u"audioDisconnectDevice") {
                    audio->disconnectDevice(field(op, u"id").as_string());
                } else if (kind == u"getAudioInputRequirements") {
                    record.set(u"result", pipeline->getAudioInputRequirements());
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
                    result.set(u"globalUniforms", globalsFor(state.globalUniforms, kind));
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
        record.set(u"state", pipeline ? state(*pipeline, *backend, kindOf) : nm::Value::null());
        if (!error.is_undefined()) record.set(u"error", error);
        std::cout << nm::json::stringify(nm::Value(std::move(record))) << '\n';
    }
    return 0;
}

}  // namespace nm_dump
