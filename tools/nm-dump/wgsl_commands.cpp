#include "wgsl_commands.h"

#include "core/value/json.h"
#include "gpu/wgsl/wgsl.h"

#include <exception>
#include <iostream>
#include <string>

namespace nm_dump {

namespace {

namespace nm = libnoisemaker;

// Every analysis of one case, in the shape tools/reference-wgsl.mjs writes.
nm::Value analyze(const nm::Value& testCase) {
    const nm::Value* sourceValue = testCase.as_object().find(u"source");
    if (!sourceValue || !sourceValue->is_string()) throw std::invalid_argument("case without a source string");
    nm::JsString source = sourceValue->as_string();
    nm::Object out;
    if (const nm::Value* defines = testCase.as_object().find(u"defines")) {
        source = nm::wgsl::injectDefines(source, *defines);
        out.set(u"injected", nm::Value(source));
    }
    const auto bindings = nm::wgsl::parseShaderBindings(source);
    out.set(u"stripped", nm::Value(nm::wgsl::stripComments(source)));
    out.set(u"entryPoints", nm::wgsl::toValue(nm::wgsl::detectEntryPoints(source)));
    nm::Array compute;
    for (const auto& name : nm::wgsl::computeEntryPoints(source)) compute.push_back(nm::Value(name));
    out.set(u"computeEntryPoints", nm::Value(std::move(compute)));
    out.set(u"hasCompute", nm::Value(nm::wgsl::hasComputeEntry(source)));
    out.set(u"hasFragment", nm::Value(nm::wgsl::hasFragmentEntry(source)));
    out.set(u"hasVertex", nm::Value(nm::wgsl::hasVertexEntry(source)));
    out.set(u"bindings", nm::wgsl::toValue(bindings));
    out.set(u"entryPointBindings", nm::wgsl::toValue(nm::wgsl::parseEntryPointBindings(source, bindings)));
    out.set(u"declaredUniformBufferSize", nm::Value(nm::wgsl::parseDeclaredUniformBufferSize(source)));
    out.set(u"packedUniformLayout", nm::wgsl::toValue(nm::wgsl::parsePackedUniformLayout(source)));
    out.set(u"samplerTextures", nm::wgsl::toValue(nm::wgsl::parseSamplerTextureUses(source)));
    out.set(u"hasBindings", nm::Value(nm::wgsl::hasShaderBindings(source)));
    return nm::Value(std::move(out));
}

}  // namespace

// Reads JSON lines {id, source, defines?} and writes {id, result} or {id, error}.
int wgsl(int, char**) {
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;
        nm::Object record;
        try {
            const nm::Value testCase = nm::json::parse(line);
            if (const nm::Value* id = testCase.as_object().find(u"id")) record.set(u"id", *id);
            record.set(u"result", analyze(testCase));
        } catch (const std::exception& error) {
            record.set(u"error", nm::Value(nm::utf8_to_js(error.what())));
        }
        std::cout << nm::json::stringify(nm::Value(std::move(record))) << '\n';
    }
    return 0;
}

}  // namespace nm_dump
