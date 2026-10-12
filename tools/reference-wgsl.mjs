#!/usr/bin/env node
// reference-wgsl.mjs — the reference WebGPU backend's WGSL source analysis, as
// the oracle of parity/check_wgsl.mjs. It reads JSON lines {id, source,
// defines?} on stdin and writes {id, result} or {id, error}, in the shape
// `nm-dump wgsl` writes: the methods of the reference's WebGPUBackend run on
// an object without a device, since none of them touches one.
//
//   NM_REFERENCE_ROOT=<reference-checkout> node tools/reference-wgsl.mjs < cases.jsonl
import { createInterface } from 'node:readline'
import { join, resolve } from 'node:path'
import { pathToFileURL } from 'node:url'

if (!process.env.NM_REFERENCE_ROOT) { console.error('NM_REFERENCE_ROOT is not set'); process.exit(3) }
const REF = resolve(process.env.NM_REFERENCE_ROOT)
const { WebGPUBackend } = await import(pathToFileURL(join(REF, 'shaders/src/runtime/backends/webgpu.js')).href)
const backend = Object.create(WebGPUBackend.prototype)

function packedLayout (layout) {
  if (layout === null || layout === undefined) return null
  // A byte layout is an array carrying structSize, which JSON would drop.
  if (layout.type === 'byte') return { type: 'byte', layout: [...layout.layout], structSize: layout.layout.structSize }
  return layout
}

function analyze (testCase) {
  let source = testCase.source
  const result = {}
  if ('defines' in testCase) {
    source = backend.injectDefines(source, testCase.defines)
    result.injected = source
  }
  const bindings = backend.parseShaderBindings(source)
  result.stripped = backend.stripWGSLComments(source)
  result.entryPoints = backend.detectEntryPoints(source)
  result.computeEntryPoints = [...source.matchAll(/@compute[^f]*fn\s+(\w+)/g)].map(m => m[1])
  result.hasCompute = /@compute\s/.test(source)
  result.hasFragment = /@fragment\s/.test(source)
  result.hasVertex = /@vertex\s/.test(source)
  result.bindings = bindings
  result.entryPointBindings = [...backend.parseEntryPointBindings(source, bindings)].map(([entry, used]) => [entry, [...used]])
  result.declaredUniformBufferSize = backend.parseDeclaredUniformBufferSize(source)
  result.packedUniformLayout = packedLayout(backend.parsePackedUniformLayout(source))
  result.samplerTextures = [...backend.parseSamplerTextureUses(source)]
  result.hasBindings = backend.hasShaderBindings(source)
  return result
}

const lines = createInterface({ input: process.stdin, crlfDelay: Infinity })
for await (const line of lines) {
  if (!line) continue
  const testCase = JSON.parse(line)
  let record
  try {
    record = { id: testCase.id, result: analyze(testCase) }
  } catch (err) {
    record = { id: testCase.id, error: String(err?.message ?? err) }
  }
  process.stdout.write(JSON.stringify(record) + '\n')
}
