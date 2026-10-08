#pragma once

#include "core/value/value.h"

namespace nm {
// Ports the reference's source-context formatter. Error values use the
// oracle's {name, message} shape; thrown plain values are also accepted.
JsString format_dsl_error(const Value& source, const Value& error,
                          const Value& options = Value());
bool is_dsl_syntax_error(const Value& error);
JsString format_compile_error(const JsString& source, const Value& error);
}  // namespace nm
