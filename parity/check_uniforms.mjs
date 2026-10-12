#!/usr/bin/env node
// check_uniforms.mjs — gate libnoisemaker's uniform packing (src/gpu/backend)
// against the reference WebGPU backend's own methods (tools/reference-uniforms.mjs).
//
// Each sequence runs on one fresh packer and one fresh WebGPUBackend, because
// the bytes depend on what earlier calls left behind. Sequences:
//   - every parity/programs graph, and the two effects that bind the audio
//     arrays (synth/scope and synth/spectrum, which the corpus lacks), compiled
//     by the reference: two frames of createUniformBuffer per pass, with the
//     pipeline's per-frame global uniforms, and createSingleUniformBuffer for
//     each plain uniform binding;
//   - seeded random sequences of uniform values and layouts;
//   - parity/uniforms/synthetic.json, sequences that reach the branches and
//     errors the others do not.
// Each call's buffer size and bytes, or its error name and message, must equal
// the reference's. Corpus calls must not throw, and every corpus program must
// compile. {"$float32": [numbers]} and
// {"$float32bits": [hex]} carry a Float32Array to both sides
// (tools/reference-uniforms.mjs).
//
//   NM_REFERENCE_ROOT=<checkout> NM_DSL=<nm-dump> node parity/check_uniforms.mjs [--verbose]
import { execFileSync } from 'node:child_process'
import { existsSync, readFileSync, readdirSync } from 'node:fs'
import { dirname, join, resolve } from 'node:path'
import { fileURLToPath, pathToFileURL } from 'node:url'

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..')
if (!process.env.NM_REFERENCE_ROOT) { console.error('NM_REFERENCE_ROOT is not set'); process.exit(3) }
const REF = resolve(process.env.NM_REFERENCE_ROOT)
const NM_DSL = process.env.NM_DSL || join(ROOT, 'build', 'nm-dump')
const verbose = process.argv.includes('--verbose')
if (!existsSync(NM_DSL)) { console.error(`nm-dump not found at ${NM_DSL}`); process.exit(2) }
const out = line => process.stdout.write(line + '\n')
const err = line => process.stderr.write(line + '\n')

const { bootstrapReference } = await import(pathToFileURL(join(ROOT, 'tools/reference-oracle.mjs')).href)
globalThis.GPUBufferUsage ??= { UNIFORM: 64, COPY_DST: 8 }
const { WebGPUBackend } = await import(pathToFileURL(join(REF, 'shaders/src/runtime/backends/webgpu.js')).href)
const analysis = new WebGPUBackend({ queue: {}, addEventListener () {} }, null)

const ops = []
const corpusIds = new Set()
let sequences = 0
const sequence = (prefix, calls, corpus = false) => {
  sequences++
  ops.push({ op: 'reset' })
  calls.forEach((call, i) => {
    const id = `${prefix}#${i}`
    if (corpus) corpusIds.add(id)
    ops.push({ ...call, id })
  })
}

// The pipeline's updateGlobalUniforms, in the order it first sets them, with
// the audio waveform and spectrum (Float32Array(128)) when audio is connected.
const float32 = values => ({ $float32: values })
const frameGlobals = (width, height, frame, audio) => ({
  time: frame * 0.5371, deltaTime: frame ? 1 / 60 : 0, frame,
  resolution: [width, height], tileOffset: [0, 0], fullResolution: [width, height],
  aspect: width / height, aspectRatio: width / height, aspectInv: 1 / (width / height),
  invFullResolution: [1 / width, 1 / height], renderScale: 1,
  ...(audio ? {
    audioWaveform: float32(Array.from({ length: 128 }, (_, i) => Math.sin(i * 0.37 + frame) * 0.5 + 0.5)),
    audioSpectrum: float32(Array.from({ length: 128 }, (_, i) => Math.exp(-i / 40) * (1 + frame) / 2))
  } : {})
})
const plainUniform = typeDecl => !(typeDecl && !typeDecl.includes('<') && typeDecl !== 'f32' && typeDecl !== 'i32' &&
  typeDecl !== 'u32' && typeDecl !== 'bool' && !typeDecl.startsWith('vec') && !typeDecl.startsWith('mat'))
