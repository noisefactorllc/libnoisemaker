#include "gpu/backend/backend.h"

#include "core/catalog/catalog.h"
#include "core/edit/jsv.h"
#include "core/value/json.h"
#include "gpu/wgsl/jsre.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace libnoisemaker::backend {

namespace {

// The runtime's own shaders (tools/catalog/extract-runtime-shaders.mjs).
const Value& runtimeShaders() {
    static const Value shaders = [] {
        const CatalogFile* file = catalog_find("runtime/shaders.json");
        if (!file) throw std::runtime_error("catalog has no runtime/shaders.json");
        return json::parse(file->bytes);
    }();
    return shaders;
}
JsString runtimeShader(const char16_t* name) { return jsv::member(runtimeShaders(), name).as_string(); }

bool startsWith(const JsString& text, const char16_t* prefix) { return text.rfind(prefix, 0) == 0; }
bool contains(const JsString& text, const char16_t* part) { return text.find(part) != JsString::npos; }

// texId.replace(/_chain_\d+$/, '')
JsString unscoped(const JsString& id) {
    size_t end = id.size();
    while (end > 0 && id[end - 1] >= u'0' && id[end - 1] <= u'9') --end;
    if (end == id.size()) return id;
    const JsString marker = u"_chain_";
    if (end < marker.size() || id.compare(end - marker.size(), marker.size(), marker) != 0) return id;
    return id.substr(0, end - marker.size());
}

// parseInt(text, 10) for the identifier characters of a binding name.
double parseLeadingDigits(const JsString& text) {
    size_t end = 0;
    while (end < text.size() && text[end] >= u'0' && text[end] <= u'9') ++end;
    if (end == 0) return std::nan("");
    return jsv::to_number(Value(text.substr(0, end)));
}

// mipLevelCount(width, height) in webgpu.js.
double mipLevelCount(const Value& width, const Value& height) {
    const double w = std::floor(jsv::to_number(width));
    const double h = std::floor(jsv::to_number(height));
    if (std::isnan(w) || std::isnan(h)) return std::nan("");
    const double maxDim = std::max({1.0, w, h});
    return std::max(1.0, std::floor(std::log2(maxDim)) + 1);
}

Extent3D extent(const Value& width, const Value& height, const Value& depth = Value(1.0)) {
    return Extent3D{jsv::to_number(width), jsv::to_number(height), jsv::to_number(depth)};
}

JsString asciiLower(const JsString& text) {
    JsString out = text;
    for (char16_t& c : out) {
        if (c > 0x7F) throw JsError(u"Error", u"a blend factor with non-ASCII letters is outside the port");
        if (c >= u'A' && c <= u'Z') c = char16_t(c + 32);
    }
    return out;
}

bool isStructUniform(const JsString& typeDecl) {
    return !typeDecl.empty() && !contains(typeDecl, u"<") && typeDecl != u"f32" && typeDecl != u"i32" &&
           typeDecl != u"u32" && typeDecl != u"bool" && !startsWith(typeDecl, u"vec") && !startsWith(typeDecl, u"mat");
}

// A bound uniform's value as createBindGroup's getUniform finds it.
Value uniformValue(const Value& pass, const FrameState& state, const JsString& name) {
    const Value uniforms = jsv::member(pass, u"uniforms");
    if (jsv::truthy(uniforms) && jsv::in_operator(Value(name), uniforms)) return jsv::get(uniforms, name);
    if (jsv::truthy(state.globalUniforms) && jsv::in_operator(Value(name), state.globalUniforms)) {
        return jsv::get(state.globalUniforms, name);
    }
    return Value();
}

// The default createBindGroup gives a missing or unusable plain uniform.
Value defaultUniform(const JsString& typeDecl) {
    if (typeDecl == u"i32" || typeDecl == u"u32") return Value(0.0);
    if (startsWith(typeDecl, u"vec2")) return Value(Array(2, Value(0.0)));
    if (startsWith(typeDecl, u"vec3")) return Value(Array(3, Value(0.0)));
    if (startsWith(typeDecl, u"vec4")) return Value(Array(4, Value(0.0)));
    if (startsWith(typeDecl, u"array<")) {
        static const jsre::Regex arrayDecl(u"^array<\\s*([^,>]+(?:<[^>]+>)?)\\s*,\\s*(\\d+)\\s*>");
        const auto m = arrayDecl.exec(typeDecl);
        if (!m) return Value(0.0);
        JsString elemType = m->str(typeDecl, 1);
        while (!elemType.empty() && (elemType.back() == u' ' || elemType.back() == u'\t' || elemType.back() == u'\n')) elemType.pop_back();
        const double count = jsv::to_number(Value(m->str(typeDecl, 2)));
        double stride = 16;
        if (elemType == u"f32" || elemType == u"i32" || elemType == u"u32") stride = 4;
        else if (startsWith(elemType, u"vec2")) stride = 8;
        else if (startsWith(elemType, u"vec3") || startsWith(elemType, u"vec4")) stride = 16;
        const double length = count * stride / 4;
        if (length > 1e8) throw JsError(u"RangeError", u"Invalid array length");
        return Value(Array(size_t(length), Value(0.0)));
    }
    if (startsWith(typeDecl, u"mat3")) {
        return Value(Array{Value(1.0), Value(0.0), Value(0.0), Value(0.0), Value(1.0), Value(0.0), Value(0.0), Value(0.0), Value(1.0)});
    }
    return Value(0.0);
}

}  // namespace

WebGPUBackend::WebGPUBackend(Device& device) : device_(device) {}

