#!/usr/bin/env node
// reference-pipeline.mjs — the reference Pipeline's device calls and state,
// the oracle of parity/check_pipeline.mjs. It replays the JSON lines `nm-dump
// pipeline` reads on the reference's own Pipeline and WebGPUBackend, whose
// device is the recording mock of tools/mock-device.mjs, with the effects
// registered as tools/reference-oracle.mjs registers them. For each op it
// writes {key, calls, state}, with result where the op returns a value and
// error: {name, message} (or {thrown}) when it throws.
//
// Ops: create {graph, options}; init and resize {width, height}; recompile
// {graph}, the steps of compiler.js recompile() once it has the graph;
// updateParameterTextures {uniforms}; resolveDimension {spec, screenSize,
// uniforms}, whose result is the number as text ("-0" for negative zero). A
// graph's textures and allocations, objects in JSON, are Maps again here.
//
//   NM_REFERENCE_ROOT=<reference-checkout> node tools/reference-pipeline.mjs < ops.jsonl
import { createInterface } from 'node:readline'
import { dirname, join, resolve } from 'node:path'
import { fileURLToPath, pathToFileURL } from 'node:url'
import { createMockDevice } from './mock-device.mjs'

if (!process.env.NM_REFERENCE_ROOT) { console.error('NM_REFERENCE_ROOT is not set'); process.exit(3) }
const REF = resolve(process.env.NM_REFERENCE_ROOT)
// The reference logs warnings and asyncInit failures; keep stdout for
// records, and each message on one line.
console.log = console.warn = console.info = console.error = (...args) => process.stderr.write(args.map(String).join(' ') + '\n')
const { bootstrapReference } = await import(pathToFileURL(join(dirname(fileURLToPath(import.meta.url)), 'reference-oracle.mjs')).href)
await bootstrapReference()
const { WebGPUBackend } = await import(pathToFileURL(join(REF, 'shaders/src/runtime/backends/webgpu.js')).href)
const { Pipeline } = await import(pathToFileURL(join(REF, 'shaders/src/runtime/pipeline.js')).href)

const decodeGraph = graph => {
  if (!graph || typeof graph !== 'object' || Array.isArray(graph)) return graph
  const out = { ...graph }
  for (const key of ['textures', 'allocations']) {
    if (out[key] && typeof out[key] === 'object' && !Array.isArray(out[key])) out[key] = new Map(Object.entries(out[key]))
  }
  return out
}
const plain = value => JSON.parse(JSON.stringify(value, (_key, member) => member instanceof Map ? Object.fromEntries(member) : member) ?? 'null')

let mock = createMockDevice()
let backend = null
let pipeline = null
const state = () => {
  if (!pipeline) return null
  const textures = []
  for (const [id, tex] of backend.textures) {
    textures.push([id, { handle: mock.idOf(tex.handle), width: tex.width, height: tex.height, depth: tex.depth, format: tex.format }])
  }
  const passes = Array.isArray(pipeline.graph?.passes) ? pipeline.graph.passes : []
  return plain({
    width: pipeline.width,
    height: pipeline.height,
    surfaces: [...pipeline.surfaces],
    mipTargets: pipeline._mipTargets,
    feedbackSurfaces: pipeline._feedbackSurfaces ? [...pipeline._feedbackSurfaces] : null,
    needsMidiNoteGrid: pipeline._needsMidiNoteGrid ?? null,
    textureAliases: [...pipeline._textureAliases],
    diagnostics: pipeline.diagnostics.records,
    lifecycleEffects: [...pipeline._lifecycleEffects.keys()],
    textures,
    passUniforms: passes.map(pass => pass?.uniforms ?? null),
    textureSpecs: pipeline.graph?.textures ?? null,
    resourcePlan: pipeline.getResourcePlan()
  })
}

const lines = createInterface({ input: process.stdin, crlfDelay: Infinity })
for await (const line of lines) {
  if (!line) continue
  const op = JSON.parse(line)
  let error
  let result
  try {
    switch (op.op) {
      case 'create':
        mock = createMockDevice()
        backend = new WebGPUBackend(mock.device, null)
        pipeline = new Pipeline(decodeGraph(op.graph), backend, op.options ?? {})
        break
      case 'init': await pipeline.init(op.width, op.height); break
      case 'resize': pipeline.resize(op.width, op.height); break
      case 'recompile': {
        pipeline.graph = decodeGraph(op.graph)
        pipeline.createSurfaces()
        const defaultUniforms = pipeline.collectDefaultUniforms()
        pipeline.recreateTextures(defaultUniforms)
        pipeline.initAsyncEffects()
        pipeline.initLifecycleEffects()
        break
      }
      case 'updateParameterTextures': pipeline.updateParameterTextures(op.uniforms); break
      case 'resolveDimension': {
        const value = pipeline.resolveDimension(op.spec, op.screenSize, op.uniforms)
        result = Object.is(value, -0) ? '-0' : String(value)
        break
      }
      default: throw new Error(`unknown op ${op.op}`)
    }
  } catch (err) {
    // A thrown plain object (`throw { code, ... }`) is compared as data.
    error = err instanceof Error
      ? { name: String(err.name), message: String(err.message) }
      : { thrown: JSON.parse(JSON.stringify(err) ?? 'null') }
  }
  // Promise callbacks (asyncInit failures, onSubmittedWorkDone) run before the next op.
  await new Promise(resolve => setImmediate(resolve))
  const record = { key: op.key, calls: mock.takeLog(), state: state() }
  if (result !== undefined) record.result = result
  if (error) record.error = error
  process.stdout.write(JSON.stringify(record) + '\n')
}