const validSingle = value => typeof value === 'number' || typeof value === 'boolean' || Array.isArray(value) ||
  Array.isArray(value?.$float32)

const mod = await bootstrapReference()
const programsDir = join(ROOT, 'parity/programs')
const sizes = [[1024, 1024], [640, 360], [333, 517]]
const corpusSources = readdirSync(programsDir).filter(f => f.endsWith('.dsl')).sort()
  .map(file => [file, readFileSync(join(programsDir, file), 'utf8')])
corpusSources.push(['audio/scope', 'search synth\nscope().write(o0)\nrender(o0)'],
  ['audio/spectrum', 'search synth\nspectrum().write(o0)\nrender(o0)'])
let corpusPrograms = 0
const uncompiled = []
for (const [index, [file, source]] of corpusSources.entries()) {
  let graph
  try { graph = mod.compileGraph(source) } catch { uncompiled.push(file); continue }
  corpusPrograms++
  const [width, height] = sizes[index % sizes.length]
  const calls = []
  for (const frame of [0, 1]) {
    const globalUniforms = frameGlobals(width, height, frame, index % 2 === 0 || file.startsWith('audio/'))
    for (const pass of graph.passes) {
      const spec = graph.programs?.[pass.program]
      if (!spec?.wgsl) continue
      const program = { source: spec.wgsl, defines: spec.defines, uniformLayout: spec.uniformLayout }
      calls.push({ op: 'struct', passUniforms: pass.uniforms, globalUniforms, program })
      const source = analysis.injectDefines(spec.wgsl, spec.defines || {})
      for (const binding of analysis.parseShaderBindings(source)) {
        if (binding.type !== 'uniform' || !plainUniform(binding.typeDecl)) continue
        const value = pass.uniforms && binding.name in pass.uniforms ? pass.uniforms[binding.name]
          : binding.name in globalUniforms ? globalUniforms[binding.name] : undefined
        if (validSingle(value)) calls.push({ op: 'single', value, typeDecl: binding.typeDecl })
      }
    }
  }
  sequence(`corpus/${file}`, calls, true)
}

// Seeded random sequences (mulberry32).
let seed = 0x5eed
const random = () => {
  seed = (seed + 0x6D2B79F5) | 0
  let t = Math.imul(seed ^ (seed >>> 15), 1 | seed)
  t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t
  return ((t ^ (t >>> 14)) >>> 0) / 4294967296
}
const pick = list => list[Math.floor(random() * list.length)]
const names = ['time', 'deltaTime', 'aspect', 'resolution', 'width', 'height', 'channels', 'channelCount',
  'a', 'b', 'c', 'd', 'e', 'f', 'speed', 'tint', '0', '7', '12', 'toString', 'frame']
