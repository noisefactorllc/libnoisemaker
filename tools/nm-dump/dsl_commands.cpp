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
struct JsThrown { nm::Value value; };

nm::Value field(const nm::Value& base, const char16_t* key) {
    if (!base.is_object()) return {};
    const nm::Value* value = base.as_object().find(key);
    return value ? *value : nm::Value();
}

nm::Value tagged(const char16_t* name) {
    nm::Object out;
    out.set(u"$js", nm::Value(name));
    return nm::Value(out);
}

nm::Value encode(const nm::Value& value) {
    if (value.is_undefined()) return tagged(u"undefined");
    if (value.is_function()) {
        nm::Object out;
        out.set(u"$js", nm::Value(u"function"));
        out.set(u"source", nm::Value(value.as_string()));
        return nm::Value(out);
    }
    if (value.is_number()) {
        const double number = value.as_number();
        if (std::isnan(number)) return tagged(u"NaN");
        if (std::isinf(number)) return tagged(number < 0 ? u"-Infinity" : u"Infinity");
        if (number == 0 && std::signbit(number)) return tagged(u"-0");
    }
    if (value.is_array()) {
        nm::Array out;
        for (const auto& item : value.as_array()) out.push_back(encode(item));
        return nm::Value(out);
    }
    if (value.is_object()) {
        nm::Object out;
        for (const auto& key : value.as_object().keys()) {
            out.set(key, encode(*value.as_object().find(key)));
        }
        if (out.find(u"$js")) {
            nm::Object wrapper;
            wrapper.set(u"$js", nm::Value(u"object"));
            wrapper.set(u"members", nm::Value(out));
            return nm::Value(wrapper);
        }
        return nm::Value(out);
    }
    return value;
}

nm::Value decode(const nm::Value& value) {
    if (value.is_array()) {
        nm::Array result;
        for (const auto& item : value.as_array()) result.push_back(decode(item));
        return nm::Value(result);
    }
    if (!value.is_object()) return value;
    const nm::Value tag = field(value, u"$js");
    if (tag.is_string()) {
        const auto& name = tag.as_string();
        if (name == u"undefined") return {};
        if (name == u"NaN") return nm::Value(std::numeric_limits<double>::quiet_NaN());
        if (name == u"Infinity") return nm::Value(std::numeric_limits<double>::infinity());
        if (name == u"-Infinity") return nm::Value(-std::numeric_limits<double>::infinity());
        if (name == u"-0") return nm::Value(-0.0);
        if (name == u"function") {
            const nm::Value source = field(value, u"source");
            return nm::Value::function(source.is_string() ? source.as_string() : nm::JsString());
        }
        if (name == u"Float32Array" || name == u"Float64Array") {
            const nm::Value values = field(value, u"values");
            return values.is_array() ? decode(values) : nm::Value(nm::Array{});
        }
        if (name == u"Map") return nm::Value(nm::Object{});
        if (name == u"object") {
            const nm::Value members = field(value, u"members");
            return members.is_object() ? decode(members) : nm::Value(nm::Object{});
        }
        if (name == u"error") {
            nm::Object error;
            error.set(u"name", field(value, u"name"));
            error.set(u"message", field(value, u"message"));
            return nm::Value(error);
        }
        if (name == u"effectInstance") {
            const nm::Value props = field(value, u"props");
            return props.is_object() ? decode(props) : nm::Value(nm::Object{});
        }
        if (name == u"effectSubclass") return nm::Value::function(u"class extends Effect {}");
    }
    nm::Object result;
    for (const auto& key : value.as_object().keys()) {
        result.set(key, decode(*value.as_object().find(key)));
    }
    return nm::Value(result);
}

nm::EffectRegistry& registry_instance() {
    static nm::EffectRegistry registry;
    static const bool loaded = (registry.loadEmbedded(), true);
    (void)loaded;
    return registry;
}

