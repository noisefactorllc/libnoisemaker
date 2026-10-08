#include "core/edit/error_formatter.h"

#include "core/js/js_unicode.h"
#include "core/value/js_number.h"
#include "core/value/json.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace nm {
namespace {

const Value* member(const Value& object, const char16_t* key) {
    return object.is_object() ? object.as_object().find(key) : nullptr;
}

Value field(const Value& object, const char16_t* key) {
    const Value* value = member(object, key);
    return value ? *value : Value();
}

bool truthy(const Value& value) {
    switch (value.kind()) {
        case Value::Kind::Undefined:
        case Value::Kind::Null: return false;
        case Value::Kind::Bool: return value.as_bool();
        case Value::Kind::Number: return value.as_number() != 0 && !std::isnan(value.as_number());
        case Value::Kind::String: return !value.as_string().empty();
        default: return true;
    }
}

JsString text(const Value& value) {
    switch (value.kind()) {
        case Value::Kind::Undefined: return u"undefined";
        case Value::Kind::Null: return u"null";
        case Value::Kind::Bool: return value.as_bool() ? u"true" : u"false";
        case Value::Kind::Number: return js_number_to_string(value.as_number());
        case Value::Kind::String: return value.as_string();
        case Value::Kind::Array: {
            JsString result;
            const auto& array = value.as_array();
            for (size_t i = 0; i < array.size(); ++i) {
                if (i) result += u",";
                if (!array[i].is_null() && !array[i].is_undefined()) result += text(array[i]);
            }
            return result;
        }
        case Value::Kind::Function: return u"function";
        case Value::Kind::Object: {
            const Value* own_to_string = value.as_object().find(u"toString");
            if (own_to_string && !own_to_string->is_function()) {
                throw std::runtime_error("Cannot convert object to primitive value");
            }
            return u"[object Object]";
        }
    }
    return {};
}

double number(const Value& value) {
    switch (value.kind()) {
        case Value::Kind::Undefined: return std::numeric_limits<double>::quiet_NaN();
        case Value::Kind::Null: return 0;
        case Value::Kind::Bool: return value.as_bool() ? 1 : 0;
        case Value::Kind::Number: return value.as_number();
        default: {
            const std::string raw = js_to_utf8(text(value));
            const char* begin = raw.c_str();
            char* end = nullptr;
            const double parsed = std::strtod(begin, &end);
            if (begin == end) return raw.empty() ? 0 : std::numeric_limits<double>::quiet_NaN();
            while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n') ++end;
            return *end == '\0' ? parsed : std::numeric_limits<double>::quiet_NaN();
        }
    }
}

double js_min(double a, double b) {
    return std::isnan(a) || std::isnan(b) ? std::numeric_limits<double>::quiet_NaN() : std::min(a, b);
}

double js_max(double a, double b) {
    return std::isnan(a) || std::isnan(b) ? std::numeric_limits<double>::quiet_NaN() : std::max(a, b);
}

double add_context(double line, const Value& context) {
    if (context.is_string() || context.is_array() || context.is_object() || context.is_function()) {
        return number(Value(js_number_to_string(line) + text(context)));
    }
    return line + number(context);
}

bool whitespace(char16_t unit) {
    return js::isWhiteSpace(unit) || js::isLineTerminator(unit);
}

JsString trim(JsString value) {
    size_t begin = 0;
    size_t end = value.size();
    while (begin < end && whitespace(value[begin])) ++begin;
    while (end > begin && whitespace(value[end - 1])) --end;
    return value.substr(begin, end - begin);
}

std::optional<std::pair<double, double>> parse_location(const JsString& message) {
    auto digits = [&](size_t& pos) -> double {
        const size_t begin = pos;
        while (pos < message.size() && message[pos] >= u'0' && message[pos] <= u'9') ++pos;
        if (pos == begin) return std::numeric_limits<double>::quiet_NaN();
        std::string raw;
        raw.reserve(pos - begin);
        for (size_t i = begin; i < pos; ++i) raw.push_back(static_cast<char>(message[i]));
        return std::strtod(raw.c_str(), nullptr);
    };
    for (size_t found = message.find(u"at line "); found != JsString::npos;
         found = message.find(u"at line ", found + 1)) {
        size_t pos = found + 8;
        const double line = digits(pos);
        if (std::isnan(line) || message.compare(pos, 4, u" col") != 0) continue;
        pos += 4;
        if (message.compare(pos, 4, u"umn ") == 0) pos += 4;
        else if (pos < message.size() && message[pos] == u' ') ++pos;
        else continue;
        const double col = digits(pos);
        if (!std::isnan(col)) return std::pair{line, col};
    }
    return std::nullopt;
}

bool suffix_location(const JsString& value, size_t start) {
    if (value.compare(start, 8, u"at line ") != 0) return false;
    size_t pos = start + 8;
    size_t digits = 0;
    while (pos < value.size() && value[pos] >= u'0' && value[pos] <= u'9') { ++pos; ++digits; }
    if (!digits || value.compare(pos, 4, u" col") != 0) return false;
    pos += 4;
    if (value.compare(pos, 4, u"umn ") == 0) pos += 4;
    else if (pos < value.size() && value[pos] == u' ') ++pos;
    else return false;
    digits = 0;
    while (pos < value.size() && value[pos] >= u'0' && value[pos] <= u'9') { ++pos; ++digits; }
    return digits && pos == value.size();
}

JsString extract_message(const JsString& message) {
    for (size_t i = 0; i < message.size();) {
        if (!whitespace(message[i])) { ++i; continue; }
        const size_t begin = i;
        while (i < message.size() && whitespace(message[i])) ++i;
        if (suffix_location(message, i)) return trim(message.substr(0, begin));
    }
    return trim(message);
}

JsString padded_number(double value, size_t width) {
    JsString result = js_number_to_string(value);
    if (result.size() < width) result.insert(0, width - result.size(), u' ');
    return result;
}

std::vector<JsString> lines(const JsString& source) {
    std::vector<JsString> result;
    size_t from = 0;
    while (from <= source.size()) {
        const size_t end = source.find(u'\n', from);
        result.push_back(source.substr(from, end == JsString::npos ? JsString::npos : end - from));
        if (end == JsString::npos) break;
        from = end + 1;
    }
    return result;
}

JsString line_at(const std::vector<JsString>& source, double line) {
    if (!std::isfinite(line) || line < 1 || std::floor(line) != line || line > source.size()) {
        return u"undefined";
    }
    return source[static_cast<size_t>(line) - 1];
}

JsString joined(const std::vector<JsString>& parts) {
    JsString result;
    for (const auto& part : parts) {
        if (!result.empty()) result += u'\n';
        result += part;
    }
    return result;
}

}  // namespace

