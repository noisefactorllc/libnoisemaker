#!/usr/bin/env node
// reference-backend.mjs — the reference WebGPU backend's device calls, the
// oracle of parity/check_backend.mjs. It replays the JSON lines `nm-dump
// backend` reads on the reference's own WebGPUBackend, whose device is a mock
// that logs every call in RecordingDevice's canonical form (src/gpu/backend/
// recording_device.h): objects numbered by creation order, WebGPU's static
// defaults applied (view members whose default comes from the texture stay
// null), extents as arrays, written bytes in hex. For each op it writes
// {key, calls}, with result for a method whose return value callers use, and
// error: {name, message} (or {thrown}) when the op throws.
//
//   NM_REFERENCE_ROOT=<reference-checkout> node tools/reference-backend.mjs < ops.jsonl
import { createInterface } from 'node:readline'
import { join, resolve } from 'node:path'
import { pathToFileURL } from 'node:url'

if (!process.env.NM_REFERENCE_ROOT) { console.error('NM_REFERENCE_ROOT is not set'); process.exit(3) }
const REF = resolve(process.env.NM_REFERENCE_ROOT)
globalThis.GPUBufferUsage ??= { MAP_READ: 1, MAP_WRITE: 2, COPY_SRC: 4, COPY_DST: 8, INDEX: 16, VERTEX: 32, UNIFORM: 64, STORAGE: 128, INDIRECT: 256, QUERY_RESOLVE: 512 }
globalThis.GPUTextureUsage ??= { COPY_SRC: 1, COPY_DST: 2, TEXTURE_BINDING: 4, STORAGE_BINDING: 8, RENDER_ATTACHMENT: 16 }
globalThis.GPUShaderStage ??= { VERTEX: 1, FRAGMENT: 2, COMPUTE: 4 }
globalThis.GPUMapMode ??= { READ: 1, WRITE: 2 }
// The reference logs warnings; keep stdout for records.
console.log = console.warn = console.info = (...args) => process.stderr.write(args.map(String).join(' ') + '\n')
const { WebGPUBackend } = await import(pathToFileURL(join(REF, 'shaders/src/runtime/backends/webgpu.js')).href)

let log = []
let next = 1
// For the gate: createBindGroup rejects a group holding one of these bindings,
// or every group with failMessage when it is set (see RecordingDevice).
let rejected = []
let failMessage = null
const ids = new WeakMap()
const kinds = new WeakMap()
const make = kind => { const o = {}; ids.set(o, next++); kinds.set(o, kind); return o }
const idOf = o => (o && typeof o === 'object' && ids.has(o)) ? ids.get(o) : null
const def = (value, fallback) => value === undefined ? fallback : value
const num = value => Number(value)
const extent = e => Array.isArray(e)
  ? [num(e[0]), num(def(e[1], 1)), num(def(e[2], 1))]
  : [num(e.width), num(def(e.height, 1)), num(def(e.depthOrArrayLayers, 1))]
const origin = o => o === undefined ? [0, 0, 0] : Array.isArray(o)
  ? [num(def(o[0], 0)), num(def(o[1], 0)), num(def(o[2], 0))]
  : [num(def(o.x, 0)), num(def(o.y, 0)), num(def(o.z, 0))]
const hex = (data, offset = 0, size) => {
  const view = data instanceof ArrayBuffer ? new Uint8Array(data) : new Uint8Array(data.buffer, data.byteOffset, data.byteLength)
  const element = data instanceof ArrayBuffer ? 1 : (data.BYTES_PER_ELEMENT ?? 1)
  const start = offset * element
  const end = size === undefined ? view.length : start + size * element
  return Buffer.from(view.subarray(start, end)).toString('hex')
}
const blendComponent = c => ({ operation: def(c?.operation, 'add'), srcFactor: def(c?.srcFactor, 'one'), dstFactor: def(c?.dstFactor, 'zero') })

