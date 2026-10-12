#include "frontend_commands.h"

#include "core/lang/diagnostics.h"
#include "core/lang/effect_registry.h"
#include "core/lang/lexer.h"
#include "core/lang/parser.h"
#include "core/lang/validator.h"
#include "core/graph/dsl_compiler.h"
#include "core/graph/expander.h"
#include "core/graph/normalize.h"
#include "core/value/json.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace nm_dump {
namespace {
libnoisemaker::Value error_record(const libnoisemaker::JsString& name, const libnoisemaker::JsString& message) {
    libnoisemaker::Object error;
    error.set(u"name", libnoisemaker::Value(name));
    error.set(u"message", libnoisemaker::Value(message));
    return libnoisemaker::Value(error);
}

std::string read_text(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot read " + path);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void plain_ast(libnoisemaker::Value& value) {
    if (value.is_array()) {
        for (auto& item : value.as_array()) plain_ast(item);
    } else if (value.is_object()) {
        auto& object = value.as_object();
        object.erase(u"subchainArgumentDiagnostics");  // Reference JSON omits this hidden member.
        for (const auto& key : object.keys()) plain_ast(object[key]);
    }
}

void restore_ast_sidecars(libnoisemaker::Value& isolated, const libnoisemaker::Value& parsed) {
    if (isolated.is_array() && parsed.is_array()) {
        auto& target = isolated.as_array();
        const auto& source = parsed.as_array();
        for (std::size_t i = 0; i < std::min(target.size(), source.size()); ++i)
            restore_ast_sidecars(target[i], source[i]);
    } else if (isolated.is_object() && parsed.is_object()) {
        const libnoisemaker::Value* diagnostics = parsed.as_object().find(u"subchainArgumentDiagnostics");
        if (diagnostics && !isolated.as_object().has(u"subchainArgumentDiagnostics"))
            isolated.as_object().set(u"subchainArgumentDiagnostics", *diagnostics);
        for (const auto& key : isolated.as_object().keys()) {
            const libnoisemaker::Value* source = parsed.as_object().find(key);
            if (source) restore_ast_sidecars(isolated.as_object()[key], *source);
        }
    }
}

libnoisemaker::EffectRegistry& registry_instance() {
    static libnoisemaker::EffectRegistry registry;
    static const bool loaded = (registry.loadEmbedded(), true);
    (void)loaded;
    return registry;
}

libnoisemaker::Value expansion_json(const libnoisemaker::ExpandResult& expanded) {
    libnoisemaker::Array passes;
    passes.reserve(expanded.passes.size());
    for (const auto& pass : expanded.passes) passes.push_back(libnoisemaker::toRawPassJson(pass));
    libnoisemaker::Object result;
    result.set(u"passes", libnoisemaker::Value(std::move(passes)));
    result.set(u"errors", expanded.errors.raw());
    result.set(u"programs", expanded.programs.raw());
    result.set(u"textureSpecs", expanded.textureSpecs.raw());
    result.set(u"renderSurface", expanded.renderSurface.raw());
    result.set(u"mediaSteps", expanded.mediaSteps.raw());
    return libnoisemaker::Value(result);
}

libnoisemaker::Value normalized_graph(const libnoisemaker::JsString& source) {
    libnoisemaker::Object graph = libnoisemaker::compileGraphJson(source, registry_instance());
    graph.erase(u"compiledAt");
    const libnoisemaker::Value* raw_programs = graph.find(u"programs");
    libnoisemaker::Object programs;
    if (raw_programs && raw_programs->is_object()) {
        for (const auto& key : raw_programs->as_object().keys()) {
            const libnoisemaker::Value* raw = raw_programs->as_object().find(key);
            if (!raw || !raw->is_object()) continue;
            libnoisemaker::Object spec = raw->as_object();
            for (const auto* name : {u"glsl", u"wgsl", u"vertex", u"fragment"}) spec.erase(name);
            programs.set(key, libnoisemaker::Value(spec));
        }
    }
    graph.set(u"programs", libnoisemaker::Value(programs));
    return libnoisemaker::Value(graph);
}

libnoisemaker::Value result_for(const std::string& stage, const libnoisemaker::JsString& source,
                     const libnoisemaker::Value* isolated) {
    if (stage == "graph") return normalized_graph(source);
    if (stage == "graph-normalized") {
        const libnoisemaker::Object graph = libnoisemaker::compileGraphJson(source, registry_instance());
        return libnoisemaker::normalizeGraphJson(libnoisemaker::Value(graph), registry_instance().defineMap().raw());
    }
    if (stage == "expanded") {
        const libnoisemaker::Value validated = isolated ? *isolated : libnoisemaker::validate(libnoisemaker::parse(libnoisemaker::lex(source)), registry_instance());
        return expansion_json(libnoisemaker::expand(libnoisemaker::JsObject(validated), registry_instance()));
    }
    if (stage == "validated") {
        libnoisemaker::Value ast = isolated ? *isolated : libnoisemaker::parse(libnoisemaker::lex(source));
        if (isolated) restore_ast_sidecars(ast, libnoisemaker::parse(libnoisemaker::lex(source)));
        libnoisemaker::Value validated = libnoisemaker::validate(ast, registry_instance());
        plain_ast(validated);
        return validated;
    }
    const auto tokens = libnoisemaker::lex(source);
    if (stage == "tokens") {
        libnoisemaker::Array result;
        result.reserve(tokens.size());
        for (const auto& token : tokens) {
            libnoisemaker::Value plain = libnoisemaker::toJson(token);
            plain.as_object().erase(u"position");  // Reference defines this non-enumerably.
            result.push_back(std::move(plain));
        }
        return libnoisemaker::Value(std::move(result));
    }
    if (stage == "ast") {
        libnoisemaker::Value ast = libnoisemaker::parse(tokens);
        plain_ast(ast);
        return ast;
    }
    throw std::invalid_argument("unsupported frontend stage: " + stage);
}
}  // namespace

