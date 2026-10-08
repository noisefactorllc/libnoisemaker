#pragma once

#include "core/js/text.h"
#include "core/value/js_number.h"
#include "core/value/json.h"
#include "core/value/value.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace nm {

class JsText : public js::JsText {
public:
    using js::JsText::JsText;
    using js::JsText::arg;
    using js::JsText::append;
    JsText() = default;
    JsText(const char* value) : js::JsText(utf8_to_js(value)) {}
    JsText(const JsString& value) : js::JsText(value) {}
    JsText(const js::JsText& value) : js::JsText(value) {}
    static JsText fromLatin1(const char* value) { return js::JsText::fromLatin1(value); }
    static JsText fromUtf8(const std::string& value) { return utf8_to_js(value); }
    static JsText number(double value) { return utf8_to_js(std::to_string(value)); }
    std::string toStdString() const { return js_to_utf8(*this); }
    std::string toUtf8() const { return js_to_utf8(*this); }
    bool isEmpty() const { return empty(); }
    bool startsWith(const JsString& prefix) const { return rfind(prefix, 0) == 0; }
    bool endsWith(const JsString& suffix) const { return size() >= suffix.size() && compare(size() - suffix.size(), suffix.size(), suffix) == 0; }
    bool endsWith(char16_t suffix) const { return !empty() && back() == suffix; }
    JsText left(std::size_t count) const { return substr(0, count); }
    std::ptrdiff_t lastIndexOf(char16_t needle) const { const auto pos = rfind(needle); return pos == npos ? -1 : static_cast<std::ptrdiff_t>(pos); }
    JsText trimmed() const {
        std::size_t a = 0, b = size();
        while (a < b && ((*this)[a] == u' ' || (*this)[a] == u'\n' || (*this)[a] == u'\t' || (*this)[a] == u'\r')) ++a;
        while (b > a && ((*this)[b - 1] == u' ' || (*this)[b - 1] == u'\n' || (*this)[b - 1] == u'\t' || (*this)[b - 1] == u'\r')) --b;
        return substr(a, b - a);
    }
    double toDouble(bool* ok = nullptr) const {
        try {
            std::size_t used = 0;
            const double value = std::stod(toStdString(), &used);
            if (ok) *ok = used == size();
            return value;
        } catch (...) { if (ok) *ok = false; return 0; }
    }
    long long toLongLong(bool* ok = nullptr) const {
        try {
            std::size_t used = 0;
            const auto value = std::stoll(toStdString(), &used);
            if (ok) *ok = used == size();
            return value;
        } catch (...) { if (ok) *ok = false; return 0; }
    }
    JsText toString() const { return *this; }
    void append(char16_t value) { push_back(value); }
    JsText arg(const JsString& a, const JsString& b, const JsString& c) const { return JsText(js::JsText::arg(a).arg(b).arg(c)); }
    JsText arg(const JsString& a, const JsString& b, const JsString& c, const JsString& d) const { return JsText(js::JsText::arg(a).arg(b).arg(c).arg(d)); }
};

class JsList : public std::vector<JsText> {
public:
    using std::vector<JsText>::vector;
    JsList(const std::vector<js::JsText>& items) { for (const auto& item : items) push_back(item); }
    bool isEmpty() const { return empty(); }
    void append(const JsText& item) { push_back(item); }
    const JsText& first() const { return front(); }
    const JsText& last() const { return back(); }
    JsList mid(std::size_t start) const { return start >= size() ? JsList{} : JsList(begin() + start, end()); }
    JsList mid(std::size_t start, std::size_t length) const { return start >= size() ? JsList{} : JsList(begin() + start, begin() + std::min(size(), start + length)); }
    std::ptrdiff_t indexOf(const JsText& value) const { auto it = std::find(begin(), end(), value); return it == end() ? -1 : it - begin(); }
    friend JsList operator+(const JsList& a, const JsList& b) { JsList out = a; out.insert(out.end(), b.begin(), b.end()); return out; }
    JsText join(char16_t separator) const {
        JsText out;
        for (std::size_t i = 0; i < size(); ++i) { if (i) out.push_back(separator); out += (*this)[i]; }
        return out;
    }
    JsText join(const JsString& separator) const {
        JsText out;
        for (std::size_t i = 0; i < size(); ++i) { if (i) out += separator; out += (*this)[i]; }
        return out;
    }
    bool contains(const JsString& value) const { return std::find(begin(), end(), value) != end(); }
    void sort() { std::sort(begin(), end()); }
    JsList& operator<<(const JsText& value) { push_back(value); return *this; }
    JsList& operator<<(const JsString& value) { push_back(JsText(value)); return *this; }
};

template<class T> class JsVector : public std::vector<T> {
public:
    using std::vector<T>::vector;
    void append(const T& value) { this->push_back(value); }
    bool isEmpty() const { return this->empty(); }
    const T& first() const { return this->front(); }
    const T& last() const { return this->back(); }
};

