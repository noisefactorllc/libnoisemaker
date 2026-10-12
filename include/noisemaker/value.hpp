#ifndef NOISEMAKER_VALUE_HPP
#define NOISEMAKER_VALUE_HPP

// JavaScript values as libnoisemaker's C++ API passes them: JSON plus
// undefined, function sources and Float32Array (host inputs such as audio
// waveforms), with JavaScript string semantics (UTF-16) and member insertion
// order.

#include <noisemaker/core.h>

#include <cstddef>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#if defined(_MSC_VER)
// Standard-library members and bases of exported classes; consumers must use
// the toolchain the library was built with (see the release's TOOLCHAIN file).
#  pragma warning(push)
#  pragma warning(disable : 4251 4275)
#endif

namespace libnoisemaker {

using JsString = std::u16string;

NM_CXX_API JsString utf8_to_js(std::string_view text);
NM_CXX_API std::string js_to_utf8(const JsString& text);

class Value;
class Object;
using Array = std::vector<Value>;

class NM_CXX_API Value {
 public:
    enum class Kind { Undefined, Null, Bool, Number, String, Array, Object, Function, Float32Array };

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
    static Value float32_array(std::vector<float> values);
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
    bool is_float32_array() const { return kind_ == Kind::Float32Array; }
    bool as_bool() const;
    double as_number() const;
    const JsString& as_string() const;
    const Array& as_array() const;
    Array& as_array();
    const Object& as_object() const;
    Object& as_object();
    const std::vector<float>& as_float32_array() const;

 private:
    Kind kind_ = Kind::Undefined;
    using Storage = std::variant<std::monostate, bool, double, JsString,
                                 std::shared_ptr<Array>, std::shared_ptr<Object>,
                                 std::shared_ptr<std::vector<float>>>;
    Storage data_;
};

class NM_CXX_API Object {
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

namespace json {
class NM_CXX_API ParseError : public std::runtime_error {
 public:
    using std::runtime_error::runtime_error;
    ~ParseError() override;
};

// JSON.stringify and JSON.parse semantics.
NM_CXX_API std::string stringify(const Value& value, int indent = 0);
NM_CXX_API Value parse(std::string_view utf8);
}  // namespace json

}  // namespace libnoisemaker

#if defined(_MSC_VER)
#  pragma warning(pop)
#endif

#endif