// The reference returns '' for "global_", which every caller treats as no
// name; so does nullopt here.
std::optional<JsString> WebGPUBackend::parseGlobalName(const Value& texId) {
    if (!texId.is_string()) return std::nullopt;
    const JsString& id = texId.as_string();
    if (startsWith(id, u"global_")) return id.size() > 7 ? std::optional<JsString>(id.substr(7)) : std::nullopt;
    if (startsWith(id, u"global") && id.size() > 6) {
        const char16_t first = id[6];
        if ((first >= u'A' && first <= u'Z') || (first >= u'0' && first <= u'9')) {
            JsString name = id.substr(6);
            if (first >= u'A' && first <= u'Z') name[0] = char16_t(first + 32);
            return name;
        }
    }
    return std::nullopt;
}

void WebGPUBackend::init() {
    Object capabilities;
    capabilities.set(u"maxTextureSize", Value(device_.maxTextureDimension2D()));
    capabilities_ = Value(std::move(capabilities));

    SamplerDescriptor linear;
    linear.minFilter = u"linear";
    linear.magFilter = u"linear";
    samplers_.emplace_back(u"default", device_.createSampler(linear));
    SamplerDescriptor nearest;
    samplers_.emplace_back(u"nearest", device_.createSampler(nearest));
    SamplerDescriptor repeat = linear;
    repeat.addressModeU = u"repeat";
    repeat.addressModeV = u"repeat";
    samplers_.emplace_back(u"repeat", device_.createSampler(repeat));
    SamplerDescriptor mipmap = linear;
    mipmap.mipmapFilter = u"linear";
    samplers_.emplace_back(u"mipmap", device_.createSampler(mipmap));

    TextureDescriptor dummy;
    dummy.size = Extent3D{1, 1, 1};
    dummy.format = u"rgba8unorm";
    dummy.usage = TextureUsage::TextureBinding | TextureUsage::CopyDst;
    const Handle dummyTexture = device_.createTexture(dummy);
    TexelCopyBufferLayout layout;
    layout.bytesPerRow = 4;
    device_.writeTexture(TexelCopyTextureInfo{dummyTexture}, std::vector<uint8_t>{0, 0, 0, 0}, layout, Extent3D{1, 1, 1});
    dummyTextureView_ = device_.createView(dummyTexture, {});
}

Handle WebGPUBackend::sampler(const Value& type) const {
    if (type.is_string()) {
        for (const auto& [name, handle] : samplers_) if (name == type.as_string()) return handle;
    }
    for (const auto& [name, handle] : samplers_) if (name == u"default") return handle;
    return 0;
}

Value WebGPUBackend::resolveFormat(const Value& format) const {
    static const Value formats = [] {
        Object table;
        const char16_t* pairs[][2] = {
            {u"rgba8", u"rgba8unorm"}, {u"rgba16f", u"rgba16float"}, {u"rgba32f", u"rgba32float"},
            {u"r8", u"r8unorm"}, {u"r16f", u"r16float"}, {u"r32f", u"r32float"}, {u"rg8", u"rg8unorm"},
            {u"rg16f", u"rg16float"}, {u"rg32f", u"rg32float"}, {u"rgba8unorm", u"rgba8unorm"},
            {u"rgba16float", u"rgba16float"}, {u"rgba32float", u"rgba32float"}, {u"r8unorm", u"r8unorm"},
            {u"r16float", u"r16float"}, {u"r32float", u"r32float"}, {u"bgra8unorm", u"bgra8unorm"}};
        for (const auto& pair : pairs) table.set(pair[0], Value(pair[1]));
        return Value(std::move(table));
    }();
    const Value mapped = jsv::member(formats, jsv::to_property_key(format));
    if (jsv::truthy(mapped)) return mapped;
    if (jsv::truthy(format)) return format;
    return Value(u"rgba8unorm");
}

uint32_t WebGPUBackend::resolveUsage(const Value& usageArray) const {
    uint32_t usage = 0;
    for (const Value& u : jsv::iterate(usageArray, u"usageArray")) {
        if (!u.is_string()) continue;
        const JsString& name = u.as_string();
        if (name == u"render") usage |= TextureUsage::RenderAttachment;
        else if (name == u"sample") usage |= TextureUsage::TextureBinding;
        else if (name == u"storage") usage |= TextureUsage::StorageBinding;
        else if (name == u"copySrc") usage |= TextureUsage::CopySrc;
        else if (name == u"copyDst") usage |= TextureUsage::CopyDst;
    }
    return usage;
}

void WebGPUBackend::createTexture(const JsString& id, const Value& spec) {
    const Value format = resolveFormat(jsv::get(spec, u"format"));
    const Value specUsage = jsv::get(spec, u"usage");
    const uint32_t usage = resolveUsage(jsv::truthy(specUsage) ? specUsage
        : Value(Array{Value(u"render"), Value(u"sample"), Value(u"copySrc"), Value(u"copyDst")}));
    const double mipLevels = jsv::truthy(jsv::get(spec, u"mipmaps"))
        ? mipLevelCount(jsv::get(spec, u"width"), jsv::get(spec, u"height")) : 1;

    TextureDescriptor descriptor;
    descriptor.size = extent(jsv::get(spec, u"width"), jsv::get(spec, u"height"));
    descriptor.format = jsv::to_string(format);
    descriptor.mipLevelCount = mipLevels;
    descriptor.usage = usage;
    TextureRecord record;
    record.handle = device_.createTexture(descriptor);
    record.view = device_.createView(record.handle, {});
    for (double level = 0; level < mipLevels; ++level) {
        TextureViewDescriptor single;
        single.baseMipLevel = level;
        single.mipLevelCount = 1;
        record.mipViews.push_back(device_.createView(record.handle, single));
    }
    if (!record.mipViews.empty()) record.renderView = record.mipViews[0];
    record.width = jsv::get(spec, u"width");
    record.height = jsv::get(spec, u"height");
    record.format = jsv::get(spec, u"format");
    record.gpuFormat = format;
    record.usage = usage;
    record.mipmaps = mipLevels > 1;
    record.mipLevels = mipLevels;
    record.persistent = jsv::truthy(jsv::get(spec, u"persistent"));
    textures_[id] = std::move(record);
}

