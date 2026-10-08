#include "core/edit/program_state/emitter.h"

#include "core/edit/program_state/console.h"

namespace nm::program_state {

void report_emitter_error(const JsString& event, const JsString& error) {
    program_console_error({Value(u"[Emitter] Error in " + event + u" handler:"), Value(error)});
}

}  // namespace nm::program_state
