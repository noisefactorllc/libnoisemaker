#include "gpu/backend/recording_device.h"

#include <algorithm>
#include <utility>

namespace libnoisemaker::backend {

namespace {

Value optional_string(const std::optional<JsString>& value) { return value ? Value(*value) : Value::null(); }
// An object reference: handle 0 is no object, which the reference passes as undefined.
Value ref(Handle handle) { return handle ? Value(double(handle)) : Value::null(); }
Value optional_number(const std::optional<double>& value) { return value ? Value(*value) : Value::null(); }
Value extent(const Extent3D& size) { return Value(Array{Value(size.width), Value(size.height), Value(size.depthOrArrayLayers)}); }

Value hex(const std::vector<uint8_t>& bytes) {
    static const char16_t* digits = u"0123456789abcdef";
    JsString text;
    text.reserve(bytes.size() * 2);
    for (uint8_t byte : bytes) {
        text += digits[byte >> 4];
        text += digits[byte & 15];
    }
    return Value(std::move(text));
}

Value blend_component(const BlendComponent& component) {
    Object out;
    out.set(u"operation", Value(component.operation));
    out.set(u"srcFactor", Value(component.srcFactor));
    out.set(u"dstFactor", Value(component.dstFactor));
    return Value(std::move(out));
}

}  // namespace

Handle RecordingDevice::next() { return next_++; }

void RecordingDevice::log(Object call) { log_.emplace_back(std::move(call)); }

Array RecordingDevice::takeLog() { return std::exchange(log_, Array{}); }

void RecordingDevice::rejectBindings(std::vector<double> bindings, std::optional<JsString> failMessage) {
    rejected_ = std::move(bindings);
    failMessage_ = std::move(failMessage);
}

Handle RecordingDevice::createTexture(const TextureDescriptor& descriptor) {
    const Handle id = next();
    Object call;
    call.set(u"call", Value(u"createTexture"));
    call.set(u"id", Value(double(id)));
    call.set(u"size", extent(descriptor.size));
    call.set(u"mipLevelCount", Value(descriptor.mipLevelCount));
    call.set(u"sampleCount", Value(descriptor.sampleCount));
    call.set(u"dimension", Value(descriptor.dimension));
    call.set(u"format", Value(descriptor.format));
    call.set(u"usage", Value(double(descriptor.usage)));
    log(std::move(call));
    return id;
}

Handle RecordingDevice::createView(Handle texture, const TextureViewDescriptor& descriptor) {
    const Handle id = next();
    Object call;
    call.set(u"call", Value(u"createView"));
    call.set(u"id", Value(double(id)));
    call.set(u"texture", ref(texture));
    call.set(u"format", optional_string(descriptor.format));
    call.set(u"dimension", optional_string(descriptor.dimension));
    call.set(u"aspect", Value(descriptor.aspect));
    call.set(u"baseMipLevel", Value(descriptor.baseMipLevel));
    call.set(u"mipLevelCount", optional_number(descriptor.mipLevelCount));
    call.set(u"baseArrayLayer", Value(descriptor.baseArrayLayer));
    call.set(u"arrayLayerCount", optional_number(descriptor.arrayLayerCount));
    log(std::move(call));
    return id;
}

void RecordingDevice::destroyTexture(Handle texture) {
    Object call;
    call.set(u"call", Value(u"destroyTexture"));
    call.set(u"texture", ref(texture));
    log(std::move(call));
}

Handle RecordingDevice::createSampler(const SamplerDescriptor& descriptor) {
    const Handle id = next();
    Object call;
    call.set(u"call", Value(u"createSampler"));
    call.set(u"id", Value(double(id)));
    call.set(u"addressModeU", Value(descriptor.addressModeU));
    call.set(u"addressModeV", Value(descriptor.addressModeV));
    call.set(u"addressModeW", Value(descriptor.addressModeW));
    call.set(u"magFilter", Value(descriptor.magFilter));
    call.set(u"minFilter", Value(descriptor.minFilter));
    call.set(u"mipmapFilter", Value(descriptor.mipmapFilter));
    call.set(u"lodMinClamp", Value(descriptor.lodMinClamp));
    call.set(u"lodMaxClamp", Value(descriptor.lodMaxClamp));
    call.set(u"compare", optional_string(descriptor.compare));
    call.set(u"maxAnisotropy", Value(descriptor.maxAnisotropy));
    log(std::move(call));
    return id;
}

Handle RecordingDevice::createBuffer(const BufferDescriptor& descriptor) {
    const Handle id = next();
    Object call;
    call.set(u"call", Value(u"createBuffer"));
    call.set(u"id", Value(double(id)));
    call.set(u"size", Value(descriptor.size));
    call.set(u"usage", Value(double(descriptor.usage)));
    call.set(u"mappedAtCreation", Value(descriptor.mappedAtCreation));
    log(std::move(call));
    return id;
}

void RecordingDevice::destroyBuffer(Handle buffer) {
    Object call;
    call.set(u"call", Value(u"destroyBuffer"));
    call.set(u"buffer", ref(buffer));
    log(std::move(call));
}

Handle RecordingDevice::createShaderModule(const JsString& code) {
    const Handle id = next();
    Object call;
    call.set(u"call", Value(u"createShaderModule"));
    call.set(u"id", Value(double(id)));
    call.set(u"code", Value(code));
    log(std::move(call));
    return id;
}

Handle RecordingDevice::createRenderPipeline(const RenderPipelineDescriptor& descriptor) {
    const Handle id = next();
    Object vertex;
    vertex.set(u"module", ref(descriptor.vertexModule));
    vertex.set(u"entryPoint", Value(descriptor.vertexEntryPoint));
    vertex.set(u"buffers", Value(Array{}));
    Object primitive;
    primitive.set(u"topology", Value(descriptor.topology));
    primitive.set(u"stripIndexFormat", Value::null());
    primitive.set(u"frontFace", Value(descriptor.frontFace));
    primitive.set(u"cullMode", Value(descriptor.cullMode));
    Object multisample;
    multisample.set(u"count", Value(1.0));
    multisample.set(u"mask", Value(4294967295.0));
    multisample.set(u"alphaToCoverageEnabled", Value(false));
    Array targets;
    for (const ColorTargetState& target : descriptor.targets) {
        Object out;
        out.set(u"format", Value(target.format));
        if (target.blend) {
            Object blend;
            blend.set(u"color", blend_component(target.blend->color));
            blend.set(u"alpha", blend_component(target.blend->alpha));
            out.set(u"blend", Value(std::move(blend)));
        } else {
            out.set(u"blend", Value::null());
        }
        out.set(u"writeMask", Value(double(target.writeMask)));
        targets.emplace_back(std::move(out));
    }
    Object fragment;
    fragment.set(u"module", ref(descriptor.fragmentModule));
    fragment.set(u"entryPoint", Value(descriptor.fragmentEntryPoint));
    fragment.set(u"targets", Value(std::move(targets)));
    Object call;
    call.set(u"call", Value(u"createRenderPipeline"));
    call.set(u"id", Value(double(id)));
    call.set(u"layout", Value(u"auto"));
    call.set(u"vertex", Value(std::move(vertex)));
    call.set(u"primitive", Value(std::move(primitive)));
    if (descriptor.depthStencil) {
        Object depth;
        depth.set(u"format", Value(descriptor.depthStencil->format));
        depth.set(u"depthWriteEnabled", Value(descriptor.depthStencil->depthWriteEnabled));
        depth.set(u"depthCompare", optional_string(descriptor.depthStencil->depthCompare));
        call.set(u"depthStencil", Value(std::move(depth)));
    } else {
        call.set(u"depthStencil", Value::null());
    }
    call.set(u"multisample", Value(std::move(multisample)));
    call.set(u"fragment", Value(std::move(fragment)));
    log(std::move(call));
    return id;
}

Handle RecordingDevice::createComputePipeline(const ComputePipelineDescriptor& descriptor) {
    const Handle id = next();
    Object compute;
    compute.set(u"module", ref(descriptor.module));
    compute.set(u"entryPoint", Value(descriptor.entryPoint));
    Object call;
    call.set(u"call", Value(u"createComputePipeline"));
    call.set(u"id", Value(double(id)));
    call.set(u"layout", Value(u"auto"));
    call.set(u"compute", Value(std::move(compute)));
    log(std::move(call));
    return id;
}

Handle RecordingDevice::getBindGroupLayout(Handle pipeline, double index) {
    const Handle id = next();
    Object call;
    call.set(u"call", Value(u"getBindGroupLayout"));
    call.set(u"id", Value(double(id)));
    call.set(u"pipeline", ref(pipeline));
    call.set(u"index", Value(index));
    log(std::move(call));
    return id;
}

Handle RecordingDevice::createBindGroup(Handle layout, const std::vector<BindGroupEntry>& entries) {
    std::optional<JsString> failure = failMessage_;
    if (!failure) {
        for (const BindGroupEntry& entry : entries) {
            if (std::find(rejected_.begin(), rejected_.end(), entry.binding) != rejected_.end()) {
                failure = u"binding index " + jsv::to_string(Value(entry.binding)) + u" not present in the bind group layout";
                break;
            }
        }
    }
    const Handle id = failure ? 0 : next();
    Array out;
    for (const BindGroupEntry& entry : entries) {
        Object item;
        item.set(u"binding", Value(entry.binding));
        switch (entry.kind) {
            case BindGroupEntry::Kind::TextureView: item.set(u"view", ref(entry.resource)); break;
            case BindGroupEntry::Kind::Sampler: item.set(u"sampler", ref(entry.resource)); break;
            case BindGroupEntry::Kind::Buffer:
                item.set(u"buffer", ref(entry.resource));
                item.set(u"offset", Value(0.0));
                item.set(u"size", Value::null());
                break;
        }
        out.emplace_back(std::move(item));
    }
    Object call;
    call.set(u"call", Value(u"createBindGroup"));
    call.set(u"id", failure ? Value::null() : Value(double(id)));
    call.set(u"layout", ref(layout));
    call.set(u"entries", Value(std::move(out)));
    if (failure) call.set(u"error", Value(*failure));
    log(std::move(call));
    if (failure) throw DeviceError(*failure);
    return id;
}

void RecordingDevice::writeBuffer(Handle buffer, double offset, const std::vector<uint8_t>& bytes) {
    Object call;
    call.set(u"call", Value(u"writeBuffer"));
    call.set(u"buffer", ref(buffer));
    call.set(u"offset", Value(offset));
    call.set(u"bytes", hex(bytes));
    log(std::move(call));
}

void RecordingDevice::writeTexture(const TexelCopyTextureInfo& destination, const std::vector<uint8_t>& bytes,
                                   const TexelCopyBufferLayout& layout, const Extent3D& size) {
    Object call;
    call.set(u"call", Value(u"writeTexture"));
    call.set(u"texture", ref(destination.texture));
    call.set(u"mipLevel", Value(destination.mipLevel));
    call.set(u"origin", Value(Array{Value(destination.origin[0]), Value(destination.origin[1]), Value(destination.origin[2])}));
    call.set(u"aspect", Value(destination.aspect));
    call.set(u"offset", Value(layout.offset));
    call.set(u"bytesPerRow", optional_number(layout.bytesPerRow));
    call.set(u"rowsPerImage", optional_number(layout.rowsPerImage));
    call.set(u"size", extent(size));
    call.set(u"bytes", hex(bytes));
    log(std::move(call));
}

Handle RecordingDevice::createCommandEncoder() {
    const Handle id = next();
    Object call;
    call.set(u"call", Value(u"createCommandEncoder"));
    call.set(u"id", Value(double(id)));
    log(std::move(call));
    return id;
}

Handle RecordingDevice::beginRenderPass(Handle encoder, const RenderPassDescriptor& descriptor) {
    const Handle id = next();
    Array colors;
    for (const RenderPassColorAttachment& attachment : descriptor.colorAttachments) {
        Object out;
        out.set(u"view", ref(attachment.view));
        out.set(u"resolveTarget", Value::null());
        const Color& c = attachment.clearValue;
        out.set(u"clearValue", Value(Array{Value(c.r), Value(c.g), Value(c.b), Value(c.a)}));
        out.set(u"loadOp", Value(attachment.loadOp));
        out.set(u"storeOp", Value(attachment.storeOp));
        colors.emplace_back(std::move(out));
    }
    Object call;
    call.set(u"call", Value(u"beginRenderPass"));
    call.set(u"id", Value(double(id)));
    call.set(u"encoder", ref(encoder));
    call.set(u"colorAttachments", Value(std::move(colors)));
    if (descriptor.depthStencilAttachment) {
        const RenderPassDepthStencilAttachment& d = *descriptor.depthStencilAttachment;
        Object depth;
        depth.set(u"view", ref(d.view));
        depth.set(u"depthClearValue", optional_number(d.depthClearValue));
        depth.set(u"depthLoadOp", optional_string(d.depthLoadOp));
        depth.set(u"depthStoreOp", optional_string(d.depthStoreOp));
        call.set(u"depthStencilAttachment", Value(std::move(depth)));
    } else {
        call.set(u"depthStencilAttachment", Value::null());
    }
    log(std::move(call));
    return id;
}

Handle RecordingDevice::beginComputePass(Handle encoder) {
    const Handle id = next();
    Object call;
    call.set(u"call", Value(u"beginComputePass"));
    call.set(u"id", Value(double(id)));
    call.set(u"encoder", ref(encoder));
    log(std::move(call));
    return id;
}

void RecordingDevice::setPipeline(Handle pass, Handle pipeline) {
    Object call;
    call.set(u"call", Value(u"setPipeline"));
    call.set(u"pass", ref(pass));
    call.set(u"pipeline", ref(pipeline));
    log(std::move(call));
}

void RecordingDevice::setBindGroup(Handle pass, double index, Handle group) {
    Object call;
    call.set(u"call", Value(u"setBindGroup"));
    call.set(u"pass", ref(pass));
    call.set(u"index", Value(index));
    call.set(u"group", ref(group));
    log(std::move(call));
}

void RecordingDevice::setViewport(Handle pass, double x, double y, double width, double height, double minDepth, double maxDepth) {
    Object call;
    call.set(u"call", Value(u"setViewport"));
    call.set(u"pass", ref(pass));
    call.set(u"args", Value(Array{Value(x), Value(y), Value(width), Value(height), Value(minDepth), Value(maxDepth)}));
    log(std::move(call));
}

void RecordingDevice::draw(Handle pass, double vertexCount, double instanceCount, double firstVertex, double firstInstance) {
    Object call;
    call.set(u"call", Value(u"draw"));
    call.set(u"pass", ref(pass));
    call.set(u"args", Value(Array{Value(vertexCount), Value(instanceCount), Value(firstVertex), Value(firstInstance)}));
    log(std::move(call));
}

void RecordingDevice::dispatchWorkgroups(Handle pass, double x, double y, double z) {
    Object call;
    call.set(u"call", Value(u"dispatchWorkgroups"));
    call.set(u"pass", ref(pass));
    call.set(u"args", Value(Array{Value(x), Value(y), Value(z)}));
    log(std::move(call));
}

void RecordingDevice::end(Handle pass) {
    Object call;
    call.set(u"call", Value(u"end"));
    call.set(u"pass", ref(pass));
    log(std::move(call));
}

Handle RecordingDevice::finish(Handle encoder) {
    const Handle id = next();
    Object call;
    call.set(u"call", Value(u"finish"));
    call.set(u"id", Value(double(id)));
    call.set(u"encoder", ref(encoder));
    log(std::move(call));
    return id;
}

void RecordingDevice::submit(const std::vector<Handle>& commandBuffers) {
    Array ids;
    for (Handle buffer : commandBuffers) ids.emplace_back(double(buffer));
    Object call;
    call.set(u"call", Value(u"submit"));
    call.set(u"commandBuffers", Value(std::move(ids)));
    log(std::move(call));
}

}  // namespace libnoisemaker::backend
