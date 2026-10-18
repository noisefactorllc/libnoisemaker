#!/usr/bin/env node
// Copied from noisemaker-for-qt 2746d46:tools/convert-definitions.mjs. Here NM_OUT_DIR is
// required, and projectGlobal, projectPass, projectTextures and convertEffect are
// tools/catalog/build-catalog.mjs's, so both project every field the catalog carries.
// tools/catalog/check-catalog.mjs runs it on the pinned reference checkout, into an empty
// directory, and compares the catalog's definitions with its output.
// convert-definitions.mjs — AUTOMATED effect-definition regenerator.
//
// Walks shaders/effects/<ns>/<name>/definition.js, imports each reference Effect
// instance, and emits a runtime-shape JSON to
//   qt/noisemaker/effects/<ns>/<func>.json
//
// The emitted shape matches the runtime contract (Effects/<ns>/<func>.json):
//   { name, namespace, func, tags, description,
//     globals{ <key>: { type, default, uniform, define, min, max, choices } },
//     passes[ { name, program, inputs, outputs, uniforms } ],
//     textures{ <id>: { width, height, [depth], [is3D], [mipmaps], [persistent], [filter], format } } }
//
// This tool regenerates definitions deterministically from the single source of truth (the JS
// definitions). The C++ runtime's definition loader consumes these files. This tool only ports DATA.
//
// Usage:
//   node convert-definitions.mjs                # convert all effects
//   node convert-definitions.mjs synth/noise    # convert one (ns/name)
//   node convert-definitions.mjs --dry-run      # print summary, write nothing
//
// Env:
//   NM_REFERENCE_ROOT  path to the Noisemaker reference repo (REQUIRED; no default, no sibling assumed)
//   NM_OUT_DIR         output dir (REQUIRED)

import { readdirSync, statSync, mkdirSync, writeFileSync, readFileSync, existsSync } from 'node:fs'
import { join, resolve, basename } from 'node:path'
import { pathToFileURL } from 'node:url'

// Reference engine path comes from NM_REFERENCE_ROOT (a checkout of the Noisemaker
// reference repo); this repo does not assume a sibling checkout.
if (!process.env.NM_REFERENCE_ROOT) {
  throw new Error('NM_REFERENCE_ROOT is not set. Point it at a checkout of the Noisemaker\n'
    + 'reference repo (the JS/WebGL2 engine), e.g. NM_REFERENCE_ROOT=/path/to/noisemaker.\n'
    + 'This repo does NOT assume a sibling checkout.')
}
const REFERENCE_ROOT = resolve(process.env.NM_REFERENCE_ROOT)
const EFFECTS_DIR = join(REFERENCE_ROOT, 'shaders', 'effects')
if (!process.env.NM_OUT_DIR) throw new Error('NM_OUT_DIR is not set')
const OUT_DIR = resolve(process.env.NM_OUT_DIR)

// Namespaces, mirrored from shaders/scripts/generate-shader-manifest.mjs.
const NAMESPACES = [
  'classicNoisedeck', 'filter', 'filter3d',
  'mixer', 'points', 'render', 'synth', 'synth3d'
]

// Bootstrap metadata — the `starter` flag is computed by the reference manifest
// generator (shaders/scripts/generate-shader-manifest.mjs isStarterEffect) and is
// the SINGLE SOURCE OF TRUTH (reference/02 §1.3 STARTER_OPS <- registerStarterOps).
// We DO NOT re-derive it; we project it straight from shaders/effects/manifest.json,
// keyed "<namespace>/<dirname>". The C++ loader keys on the explicit `starter` field.
const MANIFEST_PATH = join(EFFECTS_DIR, 'manifest.json')
let MANIFEST
try {
  MANIFEST = JSON.parse(readFileSync(MANIFEST_PATH, 'utf8'))
} catch (err) {
  process.stderr.write(`[convert] FATAL: cannot read manifest ${MANIFEST_PATH} — ${err?.message || err}\n`)
  process.exit(1)
}

// ---------------------------------------------------------------------------
// Field projection. We copy only the fields the C++ definition loader reads, in a
// stable order, so the output is byte-stable across runs and minimally diffs.
// ---------------------------------------------------------------------------

// Project one global/param spec. Drops UI-only metadata (ui, category, label,
// enabledBy) which the renderer never reads; keeps everything that influences
// uniforms, defines, defaults, ranges and enum mappings. The exception is
// `ui.control: false` and `ui.hidden: true`: the demo host shows no control for
// such a param, so it does not coerce the param's function value
// (libnoisemaker::hasHostControl).
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

// True when the on-disk JSON carries a packing layout that the reference does NOT declare —
// i.e. one authored for this port. Regeneration must carry it forward verbatim.
function portAuthoredLayouts (instance, existing) {
  return !instance.uniformLayout && !instance.uniformLayouts && !!existing?.uniformLayouts
}

