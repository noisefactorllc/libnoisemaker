#!/usr/bin/env node
// reference-backend.mjs — the reference WebGPU backend's device calls, the
// oracle of parity/check_backend.mjs. It replays the JSON lines `nm-dump
// backend` reads on the reference's own WebGPUBackend, whose device is the
// recording mock of tools/mock-device.mjs. For each op it writes
// {key, calls}, with result for a method whose return value callers use, and
// error: {name, message} (or {thrown}) when the op throws.
//
//   NM_REFERENCE_ROOT=<reference-checkout> node tools/reference-backend.mjs < ops.jsonl
import { createInterface } from 'node:readline'
import { join, resolve } from 'node:path'
import { pathToFileURL } from 'node:url'
import { createMockDevice } from './mock-device.mjs'

if (!process.env.NM_REFERENCE_ROOT) { console.error('NM_REFERENCE_ROOT is not set'); process.exit(3) }
const REF = resolve(process.env.NM_REFERENCE_ROOT)
// The reference logs warnings; keep stdout for records.
console.log = console.warn = console.info = (...args) => process.stderr.write(args.map(String).join(' ') + '\n')
const { WebGPUBackend } = await import(pathToFileURL(join(REF, 'shaders/src/runtime/backends/webgpu.js')).href)

let mock = createMockDevice()

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
      case 'reset': mock = createMockDevice(); backend = new WebGPUBackend(mock.device, null); break
      case 'rejectBindings': mock.rejectBindings(op.bindings, op.message); break
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
  const record = { key: op.key, calls: mock.takeLog() }
  // A method's return value, where a caller uses it (uploadMeshData).
  if (result !== undefined) record.result = result
  if (error) record.error = error
  process.stdout.write(JSON.stringify(record) + '\n')
}
