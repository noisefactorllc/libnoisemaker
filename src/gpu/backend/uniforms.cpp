#include "gpu/backend/uniforms.h"

#include "core/edit/jsv.h"
#include "core/value/js_number.h"
#include "gpu/wgsl/jsre.h"
#include "gpu/wgsl/wgsl.h"

#include <cmath>
#include <cstring>

namespace libnoisemaker::backend {

namespace {

constexpr double kMaxSafeInteger = 9007199254740991.0;
// Larger buffers throw instead of allocating (see uniforms.h).
constexpr double kMaxBufferBytes = 1073741824.0;

JsError range_error(const JsString& message) { return JsError(u"RangeError", message); }

// ToIntegerOrInfinity, as ToIndex starts.
double to_integer(double value) { return std::isnan(value) ? 0 : std::trunc(value) + 0.0; }

// new ArrayBuffer(length)
size_t array_buffer_length(double length) {
    const double n = to_integer(length);
    if (n < 0 || n > kMaxSafeInteger) throw range_error(u"Invalid array buffer length");
    if (n > kMaxBufferBytes) throw range_error(u"Array buffer allocation failed");
    return size_t(n);
}

// new Float32Array(length)
size_t typed_array_length(double length) {
    const double n = to_integer(length);
    if (n < 0 || n > kMaxSafeInteger) throw range_error(u"Invalid typed array length: " + js_number_to_string(length));
    if (n * 4 > kMaxBufferBytes) throw range_error(u"Array buffer allocation failed");
    return size_t(n);
}

uint32_t to_uint32(double value) {
    if (!std::isfinite(value)) return 0;
    double m = std::fmod(std::trunc(value), 4294967296.0);
    if (m < 0) m += 4294967296.0;
    return uint32_t(m);
}

// Math.round: the nearest integer, halves toward +Infinity, -0 kept.
double js_round(double x) {
    if (!std::isfinite(x) || std::trunc(x) == x) return x;
    if (x < 0 && x >= -0.5) return -0.0;
    const double floor = std::floor(x);
    return x - floor >= 0.5 ? floor + 1 : floor;
}

// Math.max and Math.min of two numbers: NaN wins, and +0 sorts above -0.
double js_max(double a, double b) {
    if (std::isnan(a) || std::isnan(b)) return std::nan("");
    if (a == b) return std::signbit(a) ? b : a;
    return a > b ? a : b;
}
double js_min(double a, double b) {
    if (std::isnan(a) || std::isnan(b)) return std::nan("");
    if (a == b) return std::signbit(a) ? a : b;
    return a < b ? a : b;
}

// a + b: string concatenation when either side is a string or an object.
Value js_add(const Value& a, const Value& b) {
    auto stringish = [](const Value& v) { return v.is_string() || v.is_array() || v.is_object() || v.is_function(); };
    if (stringish(a) || stringish(b)) return Value(jsv::to_string(a) + jsv::to_string(b));
    return Value(jsv::to_number(a) + jsv::to_number(b));
}

double number(const Value& value) { return jsv::to_number(value); }
Value element(const Value& array, double index) { return jsv::get_v(array, Value(index)); }
bool nullish(const Value& value) { return value.is_undefined() || value.is_null(); }

// double to float32 bits, rounded to nearest even. A NaN keeps its sign and the
// top of its payload and becomes quiet, as the hardware conversion V8 uses
// does; spelled out so that no compiler folds it differently.
uint32_t float_bits(double value) {
    if (std::isnan(value)) {
        uint64_t bits = 0;
        std::memcpy(&bits, &value, 8);
        return uint32_t(bits >> 63) << 31 | 0x7FC00000u | uint32_t((bits >> 29) & 0x3FFFFFu);
    }
    const float f = static_cast<float>(value);
    uint32_t bits = 0;
    std::memcpy(&bits, &f, 4);
    return bits;
}

// A Float32Array element read as a number and stored again (arr[i] = value[i]):
// a signaling NaN becomes quiet, any other value keeps its bits.
uint32_t element_bits(float value) {
    uint32_t bits = 0;
    std::memcpy(&bits, &value, 4);
    if ((bits & 0x7F800000u) == 0x7F800000u && (bits & 0x7FFFFFu)) bits |= 0x400000u;
    return bits;
}

// A DataView over a byte vector, written little-endian.
struct View {
    std::vector<uint8_t>& bytes;
    size_t check(double offset, size_t size) const {
        const double index = to_integer(offset);
        if (index < 0 || index > kMaxSafeInteger || index + double(size) > double(bytes.size())) {
            throw range_error(u"Offset is outside the bounds of the DataView");
        }
        return size_t(index);
    }
    void put(size_t index, uint32_t bits) {
        for (int i = 0; i < 4; ++i) bytes[index + i] = uint8_t(bits >> (8 * i));
    }
    void setFloat32(double offset, double value) { put(check(offset, 4), float_bits(value)); }
    void setInt32(double offset, double value) { put(check(offset, 4), to_uint32(value)); }
    void setUint32(double offset, double value) { put(check(offset, 4), to_uint32(value)); }
};

void put_bits(std::vector<uint8_t>& out, uint32_t bits) {
    for (int i = 0; i < 4; ++i) out.push_back(uint8_t(bits >> (8 * i)));
}
void put_float(std::vector<uint8_t>& out, double value) { put_bits(out, float_bits(value)); }

// The offsets of the componentOffset object literal; anything else is
// undefined or an inherited member, and the offset sum becomes NaN.
double component_offset(const Value& component) {
    const JsString key = jsv::to_property_key(component);
    if (key == u"x") return 0;
    if (key == u"y") return 4;
    if (key == u"z") return 8;
    if (key == u"w") return 12;
    return std::nan("");
}

}  // namespace

ProgramUniforms ProgramUniforms::fromSource(const JsString& processedSource, const Value& definitionUniformLayout) {
    ProgramUniforms program;
    program.declaredUniformBufferSize = wgsl::parseDeclaredUniformBufferSize(processedSource);
    if (jsv::truthy(definitionUniformLayout)) {
        program.packedUniformLayout = definitionUniformLayout;
        return program;
    }
    const wgsl::PackedLayout parsed = wgsl::parsePackedUniformLayout(processedSource);
    program.packedUniformLayout = wgsl::toValue(parsed);
    if (parsed.kind == wgsl::PackedLayout::Kind::Byte) {
        program.packedUniformLayout.as_object().erase(u"structSize");
        program.byteStructSize = parsed.structSize;
    }
    return program;
}

UniformPacker::UniformPacker() : merged_(Object{}), scratch_(512, 0) {}

void UniformPacker::setMerged(const JsString& key, const Value& value) {
    if (key == u"__proto__") {
        // The __proto__ setter: a primitive leaves the object as it is.
        if (value.is_object() || value.is_array() || value.is_function() || value.is_null()) {
            throw JsError(u"Error", u"a uniform named __proto__ with an object or null value is outside the port");
        }
        return;
    }
    merged_.as_object().set(key, value);
}

std::optional<UniformWrite> UniformPacker::createUniformBuffer(const Value& passUniforms, const Value& globalUniforms,
                                                               const ProgramUniforms& program) {
    for (const JsString& key : mergedKeys_) setMerged(key, Value());
    mergedKeys_.clear();

    const bool hasPass = jsv::truthy(passUniforms);
    if (hasPass) {
        for (const auto& [key, value] : jsv::entries(passUniforms)) {
            if (value.is_undefined()) continue;
            setMerged(key, value);
            mergedKeys_.push_back(key);
        }
    }
    if (jsv::truthy(globalUniforms)) {
        for (const auto& [key, value] : jsv::entries(globalUniforms)) {
            if (hasPass && jsv::in_operator(Value(key), passUniforms)) continue;
            if (value.is_undefined()) continue;
            if (jsv::member(merged_, key).is_undefined()) mergedKeys_.push_back(key);
            setMerged(key, value);
        }
    }
    if (mergedKeys_.empty()) return std::nullopt;

    std::vector<uint8_t> data = jsv::truthy(program.packedUniformLayout)
        ? packUniformsWithLayout(program.packedUniformLayout, program.byteStructSize)
        : packUniforms();
    const double declaredMin = std::isnan(program.declaredUniformBufferSize) ? 0 : program.declaredUniformBufferSize;
    UniformWrite write;
    write.bufferSize = js_max(js_max(double(data.size()), declaredMin), 16);
    write.bytes = std::move(data);
    return write;
}

std::vector<uint8_t> UniformPacker::packUniforms() {
    const Object& uniforms = merged_.as_object();
    const std::vector<JsString> names = uniforms.keys();
    double estimatedSize = 0;
    for (const JsString& name : names) {
        const Value& value = *uniforms.find(name);
        if (value.is_number() || value.is_bool()) estimatedSize += 4;
        else if (value.is_array()) estimatedSize += double(value.as_array().size()) * 4 + 12;
    }
    const double bufferSize = js_max(256, std::ceil((estimatedSize + 64) / 16) * 16);
    if (bufferSize > double(scratch_.size())) scratch_.assign(array_buffer_length(bufferSize), 0);
    View view{scratch_};

    auto alignTo = [](double offset, double alignment) { return std::ceil(offset / alignment) * alignment; };
    double offset = 0;
    for (const JsString& name : names) {
        const Value& value = *uniforms.find(name);
        if (nullish(value)) continue;
        if (value.is_bool()) {
            offset = alignTo(offset, 4);
            view.setInt32(offset, value.as_bool() ? 1 : 0);
            offset += 4;
        } else if (value.is_number()) {
            offset = alignTo(offset, 4);
            const double n = value.as_number();
            const bool integer = std::isfinite(n) && std::trunc(n) == n;
            if (integer && name != u"time" && name != u"deltaTime" && name != u"aspect") view.setInt32(offset, n);
            else view.setFloat32(offset, n);
            offset += 4;
        } else if (value.is_array()) {
            const Array& a = value.as_array();
            const size_t n = a.size();
            if (n == 2) {
                offset = alignTo(offset, 8);
                view.setFloat32(offset, number(a[0]));
                view.setFloat32(offset + 4, number(a[1]));
                offset += 8;
            } else if (n == 3) {
                offset = alignTo(offset, 16);
                for (size_t i = 0; i < 3; ++i) view.setFloat32(offset + double(i) * 4, number(a[i]));
                offset += 16;
            } else if (n == 4) {
                offset = alignTo(offset, 16);
                for (size_t i = 0; i < 4; ++i) view.setFloat32(offset + double(i) * 4, number(a[i]));
                offset += 16;
            } else if (n == 9) {
                offset = alignTo(offset, 16);
                for (size_t col = 0; col < 3; ++col) {
                    for (size_t row = 0; row < 3; ++row) view.setFloat32(offset + double(row) * 4, number(a[col * 3 + row]));
                    offset += 16;
                }
            } else if (n == 16) {
                offset = alignTo(offset, 16);
                for (size_t i = 0; i < 16; ++i) view.setFloat32(offset + double(i) * 4, number(a[i]));
                offset += 64;
            } else {
                for (size_t i = 0; i < n; ++i) {
                    offset = alignTo(offset, 4);
                    view.setFloat32(offset, number(a[i]));
                    offset += 4;
                }
            }
        }
    }
    // new Uint8Array(buffer, 0, usedSize): every write stayed inside the
    // buffer, whose size is a multiple of 16, so usedSize fits it.
    const size_t usedSize = size_t(js_max(256, alignTo(offset, 16)));
    return std::vector<uint8_t>(scratch_.begin(), scratch_.begin() + long(usedSize));
}

Value UniformPacker::resolveAlias(const Value& name, bool includeChannelCount) const {
    const Value value = jsv::member(merged_, jsv::to_property_key(name));
    if (!value.is_undefined()) return value;
    const Value resolution = jsv::member(merged_, u"resolution");
    if (jsv::strict_equals(name, Value(u"width")) && jsv::truthy(resolution)) return element(resolution, 0);
    if (jsv::strict_equals(name, Value(u"height")) && jsv::truthy(resolution)) return element(resolution, 1);
    if (jsv::strict_equals(name, Value(u"channels"))) return Value(4.0);
    if (includeChannelCount && jsv::strict_equals(name, Value(u"channelCount"))) return Value(4.0);
    return Value();
}

std::vector<uint8_t> UniformPacker::packUniformsWithLayout(const Value& layout, std::optional<double> byteStructSize) {
    if (jsv::strict_equals(jsv::member(layout, u"type"), Value(u"byte")) && jsv::member(layout, u"layout").is_array()) {
        return packUniformsWithByteLayout(jsv::member(layout, u"layout"), byteStructSize);
    }
    // The object format {name: {slot, components}} becomes entries.
    Array layoutArray;
    if (layout.is_array()) {
        layoutArray = layout.as_array();
    } else {
        for (const auto& [name, spec] : jsv::entries_strict(layout)) {
            Object entry;
            entry.set(u"name", Value(name));
            entry.set(u"slot", jsv::get(spec, u"slot"));
            entry.set(u"components", jsv::get(spec, u"components"));
            layoutArray.emplace_back(std::move(entry));
        }
    }
    double maxSlot = 0;
    for (const Value& entry : layoutArray) maxSlot = js_max(maxSlot, number(jsv::get(entry, u"slot")));

    std::vector<uint8_t> buffer(array_buffer_length((maxSlot + 1) * 16), 0);
    View view{buffer};
    for (const Value& entry : layoutArray) {
        const Value value = resolveAlias(jsv::get(entry, u"name"), false);
        if (nullish(value)) continue;
        const double slotOffset = number(jsv::get(entry, u"slot")) * 16;
        const Value components = jsv::get(entry, u"components");
        const Value count = jsv::get(components, u"length");
        if (jsv::strict_equals(count, Value(1.0))) {
            const double offset = slotOffset + component_offset(components);
            if (value.is_bool()) view.setFloat32(offset, value.as_bool() ? 1.0 : 0.0);
            else if (value.is_number()) view.setFloat32(offset, value.as_number());
            continue;
        }
        double width = 0;
        if (jsv::strict_equals(count, Value(2.0))) width = 2;
        else if (jsv::strict_equals(count, Value(3.0))) width = 3;
        else if (jsv::strict_equals(count, Value(4.0))) width = 4;
        else continue;
        const double offset = width == 4 ? slotOffset : slotOffset + component_offset(element(components, 0));
        if (value.is_array()) {
            const double n = js_min(double(value.as_array().size()), width);
            for (double i = 0; i < n; ++i) view.setFloat32(offset + i * 4, number(element(value, i)));
        } else if (value.is_number()) {
            view.setFloat32(offset, value.as_number());
        }
    }
    return buffer;
}

std::vector<uint8_t> UniformPacker::packUniformsWithByteLayout(const Value& layout, std::optional<double> structSize) {
    double totalSize = structSize.value_or(0);
    if (std::isnan(totalSize) || totalSize == 0) {
        totalSize = 0;
        for (const Value& entry : layout.as_array()) {
            totalSize = js_max(totalSize, number(js_add(jsv::get(entry, u"offset"), jsv::get(entry, u"size"))));
        }
    }
    const double bufferSize = std::ceil(totalSize / 16) * 16;
    std::vector<uint8_t> buffer(array_buffer_length(js_max(bufferSize, 16)), 0);
    View view{buffer};
    for (const Value& entry : layout.as_array()) {
        const Value value = resolveAlias(jsv::get(entry, u"name"), true);
        if (nullish(value)) continue;
        const Value offset = jsv::get(entry, u"offset");
        const Value type = jsv::get(entry, u"type");
        const Value components = jsv::get(entry, u"components");
        const bool isInt = jsv::strict_equals(type, Value(u"int"));
        const bool isUint = jsv::strict_equals(type, Value(u"uint"));
        auto write = [&](const Value& at, double v) {
            if (isInt) view.setInt32(number(at), js_round(v));
            else if (isUint) view.setUint32(number(at), js_round(v));
            else view.setFloat32(number(at), v);
        };
        if (jsv::strict_equals(components, Value(1.0))) {
            if (value.is_bool()) {
                if (isInt || isUint) view.setInt32(number(offset), value.as_bool() ? 1 : 0);
                else view.setFloat32(number(offset), value.as_bool() ? 1.0 : 0.0);
            } else if (value.is_number()) {
                write(offset, value.as_number());
            }
        } else if (value.is_array()) {
            const double n = js_min(double(value.as_array().size()), number(components));
            for (double i = 0; i < n; ++i) write(js_add(offset, Value(i * 4)), number(element(value, i)));
        } else if (value.is_number()) {
            write(offset, value.as_number());
        }
    }
    return buffer;
}

std::optional<UniformWrite> UniformPacker::createSingleUniformBuffer(const Value& value, const JsString& typeDecl) {
    std::vector<uint8_t> data;
    const bool integerDecl = typeDecl == u"i32" || typeDecl == u"u32";
    if (value.is_bool() && !integerDecl) {
        put_float(data, value.as_bool() ? 1 : 0);
    } else if (value.is_bool()) {
        data.resize(4);
        View{data}.setInt32(0, value.as_bool() ? 1 : 0);
    } else if (value.is_number()) {
        if (integerDecl) {
            data.resize(4);
            View{data}.setInt32(0, js_round(value.as_number()));
        } else {
            put_float(data, value.as_number());
        }
    } else if (value.is_array() || value.is_float32_array()) {
        // Array.isArray(value) || value instanceof Float32Array. Copying an
        // element (arr[i] = value[i]) reads it as a number; a Float32Array the
        // reference passes on whole (data = value) keeps its raw bits.
        const bool typed = value.is_float32_array();
        const size_t n = typed ? value.as_float32_array().size() : value.as_array().size();
        auto put = [&](size_t i) {
            if (typed) put_bits(data, element_bits(value.as_float32_array()[i]));
            else put_float(data, number(value.as_array()[i]));
        };
        auto put_raw = [&]() {
            for (float f : value.as_float32_array()) {
                uint32_t bits = 0;
                std::memcpy(&bits, &f, 4);
                put_bits(data, bits);
            }
        };
        if (n == 2 || n == 4) {
            for (size_t i = 0; i < n; ++i) put(i);
        } else if (n == 3) {
            for (size_t i = 0; i < 3; ++i) put(i);
            put_float(data, 0);
        } else if (n == 9) {
            for (size_t col = 0; col < 3; ++col) {
                for (size_t row = 0; row < 3; ++row) put(col * 3 + row);
                put_float(data, 0);
            }
        } else {
            static const jsre::Regex vec4Array(u"^array<\\s*vec4");
            static const jsre::Regex countRe(u"^array<[^,>]+(?:<[^>]+>)?\\s*,\\s*(\\d+)\\s*>");
            if (vec4Array.test(typeDecl)) {
                // parseInt of a \d run, which ToNumber reads the same way.
                const auto m = countRe.exec(typeDecl);
                const double count = m ? jsv::to_number(Value(m->str(typeDecl, 1))) : std::ceil(double(n) / 4);
                if (typed && double(n) == count * 4) {
                    put_raw();
                } else {
                    const size_t floats = typed_array_length(count * 4);
                    data.reserve(floats * 4);
                    for (size_t i = 0; i < floats; ++i) {
                        if (i < n) put(i);
                        else put_float(data, 0);
                    }
                }
            } else if (typed) {
                put_raw();  // new Float32Array(value) is not called on a Float32Array
            } else {
                for (size_t i = 0; i < n; ++i) put(i);
            }
        }
    } else {
        return std::nullopt;
    }
    UniformWrite write;
    write.bufferSize = js_max(double(data.size()), 16);
    write.bytes = std::move(data);
    return write;
}

}  // namespace libnoisemaker::backend