bool is_dsl_syntax_error(const Value& error) {
    const Value tag = field(error, u"$js");
    if (!tag.is_string() || tag.as_string() != u"error") return false;
    const Value name = field(error, u"name");
    const Value message = field(error, u"message");
    return name.is_string() && name.as_string() == u"SyntaxError" &&
           message.is_string() && parse_location(message.as_string()).has_value();
}

JsString format_dsl_error(const Value& source, const Value& error, const Value& options) {
    const Value message = field(error, u"message");
    if (!truthy(error)) return u"Unknown error";
    if (!message.is_string()) return text(error);
    const JsString& raw = message.as_string();
    const JsString core = extract_message(raw);
    const auto location = parse_location(raw);
    if (!location || !truthy(source)) return u"SyntaxError: " + core;
    if (!source.is_string()) throw std::runtime_error("source.split is not a function");
    if (options.is_null()) throw std::runtime_error("Cannot read properties of null (reading 'contextLines')");

    Value context = field(options, u"contextLines");
    if (context.is_undefined()) context = Value(2);
    const double context_n = number(context);
    const double error_line = location->first;
    const double error_col = location->second;
    const auto source_lines = lines(source.as_string());
    const double line_count = static_cast<double>(source_lines.size());
    const double last_line = js_min(add_context(error_line, context), line_count);
    const size_t width = js_number_to_string(last_line).size();
    std::vector<JsString> parts = {
        u"SyntaxError: " + core,
        u"  --> line " + js_number_to_string(error_line) + u", column " + js_number_to_string(error_col),
        JsString()
    };
    const double start = js_max(1.0, error_line - context_n);
    for (double line = start; line < error_line && parts.size() < 100000; line += 1) {
        parts.push_back(u"  " + padded_number(line, width) + u" | " + line_at(source_lines, line));
    }
    JsString error_text;
    if (std::isfinite(error_line) && error_line >= 1 && std::floor(error_line) == error_line &&
        error_line <= source_lines.size()) {
        error_text = source_lines[static_cast<size_t>(error_line) - 1];
    }
    parts.push_back(u"  " + padded_number(error_line, width) + u" | " + error_text);
    if (!std::isfinite(error_col) || error_col > 1000000) throw std::runtime_error("Invalid string length");
    const size_t padding = static_cast<size_t>(js_max(0.0, error_col - 1.0));
    parts.push_back(JsString(width + 3 + padding, u' ') + u"^-- error here");
    const double end = js_min(line_count, add_context(error_line, context));
    for (double line = error_line + 1; line <= end && parts.size() < 100000; line += 1) {
        parts.push_back(u"  " + padded_number(line, width) + u" | " + line_at(source_lines, line));
    }
    return joined(parts);
}

JsString format_compile_error(const JsString& source, const Value& error) {
    if (is_dsl_syntax_error(error)) return format_dsl_error(Value(source), error);
    const Value message = field(error, u"message");
    const Value code = field(error, u"code");
    if (code.is_undefined() && message.is_string()) {
        return message.as_string().empty() ? u"{}" : message.as_string();
    }
    JsString result;
    if (code.is_string() && code.as_string() == u"ERR_COMPILATION_FAILED") {
        const Value diagnostics = field(error, u"diagnostics");
        if (diagnostics.is_array()) {
            for (const Value& item : diagnostics.as_array()) {
                const Value severity = field(item, u"severity");
                if (!severity.is_string() || severity.as_string() != u"error") continue;
                if (!result.empty()) result += u"; ";
                const Value detail = field(item, u"message");
                result += truthy(detail) ? text(detail) : u"Unknown error";
                const Value location = field(item, u"location");
                if (truthy(location)) result += u" (line " + text(field(location, u"line")) +
                    u", col " + text(field(location, u"column")) + u")";
            }
        }
        return result.empty() ? u"Unknown compilation error" : result;
    }
    if (code.is_string() && code.as_string() == u"ERR_EXPANSION_FAILED") {
        const Value errors = field(error, u"errors");
        if (errors.is_array()) {
            for (const Value& item : errors.as_array()) {
                if (!result.empty()) result += u"; ";
                const Value detail = field(item, u"message");
                result += truthy(detail) ? text(detail) : text(item);
            }
            return result;
        }
    }
    if (code.is_string() && code.as_string() == u"ERR_SHADER_COMPILE") {
        const Value detail = field(error, u"detail");
        return truthy(detail) ? text(detail) : u"Shader compile error";
    }
    if (truthy(message)) return text(message);
    const Value detail = field(error, u"detail");
    if (truthy(detail)) return text(detail);
    if (error.is_object() || error.is_array() || error.is_null()) return utf8_to_js(json::stringify(error));
    return text(error);
}
}  // namespace nm
