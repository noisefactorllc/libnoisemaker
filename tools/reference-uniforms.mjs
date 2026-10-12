#!/usr/bin/env node
// reference-uniforms.mjs — the reference WebGPU backend's uniform buffer
// writes, as the oracle of parity/check_uniforms.mjs. It replays the JSON
// lines `nm-dump uniforms` reads on a WebGPUBackend whose device records each
// buffer it creates and each queue.writeBuffer, and writes {id, write} or
// {id, error: {name, message}}: write is null or {bufferSize, bytes} (hex).
// {"$float32": [numbers]} in a uniform value stands for a Float32Array, and
// {"$float32bits": [hex strings]} for one given by its raw bits.
//
//   NM_REFERENCE_ROOT=<reference-checkout> node tools/reference-uniforms.mjs < ops.jsonl
import { createInterface } from 'node:readline'
import { join, resolve } from 'node:path'
import { pathToFileURL } from 'node:url'

if (!process.env.NM_REFERENCE_ROOT) { console.error('NM_REFERENCE_ROOT is not set'); process.exit(3) }
const REF = resolve(process.env.NM_REFERENCE_ROOT)
globalThis.GPUBufferUsage ??= { MAP_READ: 1, MAP_WRITE: 2, COPY_SRC: 4, COPY_DST: 8, INDEX: 16, VERTEX: 32, UNIFORM: 64, STORAGE: 128, INDIRECT: 256, QUERY_RESOLVE: 512 }
const { WebGPUBackend } = await import(pathToFileURL(join(REF, 'shaders/src/runtime/backends/webgpu.js')).href)

let last = null
const device = {
  queue: {
    writeBuffer (buffer, bufferOffset, data, dataOffset = 0, size) {
      const bytes = Buffer.from(data instanceof ArrayBuffer ? data : data.buffer, dataOffset, size)
      last = { bufferSize: buffer.size, bytes: bytes.toString('hex') }
    }
  },
  addEventListener () {},
  createBuffer ({ size }) { return { size } }
}
let backend = new WebGPUBackend(device, null)

const decode = value => {
  if (!value || typeof value !== 'object' || Array.isArray(value) || Object.keys(value).length !== 1) return value
  if (Array.isArray(value.$float32)) return new Float32Array(value.$float32)
  if (Array.isArray(value.$float32bits)) return new Float32Array(new Uint32Array(value.$float32bits.map(h => parseInt(h, 16))).buffer)
  return value
}
const decodeMembers = encoded => {
  const value = decode(encoded)
  return value && typeof value === 'object' && !Array.isArray(value) && !ArrayBuffer.isView(value)
    ? Object.fromEntries(Object.entries(value).map(([key, member]) => [key, decode(member)])) : value
}

const lines = createInterface({ input: process.stdin, crlfDelay: Infinity })
for await (const line of lines) {
  if (!line) continue
  const op = JSON.parse(line)
  if (op.op === 'reset') { backend = new WebGPUBackend(device, null); continue }
  let record
  try {
    last = null
    let buffer
    if (op.op === 'struct') {
      const source = backend.injectDefines(op.program.source, op.program.defines || {})
      const program = {
        packedUniformLayout: op.program.uniformLayout || backend.parsePackedUniformLayout(source),
        declaredUniformBufferSize: backend.parseDeclaredUniformBufferSize(source)
      }
      const pass = op.passUniforms === undefined ? {} : { uniforms: decodeMembers(op.passUniforms) }
      buffer = backend.createUniformBuffer(pass, { globalUniforms: decodeMembers(op.globalUniforms) }, program)
    } else {
      buffer = backend.createSingleUniformBuffer(decode(op.value), op.typeDecl)
    }
    record = { id: op.id, write: buffer ? last : null }
  } catch (err) {
    record = { id: op.id, error: { name: String(err?.name ?? typeof err), message: String(err?.message ?? err) } }
  }
  process.stdout.write(JSON.stringify(record) + '\n')
}