void WebGPUBackend::createTexture3D(const JsString& id, const Value& spec) {
    const Value format = resolveFormat(jsv::get(spec, u"format"));
    const Value specUsage = jsv::get(spec, u"usage");
    const uint32_t usage = resolveUsage(jsv::truthy(specUsage) ? specUsage
        : Value(Array{Value(u"storage"), Value(u"sample"), Value(u"copySrc"), Value(u"copyDst")}));
    TextureDescriptor descriptor;
    descriptor.size = extent(jsv::get(spec, u"width"), jsv::get(spec, u"height"), jsv::get(spec, u"depth"));
    descriptor.dimension = u"3d";
    descriptor.format = jsv::to_string(format);
    descriptor.usage = usage;
    TextureRecord record;
    record.handle = device_.createTexture(descriptor);
    TextureViewDescriptor view;
    view.dimension = u"3d";
    record.view = device_.createView(record.handle, view);
    record.width = jsv::get(spec, u"width");
    record.height = jsv::get(spec, u"height");
    record.depth = jsv::get(spec, u"depth");
    record.format = jsv::get(spec, u"format");
    record.gpuFormat = format;
    record.usage = usage;
    record.is3D = true;
    const Value filter = jsv::get(spec, u"filter");
    record.filter = jsv::truthy(filter) ? filter : Value(u"linear");
    textures_[id] = std::move(record);
}

void WebGPUBackend::destroyTexture(const JsString& id) {
    const auto it = textures_.find(id);
    if (it == textures_.end()) return;
    device_.destroyTexture(it->second.handle);
    textures_.erase(it);
}

void WebGPUBackend::createCubeTexture(const JsString& id, const Value& size) {
    TextureDescriptor descriptor;
    descriptor.size = extent(size, size, Value(6.0));
    descriptor.format = u"rgba8unorm";
    descriptor.usage = TextureUsage::TextureBinding | TextureUsage::CopyDst | TextureUsage::RenderAttachment;
    TextureRecord record;
    record.handle = device_.createTexture(descriptor);
    TextureViewDescriptor view;
    view.dimension = u"cube";
    record.view = device_.createView(record.handle, view);
    record.width = size;
    record.height = size;
    record.cube = true;
    textures_[id] = std::move(record);
}

void WebGPUBackend::uploadDataTexture(const JsString& id, const std::vector<float>& data, const Value& width,
                                      const Value& height) {
    auto it = textures_.find(id);
    if (it == textures_.end() || !jsv::strict_equals(it->second.width, width) ||
        !jsv::strict_equals(it->second.height, height)) {
        if (it != textures_.end()) device_.destroyTexture(it->second.handle);
        TextureDescriptor descriptor;
        descriptor.size = extent(width, height);
        descriptor.format = u"rgba32float";
        descriptor.usage = TextureUsage::TextureBinding | TextureUsage::CopyDst;
        TextureRecord record;
        record.handle = device_.createTexture(descriptor);
        record.width = width;
        record.height = height;
        record.format = Value(u"rgba32float");
        record.view = device_.createView(record.handle, {});
        textures_[id] = std::move(record);
        it = textures_.find(id);
    }
    const double w = jsv::to_number(width);
    const double h = jsv::to_number(height);
    const double bytesPerRow = w * 4 * 4;
    const double alignedBytesPerRow = std::ceil(bytesPerRow / 256) * 256;
    std::vector<float> upload = data;
    if (alignedBytesPerRow != bytesPerRow) {
        const size_t srcRowFloats = size_t(w * 4);
        const size_t dstRowFloats = size_t(alignedBytesPerRow / 4);
        upload.assign(dstRowFloats * size_t(h), 0.0f);
        for (size_t y = 0; y < size_t(h); ++y) {
            for (size_t x = 0; x < srcRowFloats && y * srcRowFloats + x < data.size(); ++x) {
                upload[y * dstRowFloats + x] = data[y * srcRowFloats + x];
            }
        }
    }
    std::vector<uint8_t> bytes(upload.size() * 4);
    if (!upload.empty()) std::memcpy(bytes.data(), upload.data(), bytes.size());
    TexelCopyBufferLayout layout;
    layout.bytesPerRow = alignedBytesPerRow;
    layout.rowsPerImage = h;
    device_.writeTexture(TexelCopyTextureInfo{it->second.handle}, bytes, layout, extent(width, height));
}

const TextureRecord* WebGPUBackend::texture(const JsString& id) const {
    const auto it = textures_.find(id);
    return it == textures_.end() ? nullptr : &it->second;
}

const TextureRecord* WebGPUBackend::textureFor(const Value& texId) const {
    return texId.is_string() ? texture(texId.as_string()) : nullptr;
}

// this.textures.get(texId) || (typeof texId === 'string' ? this.textures.get(unscoped) : null)
const TextureRecord* WebGPUBackend::textureOrUnscoped(const Value& texId) const {
    if (const TextureRecord* record = textureFor(texId)) return record;
    return texId.is_string() ? texture(unscoped(texId.as_string())) : nullptr;
}

const ProgramInfo* WebGPUBackend::program(const JsString& id) const {
    const auto it = programs_.find(id);
    return it == programs_.end() ? nullptr : &it->second;
}

namespace {
double jsMax(double a, double b) { return std::isnan(a) || std::isnan(b) ? std::nan("") : std::max(a, b); }
const Value* lookup(const std::vector<std::pair<JsString, Value>>& entries, const JsString& key) {
    for (const auto& [name, value] : entries) if (name == key) return &value;
    return nullptr;
}
JsString entryPointName(const Value& value) { return jsv::to_string(value); }
}  // namespace

Handle WebGPUBackend::defaultVertexModule() {
    if (!defaultVertexModule_) defaultVertexModule_ = device_.createShaderModule(runtimeShader(u"defaultVertex"));
    return defaultVertexModule_;
}

