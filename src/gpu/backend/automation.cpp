#include "gpu/backend/automation.h"

#include "core/edit/jsv.h"
#include "core/value/js_math.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>

namespace libnoisemaker::backend {

namespace {

// Math.PI * 2
constexpr double TAU = 3.141592653589793 * 2;
constexpr int MAX_AUTOMATION_DEPTH = 8;

double oscSine(double t) { return (1.0 - js::cos(t * TAU)) * 0.5; }
double oscTri(double t) {
    const double tf = t - std::floor(t);
    return 1.0 - std::fabs(tf * 2.0 - 1.0);
}
double oscSaw(double t) { return t - std::floor(t); }
double oscSawInv(double t) { return 1.0 - (t - std::floor(t)); }
double oscSquare(double t) { return (t - std::floor(t)) >= 0.5 ? 1.0 : 0.0; }

// x % 1 is std::fmod: the sign of the dividend, -0 kept.
double hash21(double px, double py, double s) {
    double x = std::fmod(px * 234.34 + s, 1);
    double y = std::fmod(py * 435.345 + s, 1);
    if (x < 0) x += 1;
    if (y < 0) y += 1;
    const double p = x + y + (x + y) * 34.23;
    return std::fmod(x * y * p, 1);
}

double noise2D(double px, double py, double s) {
    const double ix = std::floor(px);
    const double iy = std::floor(py);
    double fx = px - ix;
    double fy = py - iy;
    fx = fx * fx * (3 - 2 * fx);
    fy = fy * fy * (3 - 2 * fy);
    const double a = hash21(ix, iy, s);
    const double b = hash21(ix + 1, iy, s);
    const double c = hash21(ix, iy + 1, s);
    const double d = hash21(ix + 1, iy + 1, s);
    return a * (1 - fx) * (1 - fy) + b * fx * (1 - fy) + c * (1 - fx) * fy + d * fx * fy;
}

// Looping noise: samples on a circle, so the loop is seamless.
double oscNoise(double t, double seed) {
    const double temporal = std::fmod(t, 1);
    const double angle = temporal * TAU;
    const double radius = 2;
    const double loopX = js::cos(angle) * radius;
    const double loopY = js::sin(angle) * radius;
    const double n1 = noise2D(loopX + seed, loopY + seed, seed);
    const double n2 = noise2D(loopX + seed * 2, loopY + seed * 2, seed);
    return (n1 + n2) / 2;
}

// Two-stage periodic noise, as the osc2d effect computes it.
double oscNoise2d(double time, double speed, double seed) {
    const auto periodicValue = [](double x, double v) { return (js::sin((x - v) * TAU) + 1) * 0.5; };
    const double px = (std::fabs(std::fmod(seed, 16)) + 0.5) / 16;
    const double py = (std::fabs(std::fmod(std::floor(seed / 16), 16)) + 0.5) / 16;
    const double timeNoise = noise2D(px, py, seed + 12345);
    const double valueNoise = noise2D(px, py, seed);
    const double scaledTime = periodicValue(time, timeNoise) * speed;
    return periodicValue(scaledTime, valueNoise);
}

Value range(double min, double max) {
    Object out;
    out.set(u"min", Value(min));
    out.set(u"max", Value(max));
    return Value(std::move(out));
}
// AUTOMATION_FIELD_RANGES
const Value& unitRange() { static const Value r = range(0, 1); return r; }
const Value& speedRange() { static const Value r = range(-20, 20); return r; }
const Value& offsetRange() { static const Value r = range(-1, 1); return r; }
const Value& seedRange() { static const Value r = range(1, 9999); return r; }
const Value& sensitivityRange() { static const Value r = range(0, 10); return r; }

// 16-, 8-, 4- and 2-point Gauss-Legendre rules on [-1, 1].
struct Rule {
    const double* nodes;
    const double* weights;
    size_t size;
};
constexpr double kNodes16[] = {
    -0.9894009349916499, -0.9445750230732326, -0.8656312023878318, -0.755404408355003,
    -0.6178762444026438, -0.4580167776572274, -0.2816035507792589, -0.0950125098376374,
    0.0950125098376374, 0.2816035507792589, 0.4580167776572274, 0.6178762444026438,
    0.755404408355003, 0.8656312023878318, 0.9445750230732326, 0.9894009349916499};
constexpr double kWeights16[] = {
    0.0271524594117541, 0.0622535239386479, 0.0951585116824928, 0.1246289712555339,
    0.1495959888165767, 0.1691565193950025, 0.1826034150449236, 0.1894506104550685,
    0.1894506104550685, 0.1826034150449236, 0.1691565193950025, 0.1495959888165767,
    0.1246289712555339, 0.0951585116824928, 0.0622535239386479, 0.0271524594117541};
constexpr double kNodes8[] = {-0.9602898564975363, -0.7966664774136267, -0.525532409916329, -0.1834346424956498,
                              0.1834346424956498,  0.525532409916329,   0.7966664774136267, 0.9602898564975363};
constexpr double kWeights8[] = {0.1012285362903763, 0.2223810344533745, 0.3137066458778873, 0.362683783378362,
                                0.362683783378362,  0.3137066458778873, 0.2223810344533745, 0.1012285362903763};
constexpr double kNodes4[] = {-0.8611363115940526, -0.3399810435848563, 0.3399810435848563, 0.8611363115940526};
constexpr double kWeights4[] = {0.3478548451374538, 0.6521451548625461, 0.6521451548625461, 0.3478548451374538};
constexpr double kNodes2[] = {-0.5773502691896257, 0.5773502691896257};
constexpr double kWeights2[] = {1, 1};
constexpr Rule kRules[] = {{kNodes16, kWeights16, 16}, {kNodes8, kWeights8, 8}, {kNodes4, kWeights4, 4}, {kNodes2, kWeights2, 2}};

// config.type === name || config._ast?.type === name
bool typeIs(const Value& config, const char16_t* name) {
    if (jsv::strict_equals(jsv::member(config, u"type"), Value(name))) return true;
    const Value ast = jsv::member(config, u"_ast");
    return !ast.is_undefined() && !ast.is_null() && jsv::strict_equals(jsv::member(ast, u"type"), Value(name));
}

double scaleAutomationValue(double value, const Value& range) {
    if (!jsv::truthy(range)) return value;
    const Value min = jsv::member(range, u"min");
    const Value max = jsv::member(range, u"max");
    if (!jsv::is_finite_number(min) || !jsv::is_finite_number(max)) return value;
    return min.as_number() + value * (max.as_number() - min.as_number());
}

double evaluate(const Value& config, double normalizedTime, const Value& range, int depth);

double resolveAutomationField(const Value& value, double normalizedTime, const Value& range, int depth, double fallback) {
    if (isAutomationValue(value)) return evaluate(value, normalizedTime, range, depth + 1);
    return jsv::is_finite_number(value) ? value.as_number() : fallback;
}

bool hasDynamicAutomationFields(const Value& config) {
    const bool midi = typeIs(config, u"Midi");
    for (const char16_t* field : {u"min", u"max", u"sensitivity"}) {
        if (!midi && JsString(field) == u"sensitivity") continue;
        if (isAutomationValue(jsv::member(config, field))) return true;
    }
    return false;
}

// The integral of the raw wave from 0 to x, or none for another type (null
// in the reference, which arithmetic reads as 0).
std::optional<double> oscPrimitive(const Value& type, double x) {
    if (!type.is_number()) return std::nullopt;
    const double kind = type.as_number();
    const double whole = std::floor(x);
    const double fraction = x - whole;
    if (kind == 0) return x * 0.5 - js::sin(x * TAU) / (2 * TAU);
    if (kind == 1) {
        const double partial = fraction < 0.5 ? fraction * fraction : 2 * fraction - fraction * fraction - 0.5;
        return whole * 0.5 + partial;
    }
    if (kind == 2) return whole * 0.5 + fraction * fraction * 0.5;
    if (kind == 3) return x - (whole * 0.5 + fraction * fraction * 0.5);
    if (kind == 4) return whole * 0.5 + js::max(0, fraction - 0.5);
    return std::nullopt;
}

bool canIntegrateOscillatorExactly(const Value& config) {
    const Value oscType = jsv::member(config, u"oscType");
    if (!(jsv::to_number(oscType) >= 0) || !(jsv::to_number(oscType) <= 4)) return false;
    for (const char16_t* field : {u"min", u"max", u"speed", u"offset", u"seed"}) {
        if (!jsv::is_finite_number(jsv::member(config, field))) return false;
    }
    return true;
}

double evaluateOscillator(const Value& osc, double normalizedTime, int depth);

double integrateSimpleOscillator(const Value& config, double normalizedTime) {
    const Value oscType = jsv::member(config, u"oscType");
    const double min = jsv::member(config, u"min").as_number();
    const double max = jsv::member(config, u"max").as_number();
    const double speed = jsv::member(config, u"speed").as_number();
    const double offset = jsv::member(config, u"offset").as_number();
    if (speed == 0) return evaluateOscillator(config, 0, 0) * normalizedTime;
    const double start = oscPrimitive(oscType, offset).value_or(0);
    const double end = oscPrimitive(oscType, offset + speed * normalizedTime).value_or(0);
    const double rawIntegral = (end - start) / speed;
    return min * normalizedTime + (max - min) * rawIntegral;
}

double integrateAutomation(const Value& config, double normalizedTime, const Value& range, int depth) {
    double integral;
    if (typeIs(config, u"Oscillator") && canIntegrateOscillatorExactly(config)) {
        integral = integrateSimpleOscillator(config, normalizedTime);
    } else if ((typeIs(config, u"Midi") || typeIs(config, u"Audio")) && !hasDynamicAutomationFields(config)) {
        // An external input exposes a snapshot, constant over the interval.
        integral = evaluate(config, normalizedTime, Value(), depth + 1) * normalizedTime;
    } else {
        // The quadrature order falls as rate modulators nest.
        const Rule& rule = kRules[std::min(depth, 3)];
        const double midpoint = normalizedTime * 0.5;
        const double halfWidth = normalizedTime * 0.5;
        double sum = 0;
        for (size_t i = 0; i < rule.size; ++i) {
            const double sampleTime = midpoint + halfWidth * rule.nodes[i];
            sum += rule.weights[i] * evaluate(config, sampleTime, Value(), depth + 1);
        }
        integral = halfWidth * sum;
    }
    if (!jsv::truthy(range)) return integral;
    const Value min = jsv::member(range, u"min");
    const Value max = jsv::member(range, u"max");
    if (!jsv::is_finite_number(min) || !jsv::is_finite_number(max)) return integral;
    return min.as_number() * normalizedTime + integral * (max.as_number() - min.as_number());
}

double evaluateOscillator(const Value& osc, double normalizedTime, int depth) {
    const double min = resolveAutomationField(jsv::member(osc, u"min"), normalizedTime, unitRange(), depth, 0);
    const double max = resolveAutomationField(jsv::member(osc, u"max"), normalizedTime, unitRange(), depth, 1);
    const double offset = resolveAutomationField(jsv::member(osc, u"offset"), normalizedTime, offsetRange(), depth, 0);
    const double seed = resolveAutomationField(jsv::member(osc, u"seed"), normalizedTime, seedRange(), depth, 1);

    // A modulated rate is frequency modulation: the phase integrates it.
    const Value speed = jsv::member(osc, u"speed");
    const double phase = isAutomationValue(speed)
        ? integrateAutomation(speed, normalizedTime, speedRange(), depth)
        : normalizedTime * (jsv::is_finite_number(speed) ? speed.as_number() : 1);
    const double t = phase + offset;

    double value = 0;
    const Value oscType = jsv::member(osc, u"oscType");
    if (oscType.is_number()) {
        const double kind = oscType.as_number();
        if (kind == 0) value = oscSine(t);
        else if (kind == 1) value = oscTri(t);
        else if (kind == 2) value = oscSaw(t);
        else if (kind == 3) value = oscSawInv(t);
        else if (kind == 4) value = oscSquare(t);
        else if (kind == 5) value = oscNoise(t, seed);
        else if (kind == 6) {
            const double rate = resolveAutomationField(speed, normalizedTime, speedRange(), depth, 1);
            value = oscNoise2d(normalizedTime + offset, std::isfinite(rate) ? rate : 1, seed);
        }
    }
    return min + value * (max - min);
}

// evaluateAutomation. A Value holds no cycles, so the reference's stack of
// configs being evaluated never finds one; the depth bound remains.
double evaluate(const Value& config, double normalizedTime, const Value& range, int depth) {
    if (!isAutomationValue(config) || depth > MAX_AUTOMATION_DEPTH) return scaleAutomationValue(0, range);
    double value;
    if (typeIs(config, u"Oscillator")) {
        value = evaluateOscillator(config, normalizedTime, depth);
    } else if (typeIs(config, u"Midi")) {
        // evaluateMidi with no MIDI state: the lower bound.
        value = resolveAutomationField(jsv::member(config, u"min"), normalizedTime, unitRange(), depth, 0);
        (void)resolveAutomationField(jsv::member(config, u"max"), normalizedTime, unitRange(), depth, 1);
        (void)resolveAutomationField(jsv::member(config, u"sensitivity"), normalizedTime, sensitivityRange(), depth, 1);
    } else if (jsv::truthy(jsv::member(config, u"_invalid"))) {
        const Value min = jsv::member(config, u"min");
        value = jsv::is_finite_number(min) ? min.as_number() : 0;
    } else {
        // evaluateAudio with no audio state: the lower bound.
        value = resolveAutomationField(jsv::member(config, u"min"), normalizedTime, unitRange(), depth, 0);
        (void)resolveAutomationField(jsv::member(config, u"max"), normalizedTime, unitRange(), depth, 1);
    }
    return scaleAutomationValue(value, range);
}

}  // namespace

bool isAutomationValue(const Value& value) {
    if (!value.is_object() && !value.is_array() && !value.is_float32_array()) return false;
    return typeIs(value, u"Oscillator") || typeIs(value, u"Midi") || typeIs(value, u"Audio");
}

double evaluateAutomation(const Value& config, double normalizedTime, const Value& range) {
    return evaluate(config, normalizedTime, range, 0);
}

}  // namespace libnoisemaker::backend
