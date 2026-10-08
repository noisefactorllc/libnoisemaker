#pragma once

// Small UTF-16 and JSON adapters for the pinned Qt parser. They store the
// public core's JsString, Value, Object and Array, with no Qt dependency.
#include "core/js/text.h"
#include "core/value/value.h"
#include "core/value/js_string.h"
#include "core/value/parse_double.h"
#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <initializer_list>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace nm {
class LangString : public js::JsText {
public:
    using js::JsText::JsText;
    using js::JsText::arg;
    LangString() = default;
    LangString(const js::JsText& s) : js::JsText(s) {}
    LangString(const JsString& s) : js::JsText(s) {}
    LangString(JsString&& s) : js::JsText(std::move(s)) {}
    LangString(char16_t ch) : js::JsText(ch) {}
    static LangString fromUtf8(const char* s) { return LangString(utf8_to_js(s)); }
    static LangString fromUtf16(const char16_t* s) { return LangString(s); }
    bool startsWith(char16_t ch) const { return !empty() && front() == ch; }
    static LangString number(int n) { return LangString(js::JsText::fromLatin1(std::to_string(n).c_str())); }
    static LangString number(double n) { return LangString(js::JsText::fromLatin1(std::to_string(n).c_str())); }
    int length() const { return static_cast<int>(size()); }
    bool isEmpty() const { return empty(); }
    LangString mid(std::ptrdiff_t start, std::ptrdiff_t length = -1) const { return LangString(js::JsText::mid(start, length)); }
    double toDouble() const {
        const std::string ascii = js_to_utf8(*this);
        double value = 0;
        const auto result = parse_double(ascii.data(), ascii.data() + ascii.size(), value);
        return result.ec == std::errc() ? value : 0;
    }
    std::string toStdString() const { return js_to_utf8(*this); }
    LangString format_args(std::initializer_list<JsString> args) const {
        LangString out;
        for (size_t i = 0; i < size(); ++i) {
            if ((*this)[i] == u'%' && i + 1 < size()) {
                const char16_t n = (*this)[i + 1];
                if (n >= u'1' && n < u'1' + args.size()) {
                    out += *(args.begin() + (n - u'1'));
                    ++i;
                    continue;
                }
            }
            out.push_back((*this)[i]);
        }
        return out;
    }
    LangString arg(const JsString& value) const {
        char16_t lowest = u'9' + 1;
        for (size_t i = 0; i + 1 < size(); ++i)
            if ((*this)[i] == u'%' && (*this)[i + 1] >= u'1' && (*this)[i + 1] <= u'9')
                lowest = std::min(lowest, (*this)[i + 1]);
        if (lowest > u'9') return *this;
        LangString out;
        for (size_t i = 0; i < size(); ++i) {
            if ((*this)[i] == u'%' && i + 1 < size() && (*this)[i + 1] == lowest) {
                out += value; ++i;
            } else out.push_back((*this)[i]);
        }
        return out;
    }
    LangString arg(char16_t ch) const { return arg(JsString(1, ch)); }
    LangString arg(int value) const { return arg(js::JsText::fromLatin1(std::to_string(value).c_str())); }
    LangString arg(std::ptrdiff_t value) const { return arg(js::JsText::fromLatin1(std::to_string(value).c_str())); }
    LangString arg(const JsString& a, const JsString& b) const { return format_args({a, b}); }
    LangString arg(const JsString& a, const JsString& b, const JsString& c, const JsString& d) const { return format_args({a, b, c, d}); }
};
using QString = LangString;
using QChar = char16_t;
using qsizetype = std::ptrdiff_t;
#define QStringLiteral(s) ::nm::LangString(u##s)
#define QLatin1Char(c) u##c

class QStringList : public std::vector<QString> {
public:
    using std::vector<QString>::vector;
    QStringList(std::initializer_list<QString> init) : std::vector<QString>(init) {}
    void append(const QString& s) { push_back(s); }
    void append(const QStringList& other) { insert(end(), other.begin(), other.end()); }
    bool isEmpty() const { return empty(); }
    bool contains(const QString& s) const { return std::find(begin(), end(), s) != end(); }
    QString first() const { return front(); }
    QString last() const { return back(); }
    QString join(const QString& delim) const {
        QString out;
        for (const auto& part : *this) { if (!out.empty()) out += delim; out += part; }
        return out;
    }
    QString join(char16_t delim) const { return join(QString(1, delim)); }
};
template<class T> class QVector : public std::vector<T> {
public:
    using std::vector<T>::vector;
    bool isEmpty() const { return this->empty(); }
    const T& last() const { return this->back(); }
    void append(const T& value) { this->push_back(value); }
};
template<class T> using QSet = std::unordered_set<T>;

