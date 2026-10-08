#pragma once

#include "core/value/js_string.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace nm::js {

// Local UTF-16 text operations used by the parser. Offsets are code units.
class JsText : public JsString {
public:
    using JsString::JsString;
    JsText() = default;
    JsText(const JsString& value) : JsString(value) {}
    JsText(JsString&& value) : JsString(std::move(value)) {}
    explicit JsText(char16_t value) : JsString(1, value) {}

    static JsText fromLatin1(const char* text) {
        JsText result;
        for (const unsigned char* p = reinterpret_cast<const unsigned char*>(text); *p; ++p)
            result.push_back(static_cast<char16_t>(*p));
        return result;
    }

    static JsText fromUcs4(const char32_t* text, std::size_t length) {
        JsText result;
        for (std::size_t i = 0; i < length; ++i) {
            const char32_t cp = text[i];
            if (cp <= 0xffff) result.push_back(static_cast<char16_t>(cp));
            else {
                result.push_back(static_cast<char16_t>(0xd800 + ((cp - 0x10000) >> 10)));
                result.push_back(static_cast<char16_t>(0xdc00 + ((cp - 0x10000) & 0x3ff)));
            }
        }
        return result;
    }

    JsText mid(std::ptrdiff_t start, std::ptrdiff_t length = -1) const {
        if (start < 0 || static_cast<std::size_t>(start) >= size()) return {};
        return substr(static_cast<std::size_t>(start), length < 0 ? npos : static_cast<std::size_t>(length));
    }
    std::ptrdiff_t indexOf(const JsString& needle, std::ptrdiff_t start = 0) const {
        if (start < 0) start = 0;
        const auto pos = find(needle, static_cast<std::size_t>(start));
        return pos == npos ? -1 : static_cast<std::ptrdiff_t>(pos);
    }
    std::ptrdiff_t indexOf(char16_t needle, std::ptrdiff_t start = 0) const {
        if (start < 0) start = 0;
        const auto pos = find(needle, static_cast<std::size_t>(start));
        return pos == npos ? -1 : static_cast<std::ptrdiff_t>(pos);
    }
    bool contains(const JsString& needle) const { return find(needle) != npos; }
    bool contains(char16_t needle) const { return find(needle) != npos; }

    std::vector<JsText> split(char16_t delimiter, bool skipEmpty) const {
        std::vector<JsText> result;
        std::size_t start = 0;
        while (start <= size()) {
            const auto end = find(delimiter, start);
            JsText part = substr(start, end == npos ? npos : end - start);
            if (!skipEmpty || !part.empty()) result.push_back(std::move(part));
            if (end == npos) break;
            start = end + 1;
        }
        return result;
    }
    int toInt(bool* ok = nullptr, int base = 10) const {
        int value = 0;
        bool valid = !empty();
        for (char16_t ch : *this) {
            int digit = ch >= u'0' && ch <= u'9' ? ch - u'0'
                        : ch >= u'a' && ch <= u'z' ? ch - u'a' + 10
                        : ch >= u'A' && ch <= u'Z' ? ch - u'A' + 10 : -1;
            if (digit < 0 || digit >= base) { valid = false; break; }
            value = value * base + digit;
        }
        if (ok) *ok = valid;
        return valid ? value : 0;
    }
    void chop(std::size_t count) { resize(count > size() ? 0 : size() - count); }

    JsText arg(const JsString& value) const {
        JsText result = *this;
        for (char16_t n = u'1'; n <= u'9'; ++n) {
            const JsString token{u'%', n};
            const auto pos = result.find(token);
            if (pos != npos) { result.replace(pos, 2, value); break; }
        }
        return result;
    }
    JsText arg(int value) const { return arg(fromLatin1(std::to_string(value).c_str())); }
    JsText arg(std::ptrdiff_t value) const { return arg(fromLatin1(std::to_string(value).c_str())); }
    JsText arg(const JsString& first, const JsString& second) const { return arg(first).arg(second); }
};

template<class Map>
JsText mapValue(const Map& map, const JsText& key) {
    const auto it = map.find(key);
    return it == map.end() ? JsText{} : it->second;
}

} // namespace nm::js

template<>
struct std::hash<nm::js::JsText> {
    std::size_t operator()(const nm::js::JsText& value) const noexcept {
        return std::hash<nm::JsString>{}(value);
    }
};