int dump(int argc, char** argv) {
    if (argc < 1) {
        std::cerr << "usage: nm-dump dump STAGE --out FILE files...\n";
        return 2;
    }
    const std::string stage = argv[0];
    if (stage != "tokens" && stage != "ast" && stage != "validated" &&
        stage != "expanded" && stage != "graph" && stage != "graph-normalized") {
        std::cerr << "nm-dump: stage " << stage << " is not implemented\n";
        return 2;
    }
    std::string out;
    std::string isolated_path;
    std::vector<std::string> files;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--out" && i + 1 < argc) out = argv[++i];
        else if (arg == "--isolated" && i + 1 < argc) isolated_path = argv[++i];
        else files.push_back(arg);
    }
    if (out.empty()) { std::cerr << "nm-dump: --out is required\n"; return 2; }
    std::ofstream output(out, std::ios::binary);
    if (!output) { std::cerr << "nm-dump: cannot open " << out << "\n"; return 2; }
    std::unordered_map<std::string, libnoisemaker::Value> isolated_records;
    if (!isolated_path.empty()) {
        std::ifstream input(isolated_path, std::ios::binary);
        if (!input) { std::cerr << "nm-dump: cannot open " << isolated_path << "\n"; return 2; }
        std::string line;
        while (std::getline(input, line)) {
            if (line.empty()) continue;
            libnoisemaker::Value entry = libnoisemaker::json::parse(line);
            const libnoisemaker::Value* name = entry.as_object().find(u"program");
            if (name && name->is_string()) isolated_records[libnoisemaker::js_to_utf8(name->as_string())] = std::move(entry);
        }
    }
    for (const auto& path : files) {
        const std::string program = std::filesystem::path(path).stem().string();
        libnoisemaker::Object record;
        record.set(u"program", libnoisemaker::Value(libnoisemaker::utf8_to_js(program)));
        record.set(u"stage", libnoisemaker::Value(libnoisemaker::utf8_to_js(stage)));
        try {
            const libnoisemaker::Value* isolated = nullptr;
            if (!isolated_path.empty()) {
                const auto it = isolated_records.find(program);
                if (it == isolated_records.end()) throw std::runtime_error("missing isolated record");
                const libnoisemaker::Value* error = it->second.as_object().find(u"error");
                if (error) {
                    record.set(u"error", *error);
                    output << libnoisemaker::json::stringify(libnoisemaker::Value(record)) << '\n';
                    continue;
                }
                isolated = it->second.as_object().find(u"result");
            }
            record.set(u"result", result_for(stage, libnoisemaker::utf8_to_js(read_text(path)), isolated));
        } catch (const libnoisemaker::DslSyntaxError& error) {
            record.set(u"error", error_record(u"SyntaxError", error.message()));
        } catch (const libnoisemaker::CompilationError& error) {
            libnoisemaker::Object thrown;
            thrown.set(u"code", libnoisemaker::Value(error.code()));
            if (error.code() == u"ERR_COMPILATION_FAILED") thrown.set(u"diagnostics", error.diagnostics().raw());
            else thrown.set(u"errors", error.errors().raw());
            record.set(u"error", libnoisemaker::Value(thrown));
        } catch (const std::exception& error) {
            const std::string message = error.what();
            const libnoisemaker::JsString name = message.find("Cannot read properties of ") == 0 ||
                                      message == "plan.chain is not iterable" ? u"TypeError" : u"Error";
            record.set(u"error", error_record(name, libnoisemaker::utf8_to_js(message)));
        }
        output << libnoisemaker::json::stringify(libnoisemaker::Value(record)) << '\n';
    }
    return 0;
}
}  // namespace nm_dump

namespace nm_dump {
int registry(int, char**) {
    libnoisemaker::EffectRegistry catalog;
    catalog.loadEmbedded();
    std::cout << libnoisemaker::json::stringify(catalog.dumpSummaryValue()) << '\n';
    return 0;
}
}
