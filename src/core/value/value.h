#pragma once

#include "core/value/js_string.h"

#include <cstddef>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace nm {
class Value;
class Object;
using Array = std::vector<Value>;

class Value {
 public:
    enum class Kind { Undefined, Null, Bool, Number, String, Array, Object, Function };

    Value();
    static Value null();
    Value(bool value);
    Value(double value);
    Value(int value);
    Value(JsString value);
    Value(const char16_t* value);
    Value(Array value);
    Value(Object value);
    static Value function(JsString source);
    Value(const Value& other);
    Value& operator=(const Value& other);
    Value(Value&&) noexcept = default;
    Value& operator=(Value&&) noexcept = default;

    Kind kind() const { return kind_; }
    bool is_undefined() const { return kind_ == Kind::Undefined; }
    bool is_null() const { return kind_ == Kind::Null; }
    bool is_bool() const { return kind_ == Kind::Bool; }
    bool is_number() const { return kind_ == Kind::Number; }
    bool is_string() const { return kind_ == Kind::String; }
    bool is_array() const { return kind_ == Kind::Array; }
    bool is_object() const { return kind_ == Kind::Object; }
    bool is_function() const { return kind_ == Kind::Function; }
    bool as_bool() const;
    double as_number() const;
    const JsString& as_string() const;
    const Array& as_array() const;
    Array& as_array();
    const Object& as_object() const;
    Object& as_object();

 private:
    Kind kind_ = Kind::Undefined;
    using Storage = std::variant<std::monostate, bool, double, JsString,
                                 std::shared_ptr<Array>, std::shared_ptr<Object>>;
    Storage data_;
};

class Object {
 public:
    bool has(const JsString& key) const;
    const Value* find(const JsString& key) const;
    Value& operator[](const JsString& key);
    void set(const JsString& key, Value value);
    bool erase(const JsString& key);
    std::vector<JsString> keys() const;
    void define_hidden(const JsString& key, Value value);
    size_t size() const;

 private:
    std::vector<std::pair<JsString, Value>> entries_;
    std::unordered_map<JsString, size_t> index_;
    std::unordered_map<JsString, Value> hidden_;
};
}  // namespace nm
