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
// uniforms}, whose result is the number as text ("-0" for negative zero);
// render {time}; syncTime {time}; setTileRegion {offset, fullResolution,
// renderScale}; clearTileRegion; clearSurface {name}; setUniform {name,
// value}; setPassUniform {pass, name, value}, the write the UI parameter
// paths make before broadcastChainScopedParam {pass, uniformName,
// scopedName}, where pass is a non-negative integer index into
// graph.passes and any other value names no pass; getFrameState, whose
// result is the frame state with texture records as their device ids. Numbers
// JSON cannot hold travel as {"$number": text}. A graph's textures and
// allocations, objects in JSON, are Maps again here.
//
// Math.sin and Math.cos are tools/js-math.mjs's, the correctly rounded
// functions the port uses: the last bit of V8's is not the same in every V8
// build.
//
//   NM_REFERENCE_ROOT=<reference-checkout> node tools/reference-pipeline.mjs < ops.jsonl
import { createInterface } from 'node:readline'
import { dirname, join, resolve } from 'node:path'
import { fileURLToPath, pathToFileURL } from 'node:url'
import { createMockDevice } from './mock-device.mjs'
import * as jsMath from './js-math.mjs'

Math.sin = jsMath.sin
Math.cos = jsMath.cos

if (!process.env.NM_REFERENCE_ROOT) { console.error('NM_REFERENCE_ROOT is not set'); process.exit(3) }
const REF = resolve(process.env.NM_REFERENCE_ROOT)
// The reference logs warnings and asyncInit failures; keep stdout for
// records, and each message on one line.
console.log = console.warn = console.info = console.error = (...args) => process.stderr.write(args.map(String).join(' ') + '\n')
const { bootstrapReference } = await import(pathToFileURL(join(dirname(fileURLToPath(import.meta.url)), 'reference-oracle.mjs')).href)
await bootstrapReference()
const { WebGPUBackend } = await import(pathToFileURL(join(REF, 'shaders/src/runtime/backends/webgpu.js')).href)
const { Pipeline } = await import(pathToFileURL(join(REF, 'shaders/src/runtime/pipeline.js')).href)
// The page's renderer, for its parameter path: CanvasRenderer methods on a
// renderer whose _pipeline is the op's pipeline and _enums the merged enum
// tree mergeIntoEnums leaves after the effects register.
const { CanvasRenderer } = await import(pathToFileURL(join(REF, 'shaders/src/renderer/canvas.js')).href)
const { default: mergedEnums } = await import(pathToFileURL(join(REF, 'shaders/src/lang/enums.js')).href)
const { MidiState, AudioState } = await import(pathToFileURL(join(REF, 'shaders/src/runtime/external-input.js')).href)
// Date.now() reads the op's time while fn runs (MIDI note times, falloff).
const realNow = Date.now
const at = (time, fn) => {
  if (time === undefined) return fn()
  Date.now = () => time
  try { return fn() } finally { Date.now = realNow }
}

// {"$number": text} stands for a number JSON cannot hold, and
// {"$float32": [numbers]} for a Float32Array.
const decodeValue = value => {
  if (Array.isArray(value)) return value.map(decodeValue)
  if (value && typeof value === 'object') {
    const keys = Object.keys(value)
    if (keys.length === 1 && keys[0] === '$number') return value.$number === '-0' ? -0 : Number(value.$number)
    if (keys.length === 1 && keys[0] === '$float32') return new Float32Array(value.$float32)
    return Object.fromEntries(keys.map(key => [key, decodeValue(value[key])]))
  }
  return value
}
// An op's pass, or undefined (see the header).
const passAt = index => {
  const passes = pipeline.graph?.passes
  return Array.isArray(passes) && Number.isInteger(index) && index >= 0 ? passes[index] : undefined
}
const decodeGraph = graph => {
  if (!graph || typeof graph !== 'object' || Array.isArray(graph)) return graph
  const out = { ...graph }
  for (const key of ['textures', 'allocations']) {
    if (out[key] && typeof out[key] === 'object' && !Array.isArray(out[key])) out[key] = new Map(Object.entries(out[key]))
  }
  return out
}
// JSON has no -0, NaN or Infinity: they travel as {"$number": text}, and a
// Float32Array as {"$float32": [numbers]}.
const plain = value => JSON.parse(JSON.stringify(value, (_key, member) => {
  if (member instanceof Map) return Object.fromEntries(member)
  if (member instanceof Float32Array) return { $float32: Array.from(member) }
  if (typeof member === 'number' && (!Number.isFinite(member) || Object.is(member, -0))) {
    return { $number: Object.is(member, -0) ? '-0' : String(member) }
  }
  return member
}) ?? 'null')
// An object whose undefined members are kept, as {"$undefined": true}.
const taggedUndefined = object => object && typeof object === 'object'
  ? Object.fromEntries(Object.entries(object).map(([key, member]) => [key, member === undefined ? { $undefined: true } : member]))
  : object

