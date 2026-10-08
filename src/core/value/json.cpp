#include "core/value/json.h"

#include "core/value/js_number.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <string>

namespace nm::json {
namespace {
void append_hex_escape(std::string& out, char16_t unit) {
    constexpr char hex[] = "0123456789abcdef";
    out += "\\u";
    out.push_back(hex[(unit >> 12) & 15]);
    out.push_back(hex[(unit >> 8) & 15]);
    out.push_back(hex[(unit >> 4) & 15]);
    out.push_back(hex[unit & 15]);
}

void append_quoted(std::string& out, const JsString& text) {
    out.push_back('"');
    for (size_t i = 0; i < text.size(); ++i) {
        const char16_t c = text[i];
        switch (c) {
            case u'"': out += "\\\""; continue;
            case u'\\': out += "\\\\"; continue;
            case u'\b': out += "\\b"; continue;
            case u'\f': out += "\\f"; continue;
            case u'\n': out += "\\n"; continue;
            case u'\r': out += "\\r"; continue;
            case u'\t': out += "\\t"; continue;
            default: break;
        }
        if (c < 0x20) { append_hex_escape(out, c); continue; }
        if (c >= 0xd800 && c <= 0xdbff) {
            if (i + 1 < text.size() && text[i + 1] >= 0xdc00 && text[i + 1] <= 0xdfff) {
                out += js_to_utf8(JsString{text[i], text[i + 1]});
                ++i;
            } else append_hex_escape(out, c);
        } else if (c >= 0xdc00 && c <= 0xdfff) append_hex_escape(out, c);
        else out += js_to_utf8(JsString{c});
    }
    out.push_back('"');
}

void spaces(std::string& out, int depth, int indent) {
    out.push_back('\n');
    out.append(static_cast<size_t>(depth * indent), ' ');
}

void append_value(std::string& out, const Value& value, int indent, int depth) {
    if (depth > 512) throw std::runtime_error("JSON nesting exceeds 512 levels");
    switch (value.kind()) {
        case Value::Kind::Undefined:
        case Value::Kind::Function:
        case Value::Kind::Null: out += "null"; return;
        case Value::Kind::Bool: out += value.as_bool() ? "true" : "false"; return;
        case Value::Kind::Number:
            out += std::isfinite(value.as_number()) ? js_to_utf8(js_number_to_string(value.as_number())) : "null";
            return;
        case Value::Kind::String: append_quoted(out, value.as_string()); return;
        case Value::Kind::Array: {
            out.push_back('[');
            bool first = true;
            for (const auto& member : value.as_array()) {
                if (!first) out.push_back(',');
                if (indent) spaces(out, depth + 1, indent);
                append_value(out, member, indent, depth + 1);
                first = false;
            }
            if (!first && indent) spaces(out, depth, indent);
            out.push_back(']');
            return;
        }
        case Value::Kind::Object: {
            out.push_back('{');
            bool first = true;
            const auto& object = value.as_object();
            for (const auto& key : object.keys()) {
                const Value* member = object.find(key);
                if (!member || member->is_undefined() || member->is_function()) continue;
                if (!first) out.push_back(',');
                if (indent) spaces(out, depth + 1, indent);
                append_quoted(out, key);
                out.push_back(':');
                if (indent) out.push_back(' ');
                append_value(out, *member, indent, depth + 1);
                first = false;
            }
            if (!first && indent) spaces(out, depth, indent);
            out.push_back('}');
        }
    }
}

class Parser {
 public:
    explicit Parser(std::string_view text) : text_(text) {}
    Value parse() {
        skip_ws();
        Value value = read_value(0);
        skip_ws();
        if (pos_ != text_.size()) fail("trailing input");
        return value;
    }

 private:
    std::string_view text_;
    size_t pos_ = 0;

