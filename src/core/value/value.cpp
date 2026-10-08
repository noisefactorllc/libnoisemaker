#include "core/value/value.h"

#include <algorithm>
#include <cstdint>

namespace nm {
Value::Value() = default;
Value Value::null() { Value v; v.kind_ = Kind::Null; return v; }
Value::Value(bool value) : kind_(Kind::Bool), data_(value) {}
Value::Value(double value) : kind_(Kind::Number), data_(value) {}
Value::Value(int value) : Value(static_cast<double>(value)) {}
Value::Value(JsString value) : kind_(Kind::String), data_(std::move(value)) {}
Value::Value(const char16_t* value) : Value(JsString(value)) {}
Value::Value(Array value) : kind_(Kind::Array), data_(std::make_shared<Array>(std::move(value))) {}
Value::Value(Object value) : kind_(Kind::Object), data_(std::make_shared<Object>(std::move(value))) {}
Value Value::function(JsString source) {
    Value v(std::move(source));
    v.kind_ = Kind::Function;
    return v;
}
Value::Value(const Value& other) : kind_(other.kind_) {
    if (kind_ == Kind::Array) data_ = std::make_shared<Array>(other.as_array());
    else if (kind_ == Kind::Object) data_ = std::make_shared<Object>(other.as_object());
    else data_ = other.data_;
}
Value& Value::operator=(const Value& other) {
    if (this != &other) {
        Value copy(other);
        kind_ = copy.kind_;
        data_ = std::move(copy.data_);
    }
    return *this;
}
bool Value::as_bool() const { return std::get<bool>(data_); }
double Value::as_number() const { return std::get<double>(data_); }
const JsString& Value::as_string() const { return std::get<JsString>(data_); }
const Array& Value::as_array() const { return *std::get<std::shared_ptr<Array>>(data_); }
Array& Value::as_array() { return *std::get<std::shared_ptr<Array>>(data_); }
const Object& Value::as_object() const { return *std::get<std::shared_ptr<Object>>(data_); }
Object& Value::as_object() { return *std::get<std::shared_ptr<Object>>(data_); }

bool Object::has(const JsString& key) const { return index_.contains(key) || hidden_.contains(key); }
const Value* Object::find(const JsString& key) const {
    if (auto it = index_.find(key); it != index_.end()) return &entries_[it->second].second;
    if (auto it = hidden_.find(key); it != hidden_.end()) return &it->second;
    return nullptr;
}
Value& Object::operator[](const JsString& key) {
    if (auto it = index_.find(key); it != index_.end()) return entries_[it->second].second;
    if (auto it = hidden_.find(key); it != hidden_.end()) return it->second;
    const size_t index = entries_.size();
    entries_.emplace_back(key, Value{});
    index_.emplace(key, index);
    return entries_.back().second;
}
void Object::set(const JsString& key, Value value) { (*this)[key] = std::move(value); }
bool Object::erase(const JsString& key) {
    if (hidden_.erase(key)) return true;
    const auto it = index_.find(key);
    if (it == index_.end()) return false;
    const size_t index = it->second;
    entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(index));
    index_.erase(it);
    for (size_t i = index; i < entries_.size(); ++i) index_[entries_[i].first] = i;
    return true;
}

namespace {
bool array_index(const JsString& key, uint32_t& value) {
    if (key.empty() || (key.size() > 1 && key[0] == u'0')) return false;
    uint64_t n = 0;
    for (char16_t c : key) {
        if (c < u'0' || c > u'9') return false;
        n = n * 10 + (c - u'0');
        if (n >= UINT32_MAX) return false;
    }
    value = static_cast<uint32_t>(n);
    return true;
}
}  // namespace

std::vector<JsString> Object::keys() const {
    std::vector<std::pair<uint32_t, JsString>> numeric;
    std::vector<JsString> other;
    for (const auto& [key, value] : entries_) {
        (void)value;
        uint32_t n = 0;
        if (array_index(key, n)) numeric.emplace_back(n, key);
        else other.push_back(key);
    }
    std::sort(numeric.begin(), numeric.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<JsString> result;
    result.reserve(entries_.size());
    for (const auto& [n, key] : numeric) { (void)n; result.push_back(key); }
    result.insert(result.end(), other.begin(), other.end());
    return result;
}
void Object::define_hidden(const JsString& key, Value value) {
    erase(key);
    hidden_.insert_or_assign(key, std::move(value));
}
size_t Object::size() const { return entries_.size() + hidden_.size(); }
}  // namespace nm
