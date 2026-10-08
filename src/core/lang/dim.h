#pragma once

#include "enums.h"

// dim.h — texture-dimension spec parse/scope helpers. Port of the DimSpec
// handling embedded in the REFERENCE shaders/src/runtime/expander.js
// (scopeDimSpec, reference/03 §6.3), cross-checked against
// td/noisemaker/compiler/graph/dim.py (a dedicated module in that port;
// godot inlines the same logic directly in expander.gd). This repo follows
// TD's factoring (a separate translation unit), since the T10 brief names
// dim.h/dim.cpp explicitly as compiler-track files.
//
// A texture dimension (width/height/depth) is kept as its RAW JSON value
// end to end here — this module never resolves a dimension to pixels (that
// is `resolveDimension`, a RENDERER concern living in
// qt/noisemaker/runtime/surface.h, reference/04 §9). The only transform
// the expander itself applies is renaming a `{param}`/`{screenDivide}`
// dimension's referenced uniform name to a scope-suffixed variant, so two
// chain/particle-pipeline instances of the same effect don't collide on a
// single shared sizing uniform (e.g. `stateSize` -> `stateSize_node_7`).
//
// A dimension is one of: a number; "screen"/"auto"; a "N%" string;
// {param, paramDefault?, multiply?, power?, default?};
// {screenDivide, default?}; {scale, clamp?}. Only the {param}/{screenDivide}
// forms are scopable; every other shape (including plain numbers/strings)
// passes through `scopeDim` completely unchanged.


namespace nm::dim {

// True iff `d` is a JSON object carrying a `param` or `screenDivide` key
// (reference/03 §6.3 `dimReferencesParam` / hasParamRef). Any other shape
// (number, "screen", "N%", {scale}, undefined) returns false.
bool referencesParam(const JsValue& d);

// Rewrites a {param}/{screenDivide} dimension's referenced name to
// `${original}_${scopeSuffix}`, recording the old->new mapping in
// `scopedParamMap` (insertion order irrelevant — reference/03 §4.9 step 12
// consumes it by key lookup only, never by iteration order). Every other
// dimension shape is returned unchanged. Mirrors the reference's
// `scopeDimSpec` exactly, including that a dimension with BOTH `param` and
// `screenDivide` never occurs in practice (the reference itself only ever
// checks one or the other, `param` first) — ported the same one-or-the-
// other precedence.
//
// `stateSizeScope`, when non-empty, overrides `scopeSuffix` SPECIFICALLY for
// a `{param: "stateSize"}` dimension (never `screenDivide`) — a non-global,
// non-particle-pipeline-scoped texture (e.g. points/heightGrid's own
// pass-through/agent programs) that still sizes itself off the pipeline's
// agent-state resolution must scope `stateSize` to the PARTICLE PIPELINE id,
// not this texture's own chain scope, so it agrees with the pipeline's real
// state-texture dimensions instead of picking up an unrelated chain-scoped
// default. Mirrors reference expander.js's inline `dimensionScope` ternary.
JsValue scope(const JsValue& d, const JsText& scopeSuffix, JsMap<JsText, JsText>& scopedParamMap,
                  const JsText& stateSizeScope = JsText());

} // namespace nm::dim
