#include "uniforms_commands.h"

#include "core/edit/jsv.h"
#include "core/value/json.h"
#include "gpu/backend/uniforms.h"
#include "gpu/wgsl/wgsl.h"

#include <cstdint>
#include <cstring>
#include <exception>
#include <iostream>
#include <memory>
#include <string>
#include <unordered_map>

namespace nm_dump {

namespace {

namespace nm = libnoisemaker;

nm::Value field(const nm::Value& object, const char16_t* key) {
    const nm::Value* found = object.is_object() ? object.as_object().find(key) : nullptr;
    return found ? *found : nm::Value();
}

nm::Value written(const std::optional<nm::backend::UniformWrite>& write) {
    if (!write) return nm::Value::null();
    static const char* digits = "0123456789abcdef";
    std::string hex;
    hex.reserve(write->bytes.size() * 2);
    for (uint8_t byte : write->bytes) {
        hex += digits[byte >> 4];
        hex += digits[byte & 15];
    }
    nm::Object out;
    out.set(u"bufferSize", nm::Value(write->bufferSize));
    out.set(u"bytes", nm::Value(nm::utf8_to_js(hex)));
    return nm::Value(std::move(out));
}

// {"$float32": [numbers]} and {"$float32bits": [hex strings]} stand for a
// Float32Array, which JSON cannot carry; the second keeps NaN payloads.
nm::Value decode(const nm::Value& value) {
    if (!value.is_object() || value.as_object().size() != 1) return value;
    std::vector<float> floats;
    if (const nm::Value* elements = value.as_object().find(u"$float32"); elements && elements->is_array()) {
        for (const nm::Value& element : elements->as_array()) floats.push_back(static_cast<float>(element.as_number()));
        return nm::Value::float32_array(std::move(floats));
    }
    if (const nm::Value* elements = value.as_object().find(u"$float32bits"); elements && elements->is_array()) {
        for (const nm::Value& element : elements->as_array()) {
            const uint32_t bits = static_cast<uint32_t>(std::stoul(nm::js_to_utf8(element.as_string()), nullptr, 16));
            float f = 0;
            std::memcpy(&f, &bits, 4);
            floats.push_back(f);
        }
        return nm::Value::float32_array(std::move(floats));
    }
    return value;
}

nm::Value decode_members(const nm::Value& encoded) {
    const nm::Value value = decode(encoded);
    if (!value.is_object()) return value;
    nm::Object out;
    for (const auto& key : value.as_object().keys()) out.set(key, decode(*value.as_object().find(key)));
    return nm::Value(std::move(out));
}

nm::Value error_record(const nm::JsString& name, const std::string& message) {
    nm::Object error;
    error.set(u"name", nm::Value(name));
    error.set(u"message", nm::Value(nm::utf8_to_js(message)));
    return nm::Value(std::move(error));
}

}  // namespace

// Replays JSON lines on one packer, as tools/reference-uniforms.mjs replays
// them on one WebGPUBackend: {op: 'reset'}; {op: 'struct', id, passUniforms,
// globalUniforms, program: {source, defines, uniformLayout}}; {op: 'single',
// id, value, typeDecl}. Writes {id, write} or {id, error: {name, message}} for
// each op but reset.
int uniforms(int, char**) {
    auto packer = std::make_unique<nm::backend::UniformPacker>();
    // The analysis of each distinct program, as compileProgram keeps it.
    std::unordered_map<std::string, nm::backend::ProgramUniforms> programs;
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;
        nm::Object record;
        try {
            const nm::Value op = nm::json::parse(line);
            const nm::Value kind = field(op, u"op");
            if (kind.is_string() && kind.as_string() == u"reset") {
                packer = std::make_unique<nm::backend::UniformPacker>();
                continue;
            }
            record.set(u"id", field(op, u"id"));
            if (kind.is_string() && kind.as_string() == u"struct") {
                const nm::Value program = field(op, u"program");
                const std::string key = nm::json::stringify(program);
                auto found = programs.find(key);
                if (found == programs.end()) {
                    const nm::Value defines = field(program, u"defines");
                    const nm::JsString processed = nm::wgsl::injectDefines(
                        field(program, u"source").as_string(), nm::jsv::truthy(defines) ? defines : nm::Value(nm::Object{}));
                    found = programs.emplace(key, nm::backend::ProgramUniforms::fromSource(
                                                      processed, field(program, u"uniformLayout"))).first;
                }
                record.set(u"write", written(packer->createUniformBuffer(decode_members(field(op, u"passUniforms")),
                                                                         decode_members(field(op, u"globalUniforms")),
                                                                         found->second)));
            } else {
                record.set(u"write", written(packer->createSingleUniformBuffer(decode(field(op, u"value")),
                                                                               field(op, u"typeDecl").as_string())));
            }
        } catch (const nm::JsError& error) {
            record.set(u"error", error_record(error.name(), nm::js_to_utf8(error.message())));
        } catch (const std::exception& error) {
            record.set(u"error", error_record(u"InternalError", error.what()));
        }
        std::cout << nm::json::stringify(nm::Value(std::move(record))) << '\n';
    }
    return 0;
}

}  // namespace nm_dump