template<class T> class JsSet : public std::set<T> {
public:
    using std::set<T>::set;
    bool contains(const T& value) const { return std::set<T>::contains(value); }
    bool isEmpty() const { return this->empty(); }
};

template<class K, class V> class JsMap : public std::map<K, V> {
public:
    using std::map<K, V>::map;
    struct Iterator {
        typename std::map<K, V>::const_iterator it;
        bool operator!=(const Iterator& other) const { return it != other.it; }
        bool operator==(const Iterator& other) const { return it == other.it; }
        Iterator& operator++() { ++it; return *this; }
        const K& key() const { return it->first; }
        const V& value() const { return it->second; }
        const std::pair<const K, V>& operator*() const { return *it; }
    };
    Iterator constBegin() const { return {std::map<K, V>::cbegin()}; }
    Iterator constEnd() const { return {std::map<K, V>::cend()}; }
    Iterator find(const K& key) const { return {std::map<K, V>::find(key)}; }
    Iterator end() const { return {std::map<K, V>::cend()}; }
    V value(const K& key) const { auto it = std::map<K, V>::find(key); return it == std::map<K, V>::end() ? V{} : it->second; }
    void insert(const K& key, V value) { std::map<K, V>::insert_or_assign(key, std::move(value)); }
    bool contains(const K& key) const { return std::map<K, V>::contains(key); }
    bool isEmpty() const { return this->empty(); }
};

class JsObject;
class JsArray;
class JsValue {
public:
    enum Type { Undefined, Null, Bool, Double, String, Array, Object, Function };
    JsValue() = default;
    explicit JsValue(Type type) : value_(type == Null ? Value::null() : Value()) {}
    JsValue(const Value& value) : value_(value) {}
    JsValue(Value&& value) : value_(std::move(value)) {}
    JsValue(const JsText& value) : value_(static_cast<const JsString&>(value)) {}
    JsValue(const JsString& value) : value_(value) {}
    JsValue(const char16_t* value) : value_(value) {}
    JsValue(const char* value) : value_(utf8_to_js(value)) {}
    JsValue(bool value) : value_(value) {}
    JsValue(int value) : value_(value) {}
    JsValue(double value) : value_(value) {}
    JsValue(const JsObject& value);
    JsValue(const JsArray& value);
    Type type() const {
        switch (value_.kind()) {
            case Value::Kind::Undefined: return Undefined;
            case Value::Kind::Null: return Null;
            case Value::Kind::Bool: return Bool;
            case Value::Kind::Number: return Double;
            case Value::Kind::String: return String;
            case Value::Kind::Function: return Function;
            case Value::Kind::Array: return Array;
            case Value::Kind::Object: return Object;
        }
        return Undefined;
    }
    bool isUndefined() const { return value_.is_undefined(); }
    bool isNull() const { return value_.is_null(); }
    bool isBool() const { return value_.is_bool(); }
    bool isDouble() const { return value_.is_number(); }
    bool isString() const { return value_.is_string(); }
    bool isFunction() const { return value_.is_function(); }
    bool isArray() const { return value_.is_array(); }
    bool isObject() const { return value_.is_object(); }
    double toDouble(double fallback = 0) const { return isDouble() ? value_.as_number() : fallback; }
    int toInt(int fallback = 0) const { return isDouble() ? static_cast<int>(value_.as_number()) : fallback; }
    bool toBool(bool fallback = false) const { return isBool() ? value_.as_bool() : fallback; }
    JsText toString(const JsText& fallback = {}) const { return isString() ? JsText(value_.as_string()) : fallback; }
    JsText toVariant() const;
    JsObject toObject() const;
    JsArray toArray() const;
    const Value& raw() const { return value_; }
    operator Value() const { return value_; }
    bool operator==(const JsValue& other) const;
    bool operator!=(const JsValue& other) const { return !(*this == other); }
    bool operator==(const JsText& other) const { return isString() && toString() == other; }
    bool operator!=(const JsText& other) const { return !(*this == other); }
private:
    Value value_;
};

class JsArray : public std::vector<JsValue> {
public:
    using std::vector<JsValue>::vector;
    bool isEmpty() const { return empty(); }
    void append(const JsValue& value) { push_back(value); }
    const JsValue& first() const { return front(); }
    const JsValue& last() const { return back(); }
    void replace(std::size_t index, const JsValue& value) { (*this)[index] = value; }
    Value raw() const { nm::Array out; for (const auto& item : *this) out.push_back(item.raw()); return Value(std::move(out)); }
};

