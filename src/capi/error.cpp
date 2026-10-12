#include "capi/error.h"

#include <noisemaker/core.h>

#include <utility>

namespace libnoisemaker::capi {
thread_local std::string last_error;

void set_error(std::string message) { last_error = std::move(message); }
void clear_error() { last_error.clear(); }
}  // namespace libnoisemaker::capi

extern "C" uint32_t nm_abi_version(void) { return NM_ABI_VERSION; }
extern "C" const char* nm_last_error(void) { return libnoisemaker::capi::last_error.c_str(); }
