#pragma once

#include "core/value/value.h"
#include <stdexcept>
#include <utility>
#include <vector>

namespace nm {
class JsError : public std::runtime_error {
public:
    JsError(JsString name, JsString message);
    const JsString& name() const { return name_; }
    const JsString& message() const { return message_; }
private:
    JsString name_;
    JsString message_;
};
}

namespace nm::jsv {
using Entries = std::vector<std::pair<JsString, Value>>;
Value member(const Value& base, const JsString& key);
Value get(const Value& base, const JsString& key);
Value get_opt(const Value& base, const JsString& key);
Value get_v(const Value& base, const Value& key);
bool has_property(const Value& base, const JsString& key);
JsString to_property_key(const Value& value);
JsString to_string(const Value& value);
double to_number(const Value& value);
bool truthy(const Value& value);
bool strict_equals(const Value& a, const Value& b);
bool same_value_zero(const Value& a, const Value& b);
bool is_finite_number(const Value& value);
JsString join(const Array& values, const JsString& separator);
bool is_object_like(const Value& value);
Entries entries(const Value& value);
Entries entries_strict(const Value& value);
std::vector<JsString> keys(const Value& value);
Array values(const Value& value);
void set_plain(Object& object, const JsString& key, Value value);
void spread_into(Object& target, const Value& source);
Array iterate(const Value& value, const JsString& expression);
Array iterate_anon(const Value& value);
bool in_operator(const Value& key, const Value& object);
JsString repeat(const JsString& source, double count);
JsString pad_start(const JsString& source, size_t width, char16_t fill);
JsString number_to_hex(double value);
JsString quote_json_string(const JsString& source);
JsString quote_json_utf16(const JsString& source);
Value json_parse_string_body(const JsString& raw);
JsError cannot_read(const Value& base, const JsString& key);
JsError not_a_function(const JsString& expression);
JsError stack_overflow();
}
