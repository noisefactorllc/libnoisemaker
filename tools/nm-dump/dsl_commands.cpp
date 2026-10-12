#include "dsl_commands.h"

#include "core/edit/error_formatter.h"
#include "core/edit/transform.h"
#include "core/edit/unparser.h"
#include "core/lang/effect_validator.h"
#include "core/value/json.h"

#include <cmath>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace nm_dump {
namespace {
struct JsThrown { libnoisemaker::Value value; };

libnoisemaker::Value field(const libnoisemaker::Value& base, const char16_t* key) {
    if (!base.is_object()) return {};
    const libnoisemaker::Value* value = base.as_object().find(key);
    return value ? *value : libnoisemaker::Value();
}

libnoisemaker::Value tagged(const char16_t* name) {
    libnoisemaker::Object out;
    out.set(u"$js", libnoisemaker::Value(name));
    return libnoisemaker::Value(out);
}

libnoisemaker::Value encode(const libnoisemaker::Value& value) {
    if (value.is_undefined()) return tagged(u"undefined");
    if (value.is_function()) {
        libnoisemaker::Object out;
        out.set(u"$js", libnoisemaker::Value(u"function"));
        out.set(u"source", libnoisemaker::Value(value.as_string()));
        return libnoisemaker::Value(out);
    }
    if (value.is_number()) {
        const double number = value.as_number();
        if (std::isnan(number)) return tagged(u"NaN");
        if (std::isinf(number)) return tagged(number < 0 ? u"-Infinity" : u"Infinity");
        if (number == 0 && std::signbit(number)) return tagged(u"-0");
    }
    if (value.is_array()) {
        libnoisemaker::Array out;
        for (const auto& item : value.as_array()) out.push_back(encode(item));
        return libnoisemaker::Value(out);
    }
    if (value.is_object()) {
        libnoisemaker::Object out;
        for (const auto& key : value.as_object().keys()) {
            out.set(key, encode(*value.as_object().find(key)));
        }
        if (out.find(u"$js")) {
            libnoisemaker::Object wrapper;
            wrapper.set(u"$js", libnoisemaker::Value(u"object"));
            wrapper.set(u"members", libnoisemaker::Value(out));
            return libnoisemaker::Value(wrapper);
        }
        return libnoisemaker::Value(out);
    }
    return value;
}

libnoisemaker::Value decode(const libnoisemaker::Value& value) {
    if (value.is_array()) {
        libnoisemaker::Array result;
        for (const auto& item : value.as_array()) result.push_back(decode(item));
        return libnoisemaker::Value(result);
    }
    if (!value.is_object()) return value;
    const libnoisemaker::Value tag = field(value, u"$js");
    if (tag.is_string()) {
        const auto& name = tag.as_string();
        if (name == u"undefined") return {};
        if (name == u"NaN") return libnoisemaker::Value(std::numeric_limits<double>::quiet_NaN());
        if (name == u"Infinity") return libnoisemaker::Value(std::numeric_limits<double>::infinity());
        if (name == u"-Infinity") return libnoisemaker::Value(-std::numeric_limits<double>::infinity());
        if (name == u"-0") return libnoisemaker::Value(-0.0);
        if (name == u"function") {
            const libnoisemaker::Value source = field(value, u"source");
            return libnoisemaker::Value::function(source.is_string() ? source.as_string() : libnoisemaker::JsString());
        }
        if (name == u"Float32Array" || name == u"Float64Array") {
            const libnoisemaker::Value values = field(value, u"values");
            return values.is_array() ? decode(values) : libnoisemaker::Value(libnoisemaker::Array{});
        }
        if (name == u"Map") return libnoisemaker::Value(libnoisemaker::Object{});
        if (name == u"object") {
            const libnoisemaker::Value members = field(value, u"members");
            return members.is_object() ? decode(members) : libnoisemaker::Value(libnoisemaker::Object{});
        }
        if (name == u"error") {
            libnoisemaker::Object error;
            error.set(u"name", field(value, u"name"));
            error.set(u"message", field(value, u"message"));
            return libnoisemaker::Value(error);
        }
        if (name == u"effectInstance") {
            const libnoisemaker::Value props = field(value, u"props");
            return props.is_object() ? decode(props) : libnoisemaker::Value(libnoisemaker::Object{});
        }
        if (name == u"effectSubclass") return libnoisemaker::Value::function(u"class extends Effect {}");
    }
    libnoisemaker::Object result;
    for (const auto& key : value.as_object().keys()) {
        result.set(key, decode(*value.as_object().find(key)));
    }
    return libnoisemaker::Value(result);
}

libnoisemaker::EffectRegistry& registry_instance() {
    static libnoisemaker::EffectRegistry registry;
    static const bool loaded = (registry.loadEmbedded(), true);
    (void)loaded;
    return registry;
}

libnoisemaker::Value run_case(const libnoisemaker::Value& item) {
    const libnoisemaker::Value op = field(item, u"op");
    if (!op.is_string()) throw std::runtime_error("case has no op");
    libnoisemaker::EffectRegistry& registry = registry_instance();
    const libnoisemaker::UnparseOptions options = libnoisemaker::UnparseOptions::from_value(
        decode(field(item, u"options")), &registry);
    if (op.as_string() == u"formatValue") {
        return libnoisemaker::format_value(decode(field(item, u"value")),
                                decode(field(item, u"spec")), options,
                                decode(field(item, u"sourceForm")));
    }
    if (op.as_string() == u"unparseCall") {
        return libnoisemaker::Value(libnoisemaker::unparse_call(decode(field(item, u"call")), options));
    }
    if (op.as_string() == u"unparseChain") {
        return libnoisemaker::Value(libnoisemaker::unparse_chain(decode(field(item, u"chain")), options));
    }
    if (op.as_string() == u"unparse") {
        return libnoisemaker::Value(libnoisemaker::unparse(decode(field(item, u"compiled")),
                                     decode(field(item, u"overrides")), options, registry));
    }
    if (op.as_string() == u"listSteps") {
        return libnoisemaker::list_steps(decode(field(item, u"compiled")),
                              decode(field(item, u"options")), registry);
    }
    if (op.as_string() == u"replaceEffect") {
        return libnoisemaker::replace_effect(decode(field(item, u"compiled")),
                                  decode(field(item, u"stepIndex")),
                                  decode(field(item, u"newEffectName")),
                                  decode(field(item, u"newArgs")),
                                  decode(field(item, u"options")), registry);
    }
    if (op.as_string() == u"replaceEffectUnparse") {
        const libnoisemaker::Value result = libnoisemaker::replace_effect(decode(field(item, u"compiled")),
            decode(field(item, u"stepIndex")), decode(field(item, u"newEffectName")),
            decode(field(item, u"newArgs")), decode(field(item, u"options")), registry);
        const libnoisemaker::Value success = field(result, u"success");
        libnoisemaker::Object output;
        output.set(u"success", success);
        output.set(u"error", field(result, u"error"));
        if (success.is_bool() && success.as_bool()) {
            const auto unparse_options = libnoisemaker::UnparseOptions::from_value(
                decode(field(item, u"unparseOptions")), &registry);
            output.set(u"dsl", libnoisemaker::Value(libnoisemaker::unparse(field(result, u"program"), libnoisemaker::Value(libnoisemaker::Object{}),
                                                   unparse_options, registry)));
        } else {
            output.set(u"dsl", libnoisemaker::Value());
        }
        return libnoisemaker::Value(output);
    }
    if (op.as_string() == u"getCompatibleReplacements") {
        return libnoisemaker::get_compatible_replacements(decode(field(item, u"compiled")),
                                               decode(field(item, u"stepIndex")),
                                               decode(field(item, u"options")), registry);
    }
    if (op.as_string() == u"predictReplacement") {
        return libnoisemaker::predict_replacement(decode(field(item, u"resolvedName")),
                                       decode(field(item, u"spec")),
                                       decode(field(item, u"newArgs")),
                                       decode(field(item, u"oldInstance")),
                                       decode(field(item, u"options")), registry);
    }
    if (op.as_string() == u"applyParameterUpdates") {
        const libnoisemaker::Value compile_error = field(item, u"compileError");
        if (!compile_error.is_undefined()) {
            if (field(compile_error, u"$js").is_string() &&
                field(compile_error, u"$js").as_string() == u"error") {
                throw libnoisemaker::JsError(libnoisemaker::jsv::to_string(field(compile_error, u"name")),
                                  libnoisemaker::jsv::to_string(field(compile_error, u"message")));
            }
            throw JsThrown{decode(compile_error)};
        }
        return libnoisemaker::Value(libnoisemaker::apply_parameter_updates(
            libnoisemaker::jsv::to_string(decode(field(item, u"dsl"))), decode(field(item, u"compiled")),
            decode(field(item, u"updates")), registry));
    }
    if (op.as_string() == u"validateEffectDefinition") {
        libnoisemaker::Array errors;
        const libnoisemaker::Value definition = field(item, u"definition");
        const libnoisemaker::Value definition_tag = field(definition, u"$js");
        libnoisemaker::Value input = decode(definition);
        if (definition_tag.is_string() && (definition_tag.as_string() == u"effectInstance" ||
                                           definition_tag.as_string() == u"effectSubclass")) {
            libnoisemaker::Object wrapper = definition.as_object();
            if (definition_tag.as_string() == u"effectInstance")
                wrapper.set(u"props", decode(field(definition, u"props")));
            else wrapper.set(u"statics", decode(field(definition, u"statics")));
            input = libnoisemaker::Value(wrapper);
        }
        for (const auto& message : libnoisemaker::validateEffectDefinition(input)) {
            errors.emplace_back(libnoisemaker::utf8_to_js(message));
        }
        return libnoisemaker::Value(std::move(errors));
    }
    if (op.as_string() == u"formatDslError") {
        libnoisemaker::Value error = field(item, u"error");
        const libnoisemaker::Value tag = field(error, u"$js");
        if (tag.is_string() && tag.as_string() != u"error") error = decode(error);
        return libnoisemaker::Value(libnoisemaker::format_dsl_error(decode(field(item, u"source")),
                                              error,
                                              decode(field(item, u"options"))));
    }
    if (op.as_string() == u"isDslSyntaxError") {
        return libnoisemaker::Value(libnoisemaker::is_dsl_syntax_error(field(item, u"error")));
    }
    throw std::runtime_error("not implemented");
}
}  // namespace