class JsObject {
public:
    JsObject() = default;
    JsObject(std::initializer_list<std::pair<JsText, JsValue>> init) { for (const auto& item : init) insert(item.first, item.second); }
    explicit JsObject(const Value& value) {
        if (!value.is_object()) return;
        for (const auto& key : value.as_object().keys()) {
            const Value* item = value.as_object().find(key);
            if (item) insert(JsText(key), *item);
        }
    }
    bool contains(const JsString& key) const { return find(key) != entries_.end(); }
    bool contains(const char* key) const { return contains(utf8_to_js(key)); }
    JsValue value(const JsString& key) const { auto it = find(key); return it == entries_.end() ? JsValue{} : it->second; }
    JsValue value(const char* key) const { return value(utf8_to_js(key)); }
    void insert(const JsText& key, JsValue value) {
        auto it = find(key);
        // QJsonObject::insert(key, Undefined) removes the member. The pinned
        // validator relies on this for optional automation fields.
        if (value.isUndefined()) {
            if (it != entries_.end()) entries_.erase(it);
        } else if (it == entries_.end()) {
            entries_.emplace_back(key, std::move(value));
        } else {
            it->second = std::move(value);
        }
    }
    void remove(const JsString& key) { auto it = find(key); if (it != entries_.end()) entries_.erase(it); }
    bool isEmpty() const { return entries_.empty(); }
    std::size_t size() const { return entries_.size(); }
    JsList keys() const { JsList out; for (const auto& item : entries_) out.push_back(item.first); return out; }
    struct Iterator {
        const JsObject* object;
        std::size_t index;
        bool operator!=(const Iterator& other) const { return index != other.index; }
        bool operator==(const Iterator& other) const { return index == other.index; }
        Iterator& operator++() { ++index; return *this; }
        const JsText& key() const { return object->entries_[index].first; }
        const JsValue& value() const { return object->entries_[index].second; }
    };
    Iterator constBegin() const { return {this, 0}; }
    Iterator constEnd() const { return {this, entries_.size()}; }
    Iterator begin() const { return constBegin(); }
    Iterator end() const { return constEnd(); }
    Value raw() const { nm::Object out; for (const auto& [key, val] : entries_) out.set(key, val.raw()); return Value(std::move(out)); }
    operator Value() const { return raw(); }
    bool operator==(const JsObject& other) const {
        if (entries_.size() != other.entries_.size()) return false;
        for (const auto& [key, value] : entries_) {
            auto it = other.find(key);
            if (it == other.entries_.end() || value != it->second) return false;
        }
        return true;
    }
    bool operator!=(const JsObject& other) const { return !(*this == other); }
private:
    std::vector<std::pair<JsText, JsValue>> entries_;
    std::vector<std::pair<JsText, JsValue>>::iterator find(const JsString& key) { return std::find_if(entries_.begin(), entries_.end(), [&](const auto& p) { return p.first == key; }); }
    std::vector<std::pair<JsText, JsValue>>::const_iterator find(const JsString& key) const { return std::find_if(entries_.begin(), entries_.end(), [&](const auto& p) { return p.first == key; }); }
};

inline JsValue::JsValue(const JsObject& value) : value_(value.raw()) {}
inline JsValue::JsValue(const JsArray& value) : value_(value.raw()) {}
inline JsObject JsValue::toObject() const { return JsObject(value_); }
inline JsArray JsValue::toArray() const { JsArray out; if (value_.is_array()) for (const auto& item : value_.as_array()) out.push_back(item); return out; }
inline JsText JsValue::toVariant() const {
    switch (type()) {
        case Undefined: return u"undefined";
        case Null: return u"null";
        case Bool: return toBool() ? u"true" : u"false";
        case Double: return js_number_to_string(toDouble());
        case String: return toString();
        case Function: return value_.as_string();
        case Object: {
            const JsObject object = toObject();
            if (object.contains(u"toString") && !object.value(u"toString").isFunction()) {
                throw std::runtime_error("Cannot convert object to primitive value");
            }
            return u"[object Object]";
        }
        case Array: {
            JsText result;
            const JsArray items = toArray();
            for (std::size_t i = 0; i < items.size(); ++i) {
                if (i) result += u',';
                if (!items[i].isUndefined() && !items[i].isNull()) result += items[i].toVariant();
            }
            return result;
        }
    }
    return {};
}
inline bool JsValue::operator==(const JsValue& other) const {
    if (type() != other.type()) return false;
    switch (type()) {
        case Undefined: case Null: return true;
        case Bool: return toBool() == other.toBool();
        case Double: return toDouble() == other.toDouble();
        case String: return toString() == other.toString();
        case Function: return value_.as_string() == other.value_.as_string();
        case Array: return toArray() == other.toArray();
        case Object: return toObject() == other.toObject();
    }
    return false;
}

class Enums {
public:
    Enums();
    static JsObject leaf(const JsValue& value);
    static bool isLeaf(const JsValue& node);
    const JsObject& std() const { return std_; }
    const JsObject& project() const { return project_; }
    JsValue tryGetHead(const JsText& head) const;
    void registerChoice(const JsList& path, const JsValue& value);
private:
    JsObject std_;
    JsObject project_;
};

} // namespace nm
