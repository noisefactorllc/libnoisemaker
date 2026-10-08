#!/usr/bin/env node
// Projection copied from noisemaker-for-qt 4058164:tools/convert-definitions.mjs.
// Inputs are the sha256-locked published ES modules and pinned shader bytes in cache.
import { mkdirSync, writeFileSync, readFileSync, readdirSync } from 'node:fs'
import { join, dirname, resolve } from 'node:path'
import { fileURLToPath, pathToFileURL } from 'node:url'

const root = resolve(dirname(fileURLToPath(import.meta.url)), '../..')
const argv = process.argv.slice(2)
function option (name) {
  const index = argv.indexOf(name)
  if (index < 0 || !argv[index + 1]) throw new Error(`${name} requires a directory`)
  return resolve(argv[index + 1])
}
const cache = option('--cache')
const outDir = option('--out')
const lock = JSON.parse(readFileSync(join(root, 'parity/reference.json'), 'utf8'))
const effectsDir = join(cache, lock.version, 'effects')
const manifest = JSON.parse(readFileSync(join(effectsDir, 'manifest.json'), 'utf8'))
// fetch-catalog caches shader files omitted from published bundles at the
// reference commit. Generation reads no source checkout or network resource.
const sourceEffects = join(cache, lock.version, 'source', lock.commit, 'shaders/effects')

// ---------------------------------------------------------------------------
// Field projection. We copy only the fields the C++ definition loader reads, in a
// stable order, so the output is byte-stable across runs and minimally diffs.
// ---------------------------------------------------------------------------

// Project one global/param spec. Drops display-only UI metadata (category,
// label, enabledBy), while retaining UI fields consumed by the host and DSL
// unparser. `ui.format: 'vector'` preserves lossless vec4 coordinates.
function projectGlobal (spec) {
  const out = {}
  if (spec.type !== undefined) out.type = spec.type
  if (spec.default !== undefined) out.default = spec.default
  if (spec.uniform !== undefined) out.uniform = spec.uniform
  // enum/enumPath reference an EXTERNAL enum table (e.g. index -> "palette",
  // smoothing -> "smoothing"). Without these the validator cannot resolve a
  // named arg (palette(index: solaris)) and silently falls back to the default
  // (reference/02 §6.10 member-param resolution). Inline `choices` are separate.
  if (spec.enum !== undefined) out.enum = spec.enum
  if (spec.enumPath !== undefined) out.enumPath = spec.enumPath
  if (spec.define !== undefined) out.define = spec.define
  if (spec.min !== undefined) out.min = spec.min
  if (spec.max !== undefined) out.max = spec.max
  if (spec.zero !== undefined) out.zero = spec.zero
  if (spec.choices !== undefined) out.choices = spec.choices
  if (spec.colorModeUniform !== undefined) out.colorModeUniform = spec.colorModeUniform
  if (spec.ui?.control !== undefined || spec.ui?.hidden === true || spec.ui?.format !== undefined) {
    out.ui = {}
    if (spec.ui.control !== undefined) out.ui.control = spec.ui.control
    if (spec.ui.hidden === true) out.ui.hidden = true
    if (spec.ui.format !== undefined) out.ui.format = spec.ui.format
  }
  return out
}

function projectGlobals (globals) {
  if (!globals) return {}
  const out = {}
  // Object.entries preserves declaration order — parity-critical for palette
  // index = positional key order (reference/03), so DO NOT sort.
  for (const [key, spec] of Object.entries(globals)) {
    out[key] = projectGlobal(spec)
  }
  return out
}

