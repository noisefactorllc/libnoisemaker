#!/usr/bin/env node
// check_pipeline.mjs — gate libnoisemaker's pipeline (src/gpu/backend/
// pipeline.h) by its device calls and state: the same op sequences run on the
// reference's own Pipeline and WebGPUBackend over a recording mock device
// (tools/reference-pipeline.mjs) and on `nm-dump pipeline` over
// RecordingDevice. Each op's calls must be equal, in order, with every
// descriptor, object id and written byte, and so must the pipeline's state
// after it: surfaces, mip targets, feedback surfaces, texture aliases,
// diagnostics, lifecycle effects, the backend's textures, the graph's pass
// uniforms and texture specs, and the resource plan.
//
// Sequences: every parity/programs graph, compiled by the reference, through
// init, resizes and recompiles, without and with texture pooling; synthetic
// graphs for the paths no catalog effect reaches at this pin (persistent and
// mipmapped textures, 3D textures, dimension forms, the volume-size clamp,
// MRT format demotion, pooling groups, errors); and resolveDimension on its
// own. Errors must be equal too; corpus ops must not throw.
//
//   NM_REFERENCE_ROOT=<checkout> NM_DSL=<nm-dump> node parity/check_pipeline.mjs [--verbose]
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
// Keys of ops that must not throw: every corpus op and every synthetic
// sequence that is not about errors.
const cleanKeys = new Set()
let sequences = 0
const sequence = (prefix, calls, clean = false) => {
  sequences++
  calls.forEach((call, i) => {
    const key = `${prefix}#${i}:${call.op}`
    if (clean) cleanKeys.add(key)
    ops.push({ ...call, key })
  })
}
// The graph as JSON, its Maps as objects.
const plain = value => JSON.parse(JSON.stringify(value, (_key, member) => member instanceof Map ? Object.fromEntries(member) : member))