std::optional<BlendState> WebGPUBackend::resolveBlendState(const Value& blend) const {
    if (!jsv::truthy(blend)) return std::nullopt;
    BlendState state;
    state.color = BlendComponent{u"add", u"one", u"one"};
    state.alpha = state.color;
    if (blend.is_array()) {
        auto toFactor = [](const Value& factor) -> JsString {
            if (!factor.is_string()) return {};
            JsString lower = asciiLower(factor.as_string());
            for (char16_t& c : lower) if (c == u'_') c = u'-';
            return lower;
        };
        const Array& factors = blend.as_array();
        const JsString src = toFactor(factors.size() > 0 ? factors[0] : Value());
        const JsString dst = toFactor(factors.size() > 1 ? factors[1] : Value());
        state.color.srcFactor = src.empty() ? u"one" : src;
        state.color.dstFactor = dst.empty() ? u"one" : dst;
        state.alpha = state.color;
    }
    return state;
}

const ProgramInfo& WebGPUBackend::compileProgram(const JsString& id, const Value& spec) {
    // resolveWGSLSource
    Value source = jsv::get(spec, u"wgsl");
    if (!jsv::truthy(source)) source = jsv::get(spec, u"source");
    if (!jsv::truthy(source)) {
        // spec.fragment && !spec.fragment.includes('#version')
        const Value fragment = jsv::get(spec, u"fragment");
        bool glsl = false;
        if (jsv::truthy(fragment)) {
            if (fragment.is_string()) glsl = contains(fragment.as_string(), u"#version");
            else if (!fragment.is_array()) throw JsError(u"TypeError", u"spec.fragment.includes is not a function");
            else glsl = true;  // an array reaches the refusal below either way
        }
        source = jsv::truthy(fragment) && !glsl ? fragment : Value::null();
    }
    if (jsv::truthy(source) && !source.is_string()) {
        throw JsError(u"TypeError", u"a program source other than a string is outside the port");
    }
    if (!jsv::truthy(source)) {
        if (jsv::get(spec, u"fragment").is_array()) {
            throw JsError(u"TypeError", u"a program source other than a string is outside the port");
        }
        JsString keys;
        for (const JsString& key : jsv::keys(spec)) keys += (keys.empty() ? u"" : u", ") + key;
        throw JsError(u"ShaderDiagnostic", u"No WGSL shader source found for program '" + id + u"'. Available keys: " + keys);
    }
    const Value defines = jsv::get(spec, u"defines");
    const JsString processed = wgsl::injectDefines(jsv::to_string(source), jsv::truthy(defines) ? defines : Value(Object{}));
    const bool hasCompute = wgsl::hasComputeEntry(processed);
    const bool hasFragment = wgsl::hasFragmentEntry(processed);
    const wgsl::EntryPoints detected = wgsl::detectEntryPoints(processed);
    Object enhanced;
    for (const auto& [key, value] : jsv::entries(spec)) enhanced.set(key, value);
    enhanced.set(u"fragmentEntryPoint", detected.fragment ? Value(*detected.fragment) : jsv::get(spec, u"fragmentEntryPoint"));
    enhanced.set(u"vertexEntryPoint", detected.vertex ? Value(*detected.vertex) : jsv::get(spec, u"vertexEntryPoint"));
    enhanced.set(u"computeEntryPoint", detected.compute ? Value(*detected.compute) : jsv::get(spec, u"computeEntryPoint"));
    const Value enhancedSpec(std::move(enhanced));
    if (hasCompute && !hasFragment) return compileComputeProgram(id, processed, enhancedSpec);
    return compileRenderProgram(id, processed, enhancedSpec);
}

const ProgramInfo& WebGPUBackend::compileComputeProgram(const JsString& id, const JsString& source, const Value& spec) {
    ProgramInfo info;
    info.isCompute = true;
    info.module = device_.createShaderModule(source);
    if (const auto errors = device_.compilationErrors(info.module); !errors.empty()) {
        throw JsError(u"ShaderDiagnostic", errors.front());
    }
    info.bindings = wgsl::parseShaderBindings(source);
    info.entryPoints = wgsl::computeEntryPoints(source);
    info.entryPointBindings = wgsl::parseEntryPointBindings(source, info.bindings);
    Value defaultEntryPoint = jsv::get(spec, u"computeEntryPoint");
    if (!jsv::truthy(defaultEntryPoint)) {
        defaultEntryPoint = !info.entryPoints.empty() && !info.entryPoints[0].empty() ? Value(info.entryPoints[0]) : Value(u"main");
    }
    info.entryPoint = entryPointName(defaultEntryPoint);
    info.pipeline = device_.createComputePipeline(ComputePipelineDescriptor{info.module, info.entryPoint});
    info.pipelines.emplace_back(info.entryPoint, info.pipeline);
    info.samplerTextureNames = wgsl::parseSamplerTextureUses(source);
    info.sourceHasBindings = wgsl::hasShaderBindings(source);
    info.uniforms = ProgramUniforms::fromSource(source, jsv::get(spec, u"uniformLayout"));
    programs_[id] = std::move(info);
    return programs_[id];
}

