#include "gpu/backend/pipeline.h"

#include "gpu/backend/automation.h"

#include "core/catalog/catalog.h"
#include "core/edit/program_state/palette.h"
#include "core/edit/jsv.h"
#include "core/value/js_math.h"
#include "core/value/js_number.h"
#include "core/value/json.h"
#include "host/audio_state.h"
#include "host/midi_state.h"

#include <algorithm>
#include <chrono>
#include <functional>
#include <cmath>
#include <unordered_map>
#include <utility>

namespace libnoisemaker::backend {

namespace {

bool startsWith(const JsString& text, const char16_t* prefix) { return text.rfind(prefix, 0) == 0; }
bool nullish(const Value& value) { return value.is_undefined() || value.is_null(); }
// a ?? b
Value orElse(const Value& value, const Value& fallback) { return nullish(value) ? fallback : value; }
// typeof value === 'object'
bool isObjectType(const Value& value) {
    return value.is_object() || value.is_array() || value.is_null() || value.is_float32_array();
}
template <typename T>
bool contains(const std::vector<T>& items, const T& item) {
    return std::find(items.begin(), items.end(), item) != items.end();
}
void addUnique(std::vector<JsString>& set, const JsString& item) {
    if (!contains(set, item)) set.push_back(item);
}
const JsString* lookup(const std::vector<std::pair<JsString, JsString>>& map, const JsString& key) {
    for (const auto& [k, v] : map) if (k == key) return &v;
    return nullptr;
}
Value strings(std::initializer_list<const char16_t*> items) {
    Array out;
    for (const char16_t* item : items) out.emplace_back(item);
    return Value(std::move(out));
}
Value textureUsage() { return strings({u"render", u"sample", u"copySrc", u"copyDst", u"storage"}); }

// /^mesh\d+_(positions|normals|uvs)$/
bool isMeshTextureName(const JsString& name) {
    if (!startsWith(name, u"mesh")) return false;
    size_t i = 4;
    while (i < name.size() && name[i] >= u'0' && name[i] <= u'9') ++i;
    if (i == 4 || i >= name.size() || name[i] != u'_') return false;
    const JsString rest = name.substr(i + 1);
    return rest == u"positions" || rest == u"normals" || rest == u"uvs";
}
// /^vol[0-7]$/
bool isVolumeSurface(const JsString& name) {
    return name.size() == 4 && startsWith(name, u"vol") && name[3] >= u'0' && name[3] <= u'7';
}

// preflight.js mrtFormatBytes
double mrtFormatBytes(const Value& format) {
    if (!format.is_string()) return 8;
    const JsString& f = format.as_string();
    if (f == u"rgba32f" || f == u"rgba32float") return 16;
    if (f == u"rgba8" || f == u"rgba8unorm" || f == u"r32f" || f == u"r32float") return 4;
    if (f == u"r16f" || f == u"r16float") return 2;
    if (f == u"r8" || f == u"r8unorm") return 1;
    return 8;
}

// The effects the runtime registry holds, by every name registerEffect gives
// them (func, namespace.func, namespace/effect, namespace.effect, in
// namespace and effect order, a later effect replacing an earlier one), with
// the lifecycle hooks each defines (the catalog's jsHooks).
struct EffectHooks {
    JsString id;  // namespace/effect
    std::vector<JsString> hooks;
    std::vector<JsString> tags;
};
const std::unordered_map<JsString, EffectHooks>& effectHooks() {
    static const std::unordered_map<JsString, EffectHooks> index = [] {
        std::vector<const CatalogFile*> files;
        for (const auto& file : catalog_files()) {
            if (file.path.starts_with("effects/") && file.path.ends_with(".json")) files.push_back(&file);
        }
        std::sort(files.begin(), files.end(), [](const auto* a, const auto* b) { return a->path < b->path; });
        std::unordered_map<JsString, EffectHooks> out;
        for (const CatalogFile* file : files) {
            const std::string_view path = file->path.substr(8, file->path.size() - 8 - 5);
            const size_t slash = path.find('/');
            if (slash == std::string_view::npos) continue;
            const JsString ns = utf8_to_js(path.substr(0, slash));
            const JsString name = utf8_to_js(path.substr(slash + 1));
            const Value definition = json::parse(file->bytes);
            std::vector<JsString> hooks;
            for (const Value& hook : jsv::values(jsv::member(definition, u"jsHooks"))) {
                if (hook.is_string()) hooks.push_back(hook.as_string());
            }
            std::vector<JsString> tags;
            for (const Value& tag : jsv::values(jsv::member(definition, u"tags"))) {
                if (tag.is_string()) tags.push_back(tag.as_string());
            }
            const JsString func = jsv::to_string(jsv::member(definition, u"func"));
            for (const JsString& key : {func, ns + u"." + func, ns + u"/" + name, ns + u"." + name}) {
                out[key] = EffectHooks{ns + u"/" + name, hooks, tags};
            }
        }
        return out;
    }();
    return index;
}
const EffectHooks* getEffect(const Value& key) {
    if (!key.is_string()) return nullptr;
    const auto& index = effectHooks();
    const auto it = index.find(key.as_string());
    return it == index.end() ? nullptr : &it->second;
}
bool hasLifecycleHook(const std::vector<JsString>& hooks, const char16_t* hook) { return contains(hooks, JsString(hook)); }

}  // namespace

Pipeline::Pipeline(Value graph, WebGPUBackend& backend, Options options)
    : graph_(std::move(graph)), backend_(backend), texturePooling_(options.texturePooling) {
    wallClock_ = [] {
        const auto now = std::chrono::system_clock::now().time_since_epoch();
        return static_cast<double>(std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
    };
    Object proxy;
    proxy.set(u"uniforms", Value(Object{}));
    oscillatorPassProxy_ = Value(std::move(proxy));
}

// this.graph.<key>: only an object graph has members the pipeline reads.
const Value* Pipeline::graphMember(const char16_t* key) const {
    return graph_.is_object() ? graph_.as_object().find(key) : nullptr;
}

const Array* Pipeline::passes() const {
    const Value* passes = graphMember(u"passes");
    if (!passes || !jsv::truthy(*passes)) return nullptr;
    if (!passes->is_array()) throw JsError(u"Error", u"graph.passes other than an array is outside the port");
    return &passes->as_array();
}
Array* Pipeline::passes() { return const_cast<Array*>(std::as_const(*this).passes()); }

const Object* Pipeline::textures() const {
    const Value* textures = graphMember(u"textures");
    if (!textures || !jsv::truthy(*textures)) return nullptr;
    if (!textures->is_object()) throw JsError(u"Error", u"graph.textures other than an object is outside the port");
    return &textures->as_object();
}
Object* Pipeline::textures() { return const_cast<Object*>(std::as_const(*this).textures()); }

Surface* Pipeline::surface(const JsString& name) {
    const auto it = surfaceIndex_.find(name);
    return it == surfaceIndex_.end() ? nullptr : &surfaces_[it->second].second;
}

void Pipeline::init(double width, double height) {
    backend_.init();
    compilePrograms();
    resize(width, height);
}

void Pipeline::compilePrograms() {
    const Array* list = passes();
    if (!list) return;
    isCompiling_ = true;
    struct Reset {
        bool& flag;
        ~Reset() { flag = false; }
    } reset{isCompiling_};
    std::vector<Value> compiled;
    const Value* programsMember = graphMember(u"programs");
    const Value programs = programsMember ? *programsMember : Value();
    for (const Value& pass : *list) {
        const Value program = jsv::get(pass, u"program");
        if (std::any_of(compiled.begin(), compiled.end(), [&](const Value& c) { return jsv::same_value_zero(c, program); })) {
            continue;
        }
        // resolveProgramSpec(pass): programs[pass.program] when truthy.
        const Value spec = jsv::truthy(programs) && isObjectType(programs)
            ? jsv::member(programs, jsv::to_property_key(jsv::get(pass, u"program"))) : Value();
        if (!jsv::truthy(spec)) {
            Object thrown;
            thrown.set(u"code", Value(u"ERR_PROGRAM_SPEC_MISSING"));
            thrown.set(u"program", jsv::get(pass, u"program"));
            thrown.set(u"pass", jsv::get(pass, u"id"));
            throw ThrownValue(Value(std::move(thrown)));
        }
        if (!program.is_string()) throw JsError(u"Error", u"a program id other than a string is outside the port");
        backend_.compileProgram(program.as_string(), spec);
        compiled.push_back(program);
    }
}

void Pipeline::resize(double width, double height) {
    width_ = width;
    height_ = height;
    createSurfaces();
    refreshMipTargets();
    const Value defaultUniforms = collectDefaultUniforms();
    recreateTextures(defaultUniforms);
    initAsyncEffects();
    initLifecycleEffects();
}

void Pipeline::recompile(Value graph) {
    graph_ = std::move(graph);
    createSurfaces();
    const Value defaultUniforms = collectDefaultUniforms();
    recreateTextures(defaultUniforms);
    initAsyncEffects();
    initLifecycleEffects();
}

// The base no-op runs for every effect but the three asyncInit overlays,
// which are host inputs (see the header).
void Pipeline::initAsyncEffects() {}

void Pipeline::initLifecycleEffects() {
    const Array* list = passes();
    if (!list) return;
    lifecycleEffects_.clear();
    std::vector<Value> seen;
    for (const Value& pass : *list) {
        const Value effectKey = jsv::get(pass, u"effectKey");
        if (!jsv::truthy(effectKey) ||
            std::any_of(seen.begin(), seen.end(), [&](const Value& s) { return jsv::same_value_zero(s, effectKey); })) {
            continue;
        }
        seen.push_back(effectKey);
        const EffectHooks* effect = getEffect(effectKey);
        if (!effect) continue;
        const std::vector<JsString>* hooks = &effect->hooks;
        if (!hasLifecycleHook(*hooks, u"onInit") && !hasLifecycleHook(*hooks, u"onUpdate") &&
            !hasLifecycleHook(*hooks, u"onDestroy")) {
            continue;
        }
        const JsString& key = effectKey.as_string();
        if (!contains(lifecycleEffects_, key)) lifecycleEffects_.push_back(key);
        // synth/media is the catalog's only effect with these hooks.
        if (effect->id != u"synth/media") throw JsError(u"Error", u"the lifecycle hooks of " + effect->id + u" are outside the port");
        if (hasLifecycleHook(*hooks, u"onInit") && !initLifecycleDone_.contains(key)) {
            initLifecycleDone_.insert(key);
            mediaImageWidth_ = 1;
            mediaImageHeight_ = 1;
        }
    }
}

Value Pipeline::collectDefaultUniforms() const {
    Object uniforms;
    if (const Array* list = passes()) {
        for (const Value& pass : *list) {
            const Value passUniforms = jsv::get(pass, u"uniforms");
            if (jsv::truthy(passUniforms)) jsv::spread_into(uniforms, passUniforms);
        }
    }
    return Value(std::move(uniforms));
}

// The pipeline's parseGlobalName: the name after "global_", where an empty
// name is no name, as at every caller.
std::optional<JsString> Pipeline::parseGlobalName(const Value& texId) {
    if (!texId.is_string() || !startsWith(texId.as_string(), u"global_") || texId.as_string().size() == 7) {
        return std::nullopt;
    }
    return texId.as_string().substr(7);
}

bool Pipeline::isVolumeSizeUniform(const JsString& name) {
    return name == u"volumeSize" || startsWith(name, u"volumeSize_chain_") || startsWith(name, u"volumeSize_node_");
}

Value Pipeline::clampVolumeSize(const Value& value) {
    const Value maxTextureSize = jsv::member(backend_.capabilities(), u"maxTextureSize");
    if (!value.is_number() || !jsv::truthy(maxTextureSize)) return value;
    const double v = value.as_number();
    const double max = jsv::to_number(maxTextureSize);
    if (v * v <= max) return value;
    double clamped = 16;
    while ((clamped * 2) * (clamped * 2) <= max && clamped * 2 < v) clamped *= 2;
    return Value(clamped);
}

void Pipeline::clampGraphVolumeSizes() {
    Array* list = passes();
    if (!list) return;
    for (Value& pass : *list) {
        const Value uniforms = jsv::get(pass, u"uniforms");
        if (!jsv::truthy(uniforms)) continue;
        for (const JsString& key : jsv::keys(uniforms)) {
            if (!isVolumeSizeUniform(key)) continue;
            const Value value = jsv::member(uniforms, key);
            if (!value.is_number()) continue;
            const Value clamped = clampVolumeSize(value);
            if (!jsv::strict_equals(clamped, value)) pass.as_object()[u"uniforms"].as_object().set(key, clamped);
        }
    }
}

void Pipeline::applyMrtFormatBudget() {
    const Value budget = jsv::member(backend_.capabilities(), u"maxColorBytesPerSample");
    if (!jsv::truthy(budget) || !passes() || !textures()) return;
    Object& specs = *textures();
    const double limit = jsv::to_number(budget);
    for (const Value& pass : *passes()) {
        const Value outputs = jsv::get(pass, u"outputs");
        if (!jsv::truthy(outputs)) continue;
        const Array texIds = jsv::values(outputs);
        if (texIds.size() <= 1) continue;
        // graph.textures.get(texId): a Map holds only the string ids.
        const auto spec = [&](const Value& texId) -> Value* {
            if (!texId.is_string()) return nullptr;
            return const_cast<Value*>(specs.find(texId.as_string()));
        };
        double total = 0;
        for (const Value& texId : texIds) {
            const Value* found = spec(texId);
            total += mrtFormatBytes(found ? jsv::get(*found, u"format") : Value());
        }
        if (total <= limit) continue;
        for (size_t i = texIds.size(); i-- > 0 && total > limit;) {
            Value* found = spec(texIds[i]);
            if (!found) continue;
            const Value format = jsv::get(*found, u"format");
            if (jsv::strict_equals(format, Value(u"rgba32f")) || jsv::strict_equals(format, Value(u"rgba32float"))) {
                found->as_object().set(u"format", Value(u"rgba16f"));
                total -= 8;
            }
        }
    }
}

void Pipeline::recreateTexturePreserving(const std::optional<JsString>& texId, const Value& spec) {
    if (!texId) throw JsError(u"Error", u"an undefined surface texture id is outside the port");
    const std::shared_ptr<TextureRecord> existing = backend_.textureRecord(*texId);
    std::optional<JsString> preserveId;
    if (existing && !existing->is3D && existing->persistent) {
        preserveId = *texId + u"__preserve_tmp";
        Object temporary;
        temporary.set(u"width", existing->width);
        temporary.set(u"height", existing->height);
        temporary.set(u"format", existing->format);
        temporary.set(u"usage", strings({u"render", u"sample", u"copySrc", u"copyDst"}));
        backend_.createTexture(*preserveId, Value(std::move(temporary)));
        backend_.copyTexture(*texId, *preserveId);
    }
    backend_.destroyTexture(*texId);
    backend_.createTexture(*texId, spec);
    if (preserveId) {
        backend_.copyTexture(*preserveId, *texId);
        backend_.destroyTexture(*preserveId);
    }
}

void Pipeline::refreshMipTargets() {
    mipTargets_.clear();
    const Object* specs = textures();
    if (!specs) return;
    for (const JsString& texId : specs->keys()) {
        if (!jsv::truthy(jsv::get(*specs->find(texId), u"mipmaps"))) continue;
        if (startsWith(texId, u"global_")) {
            const std::optional<JsString> name = parseGlobalName(Value(texId));
            if (name) {
                if (const Surface* found = surface(*name)) {
                    mipTargets_.push_back(found->read);
                    mipTargets_.push_back(found->write);
                }
            }
        } else {
            mipTargets_.push_back(texId);
        }
    }
}

void Pipeline::createSurfaces() {
    clampGraphVolumeSizes();
    applyMrtFormatBudget();

    std::vector<JsString> surfaceNames = {u"o0", u"o1", u"o2", u"o3", u"o4", u"o5", u"o6", u"o7",
                                          u"geo0", u"geo1", u"geo2", u"geo3", u"geo4", u"geo5", u"geo6", u"geo7",
                                          u"vol0", u"vol1", u"vol2", u"vol3", u"vol4", u"vol5", u"vol6", u"vol7"};
    std::vector<JsString> readBeforeWriteSurfaces;
    std::vector<JsString> writtenSurfaces;
    const Value defaultUniforms = collectDefaultUniforms();

    const Array* list = passes();
    if (list) {
        for (const Value& pass : *list) {
            const Value inputs = jsv::get(pass, u"inputs");
            if (jsv::truthy(inputs)) {
                for (const Value& texId : jsv::values(inputs)) {
                    const std::optional<JsString> name = parseGlobalName(texId);
                    if (name && !isMeshTextureName(*name)) {
                        addUnique(surfaceNames, *name);
                        if (!contains(writtenSurfaces, *name)) addUnique(readBeforeWriteSurfaces, *name);
                    }
                }
            }
            const Value outputs = jsv::get(pass, u"outputs");
            const Value written = jsv::truthy(outputs) ? outputs : jsv::get(pass, u"storageTextures");
            if (jsv::truthy(written)) {
                for (const Value& texId : jsv::values(written)) {
                    const std::optional<JsString> name = parseGlobalName(texId);
                    if (name && !isMeshTextureName(*name)) {
                        addUnique(surfaceNames, *name);
                        addUnique(writtenSurfaces, *name);
                    }
                }
            }
        }
    }
    // A surface first written and then read is frame-local scratch, not
    // cross-frame feedback.
    feedbackSurfaces_.emplace();
    for (const JsString& name : readBeforeWriteSurfaces) {
        if (contains(writtenSurfaces, name)) feedbackSurfaces_->push_back(name);
    }

    needsMidiNoteGrid_ = false;
    if (list) {
        for (const Value& pass : *list) {
            const Value inputs = jsv::get(pass, u"inputs");
            if (jsv::truthy(inputs)) {
                for (const Value& texId : jsv::values(inputs)) {
                    if (jsv::strict_equals(texId, Value(u"midiNoteGrid"))) {
                        needsMidiNoteGrid_ = true;
                        break;
                    }
                }
            }
            if (*needsMidiNoteGrid_) break;
        }
    }

    const Object* specs = textures();
    for (const JsString& name : surfaceNames) {
        const bool isVolume = isVolumeSurface(name);
        double surfaceWidth = isVolume ? 64 : width_;
        double surfaceHeight = isVolume ? 4096 : height_;
        Value surfaceFormat(u"rgba16f");

        const Value* texSpec = specs ? specs->find(u"global_" + name) : nullptr;
        if (texSpec && jsv::truthy(*texSpec)) {
            surfaceWidth = resolveDimension(jsv::get(*texSpec, u"width"), width_, defaultUniforms);
            surfaceHeight = resolveDimension(jsv::get(*texSpec, u"height"), height_, defaultUniforms);
            const Value format = jsv::get(*texSpec, u"format");
            if (jsv::truthy(format)) surfaceFormat = format;
        }
        const bool mipmaps = texSpec && jsv::strict_equals(jsv::member(*texSpec, u"mipmaps"), Value(true));
        const bool persistent = texSpec && jsv::strict_equals(jsv::member(*texSpec, u"persistent"), Value(true));
        const auto surfaceSpec = [&] {
            Object spec;
            spec.set(u"width", Value(surfaceWidth));
            spec.set(u"height", Value(surfaceHeight));
            spec.set(u"format", surfaceFormat);
            spec.set(u"usage", textureUsage());
            spec.set(u"mipmaps", Value(mipmaps));
            spec.set(u"persistent", Value(persistent));
            return Value(std::move(spec));
        };

        // Reuse only a complete matching surface (preserves sim state on recompile).
        if (const Surface* old = surface(name)) {
            const TextureRecord* existingRead = old->read ? backend_.texture(*old->read) : nullptr;
            const TextureRecord* existingWrite = old->write ? backend_.texture(*old->write) : nullptr;
            const auto matches = [&](const TextureRecord* tex) {
                return jsv::strict_equals(tex->width, Value(surfaceWidth)) &&
                       jsv::strict_equals(tex->height, Value(surfaceHeight)) &&
                       jsv::strict_equals(tex->format, surfaceFormat);
            };
            if (existingRead && existingWrite && matches(existingRead) && matches(existingWrite)) continue;
            recreateTexturePreserving(u"global_" + name + u"_read", surfaceSpec());
            recreateTexturePreserving(u"global_" + name + u"_write", surfaceSpec());
            continue;
        }

        backend_.createTexture(u"global_" + name + u"_read", surfaceSpec());
        backend_.createTexture(u"global_" + name + u"_write", surfaceSpec());
        Surface created;
        created.read = u"global_" + name + u"_read";
        created.write = u"global_" + name + u"_write";
        surfaceIndex_.emplace(name, surfaces_.size());
        surfaces_.emplace_back(name, std::move(created));
    }

    // Mesh surfaces (mesh0-mesh7): positions, normals and UVs, 256x256 rgba32f.
    for (const JsString name : {u"mesh0", u"mesh1", u"mesh2", u"mesh3", u"mesh4", u"mesh5", u"mesh6", u"mesh7"}) {
        if (surface(name)) continue;
        Surface mesh;
        mesh.mesh = true;
        mesh.positions = u"global_" + name + u"_positions";
        mesh.normals = u"global_" + name + u"_normals";
        mesh.uvs = u"global_" + name + u"_uvs";
        mesh.width = 256;
        mesh.height = 256;
        for (const JsString* id : {&mesh.positions, &mesh.normals, &mesh.uvs}) {
            Object spec;
            spec.set(u"width", Value(256.0));
            spec.set(u"height", Value(256.0));
            spec.set(u"format", Value(u"rgba32f"));
            spec.set(u"usage", textureUsage());
            backend_.createTexture(*id, Value(std::move(spec)));
        }
        surfaceIndex_.emplace(name, surfaces_.size());
        surfaces_.emplace_back(name, std::move(mesh));
    }
}

void Pipeline::recreateTextures(const Value& uniforms) {
    if (!textures()) return;

    // The pooling plan, after applyMrtFormatBudget() so demoted formats count.
    // A plan that throws leaves the previous one in place.
    std::vector<std::pair<JsString, JsString>> nextAliases =
        texturePooling_ ? buildTexturePoolingPlan() : std::vector<std::pair<JsString, JsString>>{};
    const std::vector<std::pair<JsString, JsString>> previousAliases = std::exchange(textureAliases_, std::move(nextAliases));
    releaseRegroupedTextures(previousAliases, textureAliases_);

    // Each spec is read where the loop reaches it: the specs are the graph's
    // own objects, which nothing in the loop changes.
    for (const JsString& texId : textures()->keys()) {
        const Value spec = *textures()->find(texId);
        const bool isGlobalSurface = startsWith(texId, u"global");
        const Value specWidth = jsv::get(spec, u"width");
        const Value specHeight = jsv::get(spec, u"height");
        // isDynamicDimension: anything but a number.
        if (isGlobalSurface && specWidth.is_number() && specHeight.is_number()) continue;

        const double width = resolveDimension(specWidth, width_, uniforms);
        const double height = resolveDimension(specHeight, height_, uniforms);

        if (isGlobalSurface) {
            // The complete surface name: geo0 contains "o0" but is not o0.
            const std::optional<JsString> name = parseGlobalName(Value(texId));
            const Surface* found = name ? surface(*name) : nullptr;
            if (!found) continue;
            const std::optional<JsString> readTexId = found->read;
            const std::optional<JsString> writeTexId = found->write;

            const Value specFormat = jsv::get(spec, u"format");
            const Value expectedFormat = jsv::truthy(specFormat) ? specFormat : Value(u"rgba16f");
            const TextureRecord* existingRead = readTexId ? backend_.texture(*readTexId) : nullptr;
            const TextureRecord* existingWrite = writeTexId ? backend_.texture(*writeTexId) : nullptr;
            const auto matches = [&](const TextureRecord* tex) {
                return jsv::strict_equals(tex->width, Value(width)) && jsv::strict_equals(tex->height, Value(height)) &&
                       jsv::strict_equals(tex->format, expectedFormat);
            };
            if (existingRead && existingWrite && matches(existingRead) && matches(existingWrite)) continue;

            const auto surfaceSpec = [&] {
                Object out;
                out.set(u"width", Value(width));
                out.set(u"height", Value(height));
                out.set(u"format", expectedFormat);
                out.set(u"usage", textureUsage());
                out.set(u"mipmaps", Value(jsv::strict_equals(jsv::get(spec, u"mipmaps"), Value(true))));
                out.set(u"persistent", Value(jsv::strict_equals(jsv::get(spec, u"persistent"), Value(true))));
                return Value(std::move(out));
            };
            recreateTexturePreserving(readTexId, surfaceSpec());
            recreateTexturePreserving(writeTexId, surfaceSpec());
        } else {
            // A pooled secondary member shares its group's storage texture,
            // created under the primary id and aliased below.
            const JsString* storageId = lookup(textureAliases_, texId);
            if (storageId && *storageId != texId) continue;

            const bool is3D = jsv::truthy(jsv::get(spec, u"is3D"));
            if (const TextureRecord* existing = backend_.texture(texId)) {
                if (jsv::strict_equals(existing->width, Value(width)) && jsv::strict_equals(existing->height, Value(height)) &&
                    jsv::strict_equals(existing->format, jsv::get(spec, u"format"))) {
                    if (!is3D) continue;
                    // existing.depth is read before the spec's depth resolves.
                    const Value existingDepth = existing->depth;
                    if (jsv::strict_equals(existingDepth, Value(resolveDimension(jsv::get(spec, u"depth"), width, uniforms)))) {
                        continue;
                    }
                }
            }

            Object resized;
            jsv::spread_into(resized, spec);
            if (is3D) {
                const double depth = resolveDimension(jsv::get(spec, u"depth"), width, uniforms);
                backend_.destroyTexture(texId);
                resized.set(u"width", Value(width));
                resized.set(u"height", Value(height));
                resized.set(u"depth", Value(depth));
                backend_.createTexture3D(texId, Value(std::move(resized)));
            } else {
                resized.set(u"width", Value(width));
                resized.set(u"height", Value(height));
                recreateTexturePreserving(texId, Value(std::move(resized)));
            }
        }
    }
    applyTextureAliases();
    refreshMipTargets();
}

void Pipeline::updateParameterTextures(const Value& uniforms) {
    recreateTextures(uniforms);
    refreshMipTargets();
}

std::vector<std::pair<JsString, JsString>> Pipeline::buildTexturePoolingPlan() const {
    std::vector<std::pair<JsString, JsString>> aliases;
    const Value* allocationsMember = graphMember(u"allocations");
    const Object* specs = textures();
    if (!allocationsMember || !allocationsMember->is_object() || !specs) return aliases;
    const Value& allocations = *allocationsMember;

    // First touch of each texture in pass order. Only string ids can match an
    // allocation key.
    std::vector<std::pair<JsString, bool>> firstTouchIsWrite;
    std::vector<JsString> selfSampled;
    std::vector<JsString> partiallyWritten;
    const auto touch = [&](const JsString& texId, bool write) {
        for (const auto& [id, isWrite] : firstTouchIsWrite) if (id == texId) return;
        firstTouchIsWrite.emplace_back(texId, write);
    };
    if (const Array* list = passes()) {
        for (const Value& pass : *list) {
            const Value passInputs = jsv::get(pass, u"inputs");
            const Value passOutputs = jsv::get(pass, u"outputs");
            const Array inputs = jsv::values(jsv::truthy(passInputs) ? passInputs : Value(Object{}));
            const Array outputs = jsv::values(jsv::truthy(passOutputs) ? passOutputs : Value(Object{}));
            // A pass that leaves its target's previous contents observable
            // (scatter draw modes, blending, a viewport that does not clear).
            const bool partial = jsv::truthy(jsv::get(pass, u"drawMode")) || jsv::truthy(jsv::get(pass, u"blend")) ||
                (jsv::truthy(jsv::get(pass, u"viewport")) && !jsv::truthy(jsv::get(pass, u"clear")));
            if (partial) {
                for (const Value& texId : outputs) if (texId.is_string()) addUnique(partiallyWritten, texId.as_string());
            }
            for (const Value& texId : outputs) {
                if (!texId.is_string()) continue;
                touch(texId.as_string(), true);
                if (std::any_of(inputs.begin(), inputs.end(), [&](const Value& in) { return jsv::same_value_zero(in, texId); })) {
                    addUnique(selfSampled, texId.as_string());
                }
            }
            // The inputs' first touches, each input once, in first-seen order.
            std::vector<Value> unique;
            for (const Value& texId : inputs) {
                if (std::none_of(unique.begin(), unique.end(), [&](const Value& u) { return jsv::same_value_zero(u, texId); })) {
                    unique.push_back(texId);
                }
            }
            for (const Value& texId : unique) if (texId.is_string()) touch(texId.as_string(), false);
        }
    }

    // physicalId -> members, in first-seen order (Map keys compare by SameValueZero).
    std::vector<std::pair<Value, std::vector<JsString>>> groups;
    const Object& allocationMap = allocations.as_object();
    for (const JsString& texId : allocationMap.keys()) {
        const Value physicalId = *allocationMap.find(texId);
        if (!jsv::truthy(physicalId) || !specs->find(texId)) continue;
        // Global surfaces are double-buffered and never pooled.
        if (startsWith(texId, u"global")) continue;
        const auto first = std::find_if(firstTouchIsWrite.begin(), firstTouchIsWrite.end(),
                                        [&](const auto& entry) { return entry.first == texId; });
        if (first != firstTouchIsWrite.end() && !first->second) continue;
        if (contains(selfSampled, texId) || contains(partiallyWritten, texId)) continue;
        auto group = std::find_if(groups.begin(), groups.end(),
                                  [&](const auto& g) { return jsv::same_value_zero(g.first, physicalId); });
        if (group == groups.end()) {
            groups.emplace_back(physicalId, std::vector<JsString>{});
            group = std::prev(groups.end());
        }
        group->second.push_back(texId);
    }

    for (const auto& [physicalId, members] : groups) {
        (void)physicalId;
        if (members.size() < 2) continue;
        bool poolable = true;
        std::vector<JsString> signatures;
        for (const JsString& id : members) {
            const Value spec = *specs->find(id);
            if (!jsv::truthy(spec) || jsv::strict_equals(jsv::get(spec, u"persistent"), Value(true)) ||
                jsv::strict_equals(jsv::get(spec, u"mipmaps"), Value(true)) ||
                jsv::strict_equals(jsv::get(spec, u"is3D"), Value(true))) {
                poolable = false;
                break;
            }
        }
        if (!poolable) continue;
        for (const JsString& id : members) {
            const Value spec = *specs->find(id);
            const Value signature(Array{jsv::get(spec, u"width"), jsv::get(spec, u"height"), jsv::get(spec, u"format")});
            signatures.push_back(utf8_to_js(json::stringify(signature)));
        }
        if (std::any_of(signatures.begin(), signatures.end(), [&](const JsString& s) { return s != signatures[0]; })) continue;
        for (const JsString& member : members) {
            bool replaced = false;
            for (auto& [id, storage] : aliases) {
                if (id == member) {
                    storage = members[0];
                    replaced = true;
                }
            }
            if (!replaced) aliases.emplace_back(member, members[0]);
        }
    }
    return aliases;
}

void Pipeline::releaseRegroupedTextures(const std::vector<std::pair<JsString, JsString>>& previousAliases,
                                        const std::vector<std::pair<JsString, JsString>>& nextAliases) {
    if (previousAliases.empty()) return;
    std::vector<std::pair<JsString, std::vector<JsString>>> groups;
    for (const auto& [member, storage] : previousAliases) {
        auto group = std::find_if(groups.begin(), groups.end(), [&](const auto& g) { return g.first == storage; });
        if (group == groups.end()) {
            groups.emplace_back(storage, std::vector<JsString>{});
            group = std::prev(groups.end());
        }
        group->second.push_back(member);
    }
    for (const auto& [storage, members] : groups) {
        const bool unchanged = std::all_of(members.begin(), members.end(), [&](const JsString& m) {
            const JsString* next = lookup(nextAliases, m);
            return next && *next == storage;
        });
        if (unchanged) continue;
        for (const JsString& member : members) {
            if (backend_.texture(member)) backend_.destroyTexture(member);
        }
    }
}

void Pipeline::applyTextureAliases() {
    for (const auto& [member, storage] : textureAliases_) {
        if (member == storage) continue;
        const std::shared_ptr<TextureRecord> record = backend_.textureRecord(storage);
        if (!record) continue;
        const std::shared_ptr<TextureRecord> existing = backend_.textureRecord(member);
        if (existing && existing != record) backend_.destroyTexture(member);
        backend_.setTexture(member, record);
    }
}

Value Pipeline::getResourcePlan() const {
    Object plan;
    plan.set(u"pooling", Value(texturePooling_));
    const Object* specs = textures();
    if (!specs) {
        plan.set(u"allocations", Value(Object{}));
        plan.set(u"sharedTextures", Value(Array{}));
        plan.set(u"textures", Value(Array{}));
        return Value(std::move(plan));
    }
    const Value* allocations = graphMember(u"allocations");
    plan.set(u"allocations", allocations && allocations->is_object() ? *allocations : Value(Object{}));

    // Non-global graph textures grouped by the backend record they resolve to.
    std::vector<std::pair<std::shared_ptr<TextureRecord>, std::vector<JsString>>> records;
    for (const JsString& texId : specs->keys()) {
        if (startsWith(texId, u"global")) continue;
        const std::shared_ptr<TextureRecord> record = backend_.textureRecord(texId);
        if (!record) continue;
        auto group = std::find_if(records.begin(), records.end(), [&](const auto& r) { return r.first == record; });
        if (group == records.end()) {
            records.emplace_back(record, std::vector<JsString>{});
            group = std::prev(records.end());
        }
        group->second.push_back(texId);
    }
    Array textureRecords;
    Array sharedTextures;
    for (const auto& [record, members] : records) {
        // this.backend.textures.get(id), where id is the group's first member.
        const TextureRecord* first = backend_.texture(members[0]);
        Object entry;
        entry.set(u"id", Value(members[0]));
        entry.set(u"width", first ? first->width : Value());
        entry.set(u"height", first ? first->height : Value());
        entry.set(u"format", first ? first->format : Value());
        Array virtualTextures;
        for (const JsString& member : members) virtualTextures.emplace_back(member);
        entry.set(u"virtualTextures", Value(virtualTextures));
        textureRecords.emplace_back(std::move(entry));
        if (members.size() > 1) sharedTextures.emplace_back(std::move(virtualTextures));
    }
    plan.set(u"sharedTextures", Value(std::move(sharedTextures)));
    plan.set(u"textures", Value(std::move(textureRecords)));
    return Value(std::move(plan));
}

double Pipeline::resolveDimension(const Value& spec, double screenSize, const Value& uniforms) {
    if (spec.is_number()) return js::max(1, std::floor(spec.as_number()));

    // 'input' and 'resolution' are recognized keywords whose resolution is the
    // screen dimension; they add no diagnostic.
    if (spec.is_string()) {
        const JsString& text = spec.as_string();
        if (text == u"screen" || text == u"auto" || text == u"input" || text == u"resolution") return screenSize;
        if (!text.empty() && text.back() == u'%') {
            const double percent = js::parseFloat(text);
            return js::max(1, std::floor(screenSize * percent / 100));
        }
    }

    if (isObjectType(spec)) {
        // { param, multiply, power, paramDefault, default }
        if (!jsv::get(spec, u"param").is_undefined()) {
            const Value param = jsv::get(spec, u"param");
            const Value multiply = jsv::get(spec, u"multiply");
            const Value power = jsv::get(spec, u"power");
            const Value fallback = jsv::get(spec, u"default");
            const bool hasTransform = !power.is_undefined() || !multiply.is_undefined();
            const Value paramDefault = orElse(jsv::get(spec, u"paramDefault"), Value(64.0));

            Value value = orElse(jsv::get_v(uniforms, param), paramDefault);
            const auto transform = [&] {
                if (!multiply.is_undefined()) {
                    const double base = jsv::to_number(value);
                    value = Value(base * jsv::to_number(multiply));
                }
                if (!power.is_undefined()) {
                    const double base = jsv::to_number(value);
                    value = Value(js::pow(base, jsv::to_number(power)));
                }
            };
            transform();
            // A transformed default stands in when the param is not set.
            if (hasTransform && jsv::get_v(uniforms, param).is_undefined() && !fallback.is_undefined()) value = fallback;

            // A nonnumeric authored value (a Func wrapper) falls back to the
            // spec's numeric default.
            if (!std::isfinite(jsv::to_number(value))) {
                if (!fallback.is_undefined()) {
                    value = fallback;
                } else {
                    value = paramDefault;
                    transform();
                }
            }
            return js::max(1, std::floor(jsv::to_number(value)));
        }

        // { screenDivide }: screenSize / uniforms[param], for per-effect zoom.
        if (!jsv::get(spec, u"screenDivide").is_undefined()) {
            const Value fallback = jsv::get(spec, u"default");
            Value divisor = orElse(orElse(jsv::get_v(uniforms, jsv::get(spec, u"screenDivide")), fallback), Value(1.0));
            if (!std::isfinite(jsv::to_number(divisor))) divisor = orElse(fallback, Value(1.0));
            return js::max(1, js::round(screenSize / jsv::to_number(divisor)));
        }

        // { scale, clamp: { min, max } }
        if (!jsv::get(spec, u"scale").is_undefined()) {
            double computed = std::floor(screenSize * jsv::to_number(jsv::get(spec, u"scale")));
            const Value clamp = jsv::get(spec, u"clamp");
            if (jsv::truthy(clamp)) {
                if (!jsv::get(clamp, u"min").is_undefined()) computed = js::max(jsv::to_number(jsv::get(clamp, u"min")), computed);
                if (!jsv::get(clamp, u"max").is_undefined()) computed = js::min(jsv::to_number(jsv::get(clamp, u"max")), computed);
            }
            return js::max(1, computed);
        }
    }

    // An unknown form keeps the screen-size fallback and is reported once as
    // a structured diagnostic; an absent spec is a default.
    if (!nullish(spec)) {
        const JsString key = isObjectType(spec) ? utf8_to_js(json::stringify(spec)) : jsv::to_string(spec);
        if (!warnedDimensionFallbacks_.contains(key)) {
            warnedDimensionFallbacks_.insert(key);
            Object record;
            record.set(u"code", Value(u"ERR_DIMENSION_FALLBACK"));
            record.set(u"backend", Value(WebGPUBackend::getName()));
            record.set(u"stage", Value(u"dimension"));
            record.set(u"spec", Value(key));
            record.set(u"fallback", Value(u"screen"));
            // DiagnosticCollector keeps the last 64.
            diagnostics_.emplace_back(std::move(record));
            if (diagnostics_.size() > 64) diagnostics_.erase(diagnostics_.begin());
        }
    }
    return screenSize;
}

// The frame.

namespace {

// g[key][0] = a; g[key][1] = b on an existing array or object.
void setPair(Object& g, const JsString& key, double a, double b) {
    Value& target = g[key];
    if (target.is_array()) {
        Array& array = target.as_array();
        if (array.size() < 2) array.resize(2);
        array[0] = Value(a);
        array[1] = Value(b);
    } else if (target.is_object()) {
        target.as_object().set(u"0", Value(a));
        target.as_object().set(u"1", Value(b));
    } else {
        throw JsError(u"Error", u"a global uniform " + key + u" other than an array or object is outside the port");
    }
}

// /^(xyz|vel|rgba|points_trail)_node_\d+$/
bool isScopedParticleTexture(const JsString& name) {
    for (const JsString prefix : {u"xyz_node_", u"vel_node_", u"rgba_node_", u"points_trail_node_"}) {
        if (name.size() <= prefix.size() || name.compare(0, prefix.size(), prefix) != 0) continue;
        return std::all_of(name.begin() + static_cast<std::ptrdiff_t>(prefix.size()), name.end(),
                           [](char16_t c) { return c >= u'0' && c <= u'9'; });
    }
    return false;
}
bool endsWith(const JsString& text, const char16_t* suffix) {
    const JsString s(suffix);
    return text.size() >= s.size() && text.compare(text.size() - s.size(), s.size(), s) == 0;
}
// Surfaces that persist their final bindings across frames.
bool isStateSurface(const JsString& name) {
    if (name == u"xyz" || name == u"vel" || name == u"rgba" || name == u"trail") return true;
    if (endsWith(name, u"_xyz") || endsWith(name, u"_vel") || endsWith(name, u"_rgba") || endsWith(name, u"_trail")) return true;
    if (name.find(u"state") != JsString::npos || name.find(u"State") != JsString::npos) return true;
    return isScopedParticleTexture(name);
}

// Structural equality. The reference keys a pass's viewport cache by the
// spec object's identity; a spec changes only when the graph does, which
// brings new passes, so equal values identify the same spec here.
bool sameValue(const Value& a, const Value& b) {
    if (a.kind() != b.kind()) return false;
    switch (a.kind()) {
    case Value::Kind::Undefined:
    case Value::Kind::Null: return true;
    case Value::Kind::Bool: return a.as_bool() == b.as_bool();
    case Value::Kind::Number: return jsv::same_value_zero(a, b);
    case Value::Kind::String:
    case Value::Kind::Function: return a.as_string() == b.as_string();
    case Value::Kind::Float32Array: return a.as_float32_array() == b.as_float32_array();
    case Value::Kind::Array: {
        const Array& x = a.as_array();
        const Array& y = b.as_array();
        if (x.size() != y.size()) return false;
        for (size_t i = 0; i < x.size(); ++i) if (!sameValue(x[i], y[i])) return false;
        return true;
    }
    case Value::Kind::Object: {
        const std::vector<JsString> keys = a.as_object().keys();
        if (keys != b.as_object().keys()) return false;
        for (const JsString& key : keys) if (!sameValue(*a.as_object().find(key), *b.as_object().find(key))) return false;
        return true;
    }
    }
    return false;
}

// Writes { x, y, w, h } into target, in place when it already holds a box.
void setBox(Value& target, double x, double y, double w, double h) {
    if (target.is_object() && target.as_object().keys() == std::vector<JsString>{u"x", u"y", u"w", u"h"}) {
        Object& box = target.as_object();
        box[u"x"] = Value(x);
        box[u"y"] = Value(y);
        box[u"w"] = Value(w);
        box[u"h"] = Value(h);
        return;
    }
    Object box;
    box.set(u"x", Value(x));
    box.set(u"y", Value(y));
    box.set(u"w", Value(w));
    box.set(u"h", Value(h));
    target = Value(std::move(box));
}

}  // namespace

void Pipeline::setTileRegion(std::array<double, 2> offset, std::array<double, 2> fullResolution,
                             std::optional<double> renderScale) {
    tileOffset_ = offset;
    fullResolution_ = fullResolution;
    renderScale_ = renderScale.value_or(1);
}

void Pipeline::clearTileRegion() {
    tileOffset_.reset();
    fullResolution_.reset();
    renderScale_.reset();
}

void Pipeline::updateGlobalUniforms(double time, double deltaTime) {
    ++globalUniformsVersion_;
    Object& g = globalUniforms_.as_object();
    const double aspectValue = width_ / height_;
    g.set(u"time", Value(time));
    g.set(u"deltaTime", Value(deltaTime));
    g.set(u"frame", Value(frameIndex_));
    const auto pair = [](double a, double b) { return Value(Array{Value(a), Value(b)}); };
    if (!jsv::truthy(jsv::member(globalUniforms_, u"resolution"))) g.set(u"resolution", pair(width_, height_));
    else setPair(g, u"resolution", width_, height_);
    // Tile uniforms for tiled export; the defaults change nothing.
    if (!jsv::truthy(jsv::member(globalUniforms_, u"tileOffset"))) g.set(u"tileOffset", pair(0, 0));
    if (!jsv::truthy(jsv::member(globalUniforms_, u"fullResolution"))) g.set(u"fullResolution", pair(width_, height_));
    if (tileOffset_) setPair(g, u"tileOffset", (*tileOffset_)[0], (*tileOffset_)[1]);
    else setPair(g, u"tileOffset", 0, 0);
    if (fullResolution_) {
        setPair(g, u"fullResolution", (*fullResolution_)[0], (*fullResolution_)[1]);
        // A tile's aspect is the full image's.
        const double fullAspect = (*fullResolution_)[0] / (*fullResolution_)[1];
        g.set(u"aspect", Value(fullAspect));
        g.set(u"aspectRatio", Value(fullAspect));
        g.set(u"aspectInv", Value(1.0 / fullAspect));
        g.set(u"invFullResolution", pair(1.0 / (*fullResolution_)[0], 1.0 / (*fullResolution_)[1]));
    } else {
        setPair(g, u"fullResolution", width_, height_);
        g.set(u"aspect", Value(aspectValue));
        g.set(u"aspectRatio", Value(aspectValue));
        g.set(u"aspectInv", Value(1.0 / aspectValue));
        g.set(u"invFullResolution", pair(1.0 / width_, 1.0 / height_));
    }
    // this._renderScale || 1.0
    g.set(u"renderScale", Value(renderScale_ && *renderScale_ != 0 && !std::isnan(*renderScale_) ? *renderScale_ : 1.0));

    // Audio: the 128 waveform and spectrum values.
    if (external_.audio) {
        g.set(u"audioWaveform", Value::float32_array(std::vector<float>(external_.audio->waveform.begin(), external_.audio->waveform.end())));
        g.set(u"audioSpectrum", Value::float32_array(std::vector<float>(external_.audio->spectrum.begin(), external_.audio->spectrum.end())));
    }
    // MIDI: the note grid (128x16 RGBA float) and the clock; without a state
    // the grid, when a pass reads it, is empty.
    if (external_.midi) {
        backend_.uploadDataTexture(u"midiNoteGrid", external_.midi->noteGrid(), Value(128.0), Value(16.0));
        g.set(u"midiClockCount", Value(external_.midi->clockCount()));
    } else if (needsMidiNoteGrid_.value_or(false)) {
        static const std::vector<float> emptyNoteGrid(128 * 16 * 4, 0.0f);
        backend_.uploadDataTexture(u"midiNoteGrid", emptyNoteGrid, Value(128.0), Value(16.0));
    }
    const Value clockCount = jsv::member(globalUniforms_, u"midiClockCount");
    g.set(u"midiClockCount", jsv::truthy(clockCount) ? clockCount : Value(0.0));
}

// Every managed effect's onUpdate, and the uniforms it returns.
void Pipeline::invokeUpdateHooks() {
    hasRuntimeUniforms_ = false;
    runtimeUniforms_.clear();
    for (const JsString& key : lifecycleEffects_) {
        const EffectHooks* effect = getEffect(Value(key));
        if (!effect || !hasLifecycleHook(effect->hooks, u"onUpdate")) continue;
        // synth/media's onUpdate: its media dimensions as imageSize.
        const auto orOne = [](double v) { return v == 0 || std::isnan(v) ? 1.0 : v; };
        Object uniforms;
        uniforms.set(u"imageSize", Value(Array{Value(orOne(mediaImageWidth_)), Value(orOne(mediaImageHeight_))}));
        runtimeUniforms_.emplace_back(key, Value(std::move(uniforms)));
        hasRuntimeUniforms_ = true;
    }
}

// The pass, or the proxy carrying its uniforms with automation values
// evaluated for the frame (resolvePassUniforms with resolveUniformValue).
Value* Pipeline::resolvePassUniforms(Value& pass, double time) {
    const Value uniforms = jsv::get(pass, u"uniforms");
    if (!jsv::truthy(uniforms)) return &pass;

    // Clear the reused object: its keys stay, set to undefined.
    Object& resolved = resolvedUniforms_.as_object();
    for (const JsString& key : resolved.keys()) resolved.set(key, Value());

    bool hasOscillators = false;
    const Value uniformSpecs = jsv::get(pass, u"uniformSpecs");
    for (const auto& [name, value] : jsv::entries(uniforms)) {
        const Value spec = nullish(uniformSpecs) ? Value() : jsv::get(uniformSpecs, name);
        if (isAutomationValue(value)) {
            double result = evaluateAutomation(value, time, spec, external_, wallClock_());
            const bool isInt = !nullish(spec) && jsv::strict_equals(jsv::get(spec, u"type"), Value(u"int"));
            if (isInt) result = js::round(result);
            jsv::set_plain(resolved, name, Value(result));
            hasOscillators = true;
        } else {
            jsv::set_plain(resolved, name, value);
            // resolved !== value holds only for NaN.
            if (value.is_number() && std::isnan(value.as_number())) hasOscillators = true;
        }
    }
    if (!hasOscillators) return &pass;

    Object& proxy = oscillatorPassProxy_.as_object();
    for (const char16_t* key : {u"id", u"program", u"inputs", u"outputs", u"clear", u"blend", u"drawMode", u"count",
                                u"repeat", u"conditions", u"viewport", u"viewportResolved", u"drawBuffers",
                                u"storageTextures", u"samplerTypes", u"entryPoint"}) {
        proxy.set(key, jsv::get(pass, key));
    }
    // The proxy takes the resolved object; the next resolution fills the other.
    std::swap(proxy[u"uniforms"], resolvedUniforms_);
    return &oscillatorPassProxy_;
}

bool Pipeline::shouldSkipPass(const Value& pass) const {
    const Value conditions = jsv::get(pass, u"conditions");
    if (!jsv::truthy(conditions)) return false;
    const Value skipIf = jsv::get(conditions, u"skipIf");
    const Value runIf = jsv::get(conditions, u"runIf");
    // pass.uniforms?.[name] ?? this.globalUniforms[name]
    const auto valueOf = [&](const Value& condition) {
        const Value name = jsv::get(condition, u"uniform");
        const Value uniforms = jsv::get(pass, u"uniforms");
        const Value own = nullish(uniforms) ? Value() : jsv::get_v(uniforms, name);
        return nullish(own) ? jsv::get_v(globalUniforms_, name) : own;
    };
    if (jsv::truthy(skipIf)) {
        for (const Value& condition : jsv::iterate(skipIf, u"skipIf")) {
            // The value is read before equals: C++ leaves argument order unspecified.
            const Value value = valueOf(condition);
            if (jsv::strict_equals(value, jsv::get(condition, u"equals"))) return true;
        }
    }
    if (jsv::truthy(runIf)) {
        for (const Value& condition : jsv::iterate(runIf, u"runIf")) {
            const Value value = valueOf(condition);
            if (!jsv::strict_equals(value, jsv::get(condition, u"equals"))) return true;
        }
    }
    return false;
}

double Pipeline::resolveRepeatCount(const Value& pass) const {
    const Value repeat = jsv::get(pass, u"repeat");
    if (!jsv::truthy(repeat)) return 1;
    if (repeat.is_number()) return js::max(1, std::floor(repeat.as_number()));
    // A uniform name: the global value, else the pass's.
    if (repeat.is_string()) {
        Value value = jsv::member(globalUniforms_, repeat.as_string());
        if (nullish(value)) {
            const Value uniforms = jsv::get(pass, u"uniforms");
            value = nullish(uniforms) ? Value() : jsv::get(uniforms, repeat.as_string());
        }
        if (value.is_number()) return js::max(1, std::floor(value.as_number()));
    }
    return 1;
}

// The authored viewport resolved to { x, y, w, h } on viewportResolved. The
// cache lives on the compiled pass (cacheHolder) as hidden members: the spec
// it was made for, and the box, or null for a numeric spec, which passes
// through unchanged.
void Pipeline::resolvePassViewport(Value& pass, Value& cacheHolder) {
    const Value spec = jsv::get(cacheHolder, u"viewport");
    if (!jsv::truthy(spec) || !isObjectType(spec)) return;
    Object& holder = cacheHolder.as_object();
    const auto setResolved = [&](const Value& resolved) {
        holder.set(u"viewportResolved", resolved);
        if (&pass != &cacheHolder) pass.as_object().set(u"viewportResolved", resolved);
    };

    const Value* source = holder.find(u"_viewportSpecSource");
    if (!source || !sameValue(*source, spec)) {
        holder.define_hidden(u"_viewportSpecSource", spec);
        const bool numericBox = jsv::get(spec, u"x").is_number() && jsv::get(spec, u"y").is_number() &&
                                jsv::get(spec, u"w").is_number() && jsv::get(spec, u"h").is_number();
        if (numericBox) {
            holder.define_hidden(u"_viewportBox", Value::null());
            setResolved(spec);
            return;
        }
        Object box;
        for (const char16_t* key : {u"x", u"y", u"w", u"h"}) box.set(key, Value(0.0));
        holder.define_hidden(u"_viewportBox", Value(std::move(box)));
    }
    if (holder.find(u"_viewportBox")->is_null()) {
        if (&pass != &cacheHolder) pass.as_object().set(u"viewportResolved", spec);
        return;
    }

    // The reference writes each member into its one box as it resolves it.
    // A throw here comes only from a null width or height spec, which throws
    // on every resolution, before the box is ever assigned, so writing the
    // four members after resolving them leaves the same state.
    const Value passUniforms = jsv::get(pass, u"uniforms");
    const Value uniforms = jsv::truthy(passUniforms) ? passUniforms : Value(Object{});
    const double x = resolveDimension(orElse(jsv::get(spec, u"x"), Value(0.0)), width_, uniforms);
    const double y = resolveDimension(orElse(jsv::get(spec, u"y"), Value(0.0)), height_, uniforms);
    const Value widthSpec = orElse(jsv::get(spec, u"w"), jsv::get(spec, u"width"));
    const Value heightSpec = orElse(jsv::get(spec, u"h"), jsv::get(spec, u"height"));
    const double w = widthSpec.is_undefined() ? width_ : resolveDimension(widthSpec, width_, uniforms);
    const double h = heightSpec.is_undefined() ? height_ : resolveDimension(heightSpec, height_, uniforms);
    setBox(holder[u"_viewportBox"], x, y, w, h);
    setBox(holder[u"viewportResolved"], x, y, w, h);
    if (&pass != &cacheHolder) setBox(pass.as_object()[u"viewportResolved"], x, y, w, h);
}

// A repeated pass's frame-local ping-pong bindings, mirrored into the
// cross-frame surface record after each iteration.
void Pipeline::adoptIterationBindings(const Value& pass) {
    const Value outputs = jsv::get(pass, u"outputs");
    const Value written = jsv::truthy(outputs) ? outputs : jsv::get(pass, u"storageTextures");
    if (!jsv::truthy(written)) return;
    for (const Value& outputName : jsv::values(written)) {
        if (!outputName.is_string()) continue;
        const std::optional<JsString> name = parseGlobalName(outputName);
        if (!name) continue;
        Surface* target = surface(*name);
        if (!target) continue;
        if (const auto* read = frameReadTextures_.get(*name); read && *read) target->read = *read;
        if (const auto* write = frameWriteTextures_.get(*name); write && *write) target->write = *write;
    }
}

// Within-frame ping-pong: after a pass writes a surface, later passes read
// what it wrote and write the buffer it read.
void Pipeline::updateFrameSurfaceBindings(const Value& pass, const FrameState& state) {
    const Value outputs = jsv::get(pass, u"outputs");
    const Value written = jsv::truthy(outputs) ? outputs : jsv::get(pass, u"storageTextures");
    if (!jsv::truthy(written)) return;
    for (const Value& outputName : jsv::values(written)) {
        if (!outputName.is_string()) continue;
        const std::optional<JsString> name = parseGlobalName(outputName);
        if (!name) continue;
        Value writeId;
        for (const auto& [key, id] : state.writeSurfaces) if (key == *name) writeId = id;
        if (!jsv::truthy(writeId)) continue;
        const auto* current = frameReadTextures_.get(*name);
        const std::optional<JsString> currentReadId = current ? *current : std::nullopt;
        frameReadTextures_.set(*name, writeId.as_string());
        if (currentReadId && !currentReadId->empty()) frameWriteTextures_.set(*name, currentReadId);
    }
}

// State, feedback and persistent surfaces keep the frame's final bindings;
// display surfaces swap.
void Pipeline::swapBuffers() {
    for (auto& [name, target] : surfaces_) {
        target.currentFrame = frameIndex_;
        if (target.mesh) target.meshCurrentFrame = true;
        const Object* specs = textures();
        const Value* spec = specs ? specs->find(u"global_" + name) : nullptr;
        const bool persistent = spec && !nullish(*spec) && jsv::strict_equals(jsv::member(*spec, u"persistent"), Value(true));
        const bool feedback = feedbackSurfaces_ && contains(*feedbackSurfaces_, name);
        if (isStateSurface(name) || feedback || persistent) {
            const auto* finalRead = frameReadTextures_.get(name);
            const auto* finalWrite = frameWriteTextures_.get(name);
            if (finalRead && *finalRead && !(*finalRead)->empty() && finalWrite && *finalWrite && !(*finalWrite)->empty()) {
                target.read = *finalRead;
                target.write = *finalWrite;
            }
        } else {
            std::swap(target.read, target.write);
        }
    }
}

const std::optional<JsString>* Pipeline::FrameBindings::get(const JsString& name) const {
    const auto it = index_.find(name);
    return it == index_.end() ? nullptr : &entries_[it->second].second;
}

void Pipeline::FrameBindings::set(const JsString& name, std::optional<JsString> id) {
    const auto [it, inserted] = index_.emplace(name, entries_.size());
    if (inserted) entries_.emplace_back(name, std::move(id));
    else entries_[it->second].second = std::move(id);
}

void Pipeline::FrameBindings::clear() {
    entries_.clear();
    index_.clear();
}

// The global uniforms are copied when they have changed since the last call
// (they change once per frame, before the passes).
const FrameState& Pipeline::getFrameState() {
    FrameState& state = frameState_;
    if (frameStateVersion_ != globalUniformsVersion_) {
        state.globalUniforms = globalUniforms_;
        frameStateVersion_ = globalUniformsVersion_;
    }
    state.surfaces.clear();
    state.writeSurfaces.clear();
    for (const auto& [name, target] : surfaces_) {
        const auto* read = frameReadTextures_.get(name);
        const std::optional<JsString> readId = read && *read ? *read : target.read;
        if (readId && backend_.texture(*readId)) state.surfaces.emplace_back(name, *readId);
        const auto* write = frameWriteTextures_.get(name);
        const std::optional<JsString> writeId = write && *write ? *write : target.write;
        state.writeSurfaces.emplace_back(name, writeId ? Value(*writeId) : Value());
    }
    state.frameIndex = frameIndex_;
    state.time = lastTime_;
    const Value* renderSurface = graphMember(u"renderSurface");
    state.renderSurface = renderSurface ? *renderSurface : Value();
    state.screenWidth = Value(width_);
    state.screenHeight = Value(height_);
    return state;
}

void Pipeline::clearSurface(const JsString& name) {
    const Surface* target = surface(name);
    if (!target) return;
    if (target->read) backend_.clearTexture(*target->read);
    if (target->write) backend_.clearTexture(*target->write);
}

void Pipeline::render(double time) {
    // Programs still compiling.
    if (isCompiling_) return;

    // Normalized time wraps from about 1 to about 0: then a small positive delta.
    double deltaTime = lastTime_ > 0 ? time - lastTime_ : 0;
    if (deltaTime < 0) deltaTime = 1.0 / 60.0 / 10.0;
    lastTime_ = time;

    updateGlobalUniforms(time, deltaTime);
    invokeUpdateHooks();

    frameReadTextures_.clear();
    frameWriteTextures_.clear();
    for (const auto& [name, target] : surfaces_) {
        frameReadTextures_.set(name, target.read);
        frameWriteTextures_.set(name, target.write);
    }

    backend_.beginFrame();
    double passCount = 0;
    if (Array* list = passes()) {
        for (size_t i = 0; i < list->size(); ++i) {
            Value& originalPass = (*list)[i];
            // The authored viewport resolves before and after the uniforms do.
            const bool hasViewport = !jsv::get(originalPass, u"viewport").is_undefined();
            if (hasViewport) resolvePassViewport(originalPass, originalPass);
            Value* pass = resolvePassUniforms(originalPass, time);
            if (hasViewport) resolvePassViewport(*pass, originalPass);
            // onUpdate uniforms bind only where the pass resolves no value. A
            // pass rendered through the oscillator proxy gets none: the proxy
            // carries no effectKey, in the reference as here.
            Value withRuntime;
            if (hasRuntimeUniforms_) {
                const Value effectKey = jsv::get(*pass, u"effectKey");
                const Value* runtime = nullptr;
                if (effectKey.is_string()) {
                    for (const auto& [key, uniforms] : runtimeUniforms_) if (key == effectKey.as_string()) runtime = &uniforms;
                }
                if (runtime) {
                    const Value passUniforms = jsv::get(*pass, u"uniforms");
                    Object merged;
                    if (!nullish(passUniforms)) jsv::spread_into(merged, passUniforms);
                    for (const auto& [key, value] : jsv::entries(*runtime)) {
                        if (jsv::member(Value(merged), key).is_undefined()) merged.set(key, value);
                    }
                    Object copy;
                    jsv::spread_into(copy, *pass);
                    copy.set(u"uniforms", Value(std::move(merged)));
                    withRuntime = Value(std::move(copy));
                    pass = &withRuntime;
                }
            }
            if (shouldSkipPass(*pass)) continue;

            const double repeatCount = resolveRepeatCount(*pass);
            for (double iteration = 0; iteration < repeatCount; ++iteration) {
                const FrameState& state = getFrameState();
                backend_.executePass(*pass, state);
                ++passCount;
                updateFrameSurfaceBindings(*pass, state);
                if (repeatCount > 1) adoptIterationBindings(*pass);
            }
        }
    }

    // Mip chains regenerate from the level 0 this frame wrote.
    if (!mipTargets_.empty()) {
        std::vector<JsString> ids;
        for (const auto& id : mipTargets_) if (id) ids.push_back(*id);
        backend_.generateMipmaps(ids);
    }
    backend_.endFrame();
    swapBuffers();
    lastPassCount_ = passCount;
    frameIndex_ += 1;
}

// Parameters.

namespace {
// /_node_\d+$/ or /_chain_\d+$/
bool isScopedName(const JsString& name) {
    size_t end = name.size();
    while (end > 0 && name[end - 1] >= u'0' && name[end - 1] <= u'9') --end;
    if (end == name.size()) return false;
    const JsString head = name.substr(0, end);
    return endsWith(head, u"_node_") || endsWith(head, u"_chain_");
}
}  // namespace

void Pipeline::setUniform(const JsString& name, Value value) {
    // The particle state cap of the device.
    if ((name == u"stateSize" || startsWith(name, u"stateSize_node_")) && value.is_number()) {
        const Value cap = jsv::member(backend_.capabilities(), u"maxStateSize");
        const double maxStateSize = jsv::truthy(cap) ? jsv::to_number(cap) : 2048;
        if (value.as_number() > maxStateSize) value = Value(maxStateSize);
    }
    // A volume atlas must fit the device's texture size.
    if (isVolumeSizeUniform(name) && value.is_number()) value = clampVolumeSize(value);

    const Value oldValue = jsv::member(globalUniforms_, name);
    jsv::set_plain(globalUniforms_.as_object(), name, value);
    ++globalUniformsVersion_;

    // The classic palette index expands into the uniforms its shaders read;
    // the index itself is kept too.
    if (name == u"palette" && value.is_number()) {
        const Value expanded = expand_palette_value(value);
        if (jsv::truthy(expanded)) {
            for (const auto& [uniform, expandedValue] : jsv::entries(expanded)) setUniform(uniform, expandedValue);
        }
    }

    // A scoped name targets one chain or particle pipeline.
    const bool isScoped = isScopedName(name);
    if (Array* list = passes()) {
        for (Value& pass : *list) {
            const Value uniforms = jsv::get(pass, u"uniforms");
            // Automation values the DSL set keep priority.
            if (jsv::truthy(uniforms) && jsv::in_operator(Value(name), uniforms)) {
                if (!isAutomationValue(jsv::get(uniforms, name))) jsv::set_plain(pass.as_object()[u"uniforms"].as_object(), name, value);
            }
            // Shader uniforms fed from this parameter under another name.
            const Value aliases = jsv::get(pass, u"uniformAliases");
            if (jsv::truthy(aliases) && jsv::truthy(jsv::get(pass, u"uniforms"))) {
                for (const auto& [shaderName, globalName] : jsv::entries_strict(aliases)) {
                    if (!jsv::strict_equals(globalName, Value(name))) continue;
                    if (isAutomationValue(jsv::get(jsv::get(pass, u"uniforms"), shaderName))) continue;
                    jsv::set_plain(pass.as_object()[u"uniforms"].as_object(), shaderName, value);
                }
            }
            const Value current = jsv::get(pass, u"uniforms");
            if (!isScoped && jsv::truthy(current)) {
                for (const JsString& key : jsv::keys(current)) {
                    if (!startsWith(key, (name + u"_node_").c_str()) && !startsWith(key, (name + u"_chain_").c_str())) continue;
                    if (isAutomationValue(jsv::get(jsv::get(pass, u"uniforms"), key))) continue;
                    jsv::set_plain(pass.as_object()[u"uniforms"].as_object(), key, value);
                    jsv::set_plain(globalUniforms_.as_object(), key, value);
                    ++globalUniformsVersion_;
                }
            }
        }
    }
    // _regenAsyncForUniform: the asyncInit overlays are host inputs (see the header).

    // Textures whose size reads this parameter, or a scoped variant of it.
    const Object* specs = textures();
    if (!jsv::strict_equals(oldValue, value) && specs) {
        bool affectsTextures = false;
        for (const JsString& texId : specs->keys()) {
            const Value& spec = *specs->find(texId);
            const Value width = jsv::get(spec, u"width");
            const Value height = jsv::get(spec, u"height");
            const Value depth = jsv::get(spec, u"depth");
            if (dimensionReferencesParam(width, name) || dimensionReferencesParam(height, name) ||
                (jsv::truthy(depth) && dimensionReferencesParam(depth, name)) ||
                dimensionReferencesScopedParam(width, name) || dimensionReferencesScopedParam(height, name) ||
                (jsv::truthy(depth) && dimensionReferencesScopedParam(depth, name))) {
                affectsTextures = true;
                break;
            }
        }
        if (affectsTextures) {
            // Scoped sizes live in the pass uniforms, which also take precedence.
            Object merged;
            jsv::spread_into(merged, globalUniforms_);
            jsv::spread_into(merged, collectDefaultUniforms());
            updateParameterTextures(Value(std::move(merged)));
        }
    }
}

void Pipeline::broadcastChainScopedParam(size_t sourcePass, const JsString& uniformName, const JsString& scopedName) {
    Array* list = passes();
    if (!list) return;
    // sourcePass.uniforms on a pass that is not there.
    if (sourcePass >= list->size()) throw JsError(u"TypeError", u"Cannot read properties of undefined (reading 'uniforms')");
    Value& source = (*list)[sourcePass];
    if (!jsv::truthy(jsv::get(source, u"uniforms"))) return;
    Value value = jsv::get(jsv::get(source, u"uniforms"), uniformName);
    if (uniformName == u"volumeSize" && value.is_number()) {
        const Value clamped = clampVolumeSize(value);
        if (!jsv::strict_equals(clamped, value)) {
            value = clamped;
            // The source keeps what the chain gets.
            Object& sourceUniforms = source.as_object()[u"uniforms"].as_object();
            jsv::set_plain(sourceUniforms, uniformName, clamped);
            if (jsv::in_operator(Value(scopedName), source.as_object()[u"uniforms"])) jsv::set_plain(sourceUniforms, scopedName, clamped);
        }
    }
    for (size_t i = 0; i < list->size(); ++i) {
        if (i == sourcePass) continue;
        Value& other = (*list)[i];
        const Value uniforms = jsv::get(other, u"uniforms");
        if (!jsv::truthy(uniforms) || !jsv::in_operator(Value(scopedName), uniforms)) continue;
        jsv::set_plain(other.as_object()[u"uniforms"].as_object(), scopedName, value);
        if (uniformName == u"volumeSize" && jsv::truthy(jsv::get(other, u"inheritsVolumeSize")) &&
            jsv::in_operator(Value(uniformName), other.as_object()[u"uniforms"])) {
            jsv::set_plain(other.as_object()[u"uniforms"].as_object(), uniformName, value);
        }
    }
}

void Pipeline::setPassUniform(size_t pass, const JsString& name, Value value) {
    Array* list = passes();
    if (!list || pass >= list->size()) throw JsError(u"TypeError", u"Cannot read properties of undefined (reading 'uniforms')");
    const Value uniforms = jsv::get((*list)[pass], u"uniforms");
    if (!uniforms.is_object()) throw JsError(u"Error", u"pass uniforms other than an object are outside the port");
    jsv::set_plain((*list)[pass].as_object()[u"uniforms"].as_object(), name, std::move(value));
}

// The audio captures the graph needs.

namespace {
// The audio() selector fields as the reference reads them.
Value member(const Value& value, const char16_t* key) { return jsv::member(value, key); }
bool isAudioConfig(const Value& value) {
    if (jsv::strict_equals(member(value, u"type"), Value(u"Audio"))) return true;
    const Value ast = member(value, u"_ast");
    return !ast.is_undefined() && !ast.is_null() && jsv::strict_equals(member(ast, u"type"), Value(u"Audio"));
}
Value audioSource(const Value& config) {
    const Value ast = member(config, u"_ast");
    const bool fromAst = !ast.is_undefined() && !ast.is_null() && jsv::strict_equals(member(ast, u"type"), Value(u"Audio"));
    return fromAst ? ast : config;
}
bool audioSelectorIntent(const Value& config) {
    const Value source = audioSource(config);
    for (const char16_t* field : {u"name", u"id", u"channel"}) if (!member(config, field).is_undefined()) return true;
    for (const char16_t* field : {u"name", u"id", u"channel"}) if (!member(source, field).is_undefined()) return true;
    return false;
}
bool validAudioSelector(const Value& config) {
    const Value source = audioSource(config);
    const Value name = member(config, u"name");
    const Value id = member(config, u"id");
    const Value channel = member(config, u"channel");
    if ((!member(source, u"name").is_undefined() && name.is_undefined()) ||
        (!member(source, u"id").is_undefined() && id.is_undefined()) ||
        (!member(source, u"channel").is_undefined() && channel.is_undefined())) {
        return false;
    }
    if (!name.is_undefined() && (!name.is_string() || name.as_string().empty())) return false;
    if (!id.is_undefined() && (!id.is_string() || id.as_string().empty() || !jsv::truthy(name))) return false;
    return channel.is_number() && std::trunc(channel.as_number()) == channel.as_number() && channel.as_number() >= 1 &&
           channel.as_number() <= 32;
}
bool isIntegerIn(const Value& value, double low, double high) {
    return value.is_number() && std::isfinite(value.as_number()) && std::trunc(value.as_number()) == value.as_number() &&
           value.as_number() >= low && value.as_number() <= high;
}
}  // namespace

Value Pipeline::getAudioInputRequirements() const {
    bool needsLegacy = false;
    bool needsLegacyRaw = false;
    Array selected;
    std::vector<JsString> selectedKeys;
    // The reference skips an object it has visited; a Value shares none, and
    // visiting one twice adds nothing, as requirements merge by key.
    std::function<void(const Value&)> visit = [&](const Value& value) {
        if (!jsv::truthy(value) || !(value.is_object() || value.is_array() || value.is_float32_array())) return;
        if (isAudioConfig(value)) {
            const bool hasSelectorIntent = audioSelectorIntent(value);
            const bool hasValidBand = !jsv::strict_equals(member(value, u"_invalid"), Value(true)) &&
                                      isIntegerIn(member(value, u"band"), 0, 4);
            if (!hasValidBand) return;
            // The bounds may be automation with captures of their own.
            visit(member(value, u"min"));
            visit(member(value, u"max"));
            const bool needsRaw = jsv::strict_equals(member(value, u"band"), Value(4.0));
            if (hasSelectorIntent && validAudioSelector(value)) {
                const Value id = member(value, u"id");
                const Value name = member(value, u"name");
                const Value channel = member(value, u"channel");
                Object requirement;
                requirement.set(u"id", id.is_string() && !id.as_string().empty() ? id : Value::null());
                requirement.set(u"name", name.is_undefined() || name.is_null() ? Value::null() : name);
                requirement.set(u"channel", channel.is_undefined() || channel.is_null() ? Value(1.0) : channel);
                requirement.set(u"needsRaw", Value(needsRaw));
                const JsString key = utf8_to_js(json::stringify(Value(Array{*requirement.find(u"id"), *requirement.find(u"name"), *requirement.find(u"channel")})));
                if (contains(selectedKeys, key)) {
                    for (Value& existing : selected) {
                        Object& entry = existing.as_object();
                        if (jsv::strict_equals(*entry.find(u"id"), *requirement.find(u"id")) &&
                            jsv::strict_equals(*entry.find(u"name"), *requirement.find(u"name")) &&
                            jsv::strict_equals(*entry.find(u"channel"), *requirement.find(u"channel"))) {
                            if (needsRaw) entry.set(u"needsRaw", Value(true));
                            break;
                        }
                    }
                } else {
                    selectedKeys.push_back(key);
                    selected.emplace_back(std::move(requirement));
                }
            } else if (!hasSelectorIntent) {
                needsLegacy = true;
                if (needsRaw) needsLegacyRaw = true;
            }
            return;
        }
        if (value.is_array()) {
            for (const Value& item : value.as_array()) visit(item);
            return;
        }
        if (value.is_float32_array()) return;
        for (const Value& item : jsv::values(value)) visit(item);
    };
    if (const Array* list = passes()) {
        for (const Value& pass : *list) {
            const Value effectKey = jsv::get(pass, u"effectKey");
            const EffectHooks* effect = jsv::truthy(effectKey) ? getEffect(effectKey) : nullptr;
            if (effect && contains(effect->tags, JsString(u"audio"))) needsLegacy = true;
            visit(jsv::get(pass, u"uniforms"));
        }
    }
    Object out;
    out.set(u"needsLegacy", Value(needsLegacy));
    out.set(u"needsLegacyRaw", Value(needsLegacyRaw));
    out.set(u"selected", Value(std::move(selected)));
    return Value(std::move(out));
}

// { param } or { screenDivide } naming the parameter.
bool Pipeline::dimensionReferencesParam(const Value& spec, const JsString& paramName) {
    if (!spec.is_object() && !spec.is_array() && !spec.is_float32_array()) return false;
    return jsv::strict_equals(jsv::get(spec, u"param"), Value(paramName)) ||
           jsv::strict_equals(jsv::get(spec, u"screenDivide"), Value(paramName));
}

// A param or screenDivide naming a _node_ or _chain_ variant of the parameter.
bool Pipeline::dimensionReferencesScopedParam(const Value& spec, const JsString& paramName) {
    if (!spec.is_object() && !spec.is_array() && !spec.is_float32_array()) return false;
    const Value param = jsv::get(spec, u"param");
    const Value screenDivide = jsv::get(spec, u"screenDivide");
    const Value ref = param.is_string() ? param : screenDivide.is_string() ? screenDivide : Value();
    if (!ref.is_string() || ref.as_string().empty()) return false;
    return startsWith(ref.as_string(), (paramName + u"_node_").c_str()) ||
           startsWith(ref.as_string(), (paramName + u"_chain_").c_str());
}

}  // namespace libnoisemaker::backend
