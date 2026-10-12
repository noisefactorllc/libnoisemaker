// Copied from noisemaker-for-qt 2746d46:tools/export-graph.mjs (the functions below, unchanged):
// the normalized render graph that src/core/graph/normalize.cpp ports. defineMap is
// {"<ns>.<func>": {globalKey: defineName}} from the effect definitions' globals.

function mapToObject (m) {
  if (!m) return {}
  if (m instanceof Map) {
    const out = {}
    for (const [k, v] of m) out[k] = v
    return out
  }
  return m
}

// progName = the bare program basename. Reference program ids are
// `${nodeId}_${progName}${defineSuffix}`. We recover the basename from the pass's
// effectFunc-relative program name by stripping the `node_<n>_` prefix and any
// `__KEY_VAL` define suffix. Falls back to func.
function deriveProgName (pass) {
  const raw = pass.program || ''
  let s = raw
  const nodePrefix = pass.nodeId ? `${pass.nodeId}_` : null
  if (nodePrefix && s.startsWith(nodePrefix)) s = s.slice(nodePrefix.length)
  // Strip a trailing define-variant suffix: a run of `__name_value` groups.
  const suffixIdx = s.indexOf('__')
  if (suffixIdx > 0) s = s.slice(0, suffixIdx)
  return s || pass.effectFunc || 'main'
}

// Compile-time defines for a pass come from its resolved program entry, which
// carries `defines` (NOISE_TYPE, LOOP_OFFSET, ...). They are int-valued.
function definesForPass (pass, programs) {
  const prog = programs && programs[pass.program]
  const d = prog && prog.defines
  const out = {}
  if (d) for (const [k, v] of Object.entries(d)) out[k] = v
  // Recover pass-level defines (pointsRender/pointsBillboardRender's per-viewMode deposit
  // variants, reference 0ed489ec) from the `__KEY_val` suffix deriveProgName() strips off
  // pass.program: the reference never sets an explicit pass.defines either (only
  // `programName += passDefineSuffix`), and the `programs` registry above is empty for this
  // export path (it's only populated by the live browser runtime's HTTP shader fetch), so
  // this suffix is the only place the value survives.
  const raw = pass.program || ''
  let s = raw
  const nodePrefix = pass.nodeId ? `${pass.nodeId}_` : null
  if (nodePrefix && s.startsWith(nodePrefix)) s = s.slice(nodePrefix.length)
  const suffixIdx = s.indexOf('__')
  if (suffixIdx >= 0) {
    for (const seg of s.slice(suffixIdx).split('__')) {
      if (!seg) continue
      const idx = seg.lastIndexOf('_')
      if (idx < 0) continue
      const key = seg.slice(0, idx)
      const val = seg.slice(idx + 1)
      out[key] = /^-?\d+$/.test(val) ? Number(val) : val
    }
  }
  return out
}

function normalizePass (pass, programs, defineMap) {
  const isBlit = pass.type === 'blit' || pass.program === 'blit' ||
    (pass.effectFunc === 'blit')
  const out = {
    id: pass.id,
    passType: isBlit ? 'blit' : 'effect',
    namespace: isBlit ? null : (pass.effectNamespace ?? null),
    func: isBlit ? 'blit' : (pass.effectFunc ?? null),
    progName: isBlit ? 'blit' : deriveProgName(pass),
    program: pass.program ?? null,
    defines: isBlit ? {} : definesForPass(pass, programs),
    inputs: pass.inputs || {},
    outputs: pass.outputs || {},
    uniforms: { ...(pass.uniforms || {}) },
    uniformSpecs: pass.uniformSpecs || {}
  }

  // Promote compile-time define globals (e.g. noise.type=10 -> NOISE_TYPE:10)
  // from uniforms into defines by their DEFINE name. The HLSL port binds these by
  // DEFINE name; the reference graph leaves them as plain uniforms.
  if (!isBlit && defineMap) {
    const dm = defineMap[`${pass.effectNamespace}.${pass.effectFunc}`] || {}
    for (const [globalKey, defineName] of Object.entries(dm)) {
      if (globalKey in out.uniforms) {
        const v = out.uniforms[globalKey]
        out.defines[defineName] = (typeof v === 'boolean') ? (v ? 1 : 0) : Math.trunc(Number(v))
        delete out.uniforms[globalKey]
      }
    }
  }

  // Optional execution modifiers — only emit when meaningfully present so the
  // JSON stays diffable and the C# loader's "absent vs null vs 0" model holds.
  if (pass.drawMode !== undefined) out.drawMode = pass.drawMode
  if (pass.count !== undefined) out.count = pass.count
  if (pass.countUniform !== undefined) out.countUniform = pass.countUniform
  if (pass.drawBuffers !== undefined) out.drawBuffers = pass.drawBuffers
  if (pass.blend !== undefined) out.blend = pass.blend
  if (pass.repeat !== undefined) out.repeat = pass.repeat
  // Per-pass execution predicate (reference expander.js `conditions: passDef.conditions`).
  // Evaluated at RUNTIME against that frame's resolved uniforms (Backend::shouldSkipPass /
  // reference pipeline.js shouldSkipPass) — NOT resolved away here. Dropping this from the
  // exported schema made pointsBillboardRender's deposit/deposit_alpha clones both always
  // run and double-deposit (matches the same field already captured in
  // tools/convert-definitions.mjs's projectPass for the effect-definition JSON).
  if (pass.conditions !== undefined) out.conditions = pass.conditions
  if (pass.clear !== undefined) out.clear = pass.clear

  // Metadata.
  out.effectKey = pass.effectKey ?? null
  out.nodeId = pass.nodeId ?? null
  if (pass.stepIndex !== undefined) out.stepIndex = pass.stepIndex
  if (pass.inheritsVolumeSize !== undefined) out.inheritsVolumeSize = pass.inheritsVolumeSize
  out.scopedParams = pass.scopedParams || null
  // Pass-level uniform aliases (reference bd773801 expander.js uniformAliases):
  // { shaderUniform: globalName } for a pass fed from a differently named
  // global; the runtime writes a changed parameter to these uniforms too.
  out.uniformAliases = pass.uniformAliases || null

  return out
}

function normalizePrograms (programs) {
  const out = {}
  for (const [id, prog] of Object.entries(programs || {})) {
    // The HLSL loader does not need shader source (it resolves a Unity Shader by
    // namespace/func). Keep only uniformLayout + defines for traceability/diff.
    out[id] = {
      uniformLayout: prog.uniformLayout || {},
      defines: prog.defines || {}
    }
  }
  return out
}

export function normalizeGraph (graph, defineMap) {
  const programs = graph.programs || {}
  return {
    id: graph.id,
    source: graph.source,
    renderSurface: graph.renderSurface ?? null,
    passes: (graph.passes || []).map(p => normalizePass(p, programs, defineMap)),
    allocations: mapToObject(graph.allocations),
    textures: mapToObject(graph.textures),
    programs: normalizePrograms(programs)
  }
}
