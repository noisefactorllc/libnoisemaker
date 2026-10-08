#include "core/lang/diagnostics.h"
#include <unordered_map>
namespace nm {
namespace {
struct DiagnosticInfo { const char* message; const char* severity; const char* stage; };
const std::unordered_map<JsString, DiagnosticInfo>& table() {
    static const std::unordered_map<JsString, DiagnosticInfo> t = {
        {u"L001", {"Unexpected character", "error", "lexer"}},
        {u"L002", {"Unterminated string literal", "error", "lexer"}},
        {u"L003", {"Unterminated comment", "error", "lexer"}},
        {u"L004", {"Output surface reference out of range", "error", "lexer"}},
        {u"P001", {"Unexpected token", "error", "parser"}},
        {u"P002", {"Expected closing parenthesis", "error", "parser"}},
        {u"P003", {"Invalid automation arguments", "error", "parser"}},
        {u"P004", {"Invalid search directive", "error", "parser"}},
        {u"P005", {"Invalid output operation", "error", "parser"}},
        {u"P006", {"Invalid subchain", "error", "parser"}},
        {u"P007", {"Invalid call expression", "error", "parser"}},
        {u"P008", {"Unknown subchain argument key", "warning", "parser"}},
        {u"P009", {"Duplicate subchain argument key", "warning", "parser"}},
        {u"P010", {"Missing ',' between subchain arguments", "warning", "parser"}},
        {u"S001", {"Unknown identifier", "error", "semantic"}},
        {u"S002", {"Argument out of range", "warning", "semantic"}},
        {u"S003", {"Variable used before assignment", "error", "semantic"}},
        {u"S004", {"Cannot assign null or undefined", "error", "semantic"}},
        {u"S005", {"Illegal chain structure", "error", "semantic"}},
        {u"S006", {"Starter chain missing write() call", "error", "semantic"}},
        {u"S007", {"Deprecated parameter alias", "warning", "semantic"}},
        {u"S008", {"Deprecated effect", "warning", "semantic"}},
        {u"R001", {"Runtime error", "error", "runtime"}},
    };
    return t;
}
}
DslSyntaxError DslSyntaxError::at(const JsString& core, int line, int col) {
    return DslSyntaxError(QStringLiteral("%1 at line %2 col %3").arg(core).arg(line).arg(col), line, col);
}
JsString diagStage(const JsString& code) { auto it = table().find(code); return it == table().end() ? u"unknown" : utf8_to_js(it->second.stage); }
JsString diagSeverity(const JsString& code) { auto it = table().find(code); return it == table().end() ? u"error" : utf8_to_js(it->second.severity); }
JsString diagDefaultMessage(const JsString& code) { auto it = table().find(code); return it == table().end() ? u"" : utf8_to_js(it->second.message); }
}
