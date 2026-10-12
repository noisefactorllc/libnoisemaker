#!/usr/bin/env node
// check_wgsl.mjs — gate libnoisemaker's WGSL source analysis (src/gpu/wgsl)
// against the reference WebGPU backend's own methods (tools/reference-wgsl.mjs).
//
// Cases: every WGSL file under the reference's shaders/effects at the pin, as
// written and with its effect's compile-time defines at their defaults
// (injectDefines), plus parity/wgsl/synthetic.json, shaders that reach the
// branches the catalog does not. Each case's entry points, bindings, entry
// point bindings, uniform buffer size, packed uniform layout, sampler uses,
// stripped source and injected source must equal the reference's exactly. An
// error on either side fails the case.
//
//   NM_REFERENCE_ROOT=<checkout> NM_DSL=<nm-dump> NM_CATALOG_EFFECTS_DIR=<build>/catalog/effects \
//     node parity/check_wgsl.mjs [--verbose]
import { execFileSync } from 'node:child_process'
import { existsSync, readFileSync, readdirSync } from 'node:fs'
import { dirname, join, relative, resolve } from 'node:path'
import { fileURLToPath } from 'node:url'

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..')
if (!process.env.NM_REFERENCE_ROOT) { console.error('NM_REFERENCE_ROOT is not set'); process.exit(3) }
const REF = resolve(process.env.NM_REFERENCE_ROOT)
const NM_DSL = process.env.NM_DSL || join(ROOT, 'build', 'nm-dump')
const EFFECTS_JSON = process.env.NM_CATALOG_EFFECTS_DIR || join(ROOT, 'build', 'catalog', 'effects')
const verbose = process.argv.includes('--verbose')
if (!existsSync(NM_DSL)) { console.error(`nm-dump not found at ${NM_DSL}`); process.exit(2) }

function files (dir) {
  return readdirSync(dir, { withFileTypes: true }).flatMap(entry => {
    const path = join(dir, entry.name)
    return entry.isDirectory() ? files(path) : [path]
  }).sort()
}

const effectsDir = join(REF, 'shaders/effects')
const cases = []
let catalogFiles = 0
for (const path of files(effectsDir)) {
  const rel = relative(effectsDir, path).replaceAll('\\', '/')
  const parts = rel.split('/')
  if (parts.length !== 4 || parts[2] !== 'wgsl' || !rel.endsWith('.wgsl')) continue
  catalogFiles++
  const source = readFileSync(path, 'utf8')
  cases.push({ id: rel, source })
  const definitionPath = join(EFFECTS_JSON, parts[0], `${parts[1]}.json`)
  const defines = {}
  if (existsSync(definitionPath)) {
    const definition = JSON.parse(readFileSync(definitionPath, 'utf8'))
    for (const spec of Object.values(definition.globals || {})) {
      if (spec && spec.define && spec.default !== undefined) defines[spec.define] = spec.default
    }
  }
  cases.push({ id: `${rel} with defines`, source, defines })
}
if (catalogFiles === 0) { console.error(`no WGSL files under ${effectsDir}`); process.exit(2) }
const synthetic = JSON.parse(readFileSync(join(ROOT, 'parity/wgsl/synthetic.json'), 'utf8'))
// Inputs too long to keep as data: a comment annotation repeated thousands of
// times, which a recursive matcher would overflow the stack on, and digit runs
// past the largest double, which parseInt reads as Infinity.
const names = count => Array.from({ length: count }, (_, i) => `n${i}`).join(', ')
const annotated = count => `struct LongParams { a: vec4<f32>, // (${names(count)})\n}\n` +
  '@group(0) @binding(0) var<uniform> p: LongParams;\n@fragment fn main() -> @location(0) vec4f { return p.a; }\n'
const digits = '9'.repeat(310)
synthetic.cases.push(
  { id: 'long-annotation-6000', source: annotated(6000) },
  { id: 'long-annotation-20000', source: annotated(20000) },
  { id: 'long-digit-runs', source: `struct Big { arr: array<vec4<f32>, ${digits}> }\n@group(0) @binding(${digits}) var<uniform> big: Big;\n` +
    `struct Uniforms { data: array<vec4<f32>, 2> }\n@group(${digits}) @binding(1) var<uniform> uniforms: Uniforms;\n` +
    `@fragment fn main() -> @location(0) vec4f { let x = uniforms.data[${digits}].x; return big.arr[0] + vec4f(x); }\n` })
for (const testCase of synthetic.cases) cases.push({ ...testCase, id: `synthetic/${testCase.id}` })

const input = cases.map(c => JSON.stringify(c)).join('\n') + '\n'
const run = (command, args) => {
  const out = execFileSync(command, args, { input, encoding: 'utf8', maxBuffer: 1 << 30, env: process.env })
  return new Map(out.trim().split('\n').map(line => JSON.parse(line)).map(record => [record.id, record]))
}
const reference = run('node', [join(ROOT, 'tools/reference-wgsl.mjs')])
const candidate = run(NM_DSL, ['wgsl'])

let matched = 0
let shown = 0
for (const testCase of cases) {
  const ref = reference.get(testCase.id)
  const cand = candidate.get(testCase.id)
  const same = ref?.result !== undefined && cand?.result !== undefined &&
    JSON.stringify(ref.result) === JSON.stringify(cand.result)
  if (same) { matched++; continue }
  if (verbose || shown < 10) {
    shown++
    const field = ref?.result && cand?.result
      ? Object.keys(ref.result).find(key => JSON.stringify(ref.result[key]) !== JSON.stringify(cand.result[key]))
      : null
    console.error(`  ${testCase.id}: ${field ? `${field}: reference ${JSON.stringify(ref.result[field]).slice(0, 200)} vs candidate ${JSON.stringify(cand.result[field]).slice(0, 200)}` : `reference ${JSON.stringify(ref).slice(0, 200)} vs candidate ${JSON.stringify(cand).slice(0, 200)}`}`)
  }
}
console.log(`WGSL: ${matched}/${cases.length} match (${catalogFiles} catalog files, as written and with defines; ${synthetic.cases.length} synthetic)`)
process.exitCode = matched === cases.length ? 0 : 1
