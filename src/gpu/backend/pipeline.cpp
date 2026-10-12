#include "gpu/backend/pipeline.h"

#include "core/catalog/catalog.h"
#include "core/edit/jsv.h"
#include "core/value/js_number.h"
#include "core/value/json.h"

#include <algorithm>
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
const std::unordered_map<JsString, std::vector<JsString>>& effectHooks() {
    static const std::unordered_map<JsString, std::vector<JsString>> index = [] {
        std::vector<const CatalogFile*> files;
        for (const auto& file : catalog_files()) {
            if (file.path.starts_with("effects/") && file.path.ends_with(".json")) files.push_back(&file);
        }
        std::sort(files.begin(), files.end(), [](const auto* a, const auto* b) { return a->path < b->path; });
        std::unordered_map<JsString, std::vector<JsString>> out;
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
            const JsString func = jsv::to_string(jsv::member(definition, u"func"));
            for (const JsString& key : {func, ns + u"." + func, ns + u"/" + name, ns + u"." + name}) out[key] = hooks;
        }
        return out;
    }();
    return index;
}
const std::vector<JsString>* getEffect(const Value& key) {
    if (!key.is_string()) return nullptr;
    const auto& index = effectHooks();
    const auto it = index.find(key.as_string());
    return it == index.end() ? nullptr : &it->second;
}
bool hasLifecycleHook(const std::vector<JsString>& hooks, const char16_t* hook) { return contains(hooks, JsString(hook)); }

}  // namespace

Pipeline::Pipeline(Value graph, WebGPUBackend& backend, Options options)
    : graph_(std::move(graph)), backend_(backend), texturePooling_(options.texturePooling) {}

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
    for (auto& [key, value] : surfaces_) if (key == name) return &value;
    return nullptr;
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
        const std::vector<JsString>* hooks = getEffect(effectKey);
        if (!hooks) continue;
        if (!hasLifecycleHook(*hooks, u"onInit") && !hasLifecycleHook(*hooks, u"onUpdate") &&
            !hasLifecycleHook(*hooks, u"onDestroy")) {
            continue;
        }
        const JsString& key = effectKey.as_string();
        if (!contains(lifecycleEffects_, key)) lifecycleEffects_.push_back(key);
        if (hasLifecycleHook(*hooks, u"onInit") && !initLifecycleDone_.contains(key)) {
            initLifecycleDone_.insert(key);
            // synth/media is the only effect with onInit.
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

}  // namespace libnoisemaker::backend