const numericStrings = ['', '3', 'x', '0x10', '+2', '-0x1', '0b11', '0o17', '\u00a03\u2028', '1e400', '.5', '5.', 'Infinity', '-0', '1_0', 'inf']
const scalar = () => pick([
  () => Math.floor(random() * 11) - 5, () => (random() - 0.5) * 1000, () => random() < 0.5,
  () => Math.floor((random() - 0.5) * 2 ** 34), () => null, () => pick(numericStrings), () => 0.5 * Math.floor(random() * 9 - 4)
])()
const length = () => pick([0, 1, 2, 2, 3, 3, 4, 4, 5, 9, 16, 17, 128])
const value = () => {
  const roll = random()
  if (roll < 0.5) return scalar()
  if (roll < 0.6) return float32(Array.from({ length: length() }, () => (random() - 0.5) * 100))
  return Array.from({ length: length() }, () => random() < 0.9 ? (random() - 0.5) * 100 : scalar())
}
const record = count => {
  const result = {}
  for (let i = 0; i < count; i++) result[pick(names)] = value()
  return result
}
const components = () => pick(['x', 'y', 'z', 'w', 'xy', 'yz', 'zw', 'xyz', 'yzw', 'xyzw', 'q', ''])
const slotLayout = () => {
  const entries = Array.from({ length: 1 + Math.floor(random() * 6) }, () =>
    ({ name: pick(names), slot: Math.floor(random() * 5), components: components() }))
  if (random() < 0.5) return entries
  return Object.fromEntries(entries.map(({ name, slot, components }) => [name, { slot, components }]))
}
const byteLayout = () => ({
  type: 'byte',
  layout: Array.from({ length: 1 + Math.floor(random() * 6) }, () => ({
    name: pick(names), offset: 4 * Math.floor(random() * 16), size: pick([4, 8, 12, 16]),
    type: pick(['int', 'uint', 'float', 'bool']), components: 1 + Math.floor(random() * 4)
  }))
})
const synthetic = JSON.parse(readFileSync(join(ROOT, 'parity/uniforms/synthetic.json'), 'utf8'))
const shaders = synthetic.shaders
for (let s = 0; s < 60; s++) {
  const calls = []
  for (let i = 0; i < 12; i++) {
    if (random() < 0.25) {
      calls.push({ op: 'single', value: value(), typeDecl: pick(['f32', 'i32', 'u32', 'vec2<f32>', 'vec4<f32>', 'array<vec4<f32>, 2>', 'array<vec4<f32>, 32>', 'array<f32, 3>']) })
      continue
    }
    const shader = pick(Object.keys(shaders))
    const layout = pick([undefined, undefined, slotLayout, byteLayout])
    calls.push({
      op: 'struct',
      passUniforms: random() < 0.1 ? null : record(Math.floor(random() * 7)),
      globalUniforms: record(Math.floor(random() * 5)),
      program: { source: shaders[shader], uniformLayout: layout?.() }
    })
  }
  sequence(`random/${s}`, calls)
}

for (const { id, ops: calls } of synthetic.sequences) {
  sequence(`synthetic/${id}`, calls.map(({ shader, defines, uniformLayout, ...call }) =>
    call.op === 'struct' ? { ...call, program: { source: shaders[shader], defines, uniformLayout } } : call))
}

// readline ends a line at U+2028 and U+2029 too, which JSON.stringify leaves raw.
const input = ops.map(op => JSON.stringify(op).replace(/[\u2028\u2029]/g, c => `\\u${c.charCodeAt(0).toString(16)}`)).join('\n') + '\n'
const run = (command, args) => {
  const output = execFileSync(command, args, { input, encoding: 'utf8', maxBuffer: 1 << 30, env: process.env })
  return new Map(output.trim().split('\n').map(line => JSON.parse(line)).map(r => [r.id, r]))
}
const reference = run('node', [join(ROOT, 'tools/reference-uniforms.mjs')])
const candidate = run(NM_DSL, ['uniforms'])

let total = 0
let matched = 0
let errors = 0
let shown = 0
for (const op of ops) {
  if (op.op === 'reset') continue
  total++
  const ref = reference.get(op.id)
  const cand = candidate.get(op.id)
  const same = ref !== undefined && JSON.stringify(ref) === JSON.stringify(cand) &&
    !(corpusIds.has(op.id) && ref.error !== undefined)
  if (same) {
    matched++
    if (ref.error !== undefined) errors++
    continue
  }
  if (verbose || shown < 10) {
    shown++
    err(`  ${op.id}: reference ${JSON.stringify(ref).slice(0, 300)} vs candidate ${JSON.stringify(cand).slice(0, 300)}`)
  }
}
for (const file of uncompiled) err(`  corpus/${file}: the reference does not compile it`)
total += uncompiled.length
out(`Uniforms: ${matched}/${total} match (${sequences} sequences: ${corpusPrograms} corpus graphs, 60 random, ` +
  `${synthetic.sequences.length} synthetic; ${errors} matching errors)`)
process.exitCode = matched === total ? 0 : 1
