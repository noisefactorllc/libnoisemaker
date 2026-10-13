#!/usr/bin/env node
// check_backend.mjs — gate libnoisemaker's WebGPU backend (src/gpu/backend)
// by its device calls: the same op sequences run on the reference's own
// WebGPUBackend over a recording mock device (tools/reference-backend.mjs) and
// on `nm-dump backend` over RecordingDevice, and each op's calls must be equal,
// in order, with every descriptor, object id and written byte.
//
// Sequences: every parity/programs graph, compiled by the reference, with a
// texture for each pass input and output (surfaces as read and write pairs),
// every program compiled, and a bind group for every pass over two frames.
// Errors must be equal too; corpus ops must not throw, and every corpus
// program must compile.
//
//   NM_REFERENCE_ROOT=<checkout> NM_DSL=<nm-dump> node parity/check_backend.mjs [--verbose]
import { execFileSync } from 'node:child_process'
import { existsSync, readFileSync, readdirSync } from 'node:fs'
import { dirname, join, resolve } from 'node:path'
import { fileURLToPath, pathToFileURL } from 'node:url'

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..')
if (!process.env.NM_REFERENCE_ROOT) { console.error('NM_REFERENCE_ROOT is not set'); process.exit(3) }
const NM_DSL = process.env.NM_DSL || join(ROOT, 'build', 'nm-dump')
const verbose = process.argv.includes('--verbose')
if (!existsSync(NM_DSL)) { console.error(`nm-dump not found at ${NM_DSL}`); process.exit(2) }
const out = line => process.stdout.write(line + '\n')
const err = line => process.stderr.write(line + '\n')

const { bootstrapReference } = await import(pathToFileURL(join(ROOT, 'tools/reference-oracle.mjs')).href)
const mod = await bootstrapReference()

const ops = []
const corpusKeys = new Set()
let sequences = 0
const sequence = (prefix, calls, corpus = false) => {
  sequences++
  ops.push({ op: 'reset' })
  calls.forEach((call, i) => {
    const key = `${prefix}#${i}:${call.op}`
    if (corpus) corpusKeys.add(key)
    ops.push({ ...call, key })
  })
}

// parseGlobalName in the reference backend.
const globalName = id => {
  if (typeof id !== 'string') return null
  if (id.startsWith('global_')) return id.slice(7)
  if (id.startsWith('global') && id.length > 6 && /^[A-Z0-9]/.test(id.slice(6))) return id[6].toLowerCase() + id.slice(7)
  return null
}
const frameGlobals = (width, height, frame) => ({
  time: frame * 0.5371, deltaTime: frame ? 1 / 60 : 0, frame,
  resolution: [width, height], tileOffset: [0, 0], fullResolution: [width, height],
  aspect: width / height, aspectRatio: width / height, aspectInv: height / width,
  invFullResolution: [1 / width, 1 / height], renderScale: 1
})

const programsDir = join(ROOT, 'parity/programs')
const sizes = [[256, 256], [320, 180], [97, 61]]
let corpusGraphs = 0
const uncompiled = []
for (const [index, file] of readdirSync(programsDir).filter(f => f.endsWith('.dsl')).sort().entries()) {
  let graph
  try { graph = mod.compileGraph(readFileSync(join(programsDir, file), 'utf8')) } catch { uncompiled.push(file); continue }
  corpusGraphs++
  const [width, height] = sizes[index % sizes.length]
  const calls = [{ op: 'init' }]
  const textures = new Set()
  const surfaces = {}
  const writeSurfaces = {}
  const texIds = graph.passes.flatMap(pass => [
    ...Object.values(pass.inputs ?? {}), ...Object.values(pass.outputs ?? {}), ...Object.values(pass.storageTextures ?? {})
  ]).filter(id => typeof id === 'string' && id !== 'none' && id !== 'outputTex')
  let n = 0
  for (const id of texIds) {
    const name = globalName(id)
    if (name) {
      if (surfaces[name]) continue
      surfaces[name] = `global_${name}_read`
      writeSurfaces[name] = `global_${name}_write`
      for (const surfaceId of [surfaces[name], writeSurfaces[name]]) {
        calls.push({ op: 'createTexture', id: surfaceId, spec: { width, height, format: 'rgba16f' } })
      }
      continue
    }
    if (textures.has(id)) continue
    textures.add(id)
    n++
    if (n % 7 === 3) calls.push({ op: 'createTexture3D', id, spec: { width: 16, height: 16, depth: 16, format: 'rgba16f', filter: n % 2 ? 'nearest' : undefined } })
    else calls.push({ op: 'createTexture', id, spec: { width, height, format: n % 3 ? 'rgba16f' : 'rgba8', mipmaps: n % 5 === 1 } })
  }
  for (const [id, spec] of Object.entries(graph.programs ?? {})) calls.push({ op: 'compileProgram', id, spec })
  const renderSurface = graph.renderSurface
  for (const frame of [0, 1]) {
    calls.push({ op: 'beginFrame' })
    const state = { globalUniforms: frameGlobals(width, height, frame), surfaces, writeSurfaces, renderSurface, screenWidth: width, screenHeight: height }
    for (const pass of graph.passes) {
      if (!graph.programs?.[pass.program]) continue
      calls.push({ op: 'createBindGroup', pass, program: pass.program, state })
    }
    calls.push({ op: 'endFrame' })
  }
  sequence(`corpus/${file}`, calls, true)
  // The same graph executed: every pass on the frame's encoder, over two frames.
  const exec = calls.filter(call => call.op !== 'createBindGroup')
  const executed = []
  for (const call of exec) {
    if (call.op === 'beginFrame') {
      executed.push(call)
      const frame = executed.filter(c => c.op === 'beginFrame').length - 1
      const state = { globalUniforms: frameGlobals(width, height, frame), surfaces, writeSurfaces, renderSurface, screenWidth: width, screenHeight: height }
      for (const pass of graph.passes) if (graph.programs?.[pass.program]) executed.push({ op: 'executePass', pass, state })
    } else {
      executed.push(call)
    }
  }
  sequence(`corpus-exec/${file}`, executed, true)
}