int cases(int argc, char** argv) {
    if (argc < 1) { std::cerr << "usage: nm-dump cases RESOLVED --out CANDIDATE\n"; return 2; }
    std::string output_path;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--out" && i + 1 < argc) output_path = argv[++i];
    }
    if (output_path.empty()) { std::cerr << "nm-dump cases: --out is required\n"; return 2; }
    std::ifstream input(argv[0], std::ios::binary);
    std::ofstream output(output_path, std::ios::binary);
    if (!input || !output) { std::cerr << "nm-dump cases: cannot open input/output\n"; return 2; }
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty()) continue;
        const libnoisemaker::Value item = libnoisemaker::json::parse(line);
        const libnoisemaker::Value setup = field(item, u"setup");
        if (!setup.is_undefined()) {
            if (setup.is_array()) {
                for (const auto& action : setup.as_array()) registry_instance().applySetup(decode(action));
            }
            continue;
        }
        libnoisemaker::Object record;
        record.set(u"id", field(item, u"id"));
        record.set(u"category", field(item, u"category"));
        try {
            record.set(u"result", encode(run_case(item)));
        } catch (const JsThrown& error) {
            libnoisemaker::Object fault;
            fault.set(u"thrown", encode(error.value));
            record.set(u"error", libnoisemaker::Value(fault));
        } catch (const libnoisemaker::JsError& error) {
            libnoisemaker::Object fault;
            fault.set(u"name", libnoisemaker::Value(error.name()));
            fault.set(u"message", libnoisemaker::Value(error.message()));
            record.set(u"error", libnoisemaker::Value(fault));
        } catch (const std::exception& error) {
            libnoisemaker::Object fault;
            const std::string message = error.what();
            fault.set(u"name", libnoisemaker::Value(message == "Invalid string length" ? u"RangeError" : u"TypeError"));
            fault.set(u"message", libnoisemaker::Value(libnoisemaker::utf8_to_js(message)));
            record.set(u"error", libnoisemaker::Value(fault));
        }
        output << libnoisemaker::json::stringify(libnoisemaker::Value(record)) << '\n';
    }
    return 0;
}
}  // namespace nm_dump