const ProgramInfo& WebGPUBackend::compileRenderProgram(const JsString& id, const JsString& source, const Value& spec) {
    ProgramInfo info;
    info.bindings = wgsl::parseShaderBindings(source);
    info.sourceHasBindings = wgsl::hasShaderBindings(source);
    const bool hasVertex = wgsl::hasVertexEntry(source);
    const Handle mainModule = device_.createShaderModule(source);
    if (const auto errors = device_.compilationErrors(mainModule); !errors.empty()) {
        throw JsError(u"ShaderDiagnostic", errors.front());
    }
    const JsString defaultVertexEntry = runtimeShader(u"defaultVertexEntryPoint");
    JsString vertexSource;
    info.fragmentModule = mainModule;
    Value vertexWgsl = jsv::get(spec, u"vertexWGSL");
    if (!jsv::truthy(vertexWgsl)) vertexWgsl = jsv::get(spec, u"vertexWgsl");
    const Value vertexEntry = jsv::get(spec, u"vertexEntryPoint");
    if (jsv::truthy(vertexWgsl)) {
        vertexSource = jsv::to_string(vertexWgsl);
        info.vertexModule = device_.createShaderModule(vertexSource);
        if (const auto errors = device_.compilationErrors(info.vertexModule); !errors.empty()) {
            throw JsError(u"ShaderDiagnostic", errors.front());
        }
        info.vertexEntryPoint = jsv::truthy(vertexEntry) ? entryPointName(vertexEntry) : defaultVertexEntry;
        const std::vector<wgsl::Binding> vertexBindings = wgsl::parseShaderBindings(vertexSource);
        info.sourceHasBindings = info.sourceHasBindings || wgsl::hasShaderBindings(vertexSource);
        if (!vertexBindings.empty()) {
            // Merge, keeping the fragment's binding for a group and index both use.
            auto key = [](const wgsl::Binding& b) { return jsv::to_string(Value(b.group)) + u":" + jsv::to_string(Value(b.binding)); };
            std::vector<JsString> existing;
            for (const auto& b : info.bindings) existing.push_back(key(b));
            for (const auto& b : vertexBindings) {
                if (std::find(existing.begin(), existing.end(), key(b)) == existing.end()) info.bindings.push_back(b);
            }
            std::stable_sort(info.bindings.begin(), info.bindings.end(), [](const wgsl::Binding& a, const wgsl::Binding& b) {
                if (a.group != b.group) return a.group - b.group < 0;
                return a.binding - b.binding < 0;
            });
        }
    } else if (hasVertex) {
        info.vertexModule = mainModule;
        info.vertexEntryPoint = jsv::truthy(vertexEntry) ? entryPointName(vertexEntry) : defaultVertexEntry;
    } else {
        info.vertexModule = defaultVertexModule();
        info.vertexEntryPoint = defaultVertexEntry;
    }
    Value fragmentEntry = jsv::get(spec, u"fragmentEntryPoint");
    if (!jsv::truthy(fragmentEntry)) fragmentEntry = jsv::get(spec, u"entryPoint");
    info.fragmentEntryPoint = jsv::truthy(fragmentEntry) ? entryPointName(fragmentEntry) : runtimeShader(u"defaultFragmentEntryPoint");
    const Value outputFormatSpec = jsv::get(spec, u"outputFormat");
    info.outputFormat = resolveFormat(jsv::truthy(outputFormatSpec) ? outputFormatSpec : Value(u"rgba16float"));
    const Value topology = jsv::get(spec, u"topology");
    const Value blend = jsv::get(spec, u"blend");

    RenderPipelineDescriptor descriptor;
    descriptor.vertexModule = info.vertexModule;
    descriptor.vertexEntryPoint = info.vertexEntryPoint;
    descriptor.fragmentModule = info.fragmentModule;
    descriptor.fragmentEntryPoint = info.fragmentEntryPoint;
    descriptor.targets.push_back(ColorTargetState{jsv::to_string(info.outputFormat), resolveBlendState(blend)});
    descriptor.topology = jsv::truthy(topology) ? jsv::to_string(topology) : JsString(u"triangle-list");
    info.pipeline = device_.createRenderPipeline(descriptor);
    info.module = info.fragmentModule;

    // getPipelineKey({topology, blend, format})
    const JsString blendKey = jsv::truthy(blend) ? utf8_to_js(json::stringify(blend)) : JsString(u"noblend");
    const JsString topoKey = jsv::truthy(topology) ? jsv::to_string(topology) : JsString(u"triangle-list");
    const JsString formatKey = jsv::truthy(info.outputFormat) ? jsv::to_string(info.outputFormat) : JsString(u"rgba16float");
    info.pipelineCache.emplace_back(topoKey + u"|" + blendKey + u"|" + formatKey, info.pipeline);

    info.samplerTextureNames = wgsl::parseSamplerTextureUses(source + u"\n" + vertexSource);
    info.uniforms = ProgramUniforms::fromSource(source, jsv::get(spec, u"uniformLayout"));
    programs_[id] = std::move(info);
    return programs_[id];
}

void WebGPUBackend::beginFrame() {
    while (!activeUniformBuffers_.empty()) {
        uniformBufferPool_.push_back(activeUniformBuffers_.back());
        activeUniformBuffers_.pop_back();
    }
    commandEncoder_ = device_.createCommandEncoder();
}

void WebGPUBackend::endFrame() {
    if (!commandEncoder_) return;
    const Handle commandBuffer = device_.finish(commandEncoder_);
    device_.submit({commandBuffer});
    commandEncoder_ = 0;
    if (!retiredStorageBuffers_.empty()) {
        std::vector<Buffer> retired = std::move(retiredStorageBuffers_);
        retiredStorageBuffers_.clear();
        // The reference's callback holds the buffers, not the backend.
        device_.onSubmittedWorkDone([device = &device_, retired] {
            for (const Buffer& buffer : retired) device->destroyBuffer(buffer.handle);
        });
    }
}

WebGPUBackend::Buffer WebGPUBackend::bufferFor(double size) {
    for (size_t i = 0; i < uniformBufferPool_.size(); ++i) {
        if (uniformBufferPool_[i].size >= size) {
            const Buffer buffer = uniformBufferPool_[i];
            uniformBufferPool_.erase(uniformBufferPool_.begin() + long(i));
            return buffer;
        }
    }
    return Buffer{device_.createBuffer(BufferDescriptor{size, BufferUsage::Uniform | BufferUsage::CopyDst}), size};
}

