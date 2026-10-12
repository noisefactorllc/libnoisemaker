#pragma once
#include "core/lang/tokens.h"
#include <vector>
namespace libnoisemaker {
std::vector<Token> lex(const JsString& src);
}
