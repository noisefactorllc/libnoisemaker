#!/usr/bin/env node
// extract-runtime-shaders.mjs — the WGSL the reference WebGPU backend compiles
// for itself: the default vertex stage, mip resampling, the blit to the canvas,
// the compute buffer-to-texture copy and the frame export resolve. They are
// constants inside its modules, so they are captured by running the backend's
// own methods on a device that records createShaderModule, not by reading its
// source text.
//
//   node tools/catalog/extract-runtime-shaders.mjs (--cache <catalog cache> | --src <shaders/src>) --out <catalog dir>
//     [--sites <file>]
//
// Writes <catalog dir>/runtime/shaders.json. The build reads the pinned source
// cache (fetch-catalog.mjs); check-catalog.mjs runs it on the reference checkout
// and, with --sites, gets the source line of each captured createShaderModule
// call to compare with the call sites it finds in the source text.
import { mkdirSync, readFileSync, writeFileSync } from 'node:fs'
import { dirname, join, resolve } from 'node:path'
import { fileURLToPath, pathToFileURL } from 'node:url'

const root = resolve(dirname(fileURLToPath(import.meta.url)), '../..')
const argv = process.argv.slice(2)
function option (name) {
  const index = argv.indexOf(name)
  if (index < 0) return null
  if (!argv[index + 1]) throw new Error(`${name} requires a directory`)
  return resolve(argv[index + 1])
}
const lock = JSON.parse(readFileSync(join(root, 'parity/reference.json'), 'utf8'))
const cache = option('--cache')
const src = cache ? join(cache, lock.version, 'source', lock.commit, 'shaders/src') : option('--src')
const outDir = option('--out')
const sitesPath = option('--sites')
if (!src || !outDir) throw new Error('usage: extract-runtime-shaders.mjs (--cache DIR | --src DIR) --out DIR')

globalThis.GPUBufferUsage ??= { MAP_READ: 1, MAP_WRITE: 2, COPY_SRC: 4, COPY_DST: 8, INDEX: 16, VERTEX: 32, UNIFORM: 64, STORAGE: 128, INDIRECT: 256, QUERY_RESOLVE: 512 }
globalThis.GPUTextureUsage ??= { COPY_SRC: 1, COPY_DST: 2, TEXTURE_BINDING: 4, STORAGE_BINDING: 8, RENDER_ATTACHMENT: 16 }
globalThis.GPUShaderStage ??= { VERTEX: 1, FRAGMENT: 2, COMPUTE: 4 }
globalThis.GPUMapMode ??= { READ: 1, WRITE: 2 }
const defaults = await import(pathToFileURL(join(src, 'runtime/default-shaders.js')).href)
const { WebGPUBackend } = await import(pathToFileURL(join(src, 'runtime/backends/webgpu.js')).href)
const { WebGPUFrameExportAdapter } = await import(pathToFileURL(join(src, 'runtime/backends/webgpu-frame-export.js')).href)

// Every member of the device and of what it returns is a callable stub, except
// createShaderModule, which records the code it is given.
const codes = []
const sites = []
const srcUrl = pathToFileURL(src).href + '/'
const stub = () => new Proxy(function () {}, {
  get: (_, key) => key === 'then' ? undefined : stub(),
  apply: () => stub()
})
const device = new Proxy({}, {
  get: (_, key) => {
    if (key === 'createShaderModule') {
      return ({ code }) => {
        // The caller's frame: "at Class.method (file:///…/shaders/src/<path>:<line>:<column>)".
        const caller = new Error().stack.split('\n')[2] ?? ''
        const match = caller.match(/\((.*):(\d+):\d+\)$/) ?? caller.match(/at (.*):(\d+):\d+$/)
        sites.push(match && match[1].startsWith(srcUrl) ? `${match[1].slice(srcUrl.length)}:${match[2]}` : caller.trim())
        codes.push(code)
        return stub()
      }
    }
    if (key === 'limits') return {}
    return stub()
  }
})
const backend = new WebGPUBackend(device, null)
const captured = {}
const capture = (name, run) => {
  const before = codes.length
  run()
  if (codes.length !== before + 1) throw new Error(`${name}: expected one shader module, got ${codes.length - before}`)
  captured[name] = sites[before]
  return codes[before]
}
const shaders = {
  defaultVertexEntryPoint: defaults.DEFAULT_VERTEX_ENTRY_POINT,
  defaultFragmentEntryPoint: defaults.DEFAULT_FRAGMENT_ENTRY_POINT,
  defaultVertex: capture('defaultVertex', () => backend.getDefaultVertexModule()),
  resample: capture('resample', () => backend.getResamplePipeline('rgba16float', 'fsMip')),
  blit: capture('blit', () => backend.getBlitPipeline()),
  bufferToTexture: capture('bufferToTexture', () => backend.getBufferToTextureRenderPipeline('rgba16float')),
  frameExportResolve: capture('frameExportResolve', () => new WebGPUFrameExportAdapter(backend, globalThis)._ensureShared(globalThis))
}
if (shaders.defaultVertex !== defaults.DEFAULT_VERTEX_SHADER_WGSL) throw new Error('default vertex module is not DEFAULT_VERTEX_SHADER_WGSL')
for (const [name, value] of Object.entries(shaders)) {
  if (typeof value !== 'string' || !value) throw new Error(`${name}: not captured`)
}
mkdirSync(join(outDir, 'runtime'), { recursive: true })
writeFileSync(join(outDir, 'runtime/shaders.json'), JSON.stringify(shaders, null, 2) + '\n')
if (sitesPath) writeFileSync(sitesPath, JSON.stringify(captured, null, 2) + '\n')
console.log(`RUNTIME SHADERS: ${Object.keys(shaders).length} entries`)
