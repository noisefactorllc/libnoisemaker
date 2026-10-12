#pragma once

#include "core/value/value.h"

namespace libnoisemaker {

// The normalized render graph a host executor loads: each pass gains passType,
// namespace, func, progName and defines, compile-time-define globals move from
// uniforms into defines, and programs keep only uniformLayout and defines.
// Port of normalizeGraph in noisemaker-for-qt 2746d46:tools/export-graph.mjs,
// applied to the reference compiler graph that compileGraphJson() returns.
// defineMap is EffectRegistry::defineMap(): {"<ns>.<func>": {globalKey: defineName}}.
Value normalizeGraphJson(const Value& graph, const Value& defineMap);

}  // namespace libnoisemaker
