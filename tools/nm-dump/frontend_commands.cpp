#include "frontend_commands.h"

#include "core/lang/diagnostics.h"
#include "core/lang/effect_registry.h"
#include "core/lang/lexer.h"
#include "core/lang/parser.h"
#include "core/lang/validator.h"
#include "core/graph/dsl_compiler.h"
#include "core/graph/expander.h"
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
nm::Value error_record(const nm::JsString& name, const nm::JsString& message) {
    nm::Object error;
    error.set(u"name", nm::Value(name));
    error.set(u"message", nm::Value(message));
    return nm::Value(error);
}

std::string read_text(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot read " + path);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void plain_ast(nm::Value& value) {
    if (value.is_array()) {
        for (auto& item : value.as_array()) plain_ast(item);
    } else if (value.is_object()) {
        auto& object = value.as_object();
        object.erase(u"subchainArgumentDiagnostics");  // Reference JSON omits this hidden member.
        for (const auto& key : object.keys()) plain_ast(object[key]);
    }
}

void restore_ast_sidecars(nm::Value& isolated, const nm::Value& parsed) {
    if (isolated.is_array() && parsed.is_array()) {
        auto& target = isolated.as_array();
        const auto& source = parsed.as_array();
        for (std::size_t i = 0; i < std::min(target.size(), source.size()); ++i)
            restore_ast_sidecars(target[i], source[i]);
    } else if (isolated.is_object() && parsed.is_object()) {
        const nm::Value* diagnostics = parsed.as_object().find(u"subchainArgumentDiagnostics");
        if (diagnostics && !isolated.as_object().has(u"subchainArgumentDiagnostics"))
            isolated.as_object().set(u"subchainArgumentDiagnostics", *diagnostics);
        for (const auto& key : isolated.as_object().keys()) {
            const nm::Value* source = parsed.as_object().find(key);
            if (source) restore_ast_sidecars(isolated.as_object()[key], *source);
        }
    }
}

nm::EffectRegistry& registry_instance() {
    static nm::EffectRegistry registry;
    static const bool loaded = (registry.loadEmbedded(), true);
    (void)loaded;
    return registry;
}

nm::Value expansion_json(const nm::ExpandResult& expanded) {
    nm::Array passes;
    passes.reserve(expanded.passes.size());
    for (const auto& pass : expanded.passes) passes.push_back(nm::toRawPassJson(pass));
    nm::Object result;
    result.set(u"passes", nm::Value(std::move(passes)));
    result.set(u"errors", expanded.errors.raw());
    result.set(u"programs", expanded.programs.raw());
    result.set(u"textureSpecs", expanded.textureSpecs.raw());
    result.set(u"renderSurface", expanded.renderSurface.raw());
    result.set(u"mediaSteps", expanded.mediaSteps.raw());
    return nm::Value(result);
}

nm::Value normalized_graph(const nm::JsString& source) {
    nm::Object graph = nm::compileGraphJson(source, registry_instance());
    graph.erase(u"compiledAt");
    const nm::Value* raw_programs = graph.find(u"programs");
    nm::Object programs;
    if (raw_programs && raw_programs->is_object()) {
        for (const auto& key : raw_programs->as_object().keys()) {
            const nm::Value* raw = raw_programs->as_object().find(key);
            if (!raw || !raw->is_object()) continue;
            nm::Object spec = raw->as_object();
            for (const auto* name : {u"glsl", u"wgsl", u"vertex", u"fragment"}) spec.erase(name);
            programs.set(key, nm::Value(spec));
        }
    }
    graph.set(u"programs", nm::Value(programs));
    return nm::Value(graph);
}

nm::Value result_for(const std::string& stage, const nm::JsString& source,
                     const nm::Value* isolated) {
    if (stage == "graph") return normalized_graph(source);
    if (stage == "expanded") {
        const nm::Value validated = isolated ? *isolated : nm::validate(nm::parse(nm::lex(source)), registry_instance());
        return expansion_json(nm::expand(nm::JsObject(validated), registry_instance()));
    }
    if (stage == "validated") {
        nm::Value ast = isolated ? *isolated : nm::parse(nm::lex(source));
        if (isolated) restore_ast_sidecars(ast, nm::parse(nm::lex(source)));
        nm::Value validated = nm::validate(ast, registry_instance());
        plain_ast(validated);
        return validated;
    }
    const auto tokens = nm::lex(source);
    if (stage == "tokens") {
        nm::Array result;
        result.reserve(tokens.size());
        for (const auto& token : tokens) {
            nm::Value plain = nm::toJson(token);
            plain.as_object().erase(u"position");  // Reference defines this non-enumerably.
            result.push_back(std::move(plain));
        }
        return nm::Value(std::move(result));
    }
    if (stage == "ast") {
        nm::Value ast = nm::parse(tokens);
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
        stage != "expanded" && stage != "graph") {
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
    std::unordered_map<std::string, nm::Value> isolated_records;
    if (!isolated_path.empty()) {
        std::ifstream input(isolated_path, std::ios::binary);
        if (!input) { std::cerr << "nm-dump: cannot open " << isolated_path << "\n"; return 2; }
        std::string line;
        while (std::getline(input, line)) {
            if (line.empty()) continue;
            nm::Value entry = nm::json::parse(line);
            const nm::Value* name = entry.as_object().find(u"program");
            if (name && name->is_string()) isolated_records[nm::js_to_utf8(name->as_string())] = std::move(entry);
        }
    }
    for (const auto& path : files) {
        const std::string program = std::filesystem::path(path).stem().string();
        nm::Object record;
        record.set(u"program", nm::Value(nm::utf8_to_js(program)));
        record.set(u"stage", nm::Value(nm::utf8_to_js(stage)));
        try {
            const nm::Value* isolated = nullptr;
            if (!isolated_path.empty()) {
                const auto it = isolated_records.find(program);
                if (it == isolated_records.end()) throw std::runtime_error("missing isolated record");
                const nm::Value* error = it->second.as_object().find(u"error");
                if (error) {
                    record.set(u"error", *error);
                    output << nm::json::stringify(nm::Value(record)) << '\n';
                    continue;
                }
                isolated = it->second.as_object().find(u"result");
            }
            record.set(u"result", result_for(stage, nm::utf8_to_js(read_text(path)), isolated));
        } catch (const nm::DslSyntaxError& error) {
            record.set(u"error", error_record(u"SyntaxError", error.message()));
        } catch (const nm::CompilationError& error) {
            nm::Object thrown;
            thrown.set(u"code", nm::Value(error.code()));
            if (error.code() == u"ERR_COMPILATION_FAILED") thrown.set(u"diagnostics", error.diagnostics().raw());
            else thrown.set(u"errors", error.errors().raw());
            record.set(u"error", nm::Value(thrown));
        } catch (const std::exception& error) {
            const std::string message = error.what();
            const nm::JsString name = message.find("Cannot read properties of ") == 0 ||
                                      message == "plan.chain is not iterable" ? u"TypeError" : u"Error";
            record.set(u"error", error_record(name, nm::utf8_to_js(message)));
        }
        output << nm::json::stringify(nm::Value(record)) << '\n';
    }
    return 0;
}
}  // namespace nm_dump

namespace nm_dump {
int registry(int, char**) {
    nm::EffectRegistry catalog;
    catalog.loadEmbedded();
    std::cout << nm::json::stringify(catalog.dumpSummaryValue()) << '\n';
    return 0;
}
}
