#include "gpu/backend/backend.h"

#include "core/catalog/catalog.h"
#include "core/edit/jsv.h"
#include "core/value/js_math.h"
#include "core/value/json.h"
#include "gpu/wgsl/jsre.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
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

namespace {
// Backend's capabilities ({isMobile, floatBlend, floatLinear, colorBufferFloat,
// maxDrawBuffers, maxTextureSize, maxColorBytesPerSample, maxStateSize}).
Value capabilitiesValue(double maxTextureSize, double maxColorBytesPerSample) {
    Object capabilities;
    capabilities.set(u"isMobile", Value(false));
    capabilities.set(u"floatBlend", Value(true));
    capabilities.set(u"floatLinear", Value(true));
    capabilities.set(u"colorBufferFloat", Value(true));
    capabilities.set(u"maxDrawBuffers", Value(8.0));
    capabilities.set(u"maxTextureSize", Value(maxTextureSize));
    capabilities.set(u"maxColorBytesPerSample", Value(maxColorBytesPerSample));
    capabilities.set(u"maxStateSize", Value(2048.0));
    return Value(std::move(capabilities));
}
}  // namespace

// The base class's capabilities until init() reads the device's limits.
WebGPUBackend::WebGPUBackend(Device& device)
    : device_(device), releaser_(std::make_shared<Device*>(&device)), capabilities_(capabilitiesValue(4096, 64)) {}

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
    // detectMobile() is false off a mobile browser; `limit || fallback`.
    const auto orElse = [](double limit, double fallback) { return limit == 0 || std::isnan(limit) ? fallback : limit; };
    capabilities_ = capabilitiesValue(orElse(device_.maxTextureDimension2D(), 8192),
                                      orElse(device_.maxColorAttachmentBytesPerSample(), 32));

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
    setTexture(id, ownRecord(std::move(record)));
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
    setTexture(id, ownRecord(std::move(record)));
}

void WebGPUBackend::destroyTexture(const JsString& id) {
    const auto it = textures_.find(id);
    if (it == textures_.end()) return;
    device_.destroyTexture(it->second->handle);
    textures_.erase(it);
    textureOrder_.erase(std::find(textureOrder_.begin(), textureOrder_.end(), id));
}

// A record that releases its texture, views and mip bind groups when its
// last owner (an id here, a pipeline structure or a local) drops it, as the
// reference's garbage collector may then take them.
std::shared_ptr<TextureRecord> WebGPUBackend::ownRecord(TextureRecord record) {
    std::weak_ptr<Device*> releaser = releaser_;
    return std::shared_ptr<TextureRecord>(new TextureRecord(std::move(record)), [releaser](TextureRecord* owned) {
        if (const std::shared_ptr<Device*> device = releaser.lock()) {
            // Each object once, with no allocation: renderView is
            // mipViews[0] when the texture has mips.
            const auto isMipView = [&](Handle view) {
                return std::find(owned->mipViews.begin(), owned->mipViews.end(), view) != owned->mipViews.end();
            };
            for (const Handle group : owned->mipBindGroups) (*device)->release(group);
            for (const Handle view : owned->mipViews) (*device)->release(view);
            if (owned->renderView && !isMipView(*owned->renderView) && *owned->renderView != owned->view) {
                (*device)->release(*owned->renderView);
            }
            if (!isMipView(owned->view)) (*device)->release(owned->view);
            (*device)->release(owned->handle);
        }
        delete owned;
    });
}

void WebGPUBackend::setTexture(const JsString& id, std::shared_ptr<TextureRecord> record) {
    auto [it, inserted] = textures_.insert_or_assign(id, std::move(record));
    (void)it;
    if (inserted) textureOrder_.push_back(id);
}

std::shared_ptr<TextureRecord> WebGPUBackend::textureRecord(const JsString& id) const {
    const auto it = textures_.find(id);
    return it == textures_.end() ? nullptr : it->second;
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
    setTexture(id, ownRecord(std::move(record)));
}

void WebGPUBackend::uploadDataTexture(const JsString& id, const std::vector<float>& data, const Value& width,
                                      const Value& height) {
    auto it = textures_.find(id);
    if (it == textures_.end() || !jsv::strict_equals(it->second->width, width) ||
        !jsv::strict_equals(it->second->height, height)) {
        if (it != textures_.end()) device_.destroyTexture(it->second->handle);
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
        setTexture(id, ownRecord(std::move(record)));
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
    device_.writeTexture(TexelCopyTextureInfo{it->second->handle}, bytes, layout, extent(width, height));
}

const TextureRecord* WebGPUBackend::texture(const JsString& id) const {
    const auto it = textures_.find(id);
    return it == textures_.end() ? nullptr : it->second.get();
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
            for (const Buffer& buffer : retired) {
                device->destroyBuffer(buffer.handle);
                device->release(buffer.handle);
            }
        });
    }
}

Value WebGPUBackend::updateTextureFromPixels(const JsString& id, double width, double height,
                                             const std::vector<uint8_t>& rgba, bool flipY) {
    const auto size = [](double w, double h) {
        Object out;
        out.set(u"width", Value(w));
        out.set(u"height", Value(h));
        return Value(std::move(out));
    };
    if (width == 0 || height == 0) return size(0, 0);
    // A canvas's size is whole pixels and getImageData returns all of them.
    const auto whole = [](double v) { return v >= 0 && v <= 65536 && v == std::floor(v); };
    if (!whole(width) || !whole(height) || rgba.size() != size_t(width) * size_t(height) * 4) {
        throw JsError(u"RangeError", u"updateTextureFromPixels: the pixels are not width * height * 4 bytes");
    }
    const auto it = textures_.find(id);
    std::shared_ptr<TextureRecord> tex = it == textures_.end() ? nullptr : it->second;
    if (!tex || !jsv::strict_equals(tex->width, Value(width)) || !jsv::strict_equals(tex->height, Value(height))) {
        if (tex) device_.destroyTexture(tex->handle);
        TextureDescriptor descriptor;
        descriptor.size = Extent3D{width, height, 1};
        descriptor.format = u"rgba8unorm";
        descriptor.usage = TextureUsage::TextureBinding | TextureUsage::CopyDst | TextureUsage::RenderAttachment;
        TextureRecord record;
        record.handle = device_.createTexture(descriptor);
        record.view = device_.createView(record.handle, {});
        record.width = Value(width);
        record.height = Value(height);
        record.format = Value(u"rgba8");
        record.gpuFormat = Value(u"rgba8unorm");
        record.isExternal = true;
        tex = ownRecord(std::move(record));
        setTexture(id, tex);
    }
    const auto rowBytes = size_t(width * 4);
    std::vector<uint8_t> data = rgba;
    if (flipY) {
        for (size_t row = 0; row < size_t(height); ++row) {
            const size_t from = (size_t(height) - 1 - row) * rowBytes;
            std::copy(rgba.begin() + long(from), rgba.begin() + long(from + rowBytes), data.begin() + long(row * rowBytes));
        }
    }
    device_.writeTexture(TexelCopyTextureInfo{tex->handle}, data, TexelCopyBufferLayout{0, double(rowBytes), height},
                         Extent3D{width, height, 1});
    return size(width, height);
}

