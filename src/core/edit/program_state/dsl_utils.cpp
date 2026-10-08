#include "core/edit/program_state/dsl_utils.h"

#include "core/edit/program_state/console.h"
#include "core/edit/error_formatter.h"
#include "core/edit/jsv.h"
#include "core/lang/diagnostics.h"
#include "core/lang/lexer.h"
#include "core/lang/parser.h"
#include "core/lang/validator.h"

namespace nm {
namespace {
Value property(const Value& value, const JsString& key) { return jsv::member(value, key); }
bool automation(const Value& value) {
    if (!value.is_object() && !value.is_array()) return false;
    auto is_kind = [](const Value& type) {
        return type.is_string() && (type.as_string() == u"Oscillator" ||
                                    type.as_string() == u"Midi" || type.as_string() == u"Audio");
    };
    return is_kind(property(value, u"type")) || is_kind(property(property(value, u"_ast"), u"type"));
}
Value error_arg(const JsString& name, const JsString& message) {
    Object tagged;
    tagged.set(u"$js", Value(u"error"));
    tagged.set(u"name", Value(name));
    tagged.set(u"message", Value(message));
    return Value(std::move(tagged));
}
}  // namespace

Value compile_state_dsl(const JsString& dsl, EffectRegistry& registry) {
    return nm::validate(JsObject(nm::parse(nm::lex(dsl))), registry).raw();
}

Value extract_effects_from_dsl(const JsString& dsl, EffectRegistry& registry) {
    Array effects;
    if (dsl.empty()) return Value(std::move(effects));
    try {
        const Value compiled = compile_state_dsl(dsl, registry);
        const Value plans = property(compiled, u"plans");
        if (!jsv::truthy(plans)) return Value(std::move(effects));
        std::size_t global_index = 0;
        for (const Value& plan : jsv::iterate(plans, u"result.plans")) {
            const Value chain = jsv::get(plan, u"chain");
            if (!jsv::truthy(chain)) continue;
            for (const Value& step : jsv::iterate(chain, u"plan.chain")) {
                const Value full_op = jsv::get(step, u"op");
                if (!full_op.is_string()) throw jsv::not_a_function(u"fullOpName.includes");
                const JsString& full = full_op.as_string();
                const auto dot = full.rfind(u'.');
                const JsString short_name = dot == JsString::npos ? full : full.substr(dot + 1);
                const Value ns = jsv::get(step, u"namespace");
                Value ns_value = property(ns, u"namespace");
                if (!jsv::truthy(ns_value)) ns_value = property(ns, u"resolved");
                if (!jsv::truthy(ns_value)) ns_value = Value::null();
                const Value raw_kwargs = jsv::get(step, u"rawKwargs");
                const Value raw_args = jsv::truthy(raw_kwargs) ? raw_kwargs : Value(Object{});
                Object args;
                const Value step_args = jsv::get(step, u"args");
                if (jsv::truthy(step_args)) jsv::spread_into(args, step_args);
                for (const auto& [param_name, raw_value] : jsv::entries(raw_args)) {
                    const Value* current = args.find(param_name);
                    if (automation(raw_value) && (!current || !automation(*current))) {
                        jsv::set_plain(args, param_name, raw_value);
                    }
                }
                Object info;
                info.set(u"effectKey", Value(full));
                info.set(u"namespace", ns_value);
                info.set(u"name", Value(short_name));
                info.set(u"fullName", Value(full));
                info.set(u"args", Value(std::move(args)));
                info.set(u"rawKwargs", raw_args);
                info.set(u"stepIndex", Value(static_cast<double>(global_index++)));
                info.set(u"temp", jsv::get(step, u"temp"));
                effects.emplace_back(std::move(info));
            }
        }
    } catch (const DslSyntaxError& error) {
        const Value tagged = error_arg(u"SyntaxError", error.message());
        if (is_dsl_syntax_error(tagged))
            program_console_warn({Value(u"DSL Syntax Error:\n" + format_dsl_error(Value(dsl), tagged))});
        else program_console_warn({Value(u"Failed to parse DSL for effect extraction:"), tagged});
    } catch (const JsError& error) {
        const Value tagged = error_arg(error.name(), error.message());
        if (is_dsl_syntax_error(tagged))
            program_console_warn({Value(u"DSL Syntax Error:\n" + format_dsl_error(Value(dsl), tagged))});
        else program_console_warn({Value(u"Failed to parse DSL for effect extraction:"), tagged});
    } catch (const std::exception& error) {
        const JsString message = utf8_to_js(error.what());
        const JsString name = message.substr(0, 23) == u"Cannot read properties " ? u"TypeError" : u"Error";
        program_console_warn({Value(u"Failed to parse DSL for effect extraction:"),
                              error_arg(name, message)});
    }
    return Value(std::move(effects));
}

}  // namespace nm
