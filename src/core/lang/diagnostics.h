#pragma once
#include "core/lang/compat.h"
#include <stdexcept>
namespace nm {
class DslSyntaxError : public std::runtime_error {
public:
    explicit DslSyntaxError(const JsString& message, int line = -1, int col = -1,
                            Value diagnostic = Value())
      : std::runtime_error(js_to_utf8(message)), message_(message), line_(line), col_(col), diagnostic_(std::move(diagnostic)) {}
    static DslSyntaxError at(const JsString& core, int line, int col);
    const JsString& message() const { return message_; }
    int line() const { return line_; }
    int col() const { return col_; }
    const Value& diagnostic() const { return diagnostic_; }
private:
    JsString message_;
    int line_, col_;
    Value diagnostic_;
};
JsString diagStage(const JsString& code);
JsString diagSeverity(const JsString& code);
JsString diagDefaultMessage(const JsString& code);
}
