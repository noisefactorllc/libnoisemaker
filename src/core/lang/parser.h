#pragma once
#include "core/lang/tokens.h"
#include <vector>
namespace libnoisemaker {
Value parse(const std::vector<Token>& tokens);
Value parse(const std::vector<Token>& tokens, const Value& options);
}