function projectPass (pass) {
  const out = { name: pass.name, program: pass.program }
  out.inputs = pass.inputs || {}
  if (pass.uniforms !== undefined) out.uniforms = pass.uniforms
  out.outputs = pass.outputs || {}
  // Execution modifiers (mostly used by agent/compute effects).
  if (pass.drawMode !== undefined) out.drawMode = pass.drawMode
  if (pass.drawBuffers !== undefined) out.drawBuffers = pass.drawBuffers
  if (pass.viewport !== undefined) out.viewport = pass.viewport
  if (pass.count !== undefined) out.count = pass.count
  if (pass.countUniform !== undefined) out.countUniform = pass.countUniform
  if (pass.repeat !== undefined) out.repeat = pass.repeat
  if (pass.blend !== undefined) out.blend = pass.blend
  // Per-pass execution predicate (reference/03 §4.x). pointsBillboardRender gates its two
  // deposit passes on `blendMode`; dropping this ran BOTH passes and double-deposited.
  if (pass.conditions !== undefined) out.conditions = pass.conditions
  // Per-pass compile-time defines (the `.flatMap()` per-variant clone pattern: several passes
  // share one `program` name but each carries its own defines, e.g. pointsBillboardRender's
  // deposit_0/deposit_1/depositDefocus_1/... clones need VIEW_MODE/BLEND_MODE/BLUR_LAYER baked
  // in — without this the shared byte-identical shader's `#if VIEW_MODE == 0` etc. preprocessor
  // guards see an undefined macro (treated as 0), so every clone silently runs the SAME branch).
  if (pass.defines !== undefined) out.defines = pass.defines
  if (pass.clear !== undefined) out.clear = pass.clear
  if (pass.type !== undefined) out.type = pass.type
  if (pass.entryPoint !== undefined) out.entryPoint = pass.entryPoint
  // Compute-pass fields. The reference expander copies them onto each expanded pass verbatim.
  if (pass.workgroups !== undefined) out.workgroups = pass.workgroups
  if (pass.storageBuffers !== undefined) out.storageBuffers = pass.storageBuffers
  if (pass.storageTextures !== undefined) out.storageTextures = pass.storageTextures
  return out
}

function projectTextures (textures, is3D) {
  if (!textures) return undefined
  const out = {}
  for (const [id, spec] of Object.entries(textures)) {
    const t = {}
    if (spec.width !== undefined) t.width = spec.width
    if (spec.height !== undefined) t.height = spec.height
    if (spec.depth !== undefined) t.depth = spec.depth
    if (is3D || spec.is3D) t.is3D = true
    if (spec.mipmaps !== undefined) t.mipmaps = spec.mipmaps
    if (spec.persistent !== undefined) t.persistent = spec.persistent
    if (spec.filter !== undefined) t.filter = spec.filter
    if (spec.format !== undefined) t.format = spec.format
    out[id] = t
  }
  return out
}

