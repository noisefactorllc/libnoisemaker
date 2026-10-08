#include "core/value/js_string.h"

#include <cstdint>

namespace nm {
namespace {
void append_utf16(JsString& out, uint32_t cp) {
    if (cp <= 0xffff) {
        out.push_back(static_cast<char16_t>(cp));
    } else {
        cp -= 0x10000;
        out.push_back(static_cast<char16_t>(0xd800 + (cp >> 10)));
        out.push_back(static_cast<char16_t>(0xdc00 + (cp & 0x3ff)));
    }
}

void append_utf8(std::string& out, uint32_t cp) {
    if (cp <= 0x7f) out.push_back(static_cast<char>(cp));
    else if (cp <= 0x7ff) {
        out.push_back(static_cast<char>(0xc0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    } else if (cp <= 0xffff) {
        out.push_back(static_cast<char>(0xe0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    } else {
        out.push_back(static_cast<char>(0xf0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    }
}
}  // namespace

JsString utf8_to_js(std::string_view text) {
    JsString out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size();) {
        const auto first = static_cast<unsigned char>(text[i]);
        if (first < 0x80) {
            out.push_back(static_cast<char16_t>(first));
            ++i;
            continue;
        }
        size_t count = 0;
        uint32_t cp = 0;
        uint32_t minimum = 0;
        if (first >= 0xc2 && first <= 0xdf) { count = 2; cp = first & 0x1f; minimum = 0x80; }
        else if (first >= 0xe0 && first <= 0xef) { count = 3; cp = first & 0x0f; minimum = 0x800; }
        else if (first >= 0xf0 && first <= 0xf4) { count = 4; cp = first & 0x07; minimum = 0x10000; }
        if (count == 0 || i + count > text.size()) {
            out.push_back(u'\xfffd');
            ++i;
            continue;
        }
        size_t j = 1;
        for (; j < count; ++j) {
            const auto next = static_cast<unsigned char>(text[i + j]);
            if ((next & 0xc0) != 0x80) break;
            cp = (cp << 6) | (next & 0x3f);
        }
        if (j != count || cp < minimum || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) {
            out.push_back(u'\xfffd');
            ++i;
            continue;
        }
        append_utf16(out, cp);
        i += count;
    }
    return out;
}

std::string js_to_utf8(const JsString& text) {
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        uint32_t cp = text[i];
        if (cp >= 0xd800 && cp <= 0xdbff) {
            if (i + 1 < text.size() && text[i + 1] >= 0xdc00 && text[i + 1] <= 0xdfff) {
                cp = 0x10000 + ((cp - 0xd800) << 10) + (text[++i] - 0xdc00);
            } else cp = 0xfffd;
        } else if (cp >= 0xdc00 && cp <= 0xdfff) cp = 0xfffd;
        append_utf8(out, cp);
    }
    return out;
}
}  // namespace nm
