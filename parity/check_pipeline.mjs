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
const corpusKeys = new Set()
let sequences = 0
const sequence = (prefix, calls, corpus = false) => {
  sequences++
  calls.forEach((call, i) => {
    const key = `${prefix}#${i}:${call.op}`
    if (corpus) corpusKeys.add(key)
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
    { op: 'recompile', graph }
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
])

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
])

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
])
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
])

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
])
sequence('synthetic/no-textures', [
  { op: 'create', graph: { passes: [], programs: {} } },
  { op: 'init', width: 12, height: 10 },
  { op: 'recompile', graph: { passes: [], textures: null } }
])
sequence('synthetic/empty-passes', [
  { op: 'create', graph: { passes: null, textures: { node_a: screen() } } },
  { op: 'init', width: 12, height: 10 }
])

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
  const same = ref !== undefined && canonical(ref) === canonical(cand) && !(corpusKeys.has(op.key) && ref.error)
  if (same) { matched++; calls += ref.calls.length; continue }
  if (verbose || shown < 10) {
    shown++
    const show = value => value === undefined ? '(none)' : canonical(value).slice(0, 400)
    let detail
    if (!ref || !cand) {
      detail = `reference ${show(ref)} vs candidate ${show(cand)}`
    } else if (corpusKeys.has(op.key) && ref.error) {
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
out(`Pipeline: ${matched}/${total} ops match, ${calls} device calls (${sequences} sequences: ${graphs.length} corpus graphs, without and with texture pooling, ${sequences - 2 * graphs.length} synthetic)`)
process.exitCode = matched === total ? 0 : 1