const copyInfo = info => ({ texture: idOf(info.texture), mipLevel: num(def(info.mipLevel, 0)), origin: origin(info.origin), aspect: def(info.aspect, 'all') })
const color = c => c === undefined ? [0, 0, 0, 0] : Array.isArray(c) ? c.map(num) : [num(c.r), num(c.g), num(c.b), num(c.a)]
const passObject = kind => {
  const p = make(kind)
  p.setPipeline = pipeline => log.push({ call: 'setPipeline', pass: idOf(p), pipeline: idOf(pipeline) })
  p.setBindGroup = (index, group) => log.push({ call: 'setBindGroup', pass: idOf(p), index: num(index), group: idOf(group) })
  p.setViewport = (...args) => log.push({ call: 'setViewport', pass: idOf(p), args: args.map(num) })
  p.draw = (vertexCount, instanceCount, firstVertex, firstInstance) => log.push({
    call: 'draw', pass: idOf(p), args: [num(vertexCount), num(def(instanceCount, 1)), num(def(firstVertex, 0)), num(def(firstInstance, 0))]
  })
  p.dispatchWorkgroups = (x, y, z) => log.push({ call: 'dispatchWorkgroups', pass: idOf(p), args: [num(x), num(def(y, 1)), num(def(z, 1))] })
  p.end = () => log.push({ call: 'end', pass: idOf(p) })
  return p
}
const encoder = () => {
  const e = make('encoder')
  e.beginRenderPass = d => {
    const p = passObject('renderPass')
    const depth = d.depthStencilAttachment
    log.push({
      call: 'beginRenderPass', id: idOf(p), encoder: idOf(e),
      colorAttachments: d.colorAttachments.map(a => ({
        view: idOf(a.view), resolveTarget: a.resolveTarget === undefined ? null : idOf(a.resolveTarget),
        clearValue: color(a.clearValue), loadOp: a.loadOp, storeOp: a.storeOp
      })),
      depthStencilAttachment: depth === undefined ? null : {
        view: idOf(depth.view), depthClearValue: depth.depthClearValue === undefined ? null : num(depth.depthClearValue),
        depthLoadOp: def(depth.depthLoadOp, null), depthStoreOp: def(depth.depthStoreOp, null)
      }
    })
    return p
  }
  e.beginComputePass = () => {
    const p = passObject('computePass')
    log.push({ call: 'beginComputePass', id: idOf(p), encoder: idOf(e) })
    return p
  }
  e.copyTextureToTexture = (source, destination, size) => log.push({
    call: 'copyTextureToTexture', encoder: idOf(e), source: copyInfo(source), destination: copyInfo(destination), size: extent(size)
  })
  e.finish = () => { const b = make('commandBuffer'); log.push({ call: 'finish', id: idOf(b), encoder: idOf(e) }); return b }
  return e
}
const pipelineObject = kind => {
  const p = make(kind)
  p.getBindGroupLayout = index => {
    const layout = make('bindGroupLayout')
    log.push({ call: 'getBindGroupLayout', id: idOf(layout), pipeline: idOf(p), index: num(index) })
    return layout
  }
  return p
}
const device = {
  limits: { maxTextureDimension2D: 8192, maxColorAttachmentBytesPerSample: 32 },
  features: new Set(),
  addEventListener () {},
  queue: {
    writeBuffer (buffer, bufferOffset, data, dataOffset, size) {
      log.push({ call: 'writeBuffer', buffer: idOf(buffer), offset: num(bufferOffset), bytes: hex(data, dataOffset, size) })
    },
    writeTexture (destination, data, layout, size) {
      log.push({
        call: 'writeTexture', texture: idOf(destination.texture), mipLevel: num(def(destination.mipLevel, 0)),
        origin: origin(destination.origin), aspect: def(destination.aspect, 'all'), offset: num(def(layout.offset, 0)),
        bytesPerRow: layout.bytesPerRow === undefined ? null : num(layout.bytesPerRow),
        rowsPerImage: layout.rowsPerImage === undefined ? null : num(layout.rowsPerImage),
        size: extent(size), bytes: hex(data)
      })
    },
    submit (buffers) { log.push({ call: 'submit', commandBuffers: buffers.map(idOf) }) },
    onSubmittedWorkDone () { return Promise.resolve() }
  },
  createTexture (d) {
    const t = make('texture')
    log.push({
      call: 'createTexture', id: idOf(t), size: extent(d.size), mipLevelCount: num(def(d.mipLevelCount, 1)),
      sampleCount: num(def(d.sampleCount, 1)), dimension: def(d.dimension, '2d'), format: String(d.format), usage: num(d.usage)
    })
    t.createView = (v = {}) => {
      const view = make('view')
      log.push({
        call: 'createView', id: idOf(view), texture: idOf(t), format: def(v.format, null), dimension: def(v.dimension, null),
        aspect: def(v.aspect, 'all'), baseMipLevel: num(def(v.baseMipLevel, 0)),
        mipLevelCount: v.mipLevelCount === undefined ? null : num(v.mipLevelCount),
        baseArrayLayer: num(def(v.baseArrayLayer, 0)),
        arrayLayerCount: v.arrayLayerCount === undefined ? null : num(v.arrayLayerCount)
      })
      return view
    }
    t.destroy = () => log.push({ call: 'destroyTexture', texture: idOf(t) })
    return t
  },
  createSampler (d = {}) {
    const s = make('sampler')
    log.push({
      call: 'createSampler', id: idOf(s), addressModeU: def(d.addressModeU, 'clamp-to-edge'),
      addressModeV: def(d.addressModeV, 'clamp-to-edge'), addressModeW: def(d.addressModeW, 'clamp-to-edge'),
      magFilter: def(d.magFilter, 'nearest'), minFilter: def(d.minFilter, 'nearest'), mipmapFilter: def(d.mipmapFilter, 'nearest'),
      lodMinClamp: num(def(d.lodMinClamp, 0)), lodMaxClamp: num(def(d.lodMaxClamp, 32)), compare: def(d.compare, null),
      maxAnisotropy: num(def(d.maxAnisotropy, 1))
    })
    return s
  },
  createBuffer (d) {
    const b = make('buffer')
    b.size = d.size
    b.destroy = () => log.push({ call: 'destroyBuffer', buffer: idOf(b) })
    log.push({ call: 'createBuffer', id: idOf(b), size: num(d.size), usage: num(d.usage), mappedAtCreation: !!d.mappedAtCreation })
    return b
  },
  createShaderModule (d) {
    const m = make('module')
    m.getCompilationInfo = async () => ({ messages: [] })
    log.push({ call: 'createShaderModule', id: idOf(m), code: String(d.code) })
    return m
  },
  createRenderPipeline (d) {
    const p = pipelineObject('renderPipeline')
    const primitive = d.primitive ?? {}
    const multisample = d.multisample ?? {}
    log.push({
      call: 'createRenderPipeline', id: idOf(p), layout: d.layout === 'auto' ? 'auto' : idOf(d.layout),
      vertex: { module: idOf(d.vertex.module), entryPoint: def(d.vertex.entryPoint, null), buffers: def(d.vertex.buffers, []) },
      primitive: {
        topology: def(primitive.topology, 'triangle-list'), stripIndexFormat: def(primitive.stripIndexFormat, null),
        frontFace: def(primitive.frontFace, 'ccw'), cullMode: def(primitive.cullMode, 'none')
      },
      depthStencil: d.depthStencil === undefined ? null : {
        format: String(d.depthStencil.format), depthWriteEnabled: def(d.depthStencil.depthWriteEnabled, false),
        depthCompare: def(d.depthStencil.depthCompare, null)
      },
      multisample: { count: num(def(multisample.count, 1)), mask: num(def(multisample.mask, 0xFFFFFFFF)), alphaToCoverageEnabled: def(multisample.alphaToCoverageEnabled, false) },
      fragment: d.fragment === undefined ? null : {
        module: idOf(d.fragment.module), entryPoint: def(d.fragment.entryPoint, null),
        targets: d.fragment.targets.map(t => ({
          format: String(t.format),
          blend: t.blend === undefined ? null : { color: blendComponent(t.blend.color), alpha: blendComponent(t.blend.alpha) },
          writeMask: num(def(t.writeMask, 0xF))
        }))
      }
    })
    return p
  },
  createComputePipeline (d) {
    const p = pipelineObject('computePipeline')
    log.push({
      call: 'createComputePipeline', id: idOf(p), layout: d.layout === 'auto' ? 'auto' : idOf(d.layout),
      compute: { module: idOf(d.compute.module), entryPoint: def(d.compute.entryPoint, null) }
    })
    return p
  },
  createBindGroup (d) {
    const entries = d.entries.map(e => {
      const r = e.resource
      if (r && typeof r === 'object' && !ids.has(r) && 'buffer' in r) {
        return { binding: num(e.binding), buffer: idOf(r.buffer), offset: num(def(r.offset, 0)), size: r.size === undefined ? null : num(r.size) }
      }
      const kind = kinds.get(r)
      return kind === 'sampler' ? { binding: num(e.binding), sampler: idOf(r) } : { binding: num(e.binding), view: idOf(r) }
    })
    const rejectedEntry = entries.find(e => rejected.includes(e.binding))
    const failure = failMessage ?? (rejectedEntry ? `binding index ${rejectedEntry.binding} not present in the bind group layout` : null)
    if (failure !== null) {
      log.push({ call: 'createBindGroup', id: null, layout: idOf(d.layout), entries, error: failure })
      throw new Error(failure)
    }
    const g = make('bindGroup')
    log.push({ call: 'createBindGroup', id: idOf(g), layout: idOf(d.layout), entries })
    return g
  },
  createCommandEncoder () {
    const e = encoder()
    log.push({ call: 'createCommandEncoder', id: idOf(e) })
    return e
  }
}