function convertEffect (instance, namespace, name, existing) {
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
  const mentry = MANIFEST[mkey]
  if (mentry === undefined) {
    process.stderr.write(`[convert] WARN: ${mkey} not in manifest — starter defaults to false\n`)
  }
  def.starter = !!(mentry && mentry.starter)
  if (instance.tags) def.tags = instance.tags
  if (instance.description) def.description = instance.description
  def.globals = projectGlobals(instance.globals)
  // PORT-AUTHORED packing layout, carried through untouched. The particle pipeline
  // (flow / pointsEmit / pointsRender / pointsBillboardRender) binds several programs per
  // effect but the reference declares no uniformLayout for them — WebGL2 sets loose named
  // uniforms, Vulkan/Godot cannot. So the per-program vec4[] packing was worked out here and
  // lives ONLY in these JSONs. It is not derivable from the reference: regenerating without
  // this carry-forward silently deletes it and the particle passes bind garbage.
  if (portAuthoredLayouts(instance, existing)) def.uniformLayouts = existing.uniformLayouts
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

async function loadInstance (defPath) {
  const mod = await import(pathToFileURL(defPath).href)
  const d = mod.default
  return (typeof d === 'function') ? new d() : d
}

function* enumerateEffects (filter) {
  for (const namespace of NAMESPACES) {
    const nsDir = join(EFFECTS_DIR, namespace)
    if (!existsSync(nsDir)) continue
    for (const entry of readdirSync(nsDir).sort()) {
      const effectDir = join(nsDir, entry)
      if (!statSync(effectDir).isDirectory()) continue
      const defPath = join(effectDir, 'definition.js')
      if (!existsSync(defPath)) continue
      if (filter && `${namespace}/${entry}` !== filter) continue
      yield { namespace, name: entry, defPath }
    }
  }
}

async function main () {
  const argv = process.argv.slice(2)
  const dryRun = argv.includes('--dry-run')
  const filter = argv.find(a => !a.startsWith('--')) || null

  let written = 0
  let failed = 0
  const errors = []
  const preservedLayouts = []

  let validateEffectDefinition = null
  const validatorPath = join(REFERENCE_ROOT, 'shaders', 'src', 'runtime', 'effect-validator.js')
  if (existsSync(validatorPath)) {
    try {
      const validatorMod = await import(pathToFileURL(validatorPath).href)
      validateEffectDefinition = validatorMod.validateEffectDefinition || null
      if (typeof validateEffectDefinition !== 'function') {
        throw new Error('effect-validator.js does not export validateEffectDefinition')
      }
    } catch (err) {
      process.stderr.write(`[convert] ERROR: failed to load effect-validator.js: ${err?.message || err}\n`)
      process.exit(1)
    }
  }

  for (const { namespace, name, defPath } of enumerateEffects(filter)) {
    let instance
    try {
      instance = await loadInstance(defPath)
    } catch (err) {
      failed++
      errors.push(`${namespace}/${name}: import failed — ${err?.message || err}`)
      continue
    }
    if (!instance) {
      failed++
      errors.push(`${namespace}/${name}: no default export`)
      continue
    }
    if (validateEffectDefinition) {
      const valErrors = validateEffectDefinition(instance)
      if (valErrors.length > 0) {
        failed++
        errors.push(`${namespace}/${name}: validation failed:\n    ` + valErrors.join('\n    '))
        continue
      }
    }
    const func = instance.func || name
    const outNsDir = join(OUT_DIR, namespace)
    const outPath = join(outNsDir, `${func}.json`)
    // Read the file we are about to replace, so port-authored data survives regeneration.
    let existing = null
    if (existsSync(outPath)) {
      try { existing = JSON.parse(readFileSync(outPath, 'utf8')) } catch { existing = null }
    }
    const def = convertEffect(instance, namespace, name, existing)
    const preserved = portAuthoredLayouts(instance, existing)
    if (preserved) preservedLayouts.push(`${namespace}/${func}`)
    if (!dryRun) {
      mkdirSync(outNsDir, { recursive: true })
      writeFileSync(outPath, JSON.stringify(def, null, 2) + '\n')
    }
    written++
    process.stderr.write(`[convert] ${namespace}/${name} -> Effects/${namespace}/${func}.json${preserved ? ' (+port uniformLayouts)' : ''}${dryRun ? ' (dry-run)' : ''}\n`)
  }

  process.stderr.write(`\n[convert] ${dryRun ? 'would write' : 'wrote'} ${written} effect(s), ${failed} failed.\n`)
  if (preservedLayouts.length) {
    process.stderr.write(`[convert] carried forward port-authored uniformLayouts for ${preservedLayouts.length}: ${preservedLayouts.join(', ')}\n`)
  }
  for (const e of errors) process.stderr.write(`  ! ${e}\n`)
  if (failed > 0) process.exit(1)
}

if (basename(process.argv[1] || '') === 'convert-definitions.mjs') {
  main().catch(err => {
    process.stderr.write(`[convert] FAILED: ${err?.stack || err?.message || JSON.stringify(err)}\n`)
    process.exit(1)
  })
}