std::optional<Handle> WebGPUBackend::uniformBuffer(const Value& pass, const FrameState& state, const ProgramUniforms* program) {
    static const ProgramUniforms none;
    const auto write = packer_.createUniformBuffer(jsv::member(pass, u"uniforms"), state.globalUniforms, program ? *program : none);
    if (!write) return std::nullopt;
    const Buffer buffer = bufferFor(write->bufferSize);
    device_.writeBuffer(buffer.handle, 0, write->bytes);
    activeUniformBuffers_.push_back(buffer);
    return buffer.handle;
}

std::optional<WebGPUBackend::Buffer> WebGPUBackend::singleUniformBuffer(const Value& value, const JsString& typeDecl) {
    const auto write = packer_.createSingleUniformBuffer(value, typeDecl);
    if (!write) return std::nullopt;
    const Buffer buffer = bufferFor(write->bufferSize);
    device_.writeBuffer(buffer.handle, 0, write->bytes);
    return buffer;
}

Handle WebGPUBackend::storageBuffer(const wgsl::Binding& binding, const FrameState& state) {
    const JsString& name = binding.name;
    const double width = jsv::to_number(jsv::truthy(state.screenWidth) ? state.screenWidth : Value(1280.0));
    const double height = jsv::to_number(jsv::truthy(state.screenHeight) ? state.screenHeight : Value(720.0));
    double byteSize = 0;
    if (name == u"output_buffer" || name == u"outputBuffer") {
        byteSize = width * height * 4 * 4;
    } else if (name == u"stats_buffer") {
        const double numWorkgroups = std::ceil(width / 8) * std::ceil(height / 8);
        byteSize = (2 + numWorkgroups * 2) * 4;
    } else if (contains(name, u"downsample")) {
        byteSize = std::ceil(width / 4) * std::ceil(height / 4) * 4 * 4;
    } else {
        byteSize = width * height * 4 * 4;
    }
    byteSize = jsMax(256, byteSize);
    byteSize = std::ceil(byteSize / 256) * 256;
    const auto existing = storageBuffers_.find(name);
    if (existing != storageBuffers_.end() && existing->second.size >= byteSize) return existing->second.handle;
    const Buffer buffer{device_.createBuffer(BufferDescriptor{byteSize, BufferUsage::Storage | BufferUsage::CopySrc | BufferUsage::CopyDst}), byteSize};
    if (existing != storageBuffers_.end()) retiredStorageBuffers_.push_back(existing->second);
    storageBuffers_[name] = buffer;
    return buffer.handle;
}

const TextureRecord* WebGPUBackend::surfaceTexture(const FrameState& state, const JsString& name) const {
    for (const auto& [surface, id] : state.surfaces) if (surface == name) return texture(id);
    return nullptr;
}

std::optional<Handle> WebGPUBackend::outputStorageView(const FrameState& state) {
    auto writeView = [&](const Value& writeTex) -> std::optional<Handle> {
        if (!jsv::truthy(writeTex)) return std::nullopt;
        const TextureRecord* record = textureFor(writeTex);
        if (!record) return std::nullopt;
        return record->renderView ? *record->renderView : record->view;
    };
    if (jsv::truthy(state.renderSurface)) {
        const Value* writeTex = lookup(state.writeSurfaces, jsv::to_property_key(state.renderSurface));
        if (writeTex) if (auto view = writeView(*writeTex)) return view;
    }
    const Value width = jsv::truthy(state.screenWidth) ? state.screenWidth : Value(1280.0);
    const Value height = jsv::truthy(state.screenHeight) ? state.screenHeight : Value(720.0);
    const JsString widthText = jsv::to_string(width);
    const JsString heightText = jsv::to_string(height);
    const JsString key = u"outputStorage_" + widthText + u"x" + heightText;
    if (const auto it = fallbackStorageViews_.find(key); it != fallbackStorageViews_.end()) return it->second;
    TextureDescriptor descriptor;
    descriptor.size = extent(width, height);
    descriptor.format = u"rgba16float";
    descriptor.usage = TextureUsage::StorageBinding | TextureUsage::TextureBinding | TextureUsage::CopySrc |
                       TextureUsage::CopyDst | TextureUsage::RenderAttachment;
    const Handle texture = device_.createTexture(descriptor);
    const Handle view = device_.createView(texture, {});
    fallbackStorageViews_[key] = view;
    return view;
}

std::optional<Handle> WebGPUBackend::storageTextureView(const wgsl::Binding& binding, const Value& pass, const FrameState& state) {
    const Value passStorage = jsv::member(pass, u"storageTextures");
    const Value storageTextures = jsv::truthy(passStorage) ? passStorage : Value(Object{});
    const Value textureId = jsv::member(storageTextures, binding.name);
    if (!jsv::truthy(textureId)) {
        if (binding.name == u"output_texture") return outputStorageView(state);
        return std::nullopt;
    }
    if (jsv::strict_equals(textureId, Value(u"outputTex"))) return outputStorageView(state);
    auto viewOf = [](const TextureRecord* record) -> std::optional<Handle> {
        if (!record) return std::nullopt;
        return record->renderView ? *record->renderView : record->view;
    };
    static const jsre::Regex surfacePattern(u"^o[0-7]$");
    const JsString idText = jsv::to_string(textureId);
    if (surfacePattern.test(idText)) {
        if (const Value* writeTex = lookup(state.writeSurfaces, idText); writeTex && jsv::truthy(*writeTex)) {
            if (auto view = viewOf(textureFor(*writeTex))) return view;
        }
    }
    if (const auto surfaceName = parseGlobalName(textureId)) {
        if (const Value* writeTex = lookup(state.writeSurfaces, *surfaceName); writeTex && jsv::truthy(*writeTex)) {
            if (auto view = viewOf(textureFor(*writeTex))) return view;
        }
    }
    if (auto view = viewOf(textureFor(textureId))) return view;
    return std::nullopt;
}

