#pragma once

// Automation values (osc(), midi() and audio() in the DSL) as the reference
// pipeline evaluates them each frame (shaders/src/runtime/pipeline.js, the
// functions before class Pipeline): oscillators of seven kinds, automation in
// their bounds, phase and seed, frequency modulation by integrating the rate
// (closed forms for the simple waves, Gauss-Legendre quadrature otherwise),
// and the consumer's range.
//
// MIDI and audio values read the host's MIDI and audio states, which the
// pipeline does not take yet: they evaluate as the reference does with none
// set, to their lower bound.
//
// Math.sin and Math.cos: V8 computes them with its copy of glibc's sin and
// cos; here they are correctly rounded (js::sin, js::cos). The two differ in
// the last bit of about 3% of doubles; a difference reaches a float32 uniform
// only when it crosses a float32 rounding boundary.

#include "core/value/value.h"

namespace libnoisemaker::backend {

// value && typeof value === 'object' and its type, or its _ast's, is
// Oscillator, Midi or Audio.
bool isAutomationValue(const Value& value);

// evaluateAutomation(config, normalizedTime, range, externalState): the
// value at the normalized time, scaled into range ({ min, max }) when both
// bounds are finite numbers.
double evaluateAutomation(const Value& config, double normalizedTime, const Value& range);

}  // namespace libnoisemaker::backend
