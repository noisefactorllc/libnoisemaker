#pragma once

#include "core/edit/jsv.h"

#include <algorithm>
#include <type_traits>
#include <utility>
#include <vector>

namespace nm::program_state {

// An insertion ordered JavaScript Map. Value keys use SameValueZero; object keys
// cannot match by identity because Value owns rather than references objects.
template <class V>
class JsMap {
public:
    using Entry = std::pair<Value, V>;

    std::size_t size() const { return entries_.size(); }
    bool empty() const { return entries_.empty(); }
    const std::vector<Entry>& entries() const { return entries_; }

    const V* get(const Value& key) const {
        auto it = position(key);
        return it == entries_.end() ? nullptr : &it->second;
    }
    V* get_mut(const Value& key) {
        auto it = position(key);
        return it == entries_.end() ? nullptr : &it->second;
    }
    bool has(const Value& key) const { return position(key) != entries_.end(); }
    void set(Value key, V value) {
        auto it = position(key);
        if (it != entries_.end()) {
            it->second = std::move(value);
            return;
        }
        if (key.is_number() && key.as_number() == 0.0) key = Value(0.0);
        entries_.emplace_back(std::move(key), std::move(value));
    }
    bool erase(const Value& key) {
        auto it = position(key);
        if (it == entries_.end()) return false;
        entries_.erase(it);
        return true;
    }
    void clear() { entries_.clear(); }
    std::vector<Value> keys() const {
        std::vector<Value> out;
        out.reserve(entries_.size());
        for (const auto& [key, value] : entries_) {
            (void)value;
            out.push_back(key);
        }
        return out;
    }

    static JsMap<Value> from_object(const Object& object) requires std::is_same_v<V, Value> {
        JsMap<Value> out;
        for (const auto& key : object.keys()) out.set(Value(key), *object.find(key));
        return out;
    }
    static JsMap<Value> from_entries_of(const Value& value) requires std::is_same_v<V, Value> {
        JsMap<Value> out;
        for (const auto& [key, member] : jsv::entries(value)) out.set(Value(key), member);
        return out;
    }
    Object to_object() const requires std::is_same_v<V, Value> {
        Object out;
        for (const auto& [key, value] : entries_) {
            jsv::set_plain(out, jsv::to_property_key(key), value);
        }
        return out;
    }

private:
    auto position(const Value& key) {
        return std::find_if(entries_.begin(), entries_.end(), [&](const Entry& entry) {
            return jsv::same_value_zero(entry.first, key);
        });
    }
    auto position(const Value& key) const {
        return std::find_if(entries_.begin(), entries_.end(), [&](const Entry& entry) {
            return jsv::same_value_zero(entry.first, key);
        });
    }
    std::vector<Entry> entries_;
};

}  // namespace nm::program_state
