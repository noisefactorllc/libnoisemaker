#include "gpu/dawn_device.h"

#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>

namespace libnoisemaker::gpu {

namespace {

using backend::Handle;

[[noreturn]] void type_error(const std::string& message) { throw JsError(u"TypeError", utf8_to_js(message)); }

// WebGPU's IDL conversions: [EnforceRange] integers truncate and throw
// outside their range; float members throw when not finite.
uint32_t to_u32(double value, const char* what) {
    if (!std::isfinite(value)) type_error(std::string(what) + ": value is not a finite number");
    const double whole = std::trunc(value);
    if (whole < 0 || whole > 4294967295.0) type_error(std::string(what) + ": value is outside the 'unsigned long' value range");
    return static_cast<uint32_t>(whole);
}

uint64_t to_u64(double value, const char* what) {
    if (!std::isfinite(value)) type_error(std::string(what) + ": value is not a finite number");
    const double whole = std::trunc(value);
    if (whole < 0 || whole > 9007199254740991.0) {
        type_error(std::string(what) + ": value is outside the 'unsigned long long' value range");
    }
    return static_cast<uint64_t>(whole);
}

float to_float(double value, const char* what) {
    const auto single = static_cast<float>(value);
    if (!std::isfinite(single)) type_error(std::string(what) + ": value is not a finite floating-point value");
    return single;
}

// A string member as UTF-8, alive while the descriptor is used.
struct Utf8 {
    explicit Utf8(const JsString& text) : bytes(js_to_utf8(text)) {}
    WGPUStringView view() const { return WGPUStringView{bytes.data(), bytes.size()}; }
    std::string bytes;
};

std::string to_string(WGPUStringView view) {
    if (view.data == nullptr) return {};
    if (view.length == WGPU_STRLEN) return std::string(view.data);
    return std::string(view.data, view.length);
}

template <typename E>
E lookup(const std::unordered_map<JsString, E>& table, const JsString& value, const char* type) {
    const auto it = table.find(value);
    if (it == table.end()) {
        type_error("The provided value '" + js_to_utf8(value) + "' is not a valid enum value of type " + type);
    }
    return it->second;
}

WGPUTextureFormat texture_format(const JsString& value) {
    static const std::unordered_map<JsString, WGPUTextureFormat> table{
        {u"r8unorm", WGPUTextureFormat_R8Unorm},
        {u"r8snorm", WGPUTextureFormat_R8Snorm},
        {u"r8uint", WGPUTextureFormat_R8Uint},
        {u"r8sint", WGPUTextureFormat_R8Sint},
        {u"r16unorm", WGPUTextureFormat_R16Unorm},
        {u"r16snorm", WGPUTextureFormat_R16Snorm},
        {u"r16uint", WGPUTextureFormat_R16Uint},
        {u"r16sint", WGPUTextureFormat_R16Sint},
        {u"r16float", WGPUTextureFormat_R16Float},
        {u"rg8unorm", WGPUTextureFormat_RG8Unorm},
        {u"rg8snorm", WGPUTextureFormat_RG8Snorm},
        {u"rg8uint", WGPUTextureFormat_RG8Uint},
        {u"rg8sint", WGPUTextureFormat_RG8Sint},
        {u"r32uint", WGPUTextureFormat_R32Uint},
        {u"r32sint", WGPUTextureFormat_R32Sint},
        {u"r32float", WGPUTextureFormat_R32Float},
        {u"rg16unorm", WGPUTextureFormat_RG16Unorm},
        {u"rg16snorm", WGPUTextureFormat_RG16Snorm},
        {u"rg16uint", WGPUTextureFormat_RG16Uint},
        {u"rg16sint", WGPUTextureFormat_RG16Sint},
        {u"rg16float", WGPUTextureFormat_RG16Float},
        {u"rgba8unorm", WGPUTextureFormat_RGBA8Unorm},
        {u"rgba8unorm-srgb", WGPUTextureFormat_RGBA8UnormSrgb},
        {u"rgba8snorm", WGPUTextureFormat_RGBA8Snorm},
        {u"rgba8uint", WGPUTextureFormat_RGBA8Uint},
        {u"rgba8sint", WGPUTextureFormat_RGBA8Sint},
        {u"bgra8unorm", WGPUTextureFormat_BGRA8Unorm},
        {u"bgra8unorm-srgb", WGPUTextureFormat_BGRA8UnormSrgb},
        {u"rgb9e5ufloat", WGPUTextureFormat_RGB9E5Ufloat},
        {u"rgb10a2uint", WGPUTextureFormat_RGB10A2Uint},
        {u"rgb10a2unorm", WGPUTextureFormat_RGB10A2Unorm},
        {u"rg11b10ufloat", WGPUTextureFormat_RG11B10Ufloat},
        {u"rg32uint", WGPUTextureFormat_RG32Uint},
        {u"rg32sint", WGPUTextureFormat_RG32Sint},
        {u"rg32float", WGPUTextureFormat_RG32Float},
        {u"rgba16unorm", WGPUTextureFormat_RGBA16Unorm},
        {u"rgba16snorm", WGPUTextureFormat_RGBA16Snorm},
        {u"rgba16uint", WGPUTextureFormat_RGBA16Uint},
        {u"rgba16sint", WGPUTextureFormat_RGBA16Sint},
        {u"rgba16float", WGPUTextureFormat_RGBA16Float},
        {u"rgba32uint", WGPUTextureFormat_RGBA32Uint},
        {u"rgba32sint", WGPUTextureFormat_RGBA32Sint},
        {u"rgba32float", WGPUTextureFormat_RGBA32Float},
        {u"stencil8", WGPUTextureFormat_Stencil8},
        {u"depth16unorm", WGPUTextureFormat_Depth16Unorm},
        {u"depth24plus", WGPUTextureFormat_Depth24Plus},
        {u"depth24plus-stencil8", WGPUTextureFormat_Depth24PlusStencil8},
        {u"depth32float", WGPUTextureFormat_Depth32Float},
        {u"depth32float-stencil8", WGPUTextureFormat_Depth32FloatStencil8},
    };
    return lookup(table, value, "GPUTextureFormat");
}

WGPUTextureDimension texture_dimension(const JsString& value) {
    static const std::unordered_map<JsString, WGPUTextureDimension> table{
        {u"1d", WGPUTextureDimension_1D}, {u"2d", WGPUTextureDimension_2D}, {u"3d", WGPUTextureDimension_3D}};
    return lookup(table, value, "GPUTextureDimension");
}

WGPUTextureViewDimension view_dimension(const JsString& value) {
    static const std::unordered_map<JsString, WGPUTextureViewDimension> table{
        {u"1d", WGPUTextureViewDimension_1D},          {u"2d", WGPUTextureViewDimension_2D},
        {u"2d-array", WGPUTextureViewDimension_2DArray}, {u"cube", WGPUTextureViewDimension_Cube},
        {u"cube-array", WGPUTextureViewDimension_CubeArray}, {u"3d", WGPUTextureViewDimension_3D}};
    return lookup(table, value, "GPUTextureViewDimension");
}

WGPUTextureAspect texture_aspect(const JsString& value) {
    static const std::unordered_map<JsString, WGPUTextureAspect> table{{u"all", WGPUTextureAspect_All},
                                                                       {u"stencil-only", WGPUTextureAspect_StencilOnly},
                                                                       {u"depth-only", WGPUTextureAspect_DepthOnly}};
    return lookup(table, value, "GPUTextureAspect");
}

WGPUAddressMode address_mode(const JsString& value) {
    static const std::unordered_map<JsString, WGPUAddressMode> table{{u"clamp-to-edge", WGPUAddressMode_ClampToEdge},
                                                                     {u"repeat", WGPUAddressMode_Repeat},
                                                                     {u"mirror-repeat", WGPUAddressMode_MirrorRepeat}};
    return lookup(table, value, "GPUAddressMode");
}

WGPUFilterMode filter_mode(const JsString& value) {
    static const std::unordered_map<JsString, WGPUFilterMode> table{{u"nearest", WGPUFilterMode_Nearest},
                                                                    {u"linear", WGPUFilterMode_Linear}};
    return lookup(table, value, "GPUFilterMode");
}

WGPUMipmapFilterMode mipmap_filter_mode(const JsString& value) {
    static const std::unordered_map<JsString, WGPUMipmapFilterMode> table{{u"nearest", WGPUMipmapFilterMode_Nearest},
                                                                          {u"linear", WGPUMipmapFilterMode_Linear}};
    return lookup(table, value, "GPUMipmapFilterMode");
}

WGPUCompareFunction compare_function(const JsString& value) {
    static const std::unordered_map<JsString, WGPUCompareFunction> table{
        {u"never", WGPUCompareFunction_Never},          {u"less", WGPUCompareFunction_Less},
        {u"equal", WGPUCompareFunction_Equal},          {u"less-equal", WGPUCompareFunction_LessEqual},
        {u"greater", WGPUCompareFunction_Greater},      {u"not-equal", WGPUCompareFunction_NotEqual},
        {u"greater-equal", WGPUCompareFunction_GreaterEqual}, {u"always", WGPUCompareFunction_Always}};
    return lookup(table, value, "GPUCompareFunction");
}

WGPUBlendOperation blend_operation(const JsString& value) {
    static const std::unordered_map<JsString, WGPUBlendOperation> table{
        {u"add", WGPUBlendOperation_Add}, {u"subtract", WGPUBlendOperation_Subtract},
        {u"reverse-subtract", WGPUBlendOperation_ReverseSubtract}, {u"min", WGPUBlendOperation_Min},
        {u"max", WGPUBlendOperation_Max}};
    return lookup(table, value, "GPUBlendOperation");
}

WGPUBlendFactor blend_factor(const JsString& value) {
    static const std::unordered_map<JsString, WGPUBlendFactor> table{
        {u"zero", WGPUBlendFactor_Zero},
        {u"one", WGPUBlendFactor_One},
        {u"src", WGPUBlendFactor_Src},
        {u"one-minus-src", WGPUBlendFactor_OneMinusSrc},
        {u"src-alpha", WGPUBlendFactor_SrcAlpha},
        {u"one-minus-src-alpha", WGPUBlendFactor_OneMinusSrcAlpha},
        {u"dst", WGPUBlendFactor_Dst},
        {u"one-minus-dst", WGPUBlendFactor_OneMinusDst},
        {u"dst-alpha", WGPUBlendFactor_DstAlpha},
        {u"one-minus-dst-alpha", WGPUBlendFactor_OneMinusDstAlpha},
        {u"src-alpha-saturated", WGPUBlendFactor_SrcAlphaSaturated},
        {u"constant", WGPUBlendFactor_Constant},
        {u"one-minus-constant", WGPUBlendFactor_OneMinusConstant},
        {u"src1", WGPUBlendFactor_Src1},
        {u"one-minus-src1", WGPUBlendFactor_OneMinusSrc1},
        {u"src1-alpha", WGPUBlendFactor_Src1Alpha},
        {u"one-minus-src1-alpha", WGPUBlendFactor_OneMinusSrc1Alpha},
    };
    return lookup(table, value, "GPUBlendFactor");
}

WGPUPrimitiveTopology primitive_topology(const JsString& value) {
    static const std::unordered_map<JsString, WGPUPrimitiveTopology> table{
        {u"point-list", WGPUPrimitiveTopology_PointList}, {u"line-list", WGPUPrimitiveTopology_LineList},
        {u"line-strip", WGPUPrimitiveTopology_LineStrip}, {u"triangle-list", WGPUPrimitiveTopology_TriangleList},
        {u"triangle-strip", WGPUPrimitiveTopology_TriangleStrip}};
    return lookup(table, value, "GPUPrimitiveTopology");
}

WGPUFrontFace front_face(const JsString& value) {
    static const std::unordered_map<JsString, WGPUFrontFace> table{{u"ccw", WGPUFrontFace_CCW}, {u"cw", WGPUFrontFace_CW}};
    return lookup(table, value, "GPUFrontFace");
}

WGPUCullMode cull_mode(const JsString& value) {
    static const std::unordered_map<JsString, WGPUCullMode> table{
        {u"none", WGPUCullMode_None}, {u"front", WGPUCullMode_Front}, {u"back", WGPUCullMode_Back}};
    return lookup(table, value, "GPUCullMode");
}

WGPULoadOp load_op(const JsString& value) {
    static const std::unordered_map<JsString, WGPULoadOp> table{{u"load", WGPULoadOp_Load}, {u"clear", WGPULoadOp_Clear}};
    return lookup(table, value, "GPULoadOp");
}

WGPUStoreOp store_op(const JsString& value) {
    static const std::unordered_map<JsString, WGPUStoreOp> table{{u"store", WGPUStoreOp_Store},
                                                                 {u"discard", WGPUStoreOp_Discard}};
    return lookup(table, value, "GPUStoreOp");
}

WGPUExtent3D extent(const backend::Extent3D& size) {
    return WGPUExtent3D{to_u32(size.width, "width"), to_u32(size.height, "height"),
                        to_u32(size.depthOrArrayLayers, "depthOrArrayLayers")};
}

WGPUBlendComponent blend_component(const backend::BlendComponent& component) {
    WGPUBlendComponent out = WGPU_BLEND_COMPONENT_INIT;
    out.operation = blend_operation(component.operation);
    out.srcFactor = blend_factor(component.srcFactor);
    out.dstFactor = blend_factor(component.dstFactor);
    return out;
}

// A callback's std::function, deleted after it runs or is cancelled.
struct WorkDone {
    std::function<void()> done;
};

}  // namespace

DawnDevice::DawnDevice(gpu::Device& device) : device_(device) {
    WGPULimits limits = WGPU_LIMITS_INIT;
    if (wgpuDeviceGetLimits(device_.device, &limits) == WGPUStatus_Success) {
        maxTextureDimension2D_ = double(limits.maxTextureDimension2D);
        maxColorAttachmentBytesPerSample_ = double(limits.maxColorAttachmentBytesPerSample);
    }
}

DawnDevice::~DawnDevice() {
    // Callbacks of pending work refer to this device: run them first.
    waitIdle();
    for (Object& object : objects_) release(object);
}

Handle DawnDevice::add(Kind kind, void* handle, uint64_t size) {
    if (!free_.empty()) {
        const Handle slot = free_.back();
        free_.pop_back();
        objects_[slot - 1] = Object{kind, handle, size};
        return slot;
    }
    if (objects_.size() >= std::numeric_limits<Handle>::max()) throw JsError(u"RangeError", u"too many WebGPU objects");
    objects_.push_back(Object{kind, handle, size});
    return Handle(objects_.size());
}

void* DawnDevice::get(Handle handle, Kind kind, const char* what) const {
    if (handle == 0 || handle > objects_.size()) type_error(std::string(what) + ": no object");
    const Object& object = objects_[handle - 1];
    if (object.kind != kind) type_error(std::string(what) + ": the object is not of the expected type or was used up");
    return object.handle;
}

void DawnDevice::release(Object& object) {
    void* h = object.handle;
    switch (object.kind) {
        case Kind::Released: break;
        case Kind::Texture: wgpuTextureRelease(static_cast<WGPUTexture>(h)); break;
        case Kind::View: wgpuTextureViewRelease(static_cast<WGPUTextureView>(h)); break;
        case Kind::Sampler: wgpuSamplerRelease(static_cast<WGPUSampler>(h)); break;
        case Kind::Buffer: wgpuBufferRelease(static_cast<WGPUBuffer>(h)); break;
        case Kind::ShaderModule: wgpuShaderModuleRelease(static_cast<WGPUShaderModule>(h)); break;
        case Kind::RenderPipeline: wgpuRenderPipelineRelease(static_cast<WGPURenderPipeline>(h)); break;
        case Kind::ComputePipeline: wgpuComputePipelineRelease(static_cast<WGPUComputePipeline>(h)); break;
        case Kind::BindGroupLayout: wgpuBindGroupLayoutRelease(static_cast<WGPUBindGroupLayout>(h)); break;
        case Kind::BindGroup: wgpuBindGroupRelease(static_cast<WGPUBindGroup>(h)); break;
        case Kind::CommandEncoder: wgpuCommandEncoderRelease(static_cast<WGPUCommandEncoder>(h)); break;
        case Kind::RenderPass: wgpuRenderPassEncoderRelease(static_cast<WGPURenderPassEncoder>(h)); break;
        case Kind::ComputePass: wgpuComputePassEncoderRelease(static_cast<WGPUComputePassEncoder>(h)); break;
        case Kind::CommandBuffer: wgpuCommandBufferRelease(static_cast<WGPUCommandBuffer>(h)); break;
    }
    object.kind = Kind::Released;
    object.handle = nullptr;
}

Handle DawnDevice::createTexture(const backend::TextureDescriptor& descriptor) {
    WGPUTextureDescriptor desc = WGPU_TEXTURE_DESCRIPTOR_INIT;
    desc.size = extent(descriptor.size);
    desc.mipLevelCount = to_u32(descriptor.mipLevelCount, "mipLevelCount");
    desc.sampleCount = to_u32(descriptor.sampleCount, "sampleCount");
    desc.dimension = texture_dimension(descriptor.dimension);
    desc.format = texture_format(descriptor.format);
    desc.usage = descriptor.usage;
    return add(Kind::Texture, wgpuDeviceCreateTexture(device_.device, &desc));
}

Handle DawnDevice::createView(Handle texture, const backend::TextureViewDescriptor& descriptor) {
    const auto tex = as<WGPUTexture>(texture, Kind::Texture, "createView");
    WGPUTextureViewDescriptor desc = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
    if (descriptor.format) desc.format = texture_format(*descriptor.format);
    if (descriptor.dimension) desc.dimension = view_dimension(*descriptor.dimension);
    desc.aspect = texture_aspect(descriptor.aspect);
    desc.baseMipLevel = to_u32(descriptor.baseMipLevel, "baseMipLevel");
    if (descriptor.mipLevelCount) desc.mipLevelCount = to_u32(*descriptor.mipLevelCount, "mipLevelCount");
    desc.baseArrayLayer = to_u32(descriptor.baseArrayLayer, "baseArrayLayer");
    if (descriptor.arrayLayerCount) desc.arrayLayerCount = to_u32(*descriptor.arrayLayerCount, "arrayLayerCount");
    return add(Kind::View, wgpuTextureCreateView(tex, &desc));
}

void DawnDevice::destroyTexture(Handle texture) {
    wgpuTextureDestroy(as<WGPUTexture>(texture, Kind::Texture, "destroy"));
}

Handle DawnDevice::createSampler(const backend::SamplerDescriptor& descriptor) {
    WGPUSamplerDescriptor desc = WGPU_SAMPLER_DESCRIPTOR_INIT;
    desc.addressModeU = address_mode(descriptor.addressModeU);
    desc.addressModeV = address_mode(descriptor.addressModeV);
    desc.addressModeW = address_mode(descriptor.addressModeW);
    desc.magFilter = filter_mode(descriptor.magFilter);
    desc.minFilter = filter_mode(descriptor.minFilter);
    desc.mipmapFilter = mipmap_filter_mode(descriptor.mipmapFilter);
    desc.lodMinClamp = to_float(descriptor.lodMinClamp, "lodMinClamp");
    desc.lodMaxClamp = to_float(descriptor.lodMaxClamp, "lodMaxClamp");
    if (descriptor.compare) desc.compare = compare_function(*descriptor.compare);
    const uint32_t anisotropy = to_u32(descriptor.maxAnisotropy, "maxAnisotropy");
    if (anisotropy > 65535) type_error("maxAnisotropy: value is outside the 'unsigned short' value range");
    desc.maxAnisotropy = static_cast<uint16_t>(anisotropy);
    return add(Kind::Sampler, wgpuDeviceCreateSampler(device_.device, &desc));
}

Handle DawnDevice::createBuffer(const backend::BufferDescriptor& descriptor) {
    WGPUBufferDescriptor desc = WGPU_BUFFER_DESCRIPTOR_INIT;
    desc.size = to_u64(descriptor.size, "size");
    desc.usage = descriptor.usage;
    desc.mappedAtCreation = descriptor.mappedAtCreation ? WGPU_TRUE : WGPU_FALSE;
    return add(Kind::Buffer, wgpuDeviceCreateBuffer(device_.device, &desc), desc.size);
}

void DawnDevice::destroyBuffer(Handle buffer) {
    wgpuBufferDestroy(as<WGPUBuffer>(buffer, Kind::Buffer, "destroy"));
}

Handle DawnDevice::createShaderModule(const JsString& code) {
    const Utf8 source(code);
    WGPUShaderSourceWGSL wgsl = WGPU_SHADER_SOURCE_WGSL_INIT;
    wgsl.code = source.view();
    WGPUShaderModuleDescriptor desc = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
    desc.nextInChain = &wgsl.chain;
    return add(Kind::ShaderModule, wgpuDeviceCreateShaderModule(device_.device, &desc));
}

std::vector<JsString> DawnDevice::compilationErrors(Handle module) {
    struct Reply {
        bool done = false;
        std::vector<JsString> errors;
    } reply;
    WGPUCompilationInfoCallbackInfo info = WGPU_COMPILATION_INFO_CALLBACK_INFO_INIT;
    info.mode = WGPUCallbackMode_WaitAnyOnly;
    info.callback = [](WGPUCompilationInfoRequestStatus status, const WGPUCompilationInfo* compilation, void* userdata1,
                       void*) {
        auto* r = static_cast<Reply*>(userdata1);
        r->done = true;
        if (status != WGPUCompilationInfoRequestStatus_Success || compilation == nullptr) return;
        for (size_t i = 0; i < compilation->messageCount; ++i) {
            const WGPUCompilationMessage& message = compilation->messages[i];
            if (message.type == WGPUCompilationMessageType_Error) r->errors.push_back(utf8_to_js(to_string(message.message)));
        }
    };
    info.userdata1 = &reply;
    const auto shader = as<WGPUShaderModule>(module, Kind::ShaderModule, "getCompilationInfo");
    if (!wait(device_.instance, wgpuShaderModuleGetCompilationInfo(shader, info)) || !reply.done) {
        throw JsError(u"OperationError", u"getCompilationInfo did not complete");
    }
    return reply.errors;
}

Handle DawnDevice::createRenderPipeline(const backend::RenderPipelineDescriptor& descriptor) {
    const Utf8 vertexEntry(descriptor.vertexEntryPoint);
    const Utf8 fragmentEntry(descriptor.fragmentEntryPoint);
    std::vector<WGPUBlendState> blends(descriptor.targets.size());
    std::vector<WGPUColorTargetState> targets(descriptor.targets.size());
    for (size_t i = 0; i < descriptor.targets.size(); ++i) {
        const backend::ColorTargetState& target = descriptor.targets[i];
        targets[i] = WGPU_COLOR_TARGET_STATE_INIT;
        targets[i].format = texture_format(target.format);
        if (target.blend) {
            blends[i].color = blend_component(target.blend->color);
            blends[i].alpha = blend_component(target.blend->alpha);
            targets[i].blend = &blends[i];
        }
        targets[i].writeMask = target.writeMask;
    }
    WGPUFragmentState fragment = WGPU_FRAGMENT_STATE_INIT;
    fragment.module = as<WGPUShaderModule>(descriptor.fragmentModule, Kind::ShaderModule, "fragment.module");
    if (!descriptor.fragmentEntryPoint.empty()) fragment.entryPoint = fragmentEntry.view();
    fragment.targetCount = targets.size();
    fragment.targets = targets.data();

    WGPURenderPipelineDescriptor desc = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
    desc.vertex.module = as<WGPUShaderModule>(descriptor.vertexModule, Kind::ShaderModule, "vertex.module");
    if (!descriptor.vertexEntryPoint.empty()) desc.vertex.entryPoint = vertexEntry.view();
    desc.primitive.topology = primitive_topology(descriptor.topology);
    desc.primitive.frontFace = front_face(descriptor.frontFace);
    desc.primitive.cullMode = cull_mode(descriptor.cullMode);
    WGPUDepthStencilState depth = WGPU_DEPTH_STENCIL_STATE_INIT;
    if (descriptor.depthStencil) {
        depth.format = texture_format(descriptor.depthStencil->format);
        depth.depthWriteEnabled = descriptor.depthStencil->depthWriteEnabled ? WGPUOptionalBool_True : WGPUOptionalBool_False;
        if (descriptor.depthStencil->depthCompare) depth.depthCompare = compare_function(*descriptor.depthStencil->depthCompare);
        desc.depthStencil = &depth;
    }
    desc.fragment = &fragment;
    return add(Kind::RenderPipeline, wgpuDeviceCreateRenderPipeline(device_.device, &desc));
}

Handle DawnDevice::createComputePipeline(const backend::ComputePipelineDescriptor& descriptor) {
    const Utf8 entry(descriptor.entryPoint);
    WGPUComputePipelineDescriptor desc = WGPU_COMPUTE_PIPELINE_DESCRIPTOR_INIT;
    desc.compute.module = as<WGPUShaderModule>(descriptor.module, Kind::ShaderModule, "compute.module");
    if (!descriptor.entryPoint.empty()) desc.compute.entryPoint = entry.view();
    return add(Kind::ComputePipeline, wgpuDeviceCreateComputePipeline(device_.device, &desc));
}

Handle DawnDevice::getBindGroupLayout(Handle pipeline, double index) {
    const uint32_t group = to_u32(index, "getBindGroupLayout");
    const auto key = std::make_pair(pipeline, group);
    if (const auto it = layouts_.find(key); it != layouts_.end()) return it->second;
    Handle layout = 0;
    if (pipeline != 0 && pipeline <= objects_.size() && objects_[pipeline - 1].kind == Kind::ComputePipeline) {
        layout = add(Kind::BindGroupLayout, wgpuComputePipelineGetBindGroupLayout(
                                                as<WGPUComputePipeline>(pipeline, Kind::ComputePipeline, "getBindGroupLayout"), group));
    } else {
        layout = add(Kind::BindGroupLayout, wgpuRenderPipelineGetBindGroupLayout(
                                                as<WGPURenderPipeline>(pipeline, Kind::RenderPipeline, "getBindGroupLayout"), group));
    }
    layouts_.emplace(key, layout);
    return layout;
}

Handle DawnDevice::createBindGroup(Handle layout, const std::vector<backend::BindGroupEntry>& entries) {
    std::vector<WGPUBindGroupEntry> out(entries.size());
    for (size_t i = 0; i < entries.size(); ++i) {
        const backend::BindGroupEntry& entry = entries[i];
        out[i] = WGPU_BIND_GROUP_ENTRY_INIT;
        out[i].binding = to_u32(entry.binding, "binding");
        switch (entry.kind) {
            case backend::BindGroupEntry::Kind::TextureView:
                out[i].textureView = as<WGPUTextureView>(entry.resource, Kind::View, "resource");
                break;
            case backend::BindGroupEntry::Kind::Sampler:
                out[i].sampler = as<WGPUSampler>(entry.resource, Kind::Sampler, "resource");
                break;
            case backend::BindGroupEntry::Kind::Buffer:
                out[i].buffer = as<WGPUBuffer>(entry.resource, Kind::Buffer, "resource");
                out[i].offset = 0;
                out[i].size = WGPU_WHOLE_SIZE;
                break;
        }
    }
    WGPUBindGroupDescriptor desc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    desc.layout = as<WGPUBindGroupLayout>(layout, Kind::BindGroupLayout, "layout");
    desc.entryCount = out.size();
    desc.entries = out.data();
    return add(Kind::BindGroup, wgpuDeviceCreateBindGroup(device_.device, &desc));
}

void DawnDevice::writeBuffer(Handle buffer, double offset, const std::vector<uint8_t>& bytes) {
    wgpuQueueWriteBuffer(device_.queue, as<WGPUBuffer>(buffer, Kind::Buffer, "writeBuffer"), to_u64(offset, "bufferOffset"),
                         bytes.data(), bytes.size());
}

namespace {
WGPUTexelCopyTextureInfo copy_texture(WGPUTexture texture, const backend::TexelCopyTextureInfo& info) {
    WGPUTexelCopyTextureInfo out = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
    out.texture = texture;
    out.mipLevel = to_u32(info.mipLevel, "mipLevel");
    out.origin = WGPUOrigin3D{to_u32(info.origin[0], "origin.x"), to_u32(info.origin[1], "origin.y"),
                              to_u32(info.origin[2], "origin.z")};
    out.aspect = texture_aspect(info.aspect);
    return out;
}

WGPUTexelCopyBufferLayout copy_layout(const backend::TexelCopyBufferLayout& layout) {
    WGPUTexelCopyBufferLayout out = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
    out.offset = to_u64(layout.offset, "offset");
    if (layout.bytesPerRow) out.bytesPerRow = to_u32(*layout.bytesPerRow, "bytesPerRow");
    if (layout.rowsPerImage) out.rowsPerImage = to_u32(*layout.rowsPerImage, "rowsPerImage");
    return out;
}
}  // namespace

void DawnDevice::writeTexture(const backend::TexelCopyTextureInfo& destination, const std::vector<uint8_t>& bytes,
                              const backend::TexelCopyBufferLayout& layout, const backend::Extent3D& size) {
    const WGPUTexelCopyTextureInfo dst =
        copy_texture(as<WGPUTexture>(destination.texture, Kind::Texture, "writeTexture"), destination);
    const WGPUTexelCopyBufferLayout data = copy_layout(layout);
    const WGPUExtent3D copySize = extent(size);
    wgpuQueueWriteTexture(device_.queue, &dst, bytes.data(), bytes.size(), &data, &copySize);
}

Handle DawnDevice::createCommandEncoder() {
    return add(Kind::CommandEncoder, wgpuDeviceCreateCommandEncoder(device_.device, nullptr));
}

Handle DawnDevice::beginRenderPass(Handle encoder, const backend::RenderPassDescriptor& descriptor) {
    std::vector<WGPURenderPassColorAttachment> colors(descriptor.colorAttachments.size());
    for (size_t i = 0; i < colors.size(); ++i) {
        const backend::RenderPassColorAttachment& attachment = descriptor.colorAttachments[i];
        colors[i] = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
        colors[i].view = as<WGPUTextureView>(attachment.view, Kind::View, "colorAttachments.view");
        colors[i].loadOp = load_op(attachment.loadOp);
        colors[i].storeOp = store_op(attachment.storeOp);
        colors[i].clearValue = WGPUColor{attachment.clearValue.r, attachment.clearValue.g, attachment.clearValue.b,
                                         attachment.clearValue.a};
    }
    WGPURenderPassDepthStencilAttachment depth = WGPU_RENDER_PASS_DEPTH_STENCIL_ATTACHMENT_INIT;
    WGPURenderPassDescriptor desc = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
    desc.colorAttachmentCount = colors.size();
    desc.colorAttachments = colors.data();
    if (descriptor.depthStencilAttachment) {
        const backend::RenderPassDepthStencilAttachment& d = *descriptor.depthStencilAttachment;
        depth.view = as<WGPUTextureView>(d.view, Kind::View, "depthStencilAttachment.view");
        if (d.depthClearValue) depth.depthClearValue = to_float(*d.depthClearValue, "depthClearValue");
        if (d.depthLoadOp) depth.depthLoadOp = load_op(*d.depthLoadOp);
        if (d.depthStoreOp) depth.depthStoreOp = store_op(*d.depthStoreOp);
        desc.depthStencilAttachment = &depth;
    }
    const auto enc = as<WGPUCommandEncoder>(encoder, Kind::CommandEncoder, "beginRenderPass");
    return add(Kind::RenderPass, wgpuCommandEncoderBeginRenderPass(enc, &desc));
}

Handle DawnDevice::beginComputePass(Handle encoder) {
    const auto enc = as<WGPUCommandEncoder>(encoder, Kind::CommandEncoder, "beginComputePass");
    return add(Kind::ComputePass, wgpuCommandEncoderBeginComputePass(enc, nullptr));
}

void DawnDevice::setPipeline(Handle pass, Handle pipeline) {
    if (pass != 0 && pass <= objects_.size() && objects_[pass - 1].kind == Kind::ComputePass) {
        wgpuComputePassEncoderSetPipeline(as<WGPUComputePassEncoder>(pass, Kind::ComputePass, "setPipeline"),
                                          as<WGPUComputePipeline>(pipeline, Kind::ComputePipeline, "setPipeline"));
        return;
    }
    wgpuRenderPassEncoderSetPipeline(as<WGPURenderPassEncoder>(pass, Kind::RenderPass, "setPipeline"),
                                     as<WGPURenderPipeline>(pipeline, Kind::RenderPipeline, "setPipeline"));
}

void DawnDevice::setBindGroup(Handle pass, double index, Handle group) {
    const uint32_t slot = to_u32(index, "setBindGroup");
    const auto bindGroup = as<WGPUBindGroup>(group, Kind::BindGroup, "setBindGroup");
    if (pass != 0 && pass <= objects_.size() && objects_[pass - 1].kind == Kind::ComputePass) {
        wgpuComputePassEncoderSetBindGroup(as<WGPUComputePassEncoder>(pass, Kind::ComputePass, "setBindGroup"), slot,
                                           bindGroup, 0, nullptr);
        return;
    }
    wgpuRenderPassEncoderSetBindGroup(as<WGPURenderPassEncoder>(pass, Kind::RenderPass, "setBindGroup"), slot, bindGroup,
                                      0, nullptr);
}

void DawnDevice::setViewport(Handle pass, double x, double y, double width, double height, double minDepth,
                             double maxDepth) {
    const float fx = to_float(x, "x");
    const float fy = to_float(y, "y");
    const float fw = to_float(width, "width");
    const float fh = to_float(height, "height");
    const float fmin = to_float(minDepth, "minDepth");
    const float fmax = to_float(maxDepth, "maxDepth");
    wgpuRenderPassEncoderSetViewport(as<WGPURenderPassEncoder>(pass, Kind::RenderPass, "setViewport"), fx, fy, fw, fh,
                                     fmin, fmax);
}

void DawnDevice::draw(Handle pass, double vertexCount, double instanceCount, double firstVertex, double firstInstance) {
    const uint32_t vertices = to_u32(vertexCount, "vertexCount");
    const uint32_t instances = to_u32(instanceCount, "instanceCount");
    const uint32_t first = to_u32(firstVertex, "firstVertex");
    const uint32_t firstInst = to_u32(firstInstance, "firstInstance");
    wgpuRenderPassEncoderDraw(as<WGPURenderPassEncoder>(pass, Kind::RenderPass, "draw"), vertices, instances, first,
                              firstInst);
}

void DawnDevice::dispatchWorkgroups(Handle pass, double x, double y, double z) {
    const uint32_t gx = to_u32(x, "workgroupCountX");
    const uint32_t gy = to_u32(y, "workgroupCountY");
    const uint32_t gz = to_u32(z, "workgroupCountZ");
    wgpuComputePassEncoderDispatchWorkgroups(as<WGPUComputePassEncoder>(pass, Kind::ComputePass, "dispatchWorkgroups"),
                                             gx, gy, gz);
}

void DawnDevice::end(Handle pass) {
    if (pass != 0 && pass <= objects_.size() && objects_[pass - 1].kind == Kind::ComputePass) {
        wgpuComputePassEncoderEnd(as<WGPUComputePassEncoder>(pass, Kind::ComputePass, "end"));
    } else {
        wgpuRenderPassEncoderEnd(as<WGPURenderPassEncoder>(pass, Kind::RenderPass, "end"));
    }
    release(pass);
}

void DawnDevice::copyTextureToTexture(Handle encoder, const backend::TexelCopyTextureInfo& source,
                                      const backend::TexelCopyTextureInfo& destination, const backend::Extent3D& size) {
    const WGPUTexelCopyTextureInfo src = copy_texture(as<WGPUTexture>(source.texture, Kind::Texture, "source"), source);
    const WGPUTexelCopyTextureInfo dst =
        copy_texture(as<WGPUTexture>(destination.texture, Kind::Texture, "destination"), destination);
    const WGPUExtent3D copySize = extent(size);
    wgpuCommandEncoderCopyTextureToTexture(as<WGPUCommandEncoder>(encoder, Kind::CommandEncoder, "copyTextureToTexture"),
                                           &src, &dst, &copySize);
}

void DawnDevice::copyTextureToBuffer(Handle encoder, const backend::TexelCopyTextureInfo& source, Handle buffer,
                                     const backend::TexelCopyBufferLayout& layout, const backend::Extent3D& size) {
    const WGPUTexelCopyTextureInfo src = copy_texture(as<WGPUTexture>(source.texture, Kind::Texture, "source"), source);
    WGPUTexelCopyBufferInfo dst = WGPU_TEXEL_COPY_BUFFER_INFO_INIT;
    dst.layout = copy_layout(layout);
    dst.buffer = as<WGPUBuffer>(buffer, Kind::Buffer, "destination.buffer");
    const WGPUExtent3D copySize = extent(size);
    wgpuCommandEncoderCopyTextureToBuffer(as<WGPUCommandEncoder>(encoder, Kind::CommandEncoder, "copyTextureToBuffer"),
                                          &src, &dst, &copySize);
}

Handle DawnDevice::finish(Handle encoder) {
    const auto enc = as<WGPUCommandEncoder>(encoder, Kind::CommandEncoder, "finish");
    const WGPUCommandBuffer buffer = wgpuCommandEncoderFinish(enc, nullptr);
    release(encoder);
    return add(Kind::CommandBuffer, buffer);
}

void DawnDevice::submit(const std::vector<Handle>& commandBuffers) {
    std::vector<WGPUCommandBuffer> buffers;
    buffers.reserve(commandBuffers.size());
    for (Handle buffer : commandBuffers) buffers.push_back(as<WGPUCommandBuffer>(buffer, Kind::CommandBuffer, "submit"));
    wgpuQueueSubmit(device_.queue, buffers.size(), buffers.data());
    for (Handle buffer : commandBuffers) release(buffer);
}

void DawnDevice::onSubmittedWorkDone(std::function<void()> done) {
    WGPUQueueWorkDoneCallbackInfo info = WGPU_QUEUE_WORK_DONE_CALLBACK_INFO_INIT;
    info.mode = WGPUCallbackMode_AllowProcessEvents;
    info.callback = [](WGPUQueueWorkDoneStatus status, WGPUStringView, void* userdata1, void*) {
        std::unique_ptr<WorkDone> work(static_cast<WorkDone*>(userdata1));
        // The reference runs it when the promise settles either way; a
        // cancelled callback comes at instance teardown, after this device.
        if (status != WGPUQueueWorkDoneStatus_CallbackCancelled) work->done();
    };
    info.userdata1 = new WorkDone{std::move(done)};
    wgpuQueueOnSubmittedWorkDone(device_.queue, info);
}

std::vector<uint8_t> DawnDevice::mapRead(Handle buffer) {
    const auto buf = as<WGPUBuffer>(buffer, Kind::Buffer, "mapAsync");
    const uint64_t size = objects_[buffer - 1].size;
    struct Reply {
        WGPUMapAsyncStatus status = WGPUMapAsyncStatus_Error;
        std::string message;
    } reply;
    WGPUBufferMapCallbackInfo info = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
    info.mode = WGPUCallbackMode_WaitAnyOnly;
    info.callback = [](WGPUMapAsyncStatus status, WGPUStringView message, void* userdata1, void*) {
        auto* r = static_cast<Reply*>(userdata1);
        r->status = status;
        r->message = to_string(message);
    };
    info.userdata1 = &reply;
    if (!wait(device_.instance, wgpuBufferMapAsync(buf, WGPUMapMode_Read, 0, size_t(size), info))) {
        throw JsError(u"OperationError", u"mapAsync did not complete");
    }
    if (reply.status != WGPUMapAsyncStatus_Success) throw JsError(u"OperationError", utf8_to_js(reply.message));
    const auto* mapped = static_cast<const uint8_t*>(wgpuBufferGetConstMappedRange(buf, 0, size_t(size)));
    if (mapped == nullptr) throw JsError(u"OperationError", u"getMappedRange failed");
    return std::vector<uint8_t>(mapped, mapped + size);
}

void DawnDevice::unmap(Handle buffer) {
    wgpuBufferUnmap(as<WGPUBuffer>(buffer, Kind::Buffer, "unmap"));
}

void DawnDevice::release(Handle object) noexcept {
    if (object == 0 || object > objects_.size() || objects_[object - 1].kind == Kind::Released) return;
    // A released layout leaves the cache, and a released pipeline's layouts
    // with it, before the slots are reused.
    const Kind kind = objects_[object - 1].kind;
    for (auto it = layouts_.begin(); it != layouts_.end();) {
        const bool layoutOfPipeline =
            (kind == Kind::RenderPipeline || kind == Kind::ComputePipeline) && it->first.first == object;
        if (it->second == object || layoutOfPipeline) {
            const Handle layout = it->second;
            it = layouts_.erase(it);
            if (layoutOfPipeline) release(layout);
        } else {
            ++it;
        }
    }
    if (objects_[object - 1].kind == Kind::Released) return;
    release(objects_[object - 1]);
    try {
        free_.push_back(object);
    } catch (...) {
        // Out of memory: the slot stays a tombstone.
    }
}

void DawnDevice::processEvents() {
    wgpuInstanceProcessEvents(device_.instance);
}

void DawnDevice::waitIdle() {
    bool done = false;
    WGPUQueueWorkDoneCallbackInfo info = WGPU_QUEUE_WORK_DONE_CALLBACK_INFO_INIT;
    info.mode = WGPUCallbackMode_WaitAnyOnly;
    info.callback = [](WGPUQueueWorkDoneStatus, WGPUStringView, void* userdata1, void*) {
        *static_cast<bool*>(userdata1) = true;
    };
    info.userdata1 = &done;
    wait(device_.instance, wgpuQueueOnSubmittedWorkDone(device_.queue, info));
    processEvents();
}

}  // namespace libnoisemaker::gpu
