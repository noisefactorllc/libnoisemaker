#pragma once

#include "core/value/value.h"

#include <initializer_list>
#include <utility>

namespace nm {
// Small builders for host snapshots. The stored values use the core JSON model.
class HostObject : public Object {
public:
    HostObject() = default;
    HostObject(std::initializer_list<std::pair<JsString, Value>> entries) {
        for (const auto& entry : entries) set(entry.first, entry.second);
    }
    void insert(const JsString& key, Value value) { set(key, std::move(value)); }
};

class HostArray : public Array {
public:
    using Array::Array;
    HostArray(std::initializer_list<Value> values) : Array(values) {}
    void append(Value value) { push_back(std::move(value)); }
};
}  // namespace nm