Handle WebGPUBackend::legacyBindGroup(const Value& pass, const ProgramInfo& program, const FrameState& state) {
    std::vector<BindGroupEntry> entries;
    double binding = 0;
    const Value inputs = jsv::member(pass, u"inputs");
    if (jsv::truthy(inputs)) {
        for (const auto& [samplerName, texId] : jsv::entries(inputs)) {
            Handle view = 0;
            if (jsv::strict_equals(texId, Value(u"none"))) {
                view = dummyTextureView_;
            } else if (const auto surfaceName = parseGlobalName(texId)) {
                if (const TextureRecord* record = surfaceTexture(state, *surfaceName)) view = record->view;
            } else if (const TextureRecord* record = textureFor(texId)) {
                view = record->view;
            }
            if (!view) view = dummyTextureView_;
            entries.push_back(BindGroupEntry{binding++, BindGroupEntry::Kind::TextureView, view});
            const TextureRecord* texRec = textureOrUnscoped(texId);
            const char16_t* legacyDefault = u"nearest";
            if (texRec && texRec->isExternal) legacyDefault = u"default";
            else if (texRec && texRec->is3D) legacyDefault = jsv::strict_equals(texRec->filter, Value(u"linear")) ? u"default" : u"nearest";
            else if (texRec && texRec->mipmaps) legacyDefault = u"mipmap";
            const Value requested = jsv::member(jsv::member(pass, u"samplerTypes"), samplerName);
            entries.push_back(BindGroupEntry{binding++, BindGroupEntry::Kind::Sampler,
                                             sampler(jsv::truthy(requested) ? requested : Value(legacyDefault))});
        }
    }
    if (jsv::truthy(jsv::member(pass, u"uniforms")) || jsv::truthy(state.globalUniforms)) {
        if (const auto buffer = uniformBuffer(pass, state, nullptr)) {
            entries.push_back(BindGroupEntry{binding++, BindGroupEntry::Kind::Buffer, *buffer});
        }
    }
    const Handle layout = device_.getBindGroupLayout(program.pipeline, 0);
    return device_.createBindGroup(layout, entries);
}

