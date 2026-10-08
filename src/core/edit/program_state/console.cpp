#include "core/edit/program_state/console.h"

namespace nm {
namespace { thread_local ProgramConsoleCallback callback; }
void set_program_state_console(ProgramConsoleCallback next) { callback = std::move(next); }
void program_console_warn(const std::vector<Value>& args) { if (callback) callback(u"warn", args); }
void program_console_error(const std::vector<Value>& args) { if (callback) callback(u"error", args); }
}  // namespace nm
