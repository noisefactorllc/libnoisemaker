#pragma once
#include <string>

namespace libnoisemaker::capi {
void set_error(std::string message);
void clear_error();
}  // namespace libnoisemaker::capi