namespace {

// float16ToFloat32 (webgpu.js).
double float16ToFloat32(uint16_t h) {
    const int sign = (h >> 15) & 0x1;
    const int exponent = (h >> 10) & 0x1f;
    const int mantissa = h & 0x3ff;
    if (exponent == 0) {
        if (mantissa == 0) return sign ? -0.0 : 0.0;
        const double f = mantissa / 1024.0;
        return (sign ? -1 : 1) * f * js::pow(2, -14);
    } else if (exponent == 31) {
        if (mantissa == 0) return sign ? -std::numeric_limits<double>::infinity() : std::numeric_limits<double>::infinity();
        return std::numeric_limits<double>::quiet_NaN();
    }
    const double f = 1 + mantissa / 1024.0;
    return (sign ? -1 : 1) * f * js::pow(2, exponent - 15);
}

// Math.max(0, Math.min(255, Math.round(v * 255))) stored in a Uint8Array.
uint8_t toByte(double v) {
    const double clamped = js::max(0, js::min(255, js::round(v * 255)));
    return std::isnan(clamped) ? 0 : static_cast<uint8_t>(clamped);
}

}  // namespace

Pixels WebGPUBackend::readPixels(const JsString& textureId) {
    const auto it = textures_.find(textureId);
    if (it == textures_.end()) throw JsError(u"Error", u"Texture " + textureId + u" not found");
    const std::shared_ptr<TextureRecord> tex = it->second;
    const Handle handle = tex->handle;
    const Value width = tex->width;
    const Value height = tex->height;
    double bytesPerPixel = 4;
    if (jsv::strict_equals(tex->gpuFormat, Value(u"rgba16float"))) {
        bytesPerPixel = 8;
    } else if (jsv::strict_equals(tex->gpuFormat, Value(u"rgba32float"))) {
        bytesPerPixel = 16;
    }
    const double w = jsv::to_number(width);
    const double h = jsv::to_number(height);
    const double bytesPerRow = std::ceil(w * bytesPerPixel / 256) * 256;
    const double bufferSize = bytesPerRow * h;
    const Handle stagingBuffer = device_.createBuffer(BufferDescriptor{bufferSize, BufferUsage::CopyDst | BufferUsage::MapRead});
    const Handle commandEncoder = device_.createCommandEncoder();
    device_.copyTextureToBuffer(commandEncoder, TexelCopyTextureInfo{handle}, stagingBuffer,
                                TexelCopyBufferLayout{0, bytesPerRow, std::nullopt}, Extent3D{w, h, 1});
    const Handle commandBuffer = device_.finish(commandEncoder);
    device_.submit({commandBuffer});
    const std::vector<uint8_t> mapped = device_.mapRead(stagingBuffer);

    const double length = w * h * 4;
    Pixels out{width, height, std::vector<uint8_t>(length > 0 ? size_t(length) : 0)};
    const auto u16 = [&](double index) -> uint16_t {
        if (!(index >= 0) || (index + 1) * 2 > double(mapped.size() & ~size_t(1))) return 0;
        uint16_t value = 0;
        std::memcpy(&value, mapped.data() + size_t(index) * 2, 2);
        return value;
    };
    const auto f32 = [&](double index) -> double {
        if (!(index >= 0) || (index + 1) * 4 > double(mapped.size() & ~size_t(3))) {
            return std::numeric_limits<double>::quiet_NaN();
        }
        float value = 0;
        std::memcpy(&value, mapped.data() + size_t(index) * 4, 4);
        return double(value);
    };
    if (bytesPerPixel == 8) {
        for (double row = 0; row < h; row++) {
            const double srcRowOffset = (row * bytesPerRow) / 2;
            for (double col = 0; col < w; col++) {
                const double srcPixel = srcRowOffset + col * 4;
                const size_t dstPixel = size_t((row * w + col) * 4);
                for (int c = 0; c < 4; c++) out.data[dstPixel + size_t(c)] = toByte(float16ToFloat32(u16(srcPixel + c)));
            }
        }
    } else if (bytesPerPixel == 16) {
        for (double row = 0; row < h; row++) {
            const double srcRowOffset = (row * bytesPerRow) / 4;
            for (double col = 0; col < w; col++) {
                const double srcPixel = srcRowOffset + col * 4;
                const size_t dstPixel = size_t((row * w + col) * 4);
                for (int c = 0; c < 4; c++) out.data[dstPixel + size_t(c)] = toByte(f32(srcPixel + c));
            }
        }
    } else {
        for (double row = 0; row < h; row++) {
            const size_t srcOffset = size_t(row * bytesPerRow);
            const size_t dstOffset = size_t(row * w * 4);
            const size_t end = std::min(mapped.size(), srcOffset + size_t(w * 4));
            if (srcOffset < end) std::copy(mapped.begin() + long(srcOffset), mapped.begin() + long(end), out.data.begin() + long(dstOffset));
        }
    }
    device_.unmap(stagingBuffer);
    device_.destroyBuffer(stagingBuffer);
    device_.release(stagingBuffer);
    return out;
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

// state.surfaces?.[name]. getFrameState sets a surface only when its texture
// exists, so a surface naming a missing texture is absent. An Object.prototype
// member (toString, __proto__) is inherited by the surfaces object: present,
// with no view and no size.
const TextureRecord* WebGPUBackend::surfaceTexture(const FrameState& state, const JsString& name) const {
    for (const auto& [surface, id] : state.surfaces) {
        if (surface != name) continue;
        if (const TextureRecord* record = texture(id)) return record;
        break;
    }
    static const TextureRecord inherited;
    if (jsv::has_property(Value(Object{}), name)) return &inherited;
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

namespace {
// `throw { code, pass, ... }`: members left undefined are dropped, as JSON drops them.
[[noreturn]] void throwCode(const char16_t* code, std::initializer_list<std::pair<const char16_t*, Value>> members) {
    Object thrown;
    thrown.set(u"code", Value(code));
    for (const auto& [key, value] : members) thrown.set(key, value);
    throw ThrownValue(Value(std::move(thrown)));
}
// `${value || fallback}` in a template literal.
JsString orText(const Value& value, const char16_t* fallback) {
    return jsv::truthy(value) ? jsv::to_string(value) : JsString(fallback);
}
}  // namespace

Handle WebGPUBackend::encoder() const {
    if (!commandEncoder_) throw JsError(u"TypeError", u"Cannot read properties of null (reading 'beginRenderPass')");
    return commandEncoder_;
}

void WebGPUBackend::executePass(const Value& pass, const FrameState& state) {
    const Value programId = jsv::get(pass, u"program");
    const auto it = programId.is_string() ? programs_.find(programId.as_string()) : programs_.end();
    if (it == programs_.end()) {
        throwCode(u"ERR_PROGRAM_NOT_FOUND", {{u"pass", jsv::member(pass, u"id")}, {u"program", programId}});
    }
    if (it->second.isCompute) executeComputePass(pass, it->second, state);
    else executeRenderPass(pass, it->second, state);
}

std::optional<WebGPUBackend::Viewport> WebGPUBackend::resolveViewport(const Value& pass, const TextureRecord* tex) const {
    if (tex && jsv::truthy(tex->width) && jsv::truthy(tex->height)) return Viewport{Value(0.0), Value(0.0), tex->width, tex->height};
    Value viewport = jsv::member(pass, u"viewportResolved");
    if (!jsv::truthy(viewport)) viewport = jsv::member(pass, u"viewport");
    if (jsv::truthy(viewport)) {
        return Viewport{jsv::member(viewport, u"x"), jsv::member(viewport, u"y"), jsv::member(viewport, u"w"), jsv::member(viewport, u"h")};
    }
    return std::nullopt;  // no canvas context
}

Value WebGPUBackend::resolvePointCount(const Value& pass, const FrameState& state, const Value& outputId,
                                       const TextureRecord* outputTex) const {
    Value count = jsv::member(pass, u"count");
    if (!jsv::truthy(count)) count = Value(1000.0);
    const bool input = jsv::strict_equals(count, Value(u"input"));
    if (jsv::strict_equals(count, Value(u"auto")) || jsv::strict_equals(count, Value(u"screen")) || input) {
        const TextureRecord* refTex = nullptr;
        const Value inputs = jsv::member(pass, u"inputs");
        if (input && jsv::truthy(inputs)) {
            Value stateInputId = jsv::member(inputs, u"xyzTex");
            if (!jsv::truthy(stateInputId)) stateInputId = jsv::member(inputs, u"inputTex");
            if (jsv::truthy(stateInputId)) {
                if (const auto surfaceName = parseGlobalName(stateInputId)) refTex = surfaceTexture(state, *surfaceName);
                else refTex = textureFor(stateInputId);
            }
        } else {
            refTex = outputTex ? outputTex : textureFor(outputId);
        }
        if (refTex && jsv::truthy(refTex->width) && jsv::truthy(refTex->height)) {
            count = Value(jsv::to_number(refTex->width) * jsv::to_number(refTex->height));
        }
    }
    return count;
}

Value WebGPUBackend::resolveMeshVertexCount(const Value& pass, const FrameState& state, const TextureRecord*) const {
    Value count = jsv::member(pass, u"count");
    if (!jsv::truthy(count)) count = Value(3.0);
    const Value countUniform = jsv::member(pass, u"countUniform");
    if (jsv::truthy(countUniform)) {
        const JsString name = jsv::to_property_key(countUniform);
        Value uniformValue = jsv::member(jsv::member(pass, u"uniforms"), name);
        if (uniformValue.is_undefined()) uniformValue = jsv::member(state.globalUniforms, name);
        if (uniformValue.is_number() && uniformValue.as_number() > 0) return uniformValue;
    }
    // count === 'auto' || count === 'input' || count <= 0
    const bool lessEqualZero = count.is_string() ? jsv::to_number(count) <= 0 : jsv::to_number(count) <= 0;
    if (jsv::strict_equals(count, Value(u"auto")) || jsv::strict_equals(count, Value(u"input")) || lessEqualZero) {
        const TextureRecord* refTex = nullptr;
        const Value inputs = jsv::member(pass, u"inputs");
        if (jsv::truthy(inputs)) {
            Value meshInputId = jsv::member(inputs, u"meshPositions");
            if (!jsv::truthy(meshInputId)) meshInputId = jsv::member(inputs, u"inputTex");
            if (jsv::truthy(meshInputId)) {
                refTex = textureFor(meshInputId);
                if (!refTex) {
                    if (!meshInputId.is_string()) throw JsError(u"TypeError", u"meshInputId.replace is not a function");
                    const JsString un = unscoped(meshInputId.as_string());
                    if (un != meshInputId.as_string()) refTex = texture(un);
                }
                if (!refTex) {
                    if (const auto surfaceName = parseGlobalName(meshInputId)) refTex = surfaceTexture(state, *surfaceName);
                }
            }
        }
        if (refTex && jsv::truthy(refTex->width) && jsv::truthy(refTex->height)) {
            count = Value(jsv::to_number(refTex->width) * jsv::to_number(refTex->height));
        } else {
            count = Value(3.0);
        }
    }
    return count;
}

Handle WebGPUBackend::renderPipeline(ProgramInfo& program, const JsString& key, RenderPipelineDescriptor descriptor) {
    for (const auto& [cached, pipeline] : program.pipelineCache) if (cached == key) return pipeline;
    const Handle pipeline = device_.createRenderPipeline(descriptor);
    program.pipelineCache.emplace_back(key, pipeline);
    return pipeline;
}

Handle WebGPUBackend::resolveRenderPipeline(ProgramInfo& program, const Value& blend, const JsString& topology, const Value& format) {
    // getPipelineKey({ blend, topology, format })
    const JsString key = topology + u"|" + (jsv::truthy(blend) ? utf8_to_js(json::stringify(blend)) : JsString(u"noblend")) +
                         u"|" + orText(format, u"rgba16float");
    for (const auto& [cached, pipeline] : program.pipelineCache) if (cached == key) return pipeline;
    RenderPipelineDescriptor descriptor;
    descriptor.vertexModule = program.vertexModule ? program.vertexModule : defaultVertexModule();
    descriptor.vertexEntryPoint = program.vertexEntryPoint.empty() ? runtimeShader(u"defaultVertexEntryPoint") : program.vertexEntryPoint;
    descriptor.fragmentModule = program.fragmentModule ? program.fragmentModule : program.module;
    descriptor.fragmentEntryPoint = program.fragmentEntryPoint.empty() ? runtimeShader(u"defaultFragmentEntryPoint") : program.fragmentEntryPoint;
    const Value targetFormat = jsv::truthy(format) ? format : jsv::truthy(program.outputFormat) ? program.outputFormat : Value(u"rgba16float");
    descriptor.targets.push_back(ColorTargetState{jsv::to_string(targetFormat), resolveBlendState(blend)});
    descriptor.topology = topology;
    return renderPipeline(program, key, std::move(descriptor));
}

Handle WebGPUBackend::resolve3DRenderPipeline(ProgramInfo& program, const Value& blend, const Value& format) {
    const JsString key = u"3d|" + orText(format, u"rgba16float") + u"|" +
                         (jsv::truthy(blend) ? utf8_to_js(json::stringify(blend)) : JsString(u"noblend"));
    for (const auto& [cached, pipeline] : program.pipelineCache) if (cached == key) return pipeline;
    RenderPipelineDescriptor descriptor;
    descriptor.vertexModule = program.vertexModule ? program.vertexModule : defaultVertexModule();
    descriptor.vertexEntryPoint = program.vertexEntryPoint.empty() ? runtimeShader(u"defaultVertexEntryPoint") : program.vertexEntryPoint;
    descriptor.fragmentModule = program.fragmentModule ? program.fragmentModule : program.module;
    descriptor.fragmentEntryPoint = program.fragmentEntryPoint.empty() ? runtimeShader(u"defaultFragmentEntryPoint") : program.fragmentEntryPoint;
    const Value targetFormat = jsv::truthy(format) ? format : jsv::truthy(program.outputFormat) ? program.outputFormat : Value(u"rgba16float");
    descriptor.targets.push_back(ColorTargetState{jsv::to_string(targetFormat), resolveBlendState(blend)});
    descriptor.topology = u"triangle-list";
    descriptor.cullMode = u"back";
    descriptor.frontFace = u"cw";  // the WGSL flips Y, which reverses the winding
    descriptor.depthStencil = DepthStencilState{u"depth24plus", true, JsString(u"less")};
    return renderPipeline(program, key, std::move(descriptor));
}

Handle WebGPUBackend::resolveMRTRenderPipeline(ProgramInfo& program, const Value& blend, const JsString& topology,
                                               const std::vector<Value>& formats) {
    JsString joined;
    for (size_t i = 0; i < formats.size(); ++i) {
        if (i) joined += u"_";
        if (!formats[i].is_null() && !formats[i].is_undefined()) joined += jsv::to_string(formats[i]);
    }
    const JsString key = u"mrt_" + topology + u"_" + joined + u"_" +
                         (jsv::truthy(blend) ? utf8_to_js(json::stringify(blend)) : JsString(u"noblend"));
    for (const auto& [cached, pipeline] : program.pipelineCache) if (cached == key) return pipeline;
    RenderPipelineDescriptor descriptor;
    for (const Value& format : formats) descriptor.targets.push_back(ColorTargetState{jsv::to_string(format), resolveBlendState(blend)});
    descriptor.vertexModule = program.vertexModule ? program.vertexModule : defaultVertexModule();
    descriptor.vertexEntryPoint = program.vertexEntryPoint.empty() ? runtimeShader(u"defaultVertexEntryPoint") : program.vertexEntryPoint;
    descriptor.fragmentModule = program.fragmentModule ? program.fragmentModule : program.module;
    descriptor.fragmentEntryPoint = program.fragmentEntryPoint.empty() ? runtimeShader(u"defaultFragmentEntryPoint") : program.fragmentEntryPoint;
    descriptor.topology = topology;
    return renderPipeline(program, key, std::move(descriptor));
}

Handle WebGPUBackend::getDepthTexture(const Value& width, const Value& height) {
    if (!depthTexture_ || !jsv::strict_equals(depthTextureWidth_, width) || !jsv::strict_equals(depthTextureHeight_, height)) {
        if (depthTexture_) {
            device_.destroyTexture(depthTexture_);
            device_.release(depthTexture_);
        }
        TextureDescriptor descriptor;
        descriptor.size = extent(width, height);
        descriptor.format = u"depth24plus";
        descriptor.usage = TextureUsage::RenderAttachment;
        depthTexture_ = device_.createTexture(descriptor);
        depthTextureWidth_ = width;
        depthTextureHeight_ = height;
    }
    return depthTexture_;
}

void WebGPUBackend::drawPass(Handle passEncoder, const Value& pass, const FrameState& state, const Value& outputId,
                             const TextureRecord* tex) {
    const Value drawMode = jsv::member(pass, u"drawMode");
    double vertexCount = 3;
    if (jsv::strict_equals(drawMode, Value(u"points"))) {
        vertexCount = jsv::to_number(resolvePointCount(pass, state, outputId, tex));
    } else if (jsv::strict_equals(drawMode, Value(u"billboards"))) {
        vertexCount = jsv::to_number(resolvePointCount(pass, state, outputId, tex)) * 6;
    } else if (jsv::strict_equals(drawMode, Value(u"triangles"))) {
        vertexCount = jsv::to_number(resolveMeshVertexCount(pass, state, tex));
    }
    device_.draw(passEncoder, vertexCount, 1, 0, 0);
    device_.end(passEncoder);
}

void WebGPUBackend::executeRenderPass(const Value& pass, ProgramInfo& program, const FrameState& state) {
    const Value outputs = jsv::member(pass, u"outputs");
    const std::vector<JsString> outputKeys = jsv::keys(jsv::truthy(outputs) ? outputs : Value(Object{}));
    if (jsv::to_number(jsv::member(pass, u"drawBuffers")) > 1 || outputKeys.size() > 1) {
        executeMRTRenderPass(pass, program, state, outputKeys);
        return;
    }
    Value outputId = jsv::get(outputs, u"color");
    if (!jsv::truthy(outputId)) {
        const Array values = jsv::values(outputs);
        outputId = values.empty() ? Value() : values[0];
    }
    if (const auto surfaceName = parseGlobalName(outputId)) {
        if (const Value* writeTex = lookup(state.writeSurfaces, *surfaceName); writeTex && jsv::truthy(*writeTex)) outputId = *writeTex;
    }
    const TextureRecord* outputTex = textureFor(outputId);
    if (!outputTex) outputTex = surfaceTexture(state, jsv::to_property_key(outputId));
    if (!outputTex) {
        throwCode(u"ERR_TEXTURE_NOT_FOUND", {{u"pass", jsv::member(pass, u"id")}, {u"texture", outputId}});
    }
    const Handle targetView = outputTex->renderView ? *outputTex->renderView : outputTex->view;
    const std::optional<Viewport> viewport = resolveViewport(pass, outputTex);
    RenderPassColorAttachment color;
    color.view = targetView;
    color.loadOp = jsv::truthy(jsv::member(pass, u"clear")) ? u"clear" : u"load";
    color.storeOp = u"store";
    const Value resolvedFormat = jsv::truthy(outputTex->gpuFormat) ? outputTex->gpuFormat
        : jsv::truthy(outputTex->format) ? outputTex->format : program.outputFormat;
    const Value drawMode = jsv::member(pass, u"drawMode");
    RenderPassDescriptor descriptor;
    descriptor.colorAttachments.push_back(color);
    Handle pipeline = 0;
    if (jsv::strict_equals(drawMode, Value(u"triangles"))) {
        const Handle depthTex = getDepthTexture(outputTex->width, outputTex->height);
        RenderPassDepthStencilAttachment depth;
        depth.view = device_.createView(depthTex, {});
        depth.depthClearValue = 1.0;
        depth.depthLoadOp = u"clear";
        depth.depthStoreOp = u"store";
        descriptor.depthStencilAttachment = depth;
        pipeline = resolve3DRenderPipeline(program, jsv::member(pass, u"blend"), resolvedFormat);
    } else {
        const JsString topology = jsv::strict_equals(drawMode, Value(u"points")) ? u"point-list" : u"triangle-list";
        pipeline = resolveRenderPipeline(program, jsv::member(pass, u"blend"), topology, resolvedFormat);
    }
    const Handle passEncoder = device_.beginRenderPass(encoder(), descriptor);
    // The reference's depth view and bind group are locals of this pass.
    if (descriptor.depthStencilAttachment) device_.release(descriptor.depthStencilAttachment->view);
    const Handle bindGroup = createBindGroup(pass, program, state, pipeline);
    device_.setPipeline(passEncoder, pipeline);
    device_.setBindGroup(passEncoder, 0, bindGroup);
    device_.release(bindGroup);
    if (viewport) {
        const double x = jsv::to_number(viewport->x);
        const double y = jsv::to_number(viewport->y);
        const double w = jsv::to_number(viewport->w);
        const double h = jsv::to_number(viewport->h);
        device_.setViewport(passEncoder, x, y, w, h, 0, 1);
    }
    drawPass(passEncoder, pass, state, outputId, outputTex);
}

void WebGPUBackend::executeMRTRenderPass(const Value& pass, ProgramInfo& program, const FrameState& state,
                                         const std::vector<JsString>& outputKeys) {
    RenderPassDescriptor descriptor;
    std::vector<Value> formats;
    const TextureRecord* viewportTex = nullptr;
    const Value outputs = jsv::get(pass, u"outputs");
    const JsString loadOp = jsv::truthy(jsv::member(pass, u"clear")) ? u"clear" : u"load";
    for (const JsString& outputKey : outputKeys) {
        Value outputId = jsv::get(outputs, outputKey);
        if (const auto surfaceName = parseGlobalName(outputId)) {
            if (const Value* writeTex = lookup(state.writeSurfaces, *surfaceName); writeTex && jsv::truthy(*writeTex)) outputId = *writeTex;
        }
        const TextureRecord* tex = textureFor(outputId);
        if (!tex) tex = surfaceTexture(state, jsv::to_property_key(outputId));
        if (!tex) continue;  // the reference warns and records a missing render target
        if (!viewportTex) viewportTex = tex;
        formats.push_back(jsv::truthy(tex->gpuFormat) ? tex->gpuFormat
                          : resolveFormat(jsv::truthy(tex->format) ? tex->format : Value(u"rgba16float")));
        RenderPassColorAttachment color;
        color.view = tex->renderView ? *tex->renderView : tex->view;
        color.loadOp = loadOp;
        color.storeOp = u"store";
        descriptor.colorAttachments.push_back(color);
    }
    if (descriptor.colorAttachments.empty()) throwCode(u"ERR_NO_MRT_OUTPUTS", {{u"pass", jsv::member(pass, u"id")}});
    const Value drawMode = jsv::member(pass, u"drawMode");
    const JsString topology = jsv::strict_equals(drawMode, Value(u"points")) ? u"point-list" : u"triangle-list";
    const Handle pipeline = resolveMRTRenderPipeline(program, jsv::member(pass, u"blend"), topology, formats);
    const Handle passEncoder = device_.beginRenderPass(encoder(), descriptor);
    if (viewportTex) {
        const double w = jsv::to_number(viewportTex->width);
        const double h = jsv::to_number(viewportTex->height);
        device_.setViewport(passEncoder, 0, 0, w, h, 0, 1);
    }
    const Handle bindGroup = createBindGroup(pass, program, state, pipeline);
    device_.setPipeline(passEncoder, pipeline);
    device_.setBindGroup(passEncoder, 0, bindGroup);
    device_.release(bindGroup);
    drawPass(passEncoder, pass, state, Value::null(), viewportTex);
}

Handle WebGPUBackend::getComputePipeline(ProgramInfo& program, const Value& entryPoint) {
    const JsString target = jsv::truthy(entryPoint) ? jsv::to_string(entryPoint)
        : !program.entryPoint.empty() ? program.entryPoint : JsString(u"main");
    for (const auto& [name, pipeline] : program.pipelines) if (name == target) return pipeline;
    const Handle pipeline = device_.createComputePipeline(ComputePipelineDescriptor{program.module, target});
    program.pipelines.emplace_back(target, pipeline);
    return pipeline;
}

std::vector<Value> WebGPUBackend::resolveWorkgroups(const Value& pass, const FrameState& state) const {
    const Value workgroups = jsv::member(pass, u"workgroups");
    if (jsv::truthy(workgroups)) {
        return {jsv::get_v(workgroups, Value(0.0)), jsv::get_v(workgroups, Value(1.0)), jsv::get_v(workgroups, Value(2.0))};
    }
    const Value size = jsv::member(pass, u"size");
    if (jsv::truthy(size)) {
        // const { x = size.width, y = size.height, z = size.depth || 1 } = size
        Value x = jsv::get(size, u"x");
        if (x.is_undefined()) x = jsv::get(size, u"width");
        Value y = jsv::get(size, u"y");
        if (y.is_undefined()) y = jsv::get(size, u"height");
        Value z = jsv::get(size, u"z");
        if (z.is_undefined()) {
            z = jsv::get(size, u"depth");
            if (!jsv::truthy(z)) z = Value(1.0);
        }
        if (jsv::truthy(x) && jsv::truthy(y)) return {x, y, z};
    }
    const Value outputs = jsv::member(pass, u"outputs");
    Value outputId = jsv::member(outputs, u"color");
    if (!jsv::truthy(outputId)) {
        const Array values = jsv::values(jsv::truthy(outputs) ? outputs : Value(Object{}));
        outputId = values.empty() ? Value() : values[0];
    }
    const TextureRecord* output = jsv::truthy(outputId) ? textureFor(outputId) : nullptr;
    if (output) {
        return {Value(std::ceil(jsv::to_number(output->width) / 8)), Value(std::ceil(jsv::to_number(output->height) / 8)), Value(1.0)};
    }
    if (jsv::truthy(state.screenWidth) && jsv::truthy(state.screenHeight)) {
        return {Value(std::ceil(jsv::to_number(state.screenWidth) / 8)), Value(std::ceil(jsv::to_number(state.screenHeight) / 8)), Value(1.0)};
    }
    throwCode(u"ERR_COMPUTE_DISPATCH_UNRESOLVED", {{u"pass", jsv::member(pass, u"id")},
                                                   {u"detail", Value(u"Compute dispatch dimensions could not be inferred")}});
}

void WebGPUBackend::executeComputePass(const Value& pass, ProgramInfo& program, const FrameState& state) {
    const Handle pipeline = getComputePipeline(program, jsv::member(pass, u"entryPoint"));
    const Handle bindGroup = createBindGroup(pass, program, state, pipeline);
    const std::vector<Value> workgroups = resolveWorkgroups(pass, state);
    if (!commandEncoder_) throw JsError(u"TypeError", u"Cannot read properties of null (reading 'beginComputePass')");
    const Handle passEncoder = device_.beginComputePass(commandEncoder_);
    device_.setPipeline(passEncoder, pipeline);
    device_.setBindGroup(passEncoder, 0, bindGroup);
    device_.release(bindGroup);
    // dispatchWorkgroups(x, y = 1, z = 1)
    auto dimension = [](const Value& v) { return v.is_undefined() ? 1.0 : jsv::to_number(v); };
    const double x = jsv::to_number(workgroups[0]);
    const double y = dimension(workgroups[1]);
    const double z = dimension(workgroups[2]);
    device_.dispatchWorkgroups(passEncoder, x, y, z);
    device_.end(passEncoder);

    const wgsl::Binding* outputBuffer = nullptr;
    for (const wgsl::Binding& binding : program.bindings) {
        if ((binding.name == u"output_buffer" || binding.name == u"outputBuffer") && binding.type == u"storage") {
            outputBuffer = &binding;
            break;
        }
    }
    const Value outputs = jsv::member(pass, u"outputs");
    if (outputBuffer && jsv::truthy(outputs)) {
        Value outputId = jsv::member(outputs, u"color");
        if (!jsv::truthy(outputId)) outputId = jsv::member(outputs, u"fragColor");
        if (!jsv::truthy(outputId)) {
            const Array values = jsv::values(outputs);
            outputId = values.empty() ? Value() : values[0];
        }
        if (jsv::truthy(outputId)) copyBufferToTexture(state, outputId, outputBuffer->name);
    }
}

Handle WebGPUBackend::getBufferToTextureRenderPipeline(const Value& format) {
    const JsString key = u"bufferToTexture_" + jsv::to_string(format);
    for (const auto& [cached, pipeline] : bufferToTexturePipelines_) if (cached == key) return pipeline;
    const Handle module = device_.createShaderModule(runtimeShader(u"bufferToTexture"));
    RenderPipelineDescriptor descriptor;
    descriptor.vertexModule = module;
    descriptor.vertexEntryPoint = u"vs_main";
    descriptor.fragmentModule = module;
    descriptor.fragmentEntryPoint = u"fs_main";
    descriptor.targets.push_back(ColorTargetState{jsv::to_string(format), std::nullopt});
    const Handle pipeline = device_.createRenderPipeline(descriptor);
    bufferToTexturePipelines_.emplace_back(key, pipeline);
    return pipeline;
}

void WebGPUBackend::copyBufferToTexture(const FrameState& state, const Value& outputId, const JsString& bufferName) {
    const auto storage = storageBuffers_.find(bufferName);
    if (storage == storageBuffers_.end()) return;  // the reference warns
    const TextureRecord* outputTex = nullptr;
    if (const auto surfaceName = parseGlobalName(outputId)) {
        if (const Value* writeTex = lookup(state.writeSurfaces, *surfaceName); writeTex && jsv::truthy(*writeTex)) outputTex = textureFor(*writeTex);
    }
    if (!outputTex && jsv::strict_equals(outputId, Value(u"outputTex")) && jsv::truthy(state.renderSurface)) {
        if (const Value* writeTex = lookup(state.writeSurfaces, jsv::to_property_key(state.renderSurface)); writeTex && jsv::truthy(*writeTex)) {
            outputTex = textureFor(*writeTex);
        }
    }
    if (!outputTex) outputTex = textureFor(outputId);
    if (!outputTex) return;  // the reference warns and records a missing render target

    const Value width = jsv::truthy(state.screenWidth) ? state.screenWidth : outputTex->width;
    const Value height = jsv::truthy(state.screenHeight) ? state.screenHeight : outputTex->height;
    // new Uint32Array([width, height, 0, 0])
    auto uint32 = [](const Value& v) {
        const double n = jsv::to_number(v);
        if (!std::isfinite(n)) return uint32_t(0);
        double m = std::fmod(std::trunc(n), 4294967296.0);
        if (m < 0) m += 4294967296.0;
        return uint32_t(m);
    };
    std::vector<uint8_t> params(16, 0);
    const uint32_t w = uint32(width);
    const uint32_t h = uint32(height);
    for (int i = 0; i < 4; ++i) {
        params[i] = uint8_t(w >> (8 * i));
        params[4 + i] = uint8_t(h >> (8 * i));
    }
    const Handle paramsBuffer = device_.createBuffer(BufferDescriptor{16, BufferUsage::Uniform | BufferUsage::CopyDst});
    device_.writeBuffer(paramsBuffer, 0, params);
    const Handle pipeline = getBufferToTextureRenderPipeline(jsv::truthy(outputTex->gpuFormat) ? outputTex->gpuFormat : Value(u"rgba8unorm"));
    const Handle layout = device_.getBindGroupLayout(pipeline, 0);
    const Handle bindGroup = device_.createBindGroup(layout, {BindGroupEntry{0, BindGroupEntry::Kind::Buffer, storage->second.handle},
                                                             BindGroupEntry{1, BindGroupEntry::Kind::Buffer, paramsBuffer}});
    RenderPassDescriptor descriptor;
    RenderPassColorAttachment color;
    color.view = outputTex->renderView ? *outputTex->renderView : outputTex->view;
    color.loadOp = u"clear";
    color.storeOp = u"store";
    color.clearValue = Color{0, 0, 0, 1};
    descriptor.colorAttachments.push_back(color);
    const Handle passEncoder = device_.beginRenderPass(encoder(), descriptor);
    device_.setPipeline(passEncoder, pipeline);
    device_.setBindGroup(passEncoder, 0, bindGroup);
    device_.release(bindGroup);
    device_.draw(passEncoder, 3, 1, 0, 0);
    device_.end(passEncoder);
    activeUniformBuffers_.push_back(Buffer{paramsBuffer, 16});
}

namespace {
// mipLevelSize(dim, level): max(1, floor(dim / 2^level)).
double mipLevelSize(const Value& dim, double level) {
    const double size = std::floor(jsv::to_number(dim) / std::pow(2.0, level));
    return std::isnan(size) ? std::nan("") : std::max(1.0, size);
}
std::vector<uint8_t> floatBytes(const std::vector<float>& values) {
    std::vector<uint8_t> bytes(values.size() * 4);
    if (!values.empty()) std::memcpy(bytes.data(), values.data(), bytes.size());
    return bytes;
}
}  // namespace

void WebGPUBackend::uploadCubeFace(const JsString& id, const Value& face, const Value& width, const Value& height,
                                   const std::vector<uint8_t>& data) {
    const TextureRecord* tex = texture(id);
    if (!tex) throw JsError(u"TypeError", u"Cannot read properties of undefined (reading 'handle')");
    const double w = jsv::to_number(width);
    const double h = jsv::to_number(height);
    const double unalignedBytesPerRow = w * 4;
    const double bytesPerRow = std::ceil(unalignedBytesPerRow / 256) * 256;
    std::vector<uint8_t> upload = data;
    if (bytesPerRow != unalignedBytesPerRow) {
        upload.assign(size_t(bytesPerRow * h), 0);
        const size_t src = size_t(unalignedBytesPerRow);
        for (size_t y = 0; y < size_t(h); ++y) {
            for (size_t x = 0; x < src && y * src + x < data.size(); ++x) upload[y * size_t(bytesPerRow) + x] = data[y * src + x];
        }
    }
    TexelCopyTextureInfo destination{tex->handle};
    destination.origin = {0, 0, jsv::to_number(face)};
    TexelCopyBufferLayout layout;
    layout.bytesPerRow = bytesPerRow;
    layout.rowsPerImage = h;
    device_.writeTexture(destination, upload, layout, Extent3D{w, h, 1});
}

// Rows padded to 256 bytes, as uploadDataTexture and uploadMeshData write them.
void WebGPUBackend::writeTextureRows(Handle textureHandle, const std::vector<float>& data, const Value& width,
                                     const Value& height, double bytesPerPixel) {
    const double w = jsv::to_number(width);
    const double h = jsv::to_number(height);
    const double unalignedBytesPerRow = w * bytesPerPixel;
    const double bytesPerRow = std::ceil(unalignedBytesPerRow / 256) * 256;
    std::vector<float> upload = data;
    if (bytesPerRow != unalignedBytesPerRow) {
        const size_t srcRowFloats = size_t(w * 4);
        const size_t dstRowFloats = size_t(bytesPerRow / 4);
        upload.assign(dstRowFloats * size_t(h), 0.0f);
        for (size_t y = 0; y < size_t(h); ++y) {
            for (size_t x = 0; x < srcRowFloats && y * srcRowFloats + x < data.size(); ++x) {
                upload[y * dstRowFloats + x] = data[y * srcRowFloats + x];
            }
        }
    }
    TexelCopyBufferLayout layout;
    layout.bytesPerRow = bytesPerRow;
    layout.rowsPerImage = h;
    device_.writeTexture(TexelCopyTextureInfo{textureHandle}, floatBytes(upload), layout, extent(width, height));
}

Value WebGPUBackend::uploadMeshData(const JsString& meshId, const std::vector<float>& positions,
                                    const std::vector<float>& normals, const std::vector<float>& uvs,
                                    const Value& width, const Value& height, const Value& vertexCount) {
    auto upload = [&](const JsString& id, const std::vector<float>& data) {
        auto it = textures_.find(id);
        if (it == textures_.end() || !jsv::strict_equals(it->second->width, width) ||
            !jsv::strict_equals(it->second->height, height)) {
            if (it != textures_.end()) device_.destroyTexture(it->second->handle);
            TextureDescriptor descriptor;
            descriptor.size = extent(width, height);
            descriptor.format = u"rgba32float";
            descriptor.usage = TextureUsage::TextureBinding | TextureUsage::CopyDst | TextureUsage::RenderAttachment |
                               TextureUsage::StorageBinding;
            TextureRecord record;
            record.handle = device_.createTexture(descriptor);
            record.view = device_.createView(record.handle, {});
            record.width = width;
            record.height = height;
            record.format = Value(u"rgba32f");
            record.gpuFormat = Value(u"rgba32float");
            setTexture(id, ownRecord(std::move(record)));
            it = textures_.find(id);
        }
        writeTextureRows(it->second->handle, data, width, height, 16);
    };
    upload(u"global_" + meshId + u"_positions", positions);
    upload(u"global_" + meshId + u"_normals", normals);
    upload(u"global_" + meshId + u"_uvs", uvs);
    Object result;
    result.set(u"success", Value(true));
    result.set(u"vertexCount", vertexCount);
    return Value(std::move(result));
}

void WebGPUBackend::copyTexture(const JsString& srcId, const JsString& dstId) {
    const TextureRecord* srcTex = texture(srcId);
    const TextureRecord* dstTex = texture(dstId);
    if (!srcTex || !dstTex) return;  // the reference warns
    if (!jsv::strict_equals(srcTex->width, dstTex->width) || !jsv::strict_equals(srcTex->height, dstTex->height) ||
        srcTex->mipLevels > 1 || dstTex->mipLevels > 1) {
        copyTextureScaled(srcId, dstId);
        return;
    }
    const Handle commandEncoder = device_.createCommandEncoder();
    device_.copyTextureToTexture(commandEncoder, TexelCopyTextureInfo{srcTex->handle}, TexelCopyTextureInfo{dstTex->handle},
                                 extent(srcTex->width, srcTex->height));
    const Handle commandBuffer = device_.finish(commandEncoder);
    device_.submit({commandBuffer});
}

Handle WebGPUBackend::getResamplePipeline(const Value& gpuFormat, const JsString& entryPoint) {
    const JsString key = jsv::to_string(gpuFormat) + u"|" + entryPoint;
    for (const auto& [cached, pipeline] : resamplePipelines_) if (cached == key) return pipeline;
    if (!resampleModule_) resampleModule_ = device_.createShaderModule(runtimeShader(u"resample"));
    RenderPipelineDescriptor descriptor;
    descriptor.vertexModule = resampleModule_;
    descriptor.vertexEntryPoint = u"vs";
    descriptor.fragmentModule = resampleModule_;
    descriptor.fragmentEntryPoint = entryPoint;
    descriptor.targets.push_back(ColorTargetState{jsv::to_string(gpuFormat), std::nullopt});
    const Handle pipeline = device_.createRenderPipeline(descriptor);
    resamplePipelines_.emplace_back(key, pipeline);
    return pipeline;
}

void WebGPUBackend::copyTextureScaled(const JsString& srcId, const JsString& dstId) {
    const TextureRecord* srcTex = texture(srcId);
    const TextureRecord* dstTex = texture(dstId);
    if (!srcTex || !dstTex || dstTex->is3D) return;  // the reference warns
    const Handle srcView = srcTex->renderView ? *srcTex->renderView : srcTex->view;
    const Value dstFormat = jsv::truthy(dstTex->gpuFormat) ? dstTex->gpuFormat
        : resolveFormat(jsv::truthy(dstTex->format) ? dstTex->format : Value(u"rgba16float"));
    const Handle pipeline = getResamplePipeline(dstFormat, u"fsScale");
    const Handle dimsBuffer = device_.createBuffer(BufferDescriptor{16, BufferUsage::Uniform | BufferUsage::CopyDst});
    // new Float32Array([srcWidth, srcHeight, dstWidth, dstHeight])
    std::vector<uint8_t> dims;
    for (const Value* v : {&srcTex->width, &srcTex->height, &dstTex->width, &dstTex->height}) {
        const float f = static_cast<float>(jsv::to_number(*v));
        uint32_t bits = 0;
        std::memcpy(&bits, &f, 4);
        if (std::isnan(f)) bits = 0x7FC00000u;
        for (int i = 0; i < 4; ++i) dims.push_back(uint8_t(bits >> (8 * i)));
    }
    device_.writeBuffer(dimsBuffer, 0, dims);
    const Handle commandEncoder = device_.createCommandEncoder();
    RenderPassDescriptor descriptor;
    RenderPassColorAttachment color;
    color.view = dstTex->renderView ? *dstTex->renderView : dstTex->view;
    color.loadOp = u"clear";
    color.storeOp = u"store";
    descriptor.colorAttachments.push_back(color);
    const Handle renderPass = device_.beginRenderPass(commandEncoder, descriptor);
    device_.setPipeline(renderPass, pipeline);
    const Handle layout = device_.getBindGroupLayout(pipeline, 0);
    const Handle bindGroup = device_.createBindGroup(layout, {BindGroupEntry{0, BindGroupEntry::Kind::TextureView, srcView},
                                                             BindGroupEntry{1, BindGroupEntry::Kind::Buffer, dimsBuffer}});
    device_.setBindGroup(renderPass, 0, bindGroup);
    device_.release(bindGroup);
    device_.draw(renderPass, 3, 1, 0, 0);
    device_.end(renderPass);
    const Handle commandBuffer = device_.finish(commandEncoder);
    device_.submit({commandBuffer});
    device_.destroyBuffer(dimsBuffer);
    device_.release(dimsBuffer);
}

void WebGPUBackend::generateMipmaps(const std::vector<JsString>& ids) {
    for (const JsString& id : ids) {
        const auto it = textures_.find(id);
        if (it == textures_.end()) continue;
        TextureRecord& tex = *it->second;
        if (!tex.mipmaps || tex.is3D || tex.mipViews.empty()) continue;
        const Value dstFormat = jsv::truthy(tex.gpuFormat) ? tex.gpuFormat
            : resolveFormat(jsv::truthy(tex.format) ? tex.format : Value(u"rgba16float"));
        const Handle pipeline = getResamplePipeline(dstFormat, u"fsMip");
        for (double level = 1; level < tex.mipLevels; ++level) {
            const size_t l = size_t(level);
            if (tex.mipBindGroups.empty()) tex.mipBindGroups.assign(size_t(tex.mipLevels), 0);
            Handle bindGroup = tex.mipBindGroups[l];
            if (!bindGroup) {
                const Handle layout = device_.getBindGroupLayout(pipeline, 0);
                bindGroup = device_.createBindGroup(layout, {BindGroupEntry{0, BindGroupEntry::Kind::TextureView, tex.mipViews[l - 1]}});
                tex.mipBindGroups[l] = bindGroup;
            }
            const Handle commandEncoder = device_.createCommandEncoder();
            RenderPassDescriptor descriptor;
            RenderPassColorAttachment color;
            color.view = tex.mipViews[l];
            color.loadOp = u"clear";
            color.storeOp = u"store";
            descriptor.colorAttachments.push_back(color);
            const Handle renderPass = device_.beginRenderPass(commandEncoder, descriptor);
            device_.setPipeline(renderPass, pipeline);
            const double w = mipLevelSize(tex.width, level);
            const double h = mipLevelSize(tex.height, level);
            device_.setViewport(renderPass, 0, 0, w, h, 0, 1);
            device_.setBindGroup(renderPass, 0, bindGroup);
            device_.draw(renderPass, 3, 1, 0, 0);
            device_.end(renderPass);
            const Handle commandBuffer = device_.finish(commandEncoder);
            device_.submit({commandBuffer});
        }
    }
}

void WebGPUBackend::clearTexture(const JsString& id) {
    const TextureRecord* tex = texture(id);
    if (!tex) return;
    const Handle commandEncoder = device_.createCommandEncoder();
    RenderPassDescriptor descriptor;
    RenderPassColorAttachment color;
    color.view = tex->renderView ? *tex->renderView : tex->view;
    color.loadOp = u"clear";
    color.storeOp = u"store";
    descriptor.colorAttachments.push_back(color);
    const Handle renderPass = device_.beginRenderPass(commandEncoder, descriptor);
    device_.end(renderPass);
    const Handle commandBuffer = device_.finish(commandEncoder);
    device_.submit({commandBuffer});
}

}  // namespace libnoisemaker::backend