    [[noreturn]] void fail(const char* message) const {
        throw ParseError(std::string(message) + " at byte " + std::to_string(pos_));
    }
    void skip_ws() {
        while (pos_ < text_.size() && (text_[pos_] == ' ' || text_[pos_] == '\n' ||
                                      text_[pos_] == '\r' || text_[pos_] == '\t')) ++pos_;
    }
    bool take(char ch) {
        if (pos_ < text_.size() && text_[pos_] == ch) { ++pos_; return true; }
        return false;
    }
    void expect(char ch) { if (!take(ch)) fail("unexpected character"); }
    bool literal(std::string_view word) {
        if (text_.substr(pos_, word.size()) != word) return false;
        pos_ += word.size();
        return true;
    }
    static int hex_digit(char ch) {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
        if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
        return -1;
    }
    JsString read_string() {
        expect('"');
        JsString out;
        size_t raw_start = pos_;
        while (pos_ < text_.size()) {
            const unsigned char ch = static_cast<unsigned char>(text_[pos_]);
            if (ch < 0x20) fail("unescaped control character");
            if (ch != '"' && ch != '\\') { ++pos_; continue; }
            out += utf8_to_js(text_.substr(raw_start, pos_ - raw_start));
            if (take('"')) return out;
            ++pos_;
            if (pos_ == text_.size()) fail("incomplete escape");
            const char escaped = text_[pos_++];
            switch (escaped) {
                case '"': out.push_back(u'"'); break;
                case '\\': out.push_back(u'\\'); break;
                case '/': out.push_back(u'/'); break;
                case 'b': out.push_back(u'\b'); break;
                case 'f': out.push_back(u'\f'); break;
                case 'n': out.push_back(u'\n'); break;
                case 'r': out.push_back(u'\r'); break;
                case 't': out.push_back(u'\t'); break;
                case 'u': {
                    if (text_.size() - pos_ < 4) fail("incomplete Unicode escape");
                    char16_t unit = 0;
                    for (int i = 0; i < 4; ++i) {
                        const int digit = hex_digit(text_[pos_++]);
                        if (digit < 0) fail("invalid Unicode escape");
                        unit = static_cast<char16_t>((unit << 4) | digit);
                    }
                    out.push_back(unit);
                    break;
                }
                default: fail("invalid escape");
            }
            raw_start = pos_;
        }
        fail("unterminated string");
    }
    Value read_number() {
        const size_t start = pos_;
        take('-');
        if (take('0')) {
            if (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') fail("leading zero");
        } else {
            if (pos_ == text_.size() || text_[pos_] < '1' || text_[pos_] > '9') fail("invalid number");
            while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') ++pos_;
        }
        if (take('.')) {
            if (pos_ == text_.size() || text_[pos_] < '0' || text_[pos_] > '9') fail("invalid fraction");
            while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') ++pos_;
        }
        if (take('e') || take('E')) {
            if (!take('+')) take('-');
            if (pos_ == text_.size() || text_[pos_] < '0' || text_[pos_] > '9') fail("invalid exponent");
            while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') ++pos_;
        }
        const std::string_view number = text_.substr(start, pos_ - start);
        double parsed = 0;
        const auto [end, error] = std::from_chars(number.data(), number.data() + number.size(),
                                                  parsed, std::chars_format::general);
        if (end != number.data() + number.size()) fail("invalid number");
        if (error == std::errc::result_out_of_range) {
            const bool negative = number.front() == '-';
            const size_t first = negative ? 1 : 0;
            const size_t exponent_at = number.find_first_of("eE");
            const size_t mantissa_end = exponent_at == std::string_view::npos ? number.size() : exponent_at;
            const size_t dot = number.find('.');
            const size_t before_dot = dot < mantissa_end ? dot - first : mantissa_end - first;
            size_t first_nonzero = 0;
            for (size_t i = first; i < mantissa_end; ++i) {
                if (number[i] == '.') continue;
                if (number[i] != '0') break;
                ++first_nonzero;
            }
            int64_t exponent = 0;
            if (exponent_at != std::string_view::npos) {
                size_t i = exponent_at + 1;
                const bool exponent_negative = i < number.size() && number[i] == '-';
                if (i < number.size() && (number[i] == '+' || number[i] == '-')) ++i;
                for (; i < number.size(); ++i) exponent = std::min<int64_t>(1000000, exponent * 10 + (number[i] - '0'));
                if (exponent_negative) exponent = -exponent;
            }
            const int64_t magnitude = static_cast<int64_t>(before_dot) - static_cast<int64_t>(first_nonzero) - 1 + exponent;
            parsed = magnitude < 0 ? 0.0 : std::numeric_limits<double>::infinity();
            if (negative) parsed = -parsed;
        } else if (error != std::errc{}) fail("invalid number");
        return Value(parsed);
    }
    Value read_value(int depth) {
        if (depth > 512) fail("JSON nesting exceeds 512 levels");
        if (pos_ == text_.size()) fail("unexpected end of input");
        if (take('{')) {
            Object object;
            skip_ws();
            if (take('}')) return Value(std::move(object));
            do {
                skip_ws();
                if (pos_ == text_.size() || text_[pos_] != '"') fail("expected object key");
                JsString key = read_string();
                skip_ws(); expect(':'); skip_ws();
                object.set(key, read_value(depth + 1));
                skip_ws();
                if (take('}')) return Value(std::move(object));
                expect(',');
            } while (true);
        }
        if (take('[')) {
            Array array;
            skip_ws();
            if (take(']')) return Value(std::move(array));
            do {
                skip_ws();
                array.push_back(read_value(depth + 1));
                skip_ws();
                if (take(']')) return Value(std::move(array));
                expect(',');
            } while (true);
        }
        if (text_[pos_] == '"') return Value(read_string());
        if (literal("true")) return Value(true);
        if (literal("false")) return Value(false);
        if (literal("null")) return Value::null();
        if (text_[pos_] == '-' || (text_[pos_] >= '0' && text_[pos_] <= '9')) return read_number();
        fail("invalid JSON value");
    }
};
}  // namespace

std::string stringify(const Value& value, int indent) {
    if (value.is_undefined() || value.is_function()) return {};
    std::string out;
    append_value(out, value, std::clamp(indent, 0, 10), 0);
    return out;
}
Value parse(std::string_view utf8) { return Parser(utf8).parse(); }
}  // namespace nm::json
