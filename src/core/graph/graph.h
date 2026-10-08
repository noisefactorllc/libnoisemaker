#pragma once
#include "core/graph/compat.h"


namespace nm {

// One entry of `graph.textures`: docs/GRAPH-JSON-SCHEMA.md "TextureSpec &
// dimensions" / reference/04-resources-pipeline.md §0. `width`/`height`/
// `depth` are kept as raw JsValue and resolved against a screen size at
// texture-creation time (see surface.h resolveDimension()) — a number,
// "screen"/"auto", a percent string, or a {param|screenDivide|scale}
// object.
struct TextureSpec {
    JsValue width;
    JsValue height;
    JsValue depth;   // present only for is3D specs
    bool is3D = false;
    bool mipmaps = false;
    bool persistent = false;
    JsText filter;     // present for 3D textures ("nearest" or "linear")
    JsText format = JsText(u"rgba16f");
    JsList usage;
};

// One executable step of a render graph (docs/GRAPH-JSON-SCHEMA.md "Pass
// (normalized)"). Field set matches the T3 brief's interface contract
// (id, progName, effectKey, passType, drawMode, inputs, outputs, uniforms,
// defines, repeat) plus the extra fields the backend needs to resolve the
// shader file / program cache key (effectNamespace, func, program, nodeId)
// — cheap to carry since they're already present in the source JSON.
//
// `inputs`/`outputs`/`uniforms`/`defines` are kept as JsObject (matching
// assembleShader's fixed `defines` parameter type exactly, and avoiding a
// pointless conversion for the others). Iteration order of these follows
// JsObject's own order, not necessarily the original JSON text order —
// see shader_assembly.h's note; texture-unit assignment order and uniform
// bind order are consequently also JsObject-order, which does not change
// rendered output (each named sampler/uniform still resolves to the correct
// unit/value regardless of iteration order).
struct Pass {
    JsText id;
    JsText passType;        // "effect" | "blit"
    JsText effectNamespace; // pass.namespace in the JSON; empty for blit
    JsText func;            // "blit" for blit passes
    JsText progName;        // bare program basename; "blit" for blit passes
    JsText program;         // program cache id (node-prefixed, define-suffixed)
    JsText effectKey;       // "namespace.func"; empty for blit
    JsText nodeId;
    JsText drawMode;        // e.g. "points"; T3 only executes the fullscreen-triangle default
    JsObject defines;     // #define KEY VALUE, compile-time constants
    JsObject inputs;      // samplerName -> texId
    JsObject outputs;     // outName -> texId
    JsObject uniforms;    // name -> literal value
    JsObject uniformSpecs; // name -> {min,max}, used to scale automation values
    JsValue repeat;       // int | uniform-name string | undefined
    JsValue drawBuffers;  // int | undefined; >1 signals MRT alongside outputs.size()>1
    JsValue count;        // int | "input"/"auto"/"screen" string | undefined (agent vertex count)
    JsValue countUniform; // triangles: uniform-name string; the resolved vertex count comes
                             // from pass.uniforms[name] then the globals (reference webgl2.js
                             // executePass countUniform branch)
    JsValue blend;        // bool | [srcFactor, dstFactor] string pair | undefined
    JsValue conditions;   // {runIf?:[{uniform,equals}], skipIf?:[...]} | undefined — evaluated
                              // per frame against the pass's resolved uniforms (reference
                              // pipeline.js shouldSkipPass mirror; see Backend::shouldSkipPass).
    int stepIndex = -1;      // DSL step (node) index; -1 when the graph omits it (final-chain blit)
    bool inheritsVolumeSize = false; // consumer pass: volumeSize comes from the upstream emitter
    JsObject scopedParams; // uniform -> chain/node-scoped uniform name (e.g. zoom -> zoom_chain_0)
    // Pass-level uniform aliases (reference bd773801 expander.js uniformAliases,
    // runtime/uniform-aliases.js): { shaderUniform: globalName } recorded when a
    // pass definition feeds a shader uniform from a differently named global
    // (`uniforms: { layoutMode: "layout" }`). applyStepParameterValues writes a
    // changed parameter to these aliased uniforms too, so a live change reaches
    // the shader exactly as a recompile would.
    JsObject uniformAliases; // shaderUniform -> globalName
};

// Runtime view of a compiled graph. Graph::fromJson accepts the reference
// compiler graph and derives per-pass program metadata from it.
struct Graph {
    JsText id;
    JsText source;
    JsText renderSurface;                 // e.g. "o0"; empty if the graph presents nothing
    JsVector<Pass> passes;                  // execution order
    GraphMap<JsText, JsText> allocations;   // virtual texId -> phys_N (kept for traceability only;
                                            // NOT used for GPU texture aliasing — see task report)
    GraphMap<JsText, TextureSpec> textures;  // texId -> spec

    // Parses graph JSON as produced by tools/export-graph.mjs / the live
    // compiler's normalizeGraph (docs/GRAPH-JSON-SCHEMA.md). Throws
    // std::runtime_error with a descriptive message on any schema
    // violation: invalid JSON, wrong top-level shape, or a malformed pass
    // entry.
    static Graph fromJson(const std::string& json);
};

} // namespace nm