Handle WebGPUBackend::createBindGroup(const Value& pass, const ProgramInfo& program, const FrameState& state,
                                      std::optional<Handle> pipeline) {
    std::vector<const wgsl::Binding*> bindings;
    for (const wgsl::Binding& binding : program.bindings) bindings.push_back(&binding);
    const Handle targetPipeline = pipeline && *pipeline ? *pipeline : program.pipeline;
    const Value inputs = jsv::member(pass, u"inputs");

    // A multi-entry-point compute shader binds only what this pass's entry point uses.
    const Value entryPoint = jsv::member(pass, u"entryPoint");
    if (jsv::truthy(entryPoint) && program.isCompute) {
        std::vector<JsString> needed;
        auto add = [&](const JsString& name) { if (std::find(needed.begin(), needed.end(), name) == needed.end()) needed.push_back(name); };
        if (jsv::truthy(inputs)) for (const JsString& key : jsv::keys(inputs)) add(key);
        const Value outputs = jsv::member(pass, u"outputs");
        if (jsv::truthy(outputs)) for (const JsString& key : jsv::keys(outputs)) add(key);
        const std::vector<double>* usedBindings = nullptr;
        if (entryPoint.is_string()) {
            for (const auto& [name, used] : program.entryPointBindings) if (name == entryPoint.as_string()) usedBindings = &used;
        }
        const Value storageTextures = jsv::member(pass, u"storageTextures");
        if (jsv::truthy(storageTextures)) {
            for (const JsString& storageName : jsv::keys(storageTextures)) {
                const wgsl::Binding* binding = nullptr;
                for (const wgsl::Binding* b : bindings) {
                    if (b->name == storageName && b->type == u"storage_texture") { binding = b; break; }
                }
                if (binding && (!usedBindings || std::find(usedBindings->begin(), usedBindings->end(), binding->binding) != usedBindings->end())) {
                    add(storageName);
                }
            }
        }
        add(u"params");
        for (const wgsl::Binding* b : bindings) if (b->type == u"storage") add(b->name);
        std::vector<const wgsl::Binding*> kept;
        for (const wgsl::Binding* b : bindings) {
            if (std::find(needed.begin(), needed.end(), b->name) != needed.end()) kept.push_back(b);
        }
        bindings = std::move(kept);
    }

    // Input names to texture views.
    std::vector<std::pair<JsString, Handle>> textureMap;
    auto mapSet = [&](const JsString& name, Handle view) {
        for (auto& [key, value] : textureMap) if (key == name) { value = view; return; }
        textureMap.emplace_back(name, view);
    };
    auto mapGet = [&](const JsString& name) -> Handle {
        for (const auto& [key, value] : textureMap) if (key == name) return value;
        return 0;
    };
    static const jsre::Regex meshData(u"^global_mesh\\d+_(?:positions|normals|uvs)$");
    if (jsv::truthy(inputs)) {
        for (const auto& [inputName, texId] : jsv::entries(inputs)) {
            Handle view = 0;
            const auto surfaceName = parseGlobalName(texId);
            const bool isMeshData = texId.is_string() && meshData.test(unscoped(texId.as_string()));
            if (isMeshData) {
                const TextureRecord* meshTex = textureFor(texId);
                if (!meshTex) meshTex = texture(unscoped(texId.as_string()));
                view = meshTex ? meshTex->view : 0;
            } else if (surfaceName) {
                const TextureRecord* surface = surfaceTexture(state, *surfaceName);
                if (surface && surface->view) {
                    view = surface->view;
                } else {
                    const TextureRecord* record = textureFor(texId);
                    if (!record) {
                        const JsString un = unscoped(texId.as_string());
                        if (un != texId.as_string()) record = texture(un);
                    }
                    view = record ? record->view : 0;
                }
            } else if (const TextureRecord* record = textureFor(texId)) {
                view = record->view;
            }
            if (view) {
                mapSet(inputName, view);
                if (inputName == u"inputTex") {
                    mapSet(u"tex0", view);
                    mapSet(u"inputColor", view);
                }
            }
        }
    }

    // Surface inputs sample nearest, as WebGL2 does; external media linear;
    // mipmapped textures through the mipmap sampler.
    const char16_t* inputSamplerDefault = u"nearest";
    if (jsv::truthy(inputs)) {
        bool external = false, mipmapped = false;
        for (const Value& texId : jsv::values(inputs)) {
            const TextureRecord* record = textureOrUnscoped(texId);
            if (record && record->isExternal) external = true;
        }
        if (!external) {
            for (const Value& texId : jsv::values(inputs)) {
                const TextureRecord* record = textureOrUnscoped(texId);
                if (record && record->mipmaps) mipmapped = true;
            }
        }
        inputSamplerDefault = external ? u"default" : mipmapped ? u"mipmap" : u"nearest";
    }

    std::vector<BindGroupEntry> entries;
    for (const wgsl::Binding* binding : bindings) {
        if (binding->group != 0) continue;
        if (binding->type == u"texture") {
            Handle view = mapGet(binding->name);
            if (!view && startsWith(binding->name, u"tex")) {
                const double idx = parseLeadingDigits(binding->name.substr(3));
                const std::vector<JsString> inputKeys = jsv::keys(jsv::truthy(inputs) ? inputs : Value(Object{}));
                if (!std::isnan(idx) && idx < double(inputKeys.size())) view = mapGet(inputKeys[size_t(idx)]);
            }
            if (!view) view = dummyTextureView_;
            entries.push_back(BindGroupEntry{binding->binding, BindGroupEntry::Kind::TextureView, view});
        } else if (binding->type == u"sampler") {
            Value samplerType = jsv::member(jsv::member(pass, u"samplerTypes"), binding->name);
            if (!jsv::truthy(samplerType)) {
                Value sampledName;  // Map.get: undefined when absent, null when several textures
                for (const auto& [name, textureName] : program.samplerTextureNames) {
                    if (name == binding->name) sampledName = textureName ? Value(*textureName) : Value::null();
                }
                const Value texId = jsv::member(inputs, jsv::to_property_key(sampledName));
                const TextureRecord* sampled = textureOrUnscoped(texId);
                if (sampled && sampled->is3D) {
                    samplerType = Value(jsv::strict_equals(sampled->filter, Value(u"nearest")) ? u"nearest" : u"default");
                } else {
                    samplerType = Value(inputSamplerDefault);
                }
            }
            entries.push_back(BindGroupEntry{binding->binding, BindGroupEntry::Kind::Sampler, sampler(samplerType)});
        } else if (binding->type == u"uniform") {
            if (isStructUniform(binding->typeDecl)) {
                if (const auto buffer = uniformBuffer(pass, state, &program.uniforms)) {
                    entries.push_back(BindGroupEntry{binding->binding, BindGroupEntry::Kind::Buffer, *buffer});
                }
            } else {
                Value value = uniformValue(pass, state, binding->name);
                if (value.is_undefined() || value.is_null() ||
                    (!value.is_number() && !value.is_bool() && !value.is_array() && !value.is_float32_array())) {
                    value = defaultUniform(binding->typeDecl);
                }
                if (const auto buffer = singleUniformBuffer(value, binding->typeDecl)) {
                    activeUniformBuffers_.push_back(*buffer);
                    entries.push_back(BindGroupEntry{binding->binding, BindGroupEntry::Kind::Buffer, buffer->handle});
                }
            }
        } else if (binding->type == u"storage") {
            entries.push_back(BindGroupEntry{binding->binding, BindGroupEntry::Kind::Buffer, storageBuffer(*binding, state)});
        } else if (binding->type == u"storage_texture") {
            if (const auto view = storageTextureView(*binding, pass, state)) {
                entries.push_back(BindGroupEntry{binding->binding, BindGroupEntry::Kind::TextureView, *view});
            }
        }
    }

    if (bindings.empty()) {
        // No bindings parsed: a shader that declares none uses the legacy
        // layout; one whose bindings were all dead gets an empty group.
        if (!(program.module && program.sourceHasBindings)) return legacyBindGroup(pass, program, state);
        return device_.createBindGroup(device_.getBindGroupLayout(targetPipeline, 0), {});
    }
    return createBindGroupFromEntries(device_.getBindGroupLayout(targetPipeline, 0), entries);
}

namespace {
// parseDiagnosticText's bindingIndex: /binding index (\d+) not present/.
std::optional<double> bindingIndex(const JsError& error) {
    static const jsre::Regex pattern(u"binding index (\\d+) not present");
    const JsString& message = error.message();
    const auto m = pattern.exec(message);
    if (!m) return std::nullopt;
    return jsv::to_number(Value(m->str(message, 1)));
}
}  // namespace

// Retries without each entry the device names as absent from the layout, up
// to ten times, as the reference does for bindings an entry point optimized out.
Handle WebGPUBackend::createBindGroupFromEntries(Handle layout, const std::vector<BindGroupEntry>& entries) {
    try {
        return device_.createBindGroup(layout, entries);
    } catch (const DeviceError& error) {
        std::optional<double> problem = bindingIndex(error);
        if (!problem) throw;
        std::vector<BindGroupEntry> current = entries;
        for (int attempt = 0; attempt < 10; ++attempt) {
            std::vector<BindGroupEntry> kept;
            for (const BindGroupEntry& entry : current) if (entry.binding != *problem) kept.push_back(entry);
            current = std::move(kept);
            try {
                return device_.createBindGroup(layout, current);
            } catch (const DeviceError& retryError) {
                const std::optional<double> next = bindingIndex(retryError);
                if (!next) throw;
                problem = next;
            }
        }
        throw;
    }
}

}  // namespace libnoisemaker::backend