// Synthetic sequences: the paths no catalog effect reaches at this pin
// (storage textures, entry points, sampler types, the legacy and empty bind
// groups, uniform defaults, storage buffer growth, separate vertex shaders,
// blending, source fallbacks, data and cube textures).
const W = String.raw
const shaders = {
  storage: W`struct Params { size: vec4<f32> }
@group(0) @binding(0) var<uniform> params: Params;
@group(0) @binding(1) var inputTex: texture_2d<f32>;
@group(0) @binding(2) var outTex: texture_storage_2d<rgba16float, write>;
@group(0) @binding(3) var output_texture: texture_storage_2d<rgba16float, write>;
@compute @workgroup_size(8, 8) fn main(@builtin(global_invocation_id) id: vec3<u32>) {
  let c = textureLoad(inputTex, vec2<i32>(id.xy), 0) * params.size.x;
  textureStore(outTex, vec2<i32>(id.xy), c);
  textureStore(output_texture, vec2<i32>(id.xy), c);
}
`,
  entries: W`@group(0) @binding(0) var<uniform> params: vec4<f32>;
@group(0) @binding(1) var srcTex: texture_2d<f32>;
@group(0) @binding(2) var dstA: texture_storage_2d<rgba16float, write>;
@group(0) @binding(3) var dstB: texture_storage_2d<rgba16float, write>;
@group(0) @binding(4) var<storage, read_write> stats_buffer: array<f32>;
@compute @workgroup_size(8) fn downsample_main(@builtin(global_invocation_id) id: vec3<u32>) {
  textureStore(dstA, vec2<i32>(id.xy), textureLoad(srcTex, vec2<i32>(id.xy), 0) * params.x);
}
@compute @workgroup_size(8) fn upsample_main(@builtin(global_invocation_id) id: vec3<u32>) {
  textureStore(dstB, vec2<i32>(id.xy), vec4<f32>(stats_buffer[0]));
}
`,
  legacy: W`@fragment fn main(@builtin(position) p: vec4<f32>) -> @location(0) vec4<f32> { return vec4<f32>(1.0); }
`,
  dead: W`@group(0) @binding(0) var unusedTex: texture_2d<f32>;
@fragment fn main() -> @location(0) vec4<f32> { return vec4<f32>(0.0); }
`,
  aliases: W`@group(0) @binding(0) var tex0: texture_2d<f32>;
@group(0) @binding(1) var tex1: texture_2d<f32>;
@group(0) @binding(2) var tex9: texture_2d<f32>;
@group(0) @binding(3) var inputColor: texture_2d<f32>;
@group(0) @binding(4) var samp: sampler;
@group(0) @binding(5) var volTex: texture_3d<f32>;
@group(0) @binding(6) var volSamp: sampler;
@group(0) @binding(7) var meshTex: texture_2d<f32>;
@group(0) @binding(8) var flatTex: texture_2d<f32>;
@group(0) @binding(9) var flatSamp: sampler;
@fragment fn main(@builtin(position) p: vec4<f32>) -> @location(0) vec4<f32> {
  return textureSample(tex0, samp, p.xy) + textureSample(tex1, samp, p.xy) + textureSample(tex9, samp, p.xy)
    + textureSample(inputColor, samp, p.xy) + textureSample(volTex, volSamp, p.xyz) + textureLoad(meshTex, vec2<i32>(0), 0)
    + textureSample(flatTex, flatSamp, p.xy);
}
`,
  defaults: W`@group(0) @binding(0) var<uniform> tint: vec3<f32>;
@group(0) @binding(1) var<uniform> wave: array<vec4<f32>, 4>;
@group(0) @binding(2) var<uniform> xf: mat3x3<f32>;
@group(0) @binding(3) var<uniform> count: i32;
@group(0) @binding(4) var<uniform> pos: vec2<f32>;
@group(0) @binding(5) var<uniform> amt: f32;
@group(0) @binding(6) var<uniform> flags: array<f32, 3>;
@group(0) @binding(7) var<uniform> pair: array<vec2<f32>, 2>;
@group(0) @binding(8) var<uniform> mode: u32;
@fragment fn main() -> @location(0) vec4<f32> {
  return vec4<f32>(tint, amt) + wave[0] + vec4<f32>(xf[0], f32(count)) + vec4<f32>(pos, flags[0], pair[1].x) + vec4<f32>(f32(mode));
}
`,
  buffers: W`@group(0) @binding(0) var<storage, read_write> output_buffer: array<vec4<f32>>;
@group(0) @binding(1) var<storage, read_write> stats_buffer: array<f32>;
@group(0) @binding(2) var<storage, read_write> downsample_buf: array<f32>;
@group(0) @binding(3) var<storage, read_write> other_buf: array<f32>;
@compute @workgroup_size(8) fn main(@builtin(global_invocation_id) id: vec3<u32>) {
  output_buffer[id.x] = vec4<f32>(stats_buffer[0] + downsample_buf[0] + other_buf[0]);
}
`,
  vertex: W`@group(0) @binding(0) var<uniform> vparams: vec4<f32>;
@group(0) @binding(3) var heightTex: texture_2d<f32>;
@vertex fn vs_custom(@builtin(vertex_index) i: u32) -> @builtin(position) vec4<f32> {
  return vparams + textureLoad(heightTex, vec2<i32>(0), 0);
}
`,
  fragment: W`@group(0) @binding(0) var<uniform> fparams: vec4<f32>;
@group(0) @binding(1) var inputTex: texture_2d<f32>;
@group(0) @binding(2) var samp: sampler;
@fragment fn main(@builtin(position) p: vec4<f32>) -> @location(0) vec4<f32> { return textureSample(inputTex, samp, p.xy) * fparams; }
`,
  vertexOdd: W`@group(0) @binding(1) var<uniform> vparams: vec4<f32>;
@group(0) @binding(3) var heightTex: texture_2d<f32>;
@vertex fn vs_odd(@builtin(vertex_index) i: u32) -> @builtin(position) vec4<f32> {
  return vparams + textureLoad(heightTex, vec2<i32>(0), 0);
}
`,
  fragmentEven: W`@group(0) @binding(0) var<uniform> fparams: vec4<f32>;
@group(0) @binding(2) var inputTex: texture_2d<f32>;
@group(0) @binding(4) var samp: sampler;
@fragment fn main(@builtin(position) p: vec4<f32>) -> @location(0) vec4<f32> { return textureSample(inputTex, samp, p.xy) * fparams; }
`,
  many: Array.from({ length: 12 }, (_, i) => `@group(0) @binding(${i}) var t${i}: texture_2d<f32>;\n`).join('') +
    '@fragment fn main() -> @location(0) vec4<f32> { return ' +
    Array.from({ length: 12 }, (_, i) => `textureLoad(t${i}, vec2<i32>(0), 0)`).join(' + ') + '; }\n',
  combined: W`struct Out { @builtin(position) p: vec4<f32> }
@vertex fn vs_main(@builtin(vertex_index) i: u32) -> Out { var o: Out; o.p = vec4<f32>(f32(i)); return o; }
@fragment fn fs(@builtin(position) p: vec4<f32>) -> @location(0) vec4<f32> { return p; }
`
}
const surfaceState = (extra = {}) => ({
  globalUniforms: frameGlobals(64, 32, 0),
  surfaces: { o0: 'global_o0_read', feedback: 'global_feedback_read' },
  writeSurfaces: { o0: 'global_o0_write', feedback: 'global_feedback_write' },
  renderSurface: 'o0', screenWidth: 64, screenHeight: 32, ...extra
})
const baseTextures = [
  { op: 'createTexture', id: 'plainTex', spec: { width: 64, height: 32, format: 'rgba16f' } },
  { op: 'createTexture', id: 'plainTex2', spec: { width: 64, height: 32, format: 'rgba16f', usage: ['storage', 'sample'] } },
  { op: 'createTexture', id: 'plainTex3', spec: { width: 64, height: 32, format: 'weird' } },
  { op: 'createTexture', id: 'mipTex', spec: { width: 40, height: 9, format: 'rgba8', mipmaps: true } },
  { op: 'createTexture', id: 'noFormat', spec: { width: 3, height: 3 } },
  { op: 'createTexture3D', id: 'vol3d', spec: { width: 8, height: 8, depth: 8, format: 'rgba16f', filter: 'nearest' } },
  { op: 'createTexture3D', id: 'volLinear', spec: { width: 8, height: 8, depth: 8, format: 'rgba32f' } },
  { op: 'createTexture', id: 'global_o0_read', spec: { width: 64, height: 32, format: 'rgba16f' } },
  { op: 'createTexture', id: 'global_o0_write', spec: { width: 64, height: 32, format: 'rgba16f' } },
  { op: 'createTexture', id: 'global_feedback_read', spec: { width: 64, height: 32, format: 'rgba16f' } },
  { op: 'createTexture', id: 'global_feedback_write', spec: { width: 64, height: 32, format: 'rgba16f', mipmaps: true } },
  { op: 'createTexture', id: 'global_mesh0_positions', spec: { width: 16, height: 16, format: 'rgba32f' } }
]
const bind = (pass, program, state = surfaceState()) => ({ op: 'createBindGroup', pass: { id: 'p', program, ...pass }, program, state })
sequence('synthetic/storage-textures', [
  { op: 'init' }, ...baseTextures,
  { op: 'compileProgram', id: 'storage', spec: { wgsl: shaders.storage } },
  { op: 'beginFrame' },
  ...['outputTex', 'o0', 'o9', 'global_feedback', 'globalFeedback', 'plainTex2', 'missing', null].map(target =>
    bind({ inputs: { inputTex: 'plainTex' }, uniforms: { size: [2, 0, 0, 0] }, storageTextures: target === null ? {} : { outTex: target } }, 'storage')),
  bind({ inputs: { inputTex: 'plainTex' } }, 'storage', surfaceState({ renderSurface: undefined })),
  bind({ inputs: { inputTex: 'plainTex' } }, 'storage', surfaceState({ renderSurface: undefined })),
  bind({ inputs: { inputTex: 'plainTex' } }, 'storage', surfaceState({ renderSurface: 'none', screenWidth: undefined, screenHeight: undefined })),
  { op: 'endFrame' }
])
sequence('synthetic/entry-points', [
  { op: 'init' }, ...baseTextures,
  { op: 'compileProgram', id: 'entries', spec: { wgsl: shaders.entries } },
  { op: 'compileProgram', id: 'entriesNamed', spec: { wgsl: shaders.entries, computeEntryPoint: 'upsample_main' } },
  { op: 'beginFrame' },
  bind({ entryPoint: 'downsample_main', inputs: { srcTex: 'plainTex' }, outputs: { color: 'plainTex3' }, storageTextures: { dstA: 'plainTex2', dstB: 'plainTex3' }, uniforms: { params: [1, 2, 3, 4] } }, 'entries'),
  bind({ entryPoint: 'upsample_main', inputs: { srcTex: 'plainTex' }, storageTextures: { dstA: 'plainTex2', dstB: 'plainTex3' } }, 'entries'),
  bind({ entryPoint: 'missing_main', inputs: { srcTex: 'plainTex' }, storageTextures: { dstB: 'plainTex3' } }, 'entries'),
  bind({ inputs: { srcTex: 'plainTex' }, storageTextures: { dstA: 'plainTex2', dstB: 'plainTex3' } }, 'entriesNamed'),
  { op: 'endFrame' }
])
sequence('synthetic/legacy-and-dead', [
  { op: 'init' }, ...baseTextures,
  { op: 'compileProgram', id: 'legacy', spec: { wgsl: shaders.legacy } },
  { op: 'compileProgram', id: 'dead', spec: { wgsl: shaders.dead } },
  { op: 'beginFrame' },
  bind({ inputs: { inputTex: 'plainTex', overlay: 'none', feed: 'global_o0', ghost: 'missingTex', vol: 'vol3d', volLin: 'volLinear', mip: 'mipTex', scoped: 'plainTex_chain_3', num: 5 }, samplerTypes: { feed: 'repeat', mip: 'bogus' }, uniforms: { a: 1, b: [1, 2, 3] } }, 'legacy'),
  bind({ inputs: { inputTex: 'plainTex' } }, 'legacy', { surfaces: {}, writeSurfaces: {} }),
  bind({ inputs: { inputTex: 'plainTex' } }, 'dead'),
  { op: 'endFrame' }
])
sequence('synthetic/aliases-and-samplers', [
  { op: 'init' }, ...baseTextures,
  { op: 'compileProgram', id: 'aliases', spec: { wgsl: shaders.aliases } },
  { op: 'beginFrame' },
  bind({ inputs: { inputTex: 'plainTex', second: 'mipTex', volTex: 'vol3d', meshTex: 'global_mesh0_positions_chain_2', flatTex: 'global_o0' } }, 'aliases'),
  bind({ inputs: { first: 'plainTex', second: 'global_feedback', volTex: 'volLinear_chain_1', meshTex: 'global_mesh0_positions', flatTex: 'globalFeedback' }, samplerTypes: { samp: 'repeat', volSamp: 7 } }, 'aliases'),
  bind({ inputs: { inputTex: 'global_o0_chain_4', tex1: 'noFormat' } }, 'aliases', surfaceState({ surfaces: {} })),
  // A pass sampling a host upload (isExternal) takes the linear sampler.
  { op: 'updateTextureFromPixels', id: 'hostTex', width: 2, height: 2, flipY: false, data: Array.from({ length: 16 }, (_, i) => i * 9) },
  bind({ inputs: { inputTex: 'hostTex', second: 'mipTex', volTex: 'vol3d', meshTex: 'global_mesh0_positions', flatTex: 'global_o0' } }, 'aliases'),
  { op: 'endFrame' }
])
sequence('synthetic/uniform-defaults', [
  { op: 'init' },
  { op: 'compileProgram', id: 'defaults', spec: { wgsl: shaders.defaults } },
  { op: 'beginFrame' },
  bind({ uniforms: { tint: 'red', amt: true, pos: [1, 2], wave: { $float32: Array.from({ length: 16 }, (_, i) => i / 4) } } }, 'defaults', { globalUniforms: { count: 2.6, mode: null, flags: [7] } }),
  bind({}, 'defaults', { globalUniforms: {} }),
  { op: 'endFrame' },
  { op: 'beginFrame' },
  bind({ uniforms: { xf: [1, 2, 3, 4, 5, 6, 7, 8, 9] } }, 'defaults', { globalUniforms: { tint: [0.5, 0.25, 0.125] } }),
  { op: 'endFrame' }
])
sequence('synthetic/storage-buffers', [
  { op: 'init' },
  { op: 'compileProgram', id: 'buffers', spec: { wgsl: shaders.buffers } },
  { op: 'beginFrame' },
  bind({}, 'buffers', { globalUniforms: {}, screenWidth: 64, screenHeight: 64 }),
  { op: 'endFrame' },
  { op: 'beginFrame' },
  bind({}, 'buffers', { globalUniforms: {}, screenWidth: 640, screenHeight: 480 }),
  { op: 'endFrame' },
  { op: 'beginFrame' },
  bind({}, 'buffers', { globalUniforms: {}, screenWidth: 32, screenHeight: 16 }),
  bind({}, 'buffers', { globalUniforms: {} }),
  { op: 'endFrame' }
])
sequence('synthetic/render-programs', [
  { op: 'init' }, ...baseTextures,
  { op: 'compileProgram', id: 'separate', spec: { wgsl: shaders.fragment, vertexWGSL: shaders.vertex, vertexEntryPoint: 'vs_custom', blend: ['SRC_ALPHA', 'ONE_MINUS_SRC_ALPHA'], topology: 'point-list', outputFormat: 'rgba8' } },
  { op: 'compileProgram', id: 'separateLower', spec: { wgsl: shaders.fragment, vertexWgsl: shaders.vertex, blend: [null, 'dst_alpha'] } },
  { op: 'compileProgram', id: 'combined', spec: { wgsl: shaders.combined, blend: true, fragmentEntryPoint: 'ignored' } },
  { op: 'compileProgram', id: 'interleaved', spec: { wgsl: shaders.fragmentEven, vertexWGSL: shaders.vertexOdd, vertexEntryPoint: 'vs_odd' } },
  { op: 'compileProgram', id: 'fromSource', spec: { source: shaders.fragment, entryPoint: 'main', outputFormat: 'r32f' } },
  { op: 'compileProgram', id: 'fromFragment', spec: { fragment: shaders.legacy, defines: { GAIN: 2, MODE: 1.5 } } },
  { op: 'compileProgram', id: 'glslOnly', spec: { fragment: '#version 300 es\nvoid main() {}' } },
  { op: 'compileProgram', id: 'empty', spec: {} },
  { op: 'beginFrame' },
  bind({ inputs: { inputTex: 'plainTex', heightTex: 'mipTex' }, uniforms: { fparams: [1, 1, 1, 1], vparams: [0, 0, 0, 1] } }, 'separate'),
  bind({ inputs: { inputTex: 'plainTex' } }, 'combined'),
  bind({ inputs: { inputTex: 'plainTex', heightTex: 'mipTex' }, uniforms: { fparams: [1, 2, 3, 4], vparams: [4, 3, 2, 1] } }, 'interleaved'),
  bind({ inputs: { inputTex: 'plainTex' } }, 'fromSource'),
  { op: 'endFrame' }
])
sequence('synthetic/bind-group-retries', [
  { op: 'init' }, ...baseTextures,
  { op: 'compileProgram', id: 'aliases', spec: { wgsl: shaders.aliases } },
  { op: 'compileProgram', id: 'legacy', spec: { wgsl: shaders.legacy } },
  { op: 'compileProgram', id: 'many', spec: { wgsl: shaders.many } },
  { op: 'beginFrame' },
  { op: 'rejectBindings', bindings: [1, 5] },
  bind({ inputs: { inputTex: 'plainTex', second: 'mipTex', volTex: 'vol3d' } }, 'aliases'),
  { op: 'rejectBindings', bindings: [0, 1, 2, 3, 4, 5, 6, 7, 8, 9] },
  bind({ inputs: { inputTex: 'plainTex' } }, 'aliases'),
  { op: 'rejectBindings', bindings: [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10] },
  bind({ inputs: { inputTex: 'plainTex' } }, 'aliases'),
  { op: 'rejectBindings', bindings: [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11] },
  bind({ inputs: { t0: 'plainTex' } }, 'many'),
  { op: 'rejectBindings', bindings: [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10] },
  bind({ inputs: { t0: 'plainTex' } }, 'many'),
  { op: 'rejectBindings', message: 'device lost' },
  bind({ inputs: { inputTex: 'plainTex' } }, 'aliases'),
  { op: 'rejectBindings', bindings: [0] },
  bind({ inputs: { inputTex: 'plainTex' } }, 'legacy'),
  { op: 'rejectBindings', bindings: [] },
  bind({ inputs: { inputTex: 'plainTex' } }, 'aliases'),
  { op: 'endFrame' }
])
sequence('synthetic/names-and-sources', [
  { op: 'init' }, ...baseTextures,
  { op: 'createTexture', id: 'global_', spec: { width: 2, height: 2, format: 'rgba16f' } },
  { op: 'compileProgram', id: 'aliases', spec: { wgsl: shaders.aliases } },
  { op: 'compileProgram', id: 'legacy', spec: { wgsl: shaders.legacy } },
  { op: 'compileProgram', id: 'numberFragment', spec: { fragment: 42 } },
  { op: 'compileProgram', id: 'objectFragment', spec: { fragment: { wgsl: 'x' } } },
  { op: 'beginFrame' },
  bind({ inputs: { inputTex: 'global_' } }, 'aliases', surfaceState({ surfaces: { '': 'plainTex' } })),
  bind({ inputs: { inputTex: 'global_', other: 'global_' } }, 'legacy', surfaceState({ surfaces: { '': 'plainTex' } })),
  bind({ storageTextures: { outTex: 'global_' } }, 'aliases', surfaceState({ writeSurfaces: { '': 'plainTex' } })),
  { op: 'endFrame' }
])
const exec = (pass, state = surfaceState()) => ({ op: 'executePass', pass: { id: 'p', ...pass }, state })
sequence('synthetic/execution', [
  { op: 'init' }, ...baseTextures,
  { op: 'createTexture', id: 'zeroSize', spec: { width: 0, height: 0, format: 'rgba16f' } },
  { op: 'compileProgram', id: 'fragment', spec: { wgsl: shaders.fragment } },
  { op: 'compileProgram', id: 'buffers', spec: { wgsl: shaders.buffers } },
  { op: 'compileProgram', id: 'entries', spec: { wgsl: shaders.entries } },
  exec({ program: 'fragment', outputs: { color: 'plainTex' } }),
  { op: 'beginFrame' },
  exec({ program: 'missing', outputs: { color: 'plainTex' } }),
  exec({ program: 'fragment', outputs: { color: 'nowhere' } }),
  exec({ program: 'fragment', outputs: { color: 'global_o0' }, clear: true, blend: ['ONE', 'ONE'], inputs: { inputTex: 'plainTex' } }),
  exec({ program: 'fragment', outputs: { fragColor: 'mipTex' }, drawMode: 'points', count: 'screen' }),
  exec({ program: 'fragment', outputs: { color: 'plainTex3' }, drawMode: 'points', count: 'input', inputs: { xyzTex: 'global_feedback' } }),
  exec({ program: 'fragment', outputs: { color: 'plainTex3' }, drawMode: 'points', count: 'input' }),
  exec({ program: 'fragment', outputs: { color: 'plainTex3' }, drawMode: 'billboards', count: 'auto' }),
  exec({ program: 'fragment', outputs: { color: 'plainTex3' }, drawMode: 'billboards', count: 12 }),
  exec({ program: 'fragment', outputs: { color: 'zeroSize' }, viewport: { x: 1, y: 2, w: 3, h: 4 } }),
  exec({ program: 'fragment', outputs: { color: 'zeroSize' }, viewportResolved: { x: 5, y: 6, w: 7, h: 8 }, viewport: { x: 1 } }),
  exec({ program: 'fragment', outputs: { color: 'zeroSize' } }),
  exec({ program: 'fragment', outputs: { color: 'plainTex' }, drawMode: 'triangles', countUniform: 'vertexCount', uniforms: { vertexCount: 36 } }),
  exec({ program: 'fragment', outputs: { color: 'plainTex' }, drawMode: 'triangles', count: 'auto', inputs: { meshPositions: 'global_mesh0_positions_chain_1' } }),
  exec({ program: 'fragment', outputs: { color: 'plainTex' }, drawMode: 'triangles', count: 0, inputs: { inputTex: 'global_feedback' } }),
  exec({ program: 'fragment', outputs: { color: 'mipTex' }, drawMode: 'triangles', blend: true }),
  exec({ program: 'fragment', outputs: { a: 'plainTex', b: 'global_o0', c: 'missingTex' }, drawMode: 'points', count: 'auto' }),
  exec({ program: 'fragment', outputs: { a: 'missingA', b: 'missingB' } }),
  exec({ program: 'fragment', outputs: { color: 'toString' }, inputs: { inputTex: 'global_constructor' }, drawMode: 'points', count: 'input' }, surfaceState({ surfaces: {} })),
  exec({ program: 'fragment', outputs: { a: 'valueOf', b: 'plainTex' } }, surfaceState({ surfaces: {} })),
  exec({ program: 'fragment', outputs: { color: 'toString' }, inputs: { inputTex: 'global_hasOwnProperty' } }, surfaceState({ surfaces: { toString: 'missingTex', hasOwnProperty: 'gone' } })),
  exec({ program: 'fragment', outputs: { color: 'plainTex' }, drawBuffers: 2, blend: ['SRC_ALPHA', 'ONE'] }),
  exec({ program: 'buffers', outputs: { color: 'plainTex' } }),
  exec({ program: 'buffers', outputs: { fragColor: 'global_o0' }, workgroups: [4, 2] }),
  exec({ program: 'buffers', outputs: { color: 'outputTex' }, size: { width: 16, height: 8 } }),
  exec({ program: 'buffers', outputs: { color: 'outputTex' }, size: { x: 3, height: 5, depth: 2 } }, surfaceState({ renderSurface: undefined })),
  exec({ program: 'entries', entryPoint: 'upsample_main', inputs: { srcTex: 'plainTex' }, storageTextures: { dstB: 'plainTex3' }, workgroups: 7 }),
  exec({ program: 'entries', entryPoint: 'downsample_main', inputs: { srcTex: 'plainTex' }, storageTextures: { dstA: 'plainTex2' } }, { globalUniforms: {}, surfaces: {}, writeSurfaces: {} }),
  { op: 'endFrame' },
  { op: 'beginFrame' },
  exec({ program: 'fragment', outputs: { color: 'plainTex' }, drawMode: 'triangles' }),
  exec({ program: 'fragment', outputs: { color: 'mipTex' }, drawMode: 'triangles' }),
  exec({ program: 'buffers', outputs: { color: 'plainTex' } }, surfaceState({ screenWidth: 640, screenHeight: 480 })),
  { op: 'endFrame' }
])
const floats = (count, scale) => ({ $float32: Array.from({ length: count }, (_, i) => (i % 17) * scale) })
sequence('synthetic/texture-operations', [
  { op: 'init' }, ...baseTextures,
  { op: 'createTexture', id: 'same', spec: { width: 64, height: 32, format: 'rgba16f' } },
  { op: 'createTexture', id: 'smaller', spec: { width: 30, height: 17, format: 'rgba8' } },
  { op: 'createTexture', id: 'mipDst', spec: { width: 64, height: 32, format: 'rgba16f', mipmaps: true } },
  { op: 'createCubeTexture', id: 'cube', size: 70 },
  { op: 'uploadCubeFace', id: 'cube', face: 2, width: 3, height: 2, data: Array.from({ length: 24 }, (_, i) => i * 7 % 256) },
  { op: 'uploadCubeFace', id: 'cube', face: 5, width: 64, height: 1, data: Array.from({ length: 256 }, (_, i) => i) },
  { op: 'uploadCubeFace', id: 'noCube', face: 0, width: 1, height: 1, data: [1, 2, 3, 4] },
  { op: 'uploadMeshData', id: 'mesh0', positions: floats(5 * 3 * 4, 0.5), normals: floats(5 * 3 * 4, -0.25), uvs: floats(5 * 3 * 4, 0.125), width: 5, height: 3, vertexCount: 15 },
  { op: 'uploadMeshData', id: 'mesh0', positions: floats(5 * 3 * 4, 1), normals: floats(5 * 3 * 4, 2), uvs: floats(5 * 3 * 4, 3), width: 5, height: 3, vertexCount: 15 },
  { op: 'uploadMeshData', id: 'mesh0', positions: floats(16 * 2 * 4, 1), normals: floats(16 * 2 * 4, 2), uvs: floats(16 * 2 * 4, 3), width: 16, height: 2, vertexCount: 32 },
  { op: 'uploadMeshData', id: 'mesh1', positions: floats(4, 1), normals: floats(4, 2), uvs: floats(4, 3), width: 1, height: 1 },
  { op: 'copyTexture', src: 'plainTex', dst: 'same' },
  { op: 'copyTexture', src: 'plainTex', dst: 'smaller' },
  { op: 'copyTexture', src: 'mipTex', dst: 'same' },
  { op: 'copyTexture', src: 'plainTex', dst: 'mipDst' },
  { op: 'copyTexture', src: 'missing', dst: 'same' },
  { op: 'copyTextureScaled', src: 'vol3d', dst: 'smaller' },
  { op: 'copyTextureScaled', src: 'plainTex', dst: 'vol3d' },
  { op: 'copyTextureScaled', src: 'noFormat', dst: 'plainTex3' },
  { op: 'generateMipmaps', ids: ['mipTex', 'plainTex', 'vol3d', 'missing', 'mipDst', 'global_feedback_write'] },
  { op: 'generateMipmaps', ids: ['mipTex'] },
  { op: 'clearTexture', id: 'mipTex' },
  { op: 'clearTexture', id: 'plainTex' },
  { op: 'clearTexture', id: 'missing' },
  // CPU pixels from a willReadFrequently canvas: new, same size, flipped or
  // not, resized (the old texture destroyed), empty.
  ...[[5, 3, undefined], [5, 3, false], [5, 3, true], [2, 4, false], [0, 4, false], [3, 0, true]].map(([width, height, flipY]) => ({
    op: 'updateTextureFromPixels', id: 'canvasTex', width, height, flipY,
    data: Array.from({ length: width * height * 4 }, (_, i) => (i * 29 + width) % 256)
  })),
  { op: 'updateTextureFromPixels', id: 'plainTex', width: 64, height: 32, flipY: false, data: Array.from({ length: 64 * 32 * 4 }, (_, i) => i % 251) },
  { op: 'readPixels', id: 'canvasTex' },
  // Readback: the recording devices fill a mapped buffer's 16-bit units
  // with a permutation of 0..65535, so f16all's 65536 halves are every half
  // float, infinities and NaNs included.
  { op: 'createTexture', id: 'f16all', spec: { width: 256, height: 64, format: 'rgba16f' } },
  { op: 'createTexture', id: 'f16odd', spec: { width: 30, height: 5, format: 'rgba16f' } },
  { op: 'createTexture', id: 'f32odd', spec: { width: 7, height: 3, format: 'rgba32f' } },
  ...['plainTex', 'mipTex', 'f16all', 'f16odd', 'f32odd', 'global_mesh0_positions', 'smaller', 'plainTex3', 'noFormat', 'vol3d', 'missing']
    .map(id => ({ op: 'readPixels', id }))
])
sequence('synthetic/textures', [
  { op: 'init' },
  { op: 'uploadDataTexture', id: 'small', data: { $float32: Array.from({ length: 40 }, (_, i) => i) }, width: 5, height: 2 },
  { op: 'uploadDataTexture', id: 'small', data: { $float32: Array.from({ length: 40 }, (_, i) => -i) }, width: 5, height: 2 },
  { op: 'uploadDataTexture', id: 'small', data: { $float32: Array.from({ length: 48 }, (_, i) => i / 3) }, width: 6, height: 2 },
  { op: 'uploadDataTexture', id: 'aligned', data: { $float32: Array.from({ length: 128 }, (_, i) => i * 0.5) }, width: 16, height: 2 },
  { op: 'createCubeTexture', id: 'cube', size: 8 },
  { op: 'createTexture', id: 'redo', spec: { width: 4, height: 4, format: 'rgba16f', persistent: true } },
  { op: 'createTexture', id: 'redo', spec: { width: 8, height: 8, format: 'rgba16f' } },
  { op: 'destroyTexture', id: 'redo' },
  { op: 'destroyTexture', id: 'redo' },
  { op: 'destroyTexture', id: 'neverMade' },
  { op: 'createTexture', id: 'deep', spec: { width: 1024, height: 3, format: 'rgba16f', mipmaps: true } }
])

