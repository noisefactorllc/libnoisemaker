#include "check.h"
#include "core/edit/error_formatter.h"

int main() {
    nm::Object error;
    error.set(u"$js", u"error");
    error.set(u"name", u"SyntaxError");
    error.set(u"message", u"Unexpected token DOT at line 3 col 3");
    const nm::JsString source = u"search synth\nnoise(\n  .write(o0)\nrender(o0)";
    NM_CHECK(nm::is_dsl_syntax_error(nm::Value(error)));
    NM_CHECK(nm::format_compile_error(source, nm::Value(error)) ==
             u"SyntaxError: Unexpected token DOT\n  --> line 3, column 3\n\n"
             u"  1 | search synth\n  2 | noise(\n  3 |   .write(o0)\n"
             u"      ^-- error here\n  4 | render(o0)");
    error.set(u"name", u"TypeError");
    NM_CHECK(!nm::is_dsl_syntax_error(nm::Value(error)));
    NM_CHECK(nm::format_compile_error(source, nm::Value(error)) ==
             u"Unexpected token DOT at line 3 col 3");
    return 0;
}
