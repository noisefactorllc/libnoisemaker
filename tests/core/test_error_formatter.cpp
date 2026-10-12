#include "check.h"
#include "core/edit/error_formatter.h"

int main() {
    libnoisemaker::Object error;
    error.set(u"$js", u"error");
    error.set(u"name", u"SyntaxError");
    error.set(u"message", u"Unexpected token DOT at line 3 col 3");
    const libnoisemaker::JsString source = u"search synth\nnoise(\n  .write(o0)\nrender(o0)";
    NM_CHECK(libnoisemaker::is_dsl_syntax_error(libnoisemaker::Value(error)));
    NM_CHECK(libnoisemaker::format_compile_error(source, libnoisemaker::Value(error)) ==
             u"SyntaxError: Unexpected token DOT\n  --> line 3, column 3\n\n"
             u"  1 | search synth\n  2 | noise(\n  3 |   .write(o0)\n"
             u"      ^-- error here\n  4 | render(o0)");
    error.set(u"name", u"TypeError");
    NM_CHECK(!libnoisemaker::is_dsl_syntax_error(libnoisemaker::Value(error)));
    NM_CHECK(libnoisemaker::format_compile_error(source, libnoisemaker::Value(error)) ==
             u"Unexpected token DOT at line 3 col 3");
    return 0;
}
