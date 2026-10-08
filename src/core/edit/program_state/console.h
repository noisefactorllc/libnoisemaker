#pragma once

#include "core/value/value.h"

#include <functional>
#include <vector>

namespace nm {

using ProgramConsoleCallback = std::function<void(const JsString& level, const std::vector<Value>& args)>;

// The caller may replace this per thread while running one scenario.
void set_program_state_console(ProgramConsoleCallback callback);
void program_console_warn(const std::vector<Value>& args);
void program_console_error(const std::vector<Value>& args);

}  // namespace nm