nm::Value run_case(const nm::Value& item) {
    const nm::Value op = field(item, u"op");
    if (!op.is_string()) throw std::runtime_error("case has no op");
    nm::EffectRegistry& registry = registry_instance();
    const nm::UnparseOptions options = nm::UnparseOptions::from_value(
        decode(field(item, u"options")), &registry);
    if (op.as_string() == u"formatValue") {
        return nm::format_value(decode(field(item, u"value")),
                                decode(field(item, u"spec")), options,
                                decode(field(item, u"sourceForm")));
    }
    if (op.as_string() == u"unparseCall") {
        return nm::Value(nm::unparse_call(decode(field(item, u"call")), options));
    }
    if (op.as_string() == u"unparseChain") {
        return nm::Value(nm::unparse_chain(decode(field(item, u"chain")), options));
    }
    if (op.as_string() == u"unparse") {
        return nm::Value(nm::unparse(decode(field(item, u"compiled")),
                                     decode(field(item, u"overrides")), options, registry));
    }
    if (op.as_string() == u"listSteps") {
        return nm::list_steps(decode(field(item, u"compiled")),
                              decode(field(item, u"options")), registry);
    }
    if (op.as_string() == u"replaceEffect") {
        return nm::replace_effect(decode(field(item, u"compiled")),
                                  decode(field(item, u"stepIndex")),
                                  decode(field(item, u"newEffectName")),
                                  decode(field(item, u"newArgs")),
                                  decode(field(item, u"options")), registry);
    }
    if (op.as_string() == u"replaceEffectUnparse") {
        const nm::Value result = nm::replace_effect(decode(field(item, u"compiled")),
            decode(field(item, u"stepIndex")), decode(field(item, u"newEffectName")),
            decode(field(item, u"newArgs")), decode(field(item, u"options")), registry);
        const nm::Value success = field(result, u"success");
        nm::Object output;
        output.set(u"success", success);
        output.set(u"error", field(result, u"error"));
        if (success.is_bool() && success.as_bool()) {
            const auto unparse_options = nm::UnparseOptions::from_value(
                decode(field(item, u"unparseOptions")), &registry);
            output.set(u"dsl", nm::Value(nm::unparse(field(result, u"program"), nm::Value(nm::Object{}),
                                                   unparse_options, registry)));
        } else {
            output.set(u"dsl", nm::Value());
        }
        return nm::Value(output);
    }
    if (op.as_string() == u"getCompatibleReplacements") {
        return nm::get_compatible_replacements(decode(field(item, u"compiled")),
                                               decode(field(item, u"stepIndex")),
                                               decode(field(item, u"options")), registry);
    }
    if (op.as_string() == u"predictReplacement") {
        return nm::predict_replacement(decode(field(item, u"resolvedName")),
                                       decode(field(item, u"spec")),
                                       decode(field(item, u"newArgs")),
                                       decode(field(item, u"oldInstance")),
                                       decode(field(item, u"options")), registry);
    }
    if (op.as_string() == u"applyParameterUpdates") {
        const nm::Value compile_error = field(item, u"compileError");
        if (!compile_error.is_undefined()) {
            if (field(compile_error, u"$js").is_string() &&
                field(compile_error, u"$js").as_string() == u"error") {
                throw nm::JsError(nm::jsv::to_string(field(compile_error, u"name")),
                                  nm::jsv::to_string(field(compile_error, u"message")));
            }
            throw JsThrown{decode(compile_error)};
        }
        return nm::Value(nm::apply_parameter_updates(
            nm::jsv::to_string(decode(field(item, u"dsl"))), decode(field(item, u"compiled")),
            decode(field(item, u"updates")), registry));
    }
    if (op.as_string() == u"validateEffectDefinition") {
        nm::Array errors;
        const nm::Value definition = field(item, u"definition");
        const nm::Value definition_tag = field(definition, u"$js");
        nm::Value input = decode(definition);
        if (definition_tag.is_string() && (definition_tag.as_string() == u"effectInstance" ||
                                           definition_tag.as_string() == u"effectSubclass")) {
            nm::Object wrapper = definition.as_object();
            if (definition_tag.as_string() == u"effectInstance")
                wrapper.set(u"props", decode(field(definition, u"props")));
            else wrapper.set(u"statics", decode(field(definition, u"statics")));
            input = nm::Value(wrapper);
        }
        for (const auto& message : nm::validateEffectDefinition(input)) {
            errors.emplace_back(nm::utf8_to_js(message));
        }
        return nm::Value(std::move(errors));
    }
    if (op.as_string() == u"formatDslError") {
        nm::Value error = field(item, u"error");
        const nm::Value tag = field(error, u"$js");
        if (tag.is_string() && tag.as_string() != u"error") error = decode(error);
        return nm::Value(nm::format_dsl_error(decode(field(item, u"source")),
                                              error,
                                              decode(field(item, u"options"))));
    }
    if (op.as_string() == u"isDslSyntaxError") {
        return nm::Value(nm::is_dsl_syntax_error(field(item, u"error")));
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
        const nm::Value item = nm::json::parse(line);
        const nm::Value setup = field(item, u"setup");
        if (!setup.is_undefined()) {
            if (setup.is_array()) {
                for (const auto& action : setup.as_array()) registry_instance().applySetup(decode(action));
            }
            continue;
        }
        nm::Object record;
        record.set(u"id", field(item, u"id"));
        record.set(u"category", field(item, u"category"));
        try {
            record.set(u"result", encode(run_case(item)));
        } catch (const JsThrown& error) {
            nm::Object fault;
            fault.set(u"thrown", encode(error.value));
            record.set(u"error", nm::Value(fault));
        } catch (const nm::JsError& error) {
            nm::Object fault;
            fault.set(u"name", nm::Value(error.name()));
            fault.set(u"message", nm::Value(error.message()));
            record.set(u"error", nm::Value(fault));
        } catch (const std::exception& error) {
            nm::Object fault;
            const std::string message = error.what();
            fault.set(u"name", nm::Value(message == "Invalid string length" ? u"RangeError" : u"TypeError"));
            fault.set(u"message", nm::Value(nm::utf8_to_js(message)));
            record.set(u"error", nm::Value(fault));
        }
        output << nm::json::stringify(nm::Value(record)) << '\n';
    }
    return 0;
}
}  // namespace nm_dump