function convertEffect (instance, namespace, name) {
  const func = instance.func || name
  const def = {
    name: instance.name || func,
    namespace: instance.namespace || namespace,
    func
  }
  // The bundle may omit namespace. The registry still needs the path namespace,
  // while graph JSON must preserve the original null effect namespace.
  def.sourceNamespace = instance.namespace === undefined ? null : instance.namespace
  // Authoritative starter flag from the manifest (NOT re-derived). The manifest is
  // keyed "<namespace>/<dirname>"; default false (an effect absent from the manifest
  // is not a registered starter, matching canvas.js loadManifest).
  const mkey = `${namespace}/${name}`
  const mentry = manifest[mkey]
  if (mentry === undefined) {
    process.stderr.write(`[convert] WARN: ${mkey} not in manifest — starter defaults to false\n`)
  }
  def.starter = !!(mentry && mentry.starter)
  if (instance.tags) def.tags = instance.tags
  if (instance.description) def.description = instance.description
  def.paramAliases = instance.paramAliases || {}
  def.globals = projectGlobals(instance.globals)
  def.passes = (instance.passes || []).map(projectPass)
  def.textures = projectTextures(instance.textures, false) || {}

  // Uniform packing layout (slot/components into the vec4[] UBO). The Godot port
  // binds a single packed `vec4 data[N]` UBO per pass and unpacks by slot/component
  // exactly as the WGSL does, so the reference layout is carried through verbatim.
  // (HLSL ignored this — it bound individual named uniforms — but Vulkan/Godot has
  // no loose uniforms, so the layout is load-bearing here.) `uniformLayouts` (plural,
  // per-program) is carried when present for multi-program effects.
  if (instance.uniformLayout) def.uniformLayout = instance.uniformLayout
  if (instance.uniformLayouts) def.uniformLayouts = instance.uniformLayouts

  // 3D volume textures, when present, carry is3D.
  if (instance.textures3d) {
    const t3d = projectTextures(instance.textures3d, true)
    Object.assign(def.textures, t3d)
  }

  // Carry forward optional declarative flags the runtime may key on.
  if (instance.defaultProgram !== undefined) def.defaultProgram = instance.defaultProgram
  // Output-surface passthrough declarations (reference/03 §4.10). The expander uses
  // these to update the 2D/agent-state cursors so downstream effects read the right
  // surface. The particle pipeline (pointsEmit/flow/physical/lenia/pointsRender) relies
  // on outputXyz/Vel/Rgba — dropping them broke agent-state propagation.
  if (instance.outputTex !== undefined) def.outputTex = instance.outputTex
  if (instance.outputTex3d !== undefined) def.outputTex3d = instance.outputTex3d
  if (instance.outputGeo !== undefined) def.outputGeo = instance.outputGeo
  if (instance.outputXyz !== undefined) def.outputXyz = instance.outputXyz
  if (instance.outputVel !== undefined) def.outputVel = instance.outputVel
  if (instance.outputRgba !== undefined) def.outputRgba = instance.outputRgba
  // Host-supplied texture input (media -> imageTex, text -> textTex). The expander binds
  // it to the per-step id `${externalTexture}_step_${N}`; without it the input falls
  // through to a node-local texture that no host can address.
  if (instance.externalTexture !== undefined) def.externalTexture = instance.externalTexture
  // Host-supplied mesh input (meshLoader -> mesh0) and the built-in OBJ files a host
  // offers for it. builtinMeshes becomes an array of {name, path} because the reference
  // demo host loads the FIRST entry by default (demo-ui.js _createMeshInputSection) and a
  // Qt QJsonObject would sort the keys. Paths stay relative to the data root.
  if (instance.externalMesh !== undefined) def.externalMesh = instance.externalMesh
  if (instance.builtinMeshes !== undefined) {
    def.builtinMeshes = Object.entries(instance.builtinMeshes).map(([name, path]) => ({ name, path }))
  }
  if (instance.hidden) def.hidden = true
  if (instance.deprecatedBy) def.deprecatedBy = instance.deprecatedBy

  const hooks = new Set()
  for (const key of Object.keys(instance)) {
    if (typeof instance[key] === 'function') hooks.add(key)
  }
  for (let proto = Object.getPrototypeOf(instance);
    proto && Object.getPrototypeOf(proto) !== Object.prototype;
    proto = Object.getPrototypeOf(proto)) {
    for (const key of Object.getOwnPropertyNames(proto)) {
      if (key !== 'constructor' && typeof instance[key] === 'function') hooks.add(key)
    }
  }
  for (const [key, name] of Object.entries({
    _configOnInit: 'onInit', _configOnUpdate: 'onUpdate',
    _configOnDestroy: 'onDestroy', _configAsyncInit: 'asyncInit'
  })) {
    if (typeof instance[key] === 'function') hooks.add(name)
  }
  if (hooks.size) def.jsHooks = [...hooks].sort()

  return def
}

async function loadInstance (bundlePath) {
  const mod = await import(pathToFileURL(bundlePath).href)
  const value = mod.default
  return typeof value === 'function' ? new value() : value
}

async function main () {
  let written = 0
  for (const id of Object.keys(manifest).sort()) {
    const [namespace, name] = id.split('/')
    const instance = await loadInstance(join(effectsDir, `${id}.js`))
    if (!instance) throw new Error(`${id}: no default export`)
    const func = instance.func || name
    const def = convertEffect(instance, namespace, name)
    const outPath = join(outDir, 'effects', namespace, `${func}.json`)
    mkdirSync(dirname(outPath), { recursive: true })
    writeFileSync(outPath, JSON.stringify(def, null, 2) + '\n')
    for (const lang of ['wgsl', 'glsl']) {
      const sourceDir = join(sourceEffects, namespace, name, lang)
      const sourcePrograms = readdirSync(sourceDir).filter(file => file.endsWith(`.${lang}`))
      for (const file of sourcePrograms) {
        const program = file.slice(0, -lang.length - 1)
        const attached = instance.shaders?.[program]?.[lang]
        const bytes = typeof attached === 'string' ? attached : readFileSync(join(sourceDir, file))
        const shaderPath = join(outDir, lang, namespace, func, file)
        mkdirSync(dirname(shaderPath), { recursive: true })
        writeFileSync(shaderPath, bytes)
      }
    }
    written++
  }
  for (const lang of ['wgsl', 'glsl']) {
    const sourceDir = join(sourceEffects, 'filter/_shared', lang)
    for (const file of readdirSync(sourceDir).filter(file => file.endsWith(`.${lang}`))) {
      const dest = join(outDir, lang, 'filter/_shared', file)
      mkdirSync(dirname(dest), { recursive: true })
      writeFileSync(dest, readFileSync(join(sourceDir, file)))
    }
  }
  console.log(`CATALOG BUILD: ${written} effects`)
}

await main()
