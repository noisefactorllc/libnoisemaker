#pragma once

#include "core/lang/enums.h"

#include <map>
#include <utility>

namespace nm {

template<class Key, class T> class GraphMap : public JsMap<Key, T> {
public:
    using JsMap<Key, T>::JsMap;
    struct Iterator {
        typename std::map<Key, T>::const_iterator current;
        bool operator!=(const Iterator& other) const { return current != other.current; }
        Iterator& operator++() { ++current; return *this; }
        const Key& key() const { return current->first; }
        const T& value() const { return current->second; }
    };
    Iterator cbegin() const { return {std::map<Key, T>::cbegin()}; }
    Iterator cend() const { return {std::map<Key, T>::cend()}; }
    Iterator constBegin() const { return cbegin(); }
    Iterator constEnd() const { return cend(); }
    JsVector<Key> keys() const {
        JsVector<Key> result;
        for (const auto& [key, value] : static_cast<const std::map<Key, T>&>(*this)) result.append(key);
        return result;
    }
};

}  // namespace nm