class JsonObject;
class JsonArray;
class JsonValue {
public:
    enum Special { Null };
    JsonValue() = default;
    JsonValue(Special) : value_(Value::null()) {}
    JsonValue(Value value) : value_(std::move(value)) {}
    JsonValue(const JsonObject& obj);
    JsonValue(const JsonArray& arr);
    JsonValue(const QString& s) : value_(Value(JsString(s))) {}
    JsonValue(const JsString& s) : value_(Value(s)) {}
    JsonValue(const char16_t* s) : value_(Value(s)) {}
    JsonValue(bool b) : value_(Value(b)) {}
    JsonValue(int n) : value_(Value(n)) {}
    JsonValue(double n) : value_(Value(n)) {}
    bool isUndefined() const { return value_.is_undefined(); }
    bool isNull() const { return value_.is_null(); }
    bool isObject() const { return value_.is_object(); }
    bool isArray() const { return value_.is_array(); }
    bool isDouble() const { return value_.is_number(); }
    bool isString() const { return value_.is_string(); }
    QString toString() const { return value_.is_string() ? QString(value_.as_string()) : QString(); }
    int toInt() const { return value_.is_number() ? static_cast<int>(value_.as_number()) : 0; }
    double toDouble() const { return value_.is_number() ? value_.as_number() : 0; }
    bool toBool() const { return value_.is_bool() ? value_.as_bool() : false; }
    JsonObject toObject() const;
    JsonArray toArray() const;
    const Value& native() const { return value_; }
private:
    Value value_;
};
class JsonArray : public std::vector<JsonValue> {
public:
    using std::vector<JsonValue>::vector;
    explicit JsonArray(const Array& values) { for (const auto& v : values) push_back(JsonValue(v)); }
    void append(const JsonValue& v) { push_back(v); }
    JsonValue at(int i) const { return (*this)[static_cast<size_t>(i)]; }
    JsonValue last() const { return back(); }
    void replace(int i, const JsonValue& v) { (*this)[static_cast<size_t>(i)] = v; }
    bool isEmpty() const { return empty(); }
    Value native() const { Array a; for (const auto& v : *this) a.push_back(v.native()); return Value(std::move(a)); }
};
class JsonObject {
public:
    JsonObject() = default;
    explicit JsonObject(const Object& obj) : object_(obj) {}
    JsonObject(std::initializer_list<std::pair<const QString, JsonValue>> pairs) {
        for (const auto& p : pairs) insert(p.first, p.second);
    }
    void insert(const QString& key, const JsonValue& value) { object_.set(key, value.native()); }
    JsonValue value(const QString& key) const { const Value* v = object_.find(key); return v ? JsonValue(*v) : JsonValue(); }
    bool contains(const QString& key) const { return object_.has(key); }
    void remove(const QString& key) { object_.erase(key); }
    bool isEmpty() const { return object_.size() == 0; }
    QStringList keys() const { QStringList out; for (const auto& k : object_.keys()) out.append(QString(k)); return out; }
    const Object& native_object() const { return object_; }
    Value native() const { return Value(object_); }
private:
    Object object_;
};
inline JsonValue::JsonValue(const JsonObject& obj) : value_(obj.native()) {}
inline JsonValue::JsonValue(const JsonArray& arr) : value_(arr.native()) {}
inline JsonObject JsonValue::toObject() const { return value_.is_object() ? JsonObject(value_.as_object()) : JsonObject(); }
inline JsonArray JsonValue::toArray() const { return value_.is_array() ? JsonArray(value_.as_array()) : JsonArray(); }
}
namespace std {
template<> struct hash<nm::LangString> {
    size_t operator()(const nm::LangString& s) const noexcept { return hash<nm::JsString>{}(s); }
};
}
