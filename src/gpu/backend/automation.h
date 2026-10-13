#pragma once

// Automation values (osc(), midi() and audio() in the DSL) as the reference
// pipeline evaluates them each frame (shaders/src/runtime/pipeline.js, the
// functions before class Pipeline): oscillators of seven kinds, automation in
// their bounds, phase and seed, frequency modulation by integrating the rate
// (closed forms for the simple waves, Gauss-Legendre quadrature otherwise),
// and the consumer's range.
//
// MIDI and audio values read the host's MIDI and audio states (src/host),
// selected by port or device and channel as the reference selects them;
// without a state they evaluate to their lower bound. MIDI falloff reads the
// wall time (Date.now() in the reference), which the caller supplies.
//
// Math.sin and Math.cos are correctly rounded here (js::sin, js::cos; see
// js_math.h), as Chromium's V8 returns them. Node's V8, where the gates run,
// differs in the last bit of about 3% of results; a difference reaches a
// float32 uniform only when it crosses a float32 rounding boundary.

#include "core/value/value.h"

namespace libnoisemaker {
class AudioState;
class MidiState;
}  // namespace libnoisemaker

namespace libnoisemaker::backend {

// pipeline.externalState: the host's MIDI and audio states, either absent.
struct ExternalState {
    const MidiState* midi = nullptr;
    const AudioState* audio = nullptr;
};

// value && typeof value === 'object' and its type, or its _ast's, is
// Oscillator, Midi or Audio.
bool isAutomationValue(const Value& value);

// evaluateAutomation(config, normalizedTime, range, externalState): the
// value at the normalized time, scaled into range ({ min, max }) when both
// bounds are finite numbers. wallTime is the context's Date.now().
double evaluateAutomation(const Value& config, double normalizedTime, const Value& range, const ExternalState& external,
                          double wallTime);

}  // namespace libnoisemaker::backend
