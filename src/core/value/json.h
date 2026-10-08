#pragma once

#include "core/value/value.h"

#include <stdexcept>
#include <string>
#include <string_view>

namespace nm::json {
class ParseError : public std::runtime_error {
 public:
    using std::runtime_error::runtime_error;
};

std::string stringify(const Value& value, int indent = 0);
Value parse(std::string_view utf8);
}  // namespace nm::json