const programsDir = join(ROOT, 'parity/programs')
const sizes = [[256, 256], [320, 180], [97, 61], [1024, 768]]
const graphs = []
const uncompiled = []
// The graph's Maps travel as objects, whose array-index keys would reorder:
// the port assumes texture ids are never array indices, and this checks it.
const indexKeyed = []
const isIndex = key => /^(0|[1-9][0-9]*)$/.test(key) && Number(key) < 2 ** 32 - 1
for (const file of readdirSync(programsDir).filter(f => f.endsWith('.dsl')).sort()) {
  let compiled
  try { compiled = mod.compileGraph(readFileSync(join(programsDir, file), 'utf8')) } catch { uncompiled.push(file); continue }
  if ([...(compiled.textures?.keys() ?? []), ...(compiled.allocations?.keys() ?? [])].some(isIndex)) indexKeyed.push(file)
  graphs.push({ file, graph: plain(compiled) })
}
// Frames: every corpus graph rendered, including a wrap of the normalized
// time, a resize and a render after it.
graphs.forEach(({ file, graph }, index) => {
  const [w0, h0] = sizes[index % sizes.length]
  const [w1, h1] = sizes[(index + 2) % sizes.length]
  sequence(`corpus-frames/${file}`, [
    { op: 'create', graph, options: {} },
    { op: 'init', width: w0, height: h0 },
    { op: 'render', time: 0 },
    { op: 'render', time: 0.25 },
    { op: 'render', time: 0.1 },
    { op: 'resize', width: w1, height: h1 },
    { op: 'render', time: 0.5 }
  ], true)
})
graphs.forEach(({ file, graph }, index) => {
  const [w0, h0] = sizes[index % sizes.length]
  const [w1, h1] = sizes[(index + 1) % sizes.length]
  const other = graphs[(index + 1) % graphs.length].graph
  sequence(`corpus/${file}`, [
    { op: 'create', graph, options: {} },
    { op: 'init', width: w0, height: h0 },
    { op: 'resize', width: w1, height: h1 },
    { op: 'resize', width: w1, height: h1 },
    { op: 'recompile', graph },
    { op: 'resize', width: w0, height: h0 }
  ], true)
  sequence(`corpus-pooled/${file}`, [
    { op: 'create', graph, options: { texturePooling: true } },
    { op: 'init', width: w0, height: h0 },
    { op: 'resize', width: w1, height: h1 },
    { op: 'recompile', graph: other },
    { op: 'recompile', graph },
    { op: 'render', time: 0.75 }
  ], true)
})
// Programs from the frontend corpus with osc(), midi() or audio() values,
// rendered at several times.
const automated = []
for (const dir of ['parity/corpus', 'parity/timed']) {
  for (const file of readdirSync(join(ROOT, dir)).filter(f => f.endsWith('.dsl')).sort()) {
    const source = readFileSync(join(ROOT, dir, file), 'utf8')
    if (!/\b(osc|midi|audio)\(/.test(source)) continue
    let compiled
    try { compiled = mod.compileGraph(source) } catch { continue }
    automated.push({ file: `${dir}/${file}`, graph: plain(compiled) })
  }
}
automated.forEach(({ file, graph }) => {
  sequence(`automated/${file}`, [
    { op: 'create', graph, options: {} },
    { op: 'init', width: 128, height: 72 },
    ...[0.05, 0.3, 0.61, 0.99, 0.02].map(time => ({ op: 'render', time }))
  ], true)
})

// Synthetic graphs. Every pass uses one trivial program.
const program = { wgsl: '@fragment fn main() -> @location(0) vec4<f32> { return vec4<f32>(0.0); }\n' }
const graph = (passes, textures = {}, extra = {}) => ({
  passes: passes.map((pass, i) => ({ id: `pass_${i}`, program: 'p', ...pass })),
  programs: { p: program },
  textures,
  renderSurface: 'o0',
  ...extra
})
const screen = (extra = {}) => ({ width: 'screen', height: 'screen', format: 'rgba16f', ...extra })

const surfaces = graph([
  {
    inputs: { inputTex: 'global_fb', meshPositions: 'global_mesh0_positions', notes: 'midiNoteGrid', plain: 'node_6_fixed' },
    outputs: { color: 'global_o1' },
    uniforms: { volumeSize: 128, volumeSize_chain_0: 256, volumeSize_node_9: '64', volumeSize_other: 512, zoom_chain_0: 2 }
  },
  { inputs: { inputTex: 'global_o1' }, outputs: { color: 'global_fb' }, uniforms: { volumeSize: 32 } },
  { outputs: { color: 'global_scratch' } },
  { inputs: { a: 'global_scratch', b: 'global_', c: 7 }, outputs: { color: 'global_mesh1' } },
  { storageTextures: { out: 'global_st' }, outputs: null },
  { storageTextures: { out: 'global_unused' }, outputs: { color: 'node_4_zoom' } },
  { inputs: { x: 'global_state', y: 'global_half', z: 'global_vol1', w: 'global_o2' }, outputs: { a: 'node_1_acc', b: 'node_2_mip' } },
  { inputs: { x: 'globalCamel', v: 'node_3_vol', d: 'node_8_depth', e: 'node_9_slab' }, outputs: { a: 'node_5_odd' } }
], {
  global_o1: screen({ persistent: true }),
  global_o2: { width: '50%', height: 'screen', mipmaps: true },
  global_state: { width: 64, height: 64, format: 'rgba32f' },
  global_half: { width: '50%', height: 32 },
  global_vol1: { width: { param: 'volumeSize', default: 64 }, height: { param: 'volumeSize', power: 2, default: 4096 }, format: 'rgba16f' },
  global_fb: screen({ mipmaps: true, persistent: true }),
  node_1_acc: { width: '50%', height: '50%', format: 'rgba16f', persistent: true, usage: ['render', 'sample', 'copySrc', 'copyDst'] },
  node_2_mip: { width: 'screen', height: { scale: 0.5, clamp: { min: 16, max: 100 } }, format: 'rgba8', mipmaps: true },
  node_3_vol: {
    width: { param: 'volumeSize', default: 32 }, height: { param: 'volumeSize', default: 32 }, depth: { param: 'volumeSize' },
    is3D: true, format: 'rgba16f', filter: 'nearest', usage: ['storage', 'sample', 'copySrc', 'copyDst']
  },
  node_4_zoom: { width: { screenDivide: 'zoom_chain_0', default: 1 }, height: { screenDivide: 'zoom' }, format: 'rgba16f' },
  node_5_odd: { width: 'bogus', height: {}, format: 'rgba16f' },
  node_6_fixed: { width: 17, height: 9.7 },
  node_7_auto: { width: 'auto', height: 'input' },
  node_8_depth: { width: 16, height: 16, depth: { param: 'depthCount', default: 8 }, is3D: true, format: 'rgba16f' },
  node_9_slab: { width: 'screen', height: 4, depth: '50%', is3D: true, format: 'rgba8' },
  globalCamel: screen()
})
sequence('synthetic/surfaces', [
  { op: 'create', graph: surfaces },
  { op: 'init', width: 64, height: 48 },
  { op: 'resize', width: 64, height: 48 },
  { op: 'resize', width: 100, height: 30 },
  { op: 'resize', width: 7, height: 5 },
  { op: 'updateParameterTextures', uniforms: { volumeSize: 16 } },
  { op: 'updateParameterTextures', uniforms: { volumeSize: 64, zoom_chain_0: 3, zoom: 'x' } },
  { op: 'updateParameterTextures', uniforms: { volumeSize: 64, zoom_chain_0: 3, depthCount: 4 } },
  { op: 'updateParameterTextures', uniforms: { volumeSize: 64, zoom_chain_0: 3, depthCount: 4 } },
  { op: 'updateParameterTextures' },
  { op: 'recompile', graph: surfaces },
  { op: 'recompile', graph: graph([{ outputs: { color: 'global_o1' } }], { global_o1: screen({ format: 'rgba32f' }) }) },
  { op: 'resize', width: 64, height: 48 }
], true)

const mrt = graph([
  { outputs: { xyz: 'm_xyz', vel: 'm_vel', rgba: 'm_rgba' } },
  { outputs: { a: 'm_dup', b: 'm_dup', c: 'missing' } },
  { outputs: { a: 'm_ok', b: 'm_rgba' } },
  { outputs: { a: 'm_big1', b: 'm_big2', c: 'm_big3', d: 'm_odd' } },
  { outputs: { only: 'm_single' } },
  { outputs: { a: 'm_t1', b: 'm_t2', c: 'm_t3' } }
], {
  m_xyz: screen({ format: 'rgba32f' }),
  m_vel: screen({ format: 'rgba32float' }),
  m_rgba: screen({ format: 'rgba8' }),
  m_dup: screen({ format: 'rgba32f' }),
  m_ok: screen({ format: 'r8' }),
  m_big1: screen({ format: 'rgba32f' }),
  m_big2: screen({ format: 'r32float' }),
  m_big3: screen({ format: 'rgba32f' }),
  m_odd: screen({ format: 'r16f' }),
  m_single: screen({ format: 'rgba32f' }),
  m_t1: screen({ format: 'rgba32f' }),
  m_t2: screen({ format: 'rgba32f' }),
  m_t3: screen({ format: 'rgba32float' })
})
sequence('synthetic/mrt', [
  { op: 'create', graph: mrt },
  { op: 'init', width: 32, height: 32 },
  { op: 'resize', width: 33, height: 31 }
], true)

// Pooling groups: p0 pools; p1 differs in format; p2 holds a persistent
// texture; t7 is read before it is written; t10 is drawn without clearing; t12
// samples itself; t14 renders a viewport without clearing; t9's physical id is
// falsy; global textures never pool.
const pooledTextures = Object.fromEntries(Array.from({ length: 16 }, (_, i) => [`t${i + 1}`, screen()]))
pooledTextures.t3 = screen({ format: 'rgba8' })
pooledTextures.t5 = screen({ persistent: true })
pooledTextures.t16 = screen({ mipmaps: true })
pooledTextures.global_x = screen()
const pooledPasses = [
  { outputs: { color: 't1' } },
  { inputs: { inputTex: 't1' }, outputs: { color: 't2' } },
  { outputs: { a: 't3', b: 't4' } },
  { outputs: { a: 't5', b: 't6' } },
  { inputs: { inputTex: 't7' }, outputs: { color: 't8' } },
  { outputs: { color: 't7' } },
  { outputs: { color: 't10' }, drawMode: 'points' },
  { outputs: { color: 't11' }, blend: false },
  { inputs: { inputTex: 't12' }, outputs: { color: 't12' } },
  { outputs: { color: 't13' } },
  { outputs: { color: 't14' }, viewport: { x: 0, y: 0, w: 4, h: 4 } },
  { outputs: { color: 't15' }, viewport: { x: 0, y: 0, w: 4, h: 4 }, clear: true },
  { outputs: { a: 't16', b: 'global_x', c: 't9' } }
]
const allocations = {
  t1: 'p0', t2: 'p0', t3: 'p1', t4: 'p1', t5: 'p2', t6: 'p2', t7: 'p3', t8: 'p3', t9: 0, t10: 'p4', t11: 'p4',
  t12: 'p5', t13: 'p5', t14: 'p6', t15: 'p6', t16: 'p7', global_x: 'p7', missing: 'p0'
}
const pooled = graph(pooledPasses, pooledTextures, { allocations })
const regrouped = graph(pooledPasses, pooledTextures, { allocations: { ...allocations, t2: 'p9', t9: 'p1', t6: 'p6' } })
sequence('synthetic/pooling', [
  { op: 'create', graph: pooled, options: { texturePooling: true } },
  { op: 'init', width: 40, height: 20 },
  { op: 'resize', width: 40, height: 20 },
  { op: 'resize', width: 41, height: 21 },
  { op: 'recompile', graph: regrouped },
  { op: 'recompile', graph: pooled },
  { op: 'recompile', graph: graph(pooledPasses, pooledTextures) },
  { op: 'recompile', graph: pooled }
], true)
// A throw while a recompiled graph is in place keeps the previous plan.
sequence('synthetic/pooling-throw', [
  { op: 'create', graph: pooled, options: { texturePooling: true } },
  { op: 'init', width: 40, height: 20 },
  { op: 'recompile', graph: { ...pooled, passes: [...pooled.passes, null] } },
  { op: 'updateParameterTextures', uniforms: {} },
  { op: 'recompile', graph: regrouped }
])
sequence('synthetic/pooling-off', [
  { op: 'create', graph: pooled, options: { texturePooling: 'yes' } },
  { op: 'init', width: 40, height: 20 }
], true)

// Lifecycle and asyncInit effects.
const lifecycle = graph([
  { effectKey: 'synth.media', nodeId: 'node_0' },
  { effectKey: 'synth.media', nodeId: 'node_1' },
  { effectKey: 'filter.fibers', nodeId: 'node_2', uniforms: { seed: 3 } },
  { effectKey: 'synth.noise', nodeId: 'node_3' },
  { effectKey: '' },
  {},
  { effectKey: 'media' },
  { effectKey: 'synth/media' },
  { effectKey: 'not.an.effect' },
  { effectKey: 5 }
])
sequence('synthetic/lifecycle', [
  { op: 'create', graph: lifecycle },
  { op: 'init', width: 16, height: 16 },
  { op: 'resize', width: 8, height: 8 },
  { op: 'recompile', graph: graph([{ effectKey: 'synth.noise' }]) },
  { op: 'recompile', graph: lifecycle }
], true)

// Automation values: every oscillator kind, automation inside the bounds,
// phase, seed and rate (closed-form and quadrature integration, nested to the
// depth bound), MIDI and audio with no state, invalid configs, consumer ranges
// and integer rounding.
const osc = (oscType, extra = {}) => ({ type: 'Oscillator', oscType, min: 0, max: 1, speed: 1, offset: 0, seed: 1, ...extra })
let nested = osc(2, { speed: 3 })
for (let i = 0; i < 10; i++) nested = osc(i % 7, { speed: nested, min: i % 2 ? nested : 0.1 })
const automationUniforms = {
  sine: osc(0), tri: osc(1, { speed: 2, offset: 0.3 }), saw: osc(2, { min: -2, max: 5 }), sawInv: osc(3, { speed: -3 }),
  square: osc(4, { offset: 0.75 }), noise: osc(5, { seed: 42 }), noise2d: osc(6, { seed: 7, speed: 3 }),
  noise2dSeed: osc(6, { seed: -37.5, speed: osc(0) }), unknownKind: osc(9), stringKind: osc('2', { speed: osc(1) }),
  fm: osc(0, { speed: osc(2, { min: 1, max: 4 }) }), fmZero: osc(1, { speed: 0 }), fmSquare: osc(4, { speed: osc(4) }),
  fmNoise: osc(0, { speed: osc(5, { seed: 3 }) }), fmDeep: nested, fmNull: osc(null, { speed: osc(0) }),
  fmRate: osc(0, { speed: osc(2, { min: 1, max: 4, speed: 2.5, offset: 0.2 }) }), fmNegative: osc(1, { speed: osc(3, { speed: -1.5 }) }),
  fmTri: osc(2, { speed: osc(1, { speed: 0.75, offset: 0.6 }) }), fmSine: osc(3, { speed: osc(0, { speed: 3, min: 0.5 }) }),
  fmSquareRate: osc(0, { speed: osc(4, { speed: 1.75, offset: 0.1 }) }),
  bounds: osc(0, { min: osc(1), max: osc(2, { offset: 0.5 }), offset: osc(3), seed: osc(5) }),
  astOnly: { _ast: { type: 'Oscillator' }, oscType: 1, min: 0, max: 2, speed: 1, offset: 0, seed: 1 },
  midi: { type: 'Midi', channel: 1, mode: 4, min: 0.2, max: 0.9, sensitivity: 1 },
  midiFm: osc(0, { speed: { type: 'Midi', channel: 2, mode: 0, min: osc(0), max: 1 } }),
  audio: { type: 'Audio', band: 0, min: 0.3, max: 1 }, audioBad: { type: 'Audio', band: 1, min: 'x', max: 1 },
  audioFm: osc(1, { speed: { type: 'Audio', band: 2, min: 0, max: 1 } }),
  invalid: { type: 'Oscillator', _invalid: true, min: 3 }, invalidOther: { _ast: { type: 'Audio' }, _invalid: true, min: 'q' },
  invalidAudio: { type: 'Audio', _invalid: true, min: 0.4 }, invalidAst: { _ast: { type: 'Audio' }, _invalid: true, min: 0.7 },
  invalidMidi: { type: 'Midi', _invalid: true, min: 0.6, max: 1 },
  ranged: osc(0, { speed: 2 }), rounded: osc(2, { speed: 0.5 }), roundedFm: osc(3, { speed: osc(0) }),
  plain: 0.5, list: [1, 2, 3], notAuto: { type: 'Number', value: 3 }
}
const automationSpecs = { ranged: { min: -10, max: 10 }, rounded: { type: 'int', min: 0, max: 7 }, roundedFm: { type: 'int' }, saw: { min: 'a', max: 3 } }
// Programs whose uniforms carry the values into the device calls.
const structProgram = (fields, extra = '') => ({
  wgsl: `struct Uniforms {\n${fields.map(([name, type]) => `  ${name}: ${type},`).join('\n')}\n}\n` +
    '@group(0) @binding(0) var<uniform> u: Uniforms;\n' + extra +
    `@fragment fn main() -> @location(0) vec4<f32> { return vec4<f32>(${fields.map(([name, type]) => type === 'f32' ? `u.${name}` : type === 'i32' ? `f32(u.${name})` : `u.${name}.x`).join(' + ')}); }\n`
})
const automationProgram = structProgram([
  ...Object.keys(automationUniforms).filter(name => name !== 'list' && name !== 'notAuto' && name !== 'rounded').map(name => [name, 'f32']),
  ['rounded', 'i32'], ['wave', 'f32']
])
const globalsProgram = structProgram([
  ['time', 'f32'], ['deltaTime', 'f32'], ['frame', 'i32'], ['resolution', 'vec2<f32>'], ['tileOffset', 'vec2<f32>'],
  ['fullResolution', 'vec2<f32>'], ['aspect', 'f32'], ['aspectRatio', 'f32'], ['aspectInv', 'f32'],
  ['invFullResolution', 'vec2<f32>'], ['renderScale', 'f32'], ['midiClockCount', 'f32']
])
const mediaProgram = structProgram([['imageSize', 'vec2<f32>'], ['wobble', 'f32']])
const storeProgram = {
  wgsl: '@group(0) @binding(0) var out: texture_storage_2d<rgba16float, write>;\n' +
    '@compute @workgroup_size(8, 8) fn main(@builtin(global_invocation_id) id: vec3<u32>) { textureStore(out, vec2<i32>(id.xy), vec4<f32>(1.0)); }\n'
}
const programs = { p: program, auto: automationProgram, globals: globalsProgram, media: mediaProgram, store: storeProgram }
const automation = graph([
  { program: 'auto', outputs: { color: 'global_o0' }, uniforms: automationUniforms, uniformSpecs: automationSpecs },
  { program: 'globals', outputs: { color: 'global_o1' }, uniforms: { amount: 0.25 } },
  { program: 'auto', outputs: { color: 'global_o2' }, uniforms: { wave: osc(0, { speed: 4 }), size: 3 }, workgroups: [2, 2], countUniform: 'size' },
  { program: 'auto', outputs: { color: 'global_o3' }, uniforms: { sine: 0.5, tri: osc(1) } },
  { program: 'auto', outputs: { color: 'global_o4' }, uniforms: { time: 5, sine: osc(0) } },
  { program: 'auto', outputs: { color: 'global_o5' }, uniforms: { tri: osc(1) } },
  { program: 'globals', outputs: { color: 'global_o6' }, uniforms: { amount: osc(0) } }
], {}, { programs })
sequence('synthetic/automation', [
  { op: 'create', graph: automation },
  { op: 'init', width: 32, height: 16 },
  ...[0, 0.125, 0.5, 0.9999, 0.37, 1.5, -0.25].map(time => ({ op: 'render', time }))
], true)

// The frame: conditions, repeats, viewports, state and feedback surfaces,
// within-frame ping-pong, mip regeneration, onUpdate uniforms, the MIDI note
// grid, tile regions, syncTime and clearSurface.
const frame = graph([
  { outputs: { color: 'global_o1' }, conditions: { skipIf: [{ uniform: 'flag', equals: 1 }] }, uniforms: { flag: 1 } },
  { outputs: { color: 'global_o2' }, conditions: { skipIf: [{ uniform: 'flag', equals: 1 }] }, uniforms: { flag: 0 } },
  { outputs: { color: 'global_o3' }, conditions: { runIf: [{ uniform: 'frame', equals: 1 }] } },
  { outputs: { color: 'global_o4' }, conditions: { runIf: [{ uniform: 'mode', equals: 'a' }, { uniform: 'other', equals: 2 }] }, uniforms: { mode: 'a', other: osc(2) } },
  { outputs: { color: 'global_o5' }, conditions: { skipIf: [{ uniform: 'osc', equals: 0 }] }, uniforms: { osc: osc(4) } },
  { outputs: { color: 'global_o5' }, conditions: 1 },
  { inputs: { inputTex: 'global_xyz' }, outputs: { color: 'global_xyz' }, repeat: 3 },
  { inputs: { inputTex: 'global_ca_state' }, outputs: { color: 'global_ca_state' }, repeat: 'iterations', uniforms: { iterations: 2.7 } },
  { inputs: { inputTex: 'global_flow_trail' }, outputs: { color: 'global_flow_trail' }, repeat: 'frame' },
  { outputs: { color: 'global_o3' }, repeat: 'frame', uniforms: { frame: 3 } },
  { outputs: { color: 'global_rgba_node_3' }, repeat: 'missing' },
  { outputs: { color: 'global_vel' }, repeat: -2 },
  { outputs: { color: 'global_o6' }, repeat: 0 },
  { outputs: { color: 'global_o6' }, repeat: 'nan', uniforms: { nan: 'x' } },
  { program: 'store', storageTextures: { out: 'global_xyz_node_4' }, repeat: 2 },
  { inputs: { inputTex: 'global_fb' }, outputs: { color: 'global_fb' } },
  { inputs: { inputTex: 'global_keep' }, outputs: { color: 'global_keep' } },
  { outputs: { color: 'global_o7' }, viewport: { x: 1, y: 2, w: 3, h: 4 } },
  { outputs: { color: 'global_o7' }, viewport: { x: '10%', y: 0, w: { param: 'vw', default: 8 }, h: '50%' }, uniforms: { vw: osc(2, { min: 0, max: 1 }) }, uniformSpecs: { vw: { min: 4, max: 20 } } },
  { outputs: { color: 'global_o7' }, viewport: { x: 2 }, clear: true },
  { outputs: { color: 'global_o7' }, viewport: { width: '25%', height: { screenDivide: 'div' } }, uniforms: { div: 2 } },
  { outputs: { color: 'global_o7' }, viewport: 'full' },
  { outputs: { color: 'global_o7' }, viewport: { x: 3, w: '50%' }, uniforms: { a: osc(0) } },
  { outputs: { color: 'global_o7' }, viewport: 'full', uniforms: { b: osc(1) } },
  { outputs: { color: 'global_o7' }, uniforms: { c: osc(2) } },
  { outputs: { color: 'node_12_nan' }, viewport: { x: 3, w: '50%' }, uniforms: { a: osc(0) } },
  { outputs: { color: 'node_12_nan' }, viewport: 'full', uniforms: { b: osc(1) } },
  { inputs: { inputTex: 'global_trail' }, outputs: { color: 'node_13_out' } },
  { program: 'media', effectKey: 'synth.media', nodeId: 'node_9', outputs: { color: 'global_geo1' } },
  { program: 'media', effectKey: 'synth.media', nodeId: 'node_9', outputs: { color: 'global_geo2' }, uniforms: { imageSize: [3, 4] } },
  { program: 'media', effectKey: 'synth.media', nodeId: 'node_9', outputs: { color: 'global_geo3' }, uniforms: { wobble: osc(0) } },
  { program: 'globals', inputs: { notes: 'midiNoteGrid' }, outputs: { color: 'node_10_out' } },
  { outputs: { color: 'node_11_mip' } }
], {
  global_keep: screen({ persistent: true }),
  global_o6: screen({ mipmaps: true }),
  node_10_out: screen(),
  node_11_mip: screen({ mipmaps: true }),
  node_12_nan: { width: 'x%', height: 'x%' },
  node_13_out: screen()
}, { programs })
sequence('synthetic/frame', [
  { op: 'create', graph: frame },
  { op: 'init', width: 40, height: 30 },
  { op: 'render', time: 0 },
  { op: 'render', time: 0.2 },
  { op: 'setTileRegion', offset: [10, 20], fullResolution: [400, 300], renderScale: 2.5 },
  { op: 'render', time: 0.4 },
  { op: 'setTileRegion', offset: [0, 5], fullResolution: [400, 100] },
  { op: 'render', time: 0.45 },
  { op: 'clearTileRegion' },
  { op: 'render', time: 0.3 },
  { op: 'syncTime', time: 0.6 },
  { op: 'render', time: 0.6 },
  { op: 'clearSurface', name: 'o1' },
  { op: 'clearSurface', name: 'mesh0' },
  { op: 'clearSurface', name: 'missing' },
  { op: 'resize', width: 20, height: 50 },
  { op: 'render', time: 0.7 },
  { op: 'recompile', graph: frame },
  { op: 'render', time: 0.8 }
], true)
sequence('synthetic/frame-pooled', [
  { op: 'create', graph: pooled, options: { texturePooling: true } },
  { op: 'init', width: 24, height: 12 },
  { op: 'render', time: 0.1 },
  { op: 'render', time: 0.2 }
], true)
sequence('synthetic/frame-errors', [
  { op: 'create', graph: graph([{ outputs: { color: 'global_o1' } }, { outputs: { color: 'global_o2' }, conditions: { skipIf: [null] } }]) },
  { op: 'init', width: 8, height: 8 },
  { op: 'render', time: 0.1 },
  { op: 'render', time: 0.2 },
  { op: 'create', graph: graph([{ outputs: { color: 'global_o1' }, conditions: { runIf: 5 } }]) },
  { op: 'init', width: 8, height: 8 },
  { op: 'render', time: 0.1 },
  { op: 'create', graph: graph([{ outputs: { color: 'global_o1' }, viewport: { x: 1, w: null } }]) },
  { op: 'init', width: 8, height: 8 },
  { op: 'render', time: 0.1 },
  { op: 'create', graph: graph([{ outputs: { color: 'global_o1' }, program: 'missing' }], {}, { programs: { p: program, missing: program } }) },
  { op: 'init', width: 8, height: 8 },
  { op: 'render', time: 0.1 }
])

// Errors and degenerate graphs.
sequence('synthetic/missing-program', [
  { op: 'create', graph: { passes: [{ id: 'x', program: 'nope' }], programs: {}, textures: {} } },
  { op: 'init', width: 8, height: 8 }
])
sequence('synthetic/no-programs', [
  { op: 'create', graph: { passes: [{ id: 'x' }] } },
  { op: 'init', width: 8, height: 8 }
])
sequence('synthetic/null-pass', [
  { op: 'create', graph: { passes: [null], programs: {} } },
  { op: 'init', width: 8, height: 8 }
])
sequence('synthetic/null-dimension', [
  { op: 'create', graph: graph([{ outputs: { color: 'node_n' } }], { node_n: { width: null, height: 'screen' } }) },
  { op: 'init', width: 8, height: 8 }
])
sequence('synthetic/null-global-dimension', [
  { op: 'create', graph: graph([{ outputs: { color: 'global_o3' } }], { global_o3: { width: 4, height: null } }) },
  { op: 'init', width: 8, height: 8 }
])
sequence('synthetic/null-graph', [
  { op: 'create', graph: null },
  { op: 'init', width: 12, height: 10 },
  { op: 'resize', width: 6, height: 5 }
], true)
sequence('synthetic/no-textures', [
  { op: 'create', graph: { passes: [], programs: {} } },
  { op: 'init', width: 12, height: 10 },
  { op: 'recompile', graph: { passes: [], textures: null } }
], true)
sequence('synthetic/empty-passes', [
  { op: 'create', graph: { passes: null, textures: { node_a: screen() } } },
  { op: 'init', width: 12, height: 10 }
], true)

// resolveDimension on its own: every form, the fallbacks and the diagnostics,
// which keep the last 64 unknown forms.
const uniforms = {
  v: 8, s: '12', arr: [5], obj: {}, nul: null, str: 'abc', z: 2, z0: 0, neg: -3, h: 2, 5: 7, null: 3,
  '[object Object]': 11, huge: 1e300, frac: 2.5
}
const specs = [
  0, -5, 3.7, 1e9, 0.5,
  'screen', 'auto', 'input', 'resolution', '50%', ' 25%', '%', 'abc%', '12.5%', '1e2%', 'Infinity%', '-50%', '\u00a050%', '50.%', '.5e1%', '12px%', '0x10%',
  'bogus', '', 'Screen',
  { param: 'v' }, { param: 'v', multiply: 2 }, { param: 'v', power: 2 }, { param: 'v', power: 2, default: 4096 },
  { param: 'missing', power: 2, default: 4096 }, { param: 'missing' }, { param: 'missing', paramDefault: 10, multiply: 3 },
  { param: 's' }, { param: 's', multiply: '2' }, { param: 'arr' }, { param: 'arr', power: 3 }, { param: 'obj' }, { param: 'constructor' },
  { param: 'toString', default: 5 }, { param: 'nul' }, { param: 'v', default: 'x' }, { param: 'str', default: 9 }, { param: 'str' },
  { param: 'str', multiply: 2 }, { param: 'str', power: 2, paramDefault: 3 }, { param: 'str', default: 'abc' }, { param: 5 },
  { param: {} }, { param: null }, { param: 'huge', power: 2 }, { param: 'frac', power: 0.5 }, { param: 'v', power: -1 },
  { param: 'missing', paramDefault: null }, { param: 'v', multiply: null }, { param: 'v', power: null }, { param: 'v', paramDefault: 1, default: null },
  { screenDivide: 'z' }, { screenDivide: 'z0' }, { screenDivide: 'missing', default: 4 }, { screenDivide: 'str' },
  { screenDivide: 'str', default: 'q' }, { screenDivide: 'neg' }, { screenDivide: 'h' }, { screenDivide: 'missing' }, { screenDivide: 'frac' },
  { scale: 0.5 }, { scale: 0.5, clamp: { min: 100 } }, { scale: 2, clamp: { max: 10 } }, { scale: 'x' }, { scale: 0.25, clamp: null },
  { scale: 0.1, clamp: { min: 'abc' } }, { scale: -1 }, { scale: 0.5, clamp: { min: 0, max: 0 } }, { scale: 1, clamp: 1 },
  {}, { foo: 1 }, [], [1, 2], true, false, { param: undefined, screenDivide: 'z' }, { b: 1, a: [2, { c: 'd' }] }
]
const dimensionOps = [{ op: 'create', graph: { passes: [], textures: {} } }]
for (const screenSize of [64, 5, 7, 0, -9]) {
  for (const spec of specs) dimensionOps.push({ op: 'resolveDimension', spec, screenSize, uniforms })
}
dimensionOps.push({ op: 'resolveDimension', screenSize: 33, uniforms })
dimensionOps.push({ op: 'resolveDimension', spec: null, screenSize: 33, uniforms })
dimensionOps.push({ op: 'resolveDimension', spec: { param: 'v' }, screenSize: 33 })
dimensionOps.push({ op: 'resolveDimension', spec: { param: 'v' }, screenSize: 33, uniforms: null })
dimensionOps.push({ op: 'resolveDimension', spec: '50%', screenSize: 33, uniforms: null })
for (let i = 0; i < 70; i++) dimensionOps.push({ op: 'resolveDimension', spec: `unknown${i}`, screenSize: 10, uniforms })
dimensionOps.push({ op: 'resolveDimension', spec: 'unknown3', screenSize: 10, uniforms })
sequence('synthetic/resolveDimension', dimensionOps)

const input = ops.map(op => JSON.stringify(op).replace(/[\u2028\u2029]/g, c => `\\u${c.charCodeAt(0).toString(16)}`)).join('\n') + '\n'
const run = (command, args) => {
  const output = execFileSync(command, args, { input, encoding: 'utf8', maxBuffer: 1 << 30, env: process.env })
  return new Map(output.trim().split('\n').map(line => JSON.parse(line)).map(record => [record.key, record]))
}
const reference = run('node', [join(ROOT, 'tools/reference-pipeline.mjs')])
const candidate = run(NM_DSL, ['pipeline'])

// Key order inside an object does not matter; the order of calls and of
// array elements does.
const canonical = value => JSON.stringify(value, (key, member) => member && typeof member === 'object' && !Array.isArray(member)
  ? Object.fromEntries(Object.keys(member).sort().map(k => [k, member[k]])) : member)
let total = 0
let matched = 0
let calls = 0
let shown = 0
for (const op of ops) {
  total++
  const ref = reference.get(op.key)
  const cand = candidate.get(op.key)
  const same = ref !== undefined && canonical(ref) === canonical(cand) && !(cleanKeys.has(op.key) && ref.error)
  if (same) { matched++; calls += ref.calls.length; continue }
  if (verbose || shown < 10) {
    shown++
    const show = value => value === undefined ? '(none)' : canonical(value).slice(0, 400)
    let detail
    if (!ref || !cand) {
      detail = `reference ${show(ref)} vs candidate ${show(cand)}`
    } else if (cleanKeys.has(op.key) && ref.error) {
      detail = `the reference throws ${show(ref.error)}`
    } else {
      const length = Math.max(ref.calls.length, cand.calls.length)
      const index = Array.from({ length }, (_, i) => i).find(i => canonical(ref.calls[i]) !== canonical(cand.calls[i])) ?? -1
      if (index >= 0) {
        detail = `call ${index}: reference ${show(ref.calls[index])} vs candidate ${show(cand.calls[index])}`
      } else {
        const field = [...new Set([...Object.keys(ref), ...Object.keys(cand), ...Object.keys(ref.state ?? {}), ...Object.keys(cand.state ?? {})])]
          .find(k => k in (ref.state ?? {}) || k in (cand.state ?? {})
            ? canonical(ref.state?.[k]) !== canonical(cand.state?.[k])
            : canonical(ref[k]) !== canonical(cand[k]))
        const pick = record => field in (record.state ?? {}) ? record.state[field] : record[field]
        detail = `${field}: reference ${show(pick(ref))} vs candidate ${show(pick(cand))}`
      }
    }
    err(`  ${op.key}: ${detail}`)
  }
}
for (const file of uncompiled) err(`  corpus/${file}: the reference does not compile it`)
for (const file of indexKeyed) err(`  corpus/${file}: a texture or allocation id is an array index`)
total += uncompiled.length + indexKeyed.length
out(`Pipeline: ${matched}/${total} ops match, ${calls} device calls (${sequences} sequences: ${graphs.length} corpus graphs set up, pooled and rendered, ${automated.length} graphs with automation, ${sequences - 3 * graphs.length - automated.length} synthetic)`)
process.exitCode = matched === total ? 0 : 1
