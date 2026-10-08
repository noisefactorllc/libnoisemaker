#pragma once
#include <string>

namespace nm::capi {
void set_error(std::string message);
void clear_error();
}  // namespace nm::capi