let mock = createMockDevice()
let backend = null
let pipeline = null
let midiState = null
let audioState = null
// The global audioWaveform and audioSpectrum alias the audio state's arrays
// here and are copies the frame takes in the port: they are compared only
// after a render, when both hold the frame's values.
const globalsFor = op => {
  const globals = { ...pipeline.globalUniforms }
  if (op.op !== 'render') { delete globals.audioWaveform; delete globals.audioSpectrum }
  return globals
}
const state = op => {
  if (!pipeline) return null
  const textures = []
  for (const [id, tex] of backend.textures) {
    textures.push([id, {
      handle: mock.idOf(tex.handle), width: tex.width, height: tex.height, depth: tex.depth, format: tex.format,
      persistent: tex.persistent === true, is3D: tex.is3D === true
    }])
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
    resourcePlan: pipeline.getResourcePlan(),
    globalUniforms: globalsFor(op),
    frameIndex: pipeline.frameIndex,
    lastTime: pipeline.lastTime,
    lastPassCount: pipeline.lastPassCount,
    frameReadTextures: pipeline.frameReadTextures ? [...pipeline.frameReadTextures] : null,
    frameWriteTextures: pipeline.frameWriteTextures ? [...pipeline.frameWriteTextures] : null,
    passViewports: passes.map(pass => pass?.viewportResolved ?? null),
    proxyUniforms: taggedUndefined(pipeline._oscillatorPassProxy.uniforms),
    resolvedUniforms: taggedUndefined(pipeline._resolvedUniforms)
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
        midiState = null
        audioState = null
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
      case 'render': at(op.wallTime, () => pipeline.render(op.time)); break
      case 'attachMidi': midiState = new MidiState({ portRegistry: op.registry !== false }); pipeline.setMidiState(midiState); break
      case 'detachMidi': pipeline.setMidiState(null); break
      case 'midiMessage': at(op.time, () => midiState.handleMessage(Uint8Array.from(op.data), op.port ?? undefined)); break
      case 'midiRegisterPort': midiState.registerPort(op.port); break
      case 'midiDisconnectPort': midiState.disconnectPort(op.id); break
      case 'midiInventory': midiState.setPortInventory(op.ports); break
      case 'attachAudio': audioState = new AudioState({ deviceRegistry: op.registry !== false }); pipeline.setAudioState(audioState); break
      case 'detachAudio': pipeline.setAudioState(null); break
      case 'audioBands': audioState.setBands(op.low, op.mid, op.high); break
      case 'audioRaw': audioState.setRaw(op.value); break
      case 'audioRawUnavailable': audioState.setRawUnavailable(); break
      case 'audioSpectrum': audioState.setSpectrum(Uint8Array.from(op.bytes)); break
      case 'audioWaveform': audioState.setWaveform(Uint8Array.from(op.bytes)); break
      case 'audioRegisterDevice': audioState.registerDevice(op.device); break
      case 'audioChannelValues': audioState.setChannelValues(op.id, op.channel, op.values); break
      case 'audioRegisterDefault': audioState.registerDefaultChannels(op.count); break
      case 'audioDefaultBands': audioState.getDefaultChannelState(op.channel)?.setBands(op.low, op.mid, op.high); break
      case 'audioInventory': audioState.setDeviceInventory(op.devices); break
      case 'audioDisconnectDevice': audioState.disconnectDevice(op.id); break
      case 'midiReset': midiState.reset(); break
      case 'audioDisconnectDefault': audioState.disconnectDefaultInput(); break
      case 'audioDeviceRawUnavailable': audioState.setDeviceRawUnavailable(op.id); break
      case 'getAudioInputRequirements': result = plain(pipeline.getAudioInputRequirements()); break
      case 'syncTime': pipeline.syncTime(op.time); break
      case 'setTileRegion': pipeline.setTileRegion({ offset: op.offset, fullResolution: op.fullResolution, renderScale: op.renderScale }); break
      case 'clearTileRegion': pipeline.clearTileRegion(); break
      case 'clearSurface': pipeline.clearSurface(op.name); break
      case 'setUniform': pipeline.setUniform(op.name, decodeValue(op.value)); break
      case 'setPassUniform': passAt(op.pass).uniforms[op.name] = decodeValue(op.value); break
      case 'applyStepParameterValues': {
        const renderer = Object.create(CanvasRenderer.prototype)
        renderer._pipeline = pipeline
        renderer._enums = mergedEnums
        renderer.applyStepParameterValues(decodeValue(op.values))
        break
      }
      case 'broadcastChainScopedParam': pipeline.broadcastChainScopedParam(passAt(op.pass), op.uniformName, op.scopedName); break
      case 'getFrameState': {
        const state = pipeline.getFrameState()
        const surfaces = {}
        for (const [name, tex] of Object.entries(state.surfaces)) if (tex !== undefined) surfaces[name] = mock.idOf(tex.handle)
        const writeSurfaces = {}
        for (const [name, id] of Object.entries(state.writeSurfaces)) writeSurfaces[name] = id ?? null
        result = plain({
          frameIndex: state.frameIndex, time: state.time, globalUniforms: globalsFor(op), surfaces, writeSurfaces,
          screenWidth: state.screenWidth, screenHeight: state.screenHeight, renderSurface: state.graph?.renderSurface ?? null
        })
        break
      }
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
  const record = { key: op.key, calls: mock.takeLog(), state: state(op) }
  if (result !== undefined) record.result = result
  if (error) record.error = error
  process.stdout.write(JSON.stringify(record) + '\n')
}
