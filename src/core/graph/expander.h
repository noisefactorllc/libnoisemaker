#pragma once
#include "core/graph/compat.h"

// expander.h — Logical Graph (validate() plans) -> Render Graph (passes).
// Port of the REFERENCE shaders/src/runtime/expander.js (source of truth,
// 1190 lines, read in full) + shaders/src/runtime/palette-expansion.js,
// matching reference/03-expander.md section-for-section. Cross-checked
// against godot/addons/noisemaker/compiler/graph/expander.gd and
// td/noisemaker/compiler/lang/expander.py — both agree with the JS on
// every point below; where TD's own header comments flagged a reference
// subtlety (TEXTURE_ARG_KINDS incl. vol/geo/pipeline; the defines SORT KEY
// — see below), this port re-verified directly against the live JS source
// rather than trusting either prior port's word.
//
// PARITY-CRITICAL DEVIATION FROM THE TASK BRIEF (verified, not assumed):
// the brief states compile-time defines are "sorted by define name". The
// ACTUAL reference (expander.js) sorts `Object.keys(effectDef.globals)`
// (the camelCase GLOBAL key) and inserts into `compileTimeDefines` in THAT
// order; `Object.entries(compileTimeDefines)` (which builds the program-
// cache-key suffix) then iterates in JS's insertion order = global-name-
// sorted order, NOT define-name-sorted order. These two orders genuinely
// DIFFER for real, corpus-relevant effects — e.g.
// classicNoisedeck/noise.json's globals sorted give
// [colorMode,loopOffset,metric,refractMode,type], i.e. defines order
// [COLOR_MODE,LOOP_OFFSET,METRIC,REFRACT_MODE,NOISE_TYPE] — REFRACT_MODE
// before NOISE_TYPE, the OPPOSITE of a define-name sort (verified directly
// against qt/noisemaker/effects/classicNoisedeck/noise.json; TD's own
// expander.py independently documents the identical finding). This port
// sorts by GLOBAL name (matching the reference exactly); see expander.cpp
// `collectDefines()`.
//
// collectDefines() explicitly sorts global names as the reference does.

// Pure data transformation — no GPU calls, no floating-point pixel math.


namespace nm {

class EffectRegistry;

// Compute PassDef fields (entryPoint/workgroups/storageBuffers/
// storageTextures) are copied onto each expanded pass verbatim, as
// expander.js does. No catalog definition declares them at the pinned
// reference. The normalized graph drops them (tools/export-graph.mjs
// normalizePass), and nm::Backend has no compute path.

// One expanded GPU pass in the RAW (pre-normalization) shape
// shaders/src/runtime/expander.js itself produces (reference/03 §2.1
// effect passes, §2.2 blit passes) — i.e. BEFORE tools/export-graph.mjs's
// normalizeGraph() derives passType/progName/promotes defines/etc. Both
// the EXPAND gate's raw dump (qt/tests/expand_dump.cpp) and the GRAPH
// gate's normalized dump (dsl_compiler.cpp) are built FROM this shape, so
// it carries every field either consumer needs.
//
// Optional fields absent in the reference's own object literal for a given
// pass kind (see effect-vs-blit shape notes below) use
// JsValue(JsValue::Undefined) as their sentinel (T9's established
// idiom, effect_registry.h) so RAW serialization can OMIT the key exactly
// where JS's `JSON.stringify` would drop an `undefined`-valued property —
// never emit a JSON `null` in its place.
struct ExpandedPass {
    JsText id;
    bool isBlit = false;  // true for _write/_write3d/final blit passes (reference/03 §2.2)

    JsText program;  // program cache id ('blit' for blit passes)

    // --- effect-pass-only fields (always JsValue::Undefined on a blit
    // pass — blit's own JS object literal never has these keys at all) ---
    JsValue entryPoint = JsValue(JsValue::Undefined);
    JsValue drawMode = JsValue(JsValue::Undefined);
    JsValue drawBuffers = JsValue(JsValue::Undefined);
    JsValue count = JsValue(JsValue::Undefined);
    JsValue countUniform = JsValue(JsValue::Undefined);
    JsValue repeat = JsValue(JsValue::Undefined);
    JsValue blend = JsValue(JsValue::Undefined);
    // Per-pass execution predicate (reference expander.js: `conditions: passDef.conditions`).
    // Evaluated at RUNTIME (per frame, against that frame's resolved uniforms), not here —
    // this Expander only carries the JSON through unevaluated, mirroring the reference.
    JsValue conditions = JsValue(JsValue::Undefined);
    // Resolved compile-time defines for this pass (node-level compileTimeDefines merged with
    // any pass-level clone override) — see expandPasses()'s "Pass-level compile-time defines"
    // comment for why this bypasses the (permanently empty, for this port) `programs_` map.
    JsObject passDefines;
    JsValue workgroups = JsValue(JsValue::Undefined);
    JsValue storageBuffers = JsValue(JsValue::Undefined);
    JsValue storageTextures = JsValue(JsValue::Undefined);
    // Pass-field propagation (reference fa83eeabf): pass labels and
    // per-pass execution controls copied verbatim. `name`/`type` stay
    // queryable metadata (backend shader-kind dispatch remains
    // source-derived); `viewport` is the authored spec, resolved to backend
    // x/y/w/h numbers by the runtime; `clear` drives the render-pass
    // loadOp; `samplerTypes` selects per-binding samplers. No catalog
    // definition declares them at the pinned reference, so they stay
    // Undefined (and unserialized) for every real program today.
    JsValue passName = JsValue(JsValue::Undefined);
    JsValue passType = JsValue(JsValue::Undefined);
    JsValue clear = JsValue(JsValue::Undefined);
    JsValue viewport = JsValue(JsValue::Undefined);
    JsValue samplerTypes = JsValue(JsValue::Undefined);