const input = ops.map(op => JSON.stringify(op).replace(/[\u2028\u2029]/g, c => `\\u${c.charCodeAt(0).toString(16)}`)).join('\n') + '\n'
const run = (command, args) => {
  const output = execFileSync(command, args, { input, encoding: 'utf8', maxBuffer: 1 << 30, env: process.env })
  return new Map(output.trim().split('\n').map(line => JSON.parse(line)).map(record => [record.key, record]))
}
const reference = run('node', [join(ROOT, 'tools/reference-backend.mjs')])
const candidate = run(NM_DSL, ['backend'])

// Key order inside a call does not matter; the order of calls does.
const canonical = value => JSON.stringify(value, (key, member) => member && typeof member === 'object' && !Array.isArray(member)
  ? Object.fromEntries(Object.keys(member).sort().map(k => [k, member[k]])) : member)
let total = 0
let matched = 0
let calls = 0
let shown = 0
for (const op of ops) {
  if (op.op === 'reset') continue
  total++
  const ref = reference.get(op.key)
  const cand = candidate.get(op.key)
  const same = ref !== undefined && canonical(ref) === canonical(cand) && !(corpusKeys.has(op.key) && ref.error)
  if (same) { matched++; calls += ref.calls.length; continue }
  if (verbose || shown < 10) {
    shown++
    const show = value => value === undefined ? '(none)' : canonical(value).slice(0, 300)
    const length = Math.max(ref?.calls?.length ?? 0, cand?.calls?.length ?? 0)
    const index = ref && cand ? Array.from({ length }, (_, i) => i).find(i => canonical(ref.calls[i]) !== canonical(cand.calls[i])) ?? -1 : -1
    err(`  ${op.key}: ${index >= 0
      ? `call ${index}: reference ${show(ref.calls[index])} vs candidate ${show(cand.calls[index])}`
      : `reference ${show(ref)} vs candidate ${show(cand)}`}`)
  }
}
for (const file of uncompiled) err(`  corpus/${file}: the reference does not compile it`)
if (uncompiled.length) total += uncompiled.length
out(`Backend: ${matched}/${total} ops match, ${calls} device calls (${sequences} sequences: ${corpusGraphs} corpus graphs bound and executed, ${sequences - 2 * corpusGraphs} synthetic)`)
process.exitCode = matched === total ? 0 : 1