const decode = value => {
  if (!value || typeof value !== 'object' || Array.isArray(value) || Object.keys(value).length !== 1) return value
  if (Array.isArray(value.$float32)) return new Float32Array(value.$float32)
  return value
}
const decodeMembers = encoded => {
  const value = decode(encoded)
  return value && typeof value === 'object' && !Array.isArray(value) && !ArrayBuffer.isView(value)
    ? Object.fromEntries(Object.entries(value).map(([key, member]) => [key, decode(member)])) : value
}

let backend = null
const frameState = s => {
  const surfaces = {}
  for (const [name, texId] of Object.entries(s?.surfaces ?? {})) {
    const tex = backend.textures.get(texId)
    if (tex) surfaces[name] = tex
  }
  return {
    globalUniforms: decodeMembers(s?.globalUniforms),
    surfaces,
    writeSurfaces: s?.writeSurfaces ?? {},
    graph: s?.renderSurface === undefined ? {} : { renderSurface: s.renderSurface },
    screenWidth: s?.screenWidth,
    screenHeight: s?.screenHeight
  }
}

const lines = createInterface({ input: process.stdin, crlfDelay: Infinity })
for await (const line of lines) {
  if (!line) continue
  const op = JSON.parse(line)
  let error
  let result
  try {
    switch (op.op) {
      case 'reset': next = 1; log = []; rejected = []; failMessage = null; backend = new WebGPUBackend(device, null); break
      case 'rejectBindings': rejected = op.bindings ?? []; failMessage = op.message ?? null; break
      case 'init': await backend.init(); break
      case 'createTexture': backend.createTexture(op.id, op.spec); break
      case 'createTexture3D': backend.createTexture3D(op.id, op.spec); break
      case 'createCubeTexture': backend.createCubeTexture(op.id, { size: op.size }); break
      case 'uploadDataTexture': backend.uploadDataTexture(op.id, decode(op.data), op.width, op.height); break
      case 'destroyTexture': backend.destroyTexture(op.id); break
      case 'compileProgram': await backend.compileProgram(op.id, op.spec); break
      case 'beginFrame': backend.beginFrame(); break
      case 'endFrame': backend.endFrame(); break
      case 'createBindGroup': backend.createBindGroup(op.pass, backend.programs.get(op.program), frameState(op.state)); break
      case 'executePass': backend.executePass(op.pass, frameState(op.state)); break
      case 'uploadCubeFace': backend.uploadCubeFace(op.id, op.face, { width: op.width, height: op.height, data: Uint8Array.from(op.data) }); break
      case 'uploadMeshData': result = backend.uploadMeshData(op.id, decode(op.positions), decode(op.normals), decode(op.uvs), op.width, op.height, op.vertexCount); break
      case 'copyTexture': backend.copyTexture(op.src, op.dst); break
      case 'copyTextureScaled': backend.copyTextureScaled(op.src, op.dst); break
      case 'generateMipmaps': backend.generateMipmaps(op.ids); break
      case 'clearTexture': backend.clearTexture(op.id); break
      default: throw new Error(`unknown op ${op.op}`)
    }
  } catch (err) {
    // A thrown plain object (`throw { code, ... }`) is compared as data.
    error = err instanceof Error
      ? { name: String(err.name), message: String(err.message) }
      : { thrown: JSON.parse(JSON.stringify(err) ?? 'null') }
  }
  // Promise callbacks (onSubmittedWorkDone) run before the next op.
  await new Promise(resolve => setImmediate(resolve))
  if (op.op === 'reset') continue
  const record = { key: op.key, calls: log.splice(0) }
  // A method's return value, where a caller uses it (uploadMeshData).
  if (result !== undefined) record.result = result
  if (error) record.error = error
  process.stdout.write(JSON.stringify(record) + '\n')
}