    // inputs/outputs: ORDER PRESERVED in DECLARATION order (read from the ordered effect
    // definition; blit passes always have
    // exactly one input {"src":...} / one output {"color":...}).
    // resources.cpp's allocateResources() depends on this order for
    // correct phys_N assignment (reference/04 §1.3).
    JsVector<std::pair<JsText, JsText>> inputs;
    JsVector<std::pair<JsText, JsText>> outputs;

    JsObject uniforms;  // name -> literal value (bool/double/string/array/{type:'Oscillator',...})

    // --- metadata (effect passes: always present; blit passes: see
    // effectKey/effectFunc/effectNamespace notes) ---
    JsValue effectKey = JsValue(JsValue::Undefined);       // undefined for blit
    JsValue effectFunc = JsValue(JsValue::Undefined);      // undefined for blit (normalizer supplies 'blit')
    JsValue effectNamespace = JsValue(JsValue::Undefined); // undefined for blit; JSON null if effectDef.namespace unset
    JsValue nodeId = JsValue(JsValue::Undefined);          // undefined ONLY for the final-chain blit
    JsValue stepIndex = JsValue(JsValue::Undefined);       // undefined ONLY for the final-chain blit

    bool inheritsVolumeSize = false;  // emitted only if true (both raw + normalized shapes)

    JsObject uniformSpecs;  // effect passes only; absent (empty + not emitted) on blit
    bool hasUniformSpecs = false;

    JsObject scopedParams;  // emitted only if non-empty

    // Pass-level uniform aliases (reference bd773801, runtime/uniform-aliases.js):
    // `{ shaderUniform: globalName }` recorded when a pass definition feeds a
    // shader uniform from a differently named global
    // (`uniforms: { layoutMode: "layout" }`). Emitted only if non-empty; the
    // runtime writes a changed parameter to these aliased uniforms too
    // (Backend::applyStepParameterValues).
    JsObject uniformAliases;  // emitted only if non-empty
};

// Full expand() result (reference/03 §1: `{ passes, errors, programs,
// textureSpecs, renderSurface }`).
struct ExpandResult {
    JsVector<ExpandedPass> passes;
    JsArray errors;         // [{message[, step]}] — step echoed verbatim on "Effect not found"
    JsObject programs;      // uniqueProgName -> {...shaders, uniformLayout, defines} (RAW shape)
    JsObject textureSpecs;  // virtualTexId -> spec (RAW; width/height may carry scoped {param} refs)
    JsValue renderSurface = JsValue(JsValue::Undefined);  // string, or JSON null if unresolved
    // Per-step external texture bindings ({textureId, uniform, stepIndex, effect}), one per
    // texture id, so hosts can enumerate media texture ids (reference expander mediaSteps).
    JsArray mediaSteps;
};

// Expands `validated` (nm::validate()'s output: {plans, diagnostics,
// render, vars, searchNamespaces}) into a Render Graph. `registry` supplies
// effect definitions (getEffect), the std enum tree (for member-typed
// global DEFAULTS only — reference/03 §1: the expander's own local
// resolveEnum walks ONLY the fixed std tree, never the dynamic per-effect
// choices/project tree; by the time a DSL value reaches here the validator
// has already resolved any dynamic choice to its integer), and declaration-ordered global and pass definitions.
//
// Throws std::runtime_error("plan.chain is not iterable") for a plan with
// no chain (control flow), as expander.js's for...of does.
ExpandResult expand(const JsObject& validated, EffectRegistry& registry,
                    const JsObject& options = {});

// Serializes one ExpandedPass to the exact RAW shape expander.js's own
// object literals produce for that pass kind (reference/03 §2.1 effect /
// §2.2 blit) — omitting every JsValue::Undefined field, matching
// `JSON.stringify`'s undefined-drops-the-key semantics. Used by both the expanded and graph dumps.
Value toRawPassJson(const ExpandedPass& pass);

} // namespace nm
