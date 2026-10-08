#pragma once
#include "core/lang/tokens.h"
#include <vector>
namespace nm {
std::vector<Token> lex(const JsString& src);
}
