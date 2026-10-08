// Unit tests for nm::js::checkFunctionBody (qt/noisemaker/compiler/
// js_syntax.{h,cpp}): whether V8's `new Function('state', body)` accepts a
// body. Plain assert-style checks, no test framework dependency.
//
// Each expected verdict was taken from V8 itself (`new Function('state',
// body)` in node 26.10.0 / V8 14.6 and node 24.21.0 / V8 13.6, which agree
// on every case). The cases cover the places where V8 differs from acorn,
// whose parser js_syntax.cpp ports, plus a sample of each grammar area. The
// DSL-level behaviour is oracle-gated by parity/corpus/func_*.dsl.

#include "core/js/js_syntax.h"
#include "core/value/js_string.h"

#include <string>

#include <cstdio>

namespace {

int g_failures = 0;

void check(bool condition, const nm::JsString& description) {
    std::printf("%s: %s\n", condition ? "PASS" : "FAIL", nm::js_to_utf8(description).c_str());
    if (!condition) ++g_failures;
}

struct Case {
    const char16_t* body;
    bool valid;
};

const Case kCases[] = {
        {u"f() = 1", true},
        {u"f()++", true},
        {u"--f()", true},
        {u"f() += 1", true},
        {u"f() &&= 1", false},
        {u"f() ?\?= 1", false},
        {u"[f()] = x", false},
        {u"({a: f()} = x)", false},
        {u"for (f() in x);", true},
        {u"for (f() of x);", true},
        {u"(f()) = 1", true},
        {u"new f() = 1", false},
        {u"f()?.x = 1", false},
        {u"let\ntrue", true},
        {u"let\nthis.x", true},
        {u"let\nvar x", true},
        {u"let //c\n throw x", true},
        {u"let\nx = 1", true},
        {u"let [a] = b", true},
        {u"if (1) let\nx = 1", true},
        {u"if (1) let [a] = b", false},
        {u"let let = 1", false},
        {u"{ using x = y }", true},
        {u"const x\nof = 1", false},
        {u"{ using x\nof = 1 }", false},
        {u"for (const x of y);", true},
        {u"for (const x in y);", true},
        {u"for (using x in y);", false},
        {u"async function f(){ for (await using x in y); }", false},
        {u"if (x) using y = z", false},
        {u"l: using x = y", false},
        {u"for (using x of y);", true},
        {u"async function f(){ await using x = y }", true},
        {u"let f; l: function f(){}", false},
        {u"l: function f(){}", true},
        {u"l: function* g(){}", false},
        {u"if (1) function f(){}", true},
        {u"let f; if (1) function f(){}", true},
        {u"class A { static { () => arguments } }", false},
        {u"class A { static { function f(){ arguments } } }", true},
        {u"class A { x = () => arguments }", false},
        {u"class A { static { () => await } }", true},
        {u"import.source(x)", true},
        {u"import.source(x, y)", false},
        {u"import.defer(x)", false},
        {u"import.meta", false},
        {u"import(x)", true},
        {u"import(x, y)", true},
        {u"new import(x)", false},
        {u"with(state){ return time * 4; }", true},
        {u"with(state){ return ; }", true},
        {u"with(state){ return time // c; }", false},
        {u"with(state){ return \"(\" ; } ; x = 1 ; { \")\"; }", true},
        {u"with(state){ return \"(\" ; } let state = 1 ; { \")\"; }", false},
        {u"let state = 1", false},
        {u"var state = 1", true},
        {u"{ let state = 1 }", true},
        {u"function state(){}", true},
        {u"08.5", true},
        {u"07.5", false},
        {u"08n", false},
        {u"0n", true},
        {u"1_000", true},
        {u"1__0", false},
        {u"0_1", false},
        {u"0x_1", false},
        {u"1.5n", false},
        {u"1e3n", false},
        {u".5", true},
        {u"5..a", true},
        {u"5.a", false},
        {u"0b12", false},
        {u"0o8", false},
        {u"3in x", false},
        {u"\"\\8\"", true},
        {u"\"use strict\"; \"\\8\"", false},
        {u"\"use strict\"; 08", false},
        {u"\"\\01\"; \"use strict\";", false},
        {u"\"\\x4\"", false},
        {u"\"\\u{110000}\"", false},
        {u"\"a\u2028b\"", true},
        {u"`\\u`", false},
        {u"tag`\\u`", true},
        {u"`\\01`", false},
        {u"`${a}`", true},
        {u"`${a`", false},
        {u"\\u0061 = 1", true},
        {u"var \\u0069f", false},
        {u"l\\u0065t x = 1", false},
        {u"x\u200c = 1", true},
        {u"\u00e9 = 1", true},
        {u"\U0001d431 = 1", true},
        {u"#x", false},
        {u"a\u0085", false},
        {u"<!-- x", true},
        {u"x = 1 /*\n*/ --> y", true},
        {u"x = 1 /* */ --> y", false},
        {u"--> x", true},
        {u"/(/", false},
        {u"/a/gg", false},
        {u"/a/uv", false},
        {u"/\\p{L}/u", true},
        {u"/\\p{Script=Garay}/u", true},
        {u"/\\p{sc=Hrkt}/u", false},
        {u"/\\p{RGI_Emoji}/v", true},
        {u"/[\\p{L}--\\p{Lu}]/v", true},
        {u"/(?<a>x)|(?<a>y)/", true},
        {u"/(?<a>x)(?<a>y)/", false},
        {u"/(?i:a)/", true},
        {u"/(?i-i:a)/", false},
        {u"/{1}/", false},
        {u"/a{2,1}/", false},
        {u"/\\1(a)/", true},
        {u"/\\1(a)/u", true},
        {u"/]/", true},
        {u"/]/u", false},
        {u"/\\c/", true},
        {u"/\\c/u", false},
        {u"/[\\d-z]/", true},
        {u"/[\\d-z]/u", false},
        {u"break", false},
        {u"l: { break l }", true},
        {u"l: { continue l }", false},
        {u"l: while (1) { continue l }", true},
        {u"l: l: 1", false},
        {u"switch (a) { default: default: }", false},
        {u"switch (a) { case 1: let x; case 2: let x }", false},
        {u"try {} catch (e) { var e }", true},
        {u"try {} catch (e) { let e }", false},
        {u"try {} catch ([e]) { var e }", false},
        {u"try {} catch (e) { for (var e of []); }", true},
        {u"for (let x in y) { var x }", false},
        {u"for (var x = 1 in o);", true},
        {u"for (let x = 1 in o);", false},
        {u"for (let of x);", false},
        {u"for (let.x of y);", false},
        {u"for (let.x in y);", true},
        {u"for (async of x);", false},
        {u"for (async of => 1;;);", true},
        {u"for await (x of y);", false},
        {u"async function f(){ for await (x of y); }", true},
        {u"a\n++b", true},
        {u"return\nx", true},
        {u"throw\nx", false},
        {u"do x; while (0) y", true},
        {u"with (a) b", true},
        {u"\"use strict\"; with (a) b", false},
        {u"debugger", true},
        {u"(a, a) => 1", false},
        {u"function f(a, a){}", true},
        {u"function f(a, a){ \"use strict\" }", false},
        {u"(a = 1) => { \"use strict\" }", false},
        {u"(eval) => { \"use strict\" }", false},
        {u"function eval(){ \"use strict\" }", false},
        {u"async (await) => 1", false},
        {u"async function f(){ () => await }", true},
        {u"function* g(){ () => yield }", true},
        {u"function* g(){ (a = yield) => 1 }", false},
        {u"async\n() => 1", false},
        {u"async a => a", true},
        {u"async a\n=> a", false},
        {u"({a = 1})", false},
        {u"({a = 1} = x)", true},
        {u"({__proto__: 1, __proto__: 2})", false},
        {u"({__proto__: 1, __proto__: 2} = x)", false},
        {u"({ get x(a){} })", false},
        {u"({ set x(...a){} })", false},
        {u"({ m(){ super.x } })", true},
        {u"({ m: function(){ super.x } })", false},
        {u"class A { #x; #x }", false},
        {u"class A { get #x(){} set #x(v){} }", true},
        {u"class A { static get #x(){} set #x(v){} }", false},
        {u"class A { m(){ this.#y } }", false},
        {u"class A { #x; m(){ delete this.#x } }", false},
        {u"class A { #x; m(){ #x in this } }", true},
        {u"class A { constructor(){} constructor(){} }", false},
        {u"class A { constructor(){ super() } }", false},
        {u"class A extends B { constructor(){ super() } }", true},
        {u"class A { static prototype(){} }", false},
        {u"class A { constructor = 1 }", false},
        {u"a ?\? b || c", false},
        {u"(a ?\? b) || c", true},
        {u"-a ** 2", false},
        {u"(-a) ** 2", true},
        {u"typeof a ** 2", false},
        {u"a?.b`c`", false},
        {u"new a?.b()", false},
        {u"delete a", true},
        {u"\"use strict\"; delete a", false},
        {u"\"use strict\"; delete (a)", false},
        {u"new.target", true},
        {u"super.x", false},
        {u"super()", false},
        // Identifier code points (each verdict from node 24.21.0/26.10.0
        // V8, re-verified 2026-09-26 under node 24.10.0 and 26.5.1):
        // capital sharp S is ID_Start; ZWNJ and ZWJ are IdentifierPart
        // (but not IdentifierStart); astral letters work; lone surrogates
        // and U+180E (Mongolian vowel separator, Cf, not ID_Continue
        // since Unicode 6.3) do not.
        {u"Co\u1e9e", true},
        {u"A\u200cb", true},
        {u"A\u200db", true},
        {u"a\u200cb\u200dc", true},
        {u"\u200ca", false},
        {u"\U0001D49Eb", true},
        {u"x\U0001D49E", true},
        // A lone surrogate (u"\ud800" is not a valid C++ UCN; the verdict is
        // Invalid) is covered by the differential corpus instead.
        {u"\u180e", false},
        {u"a\u180e", false},
};

} // namespace

int main() {
    for (const Case& c : kCases) {
        const nm::JsString body = nm::JsString(c.body);
        const nm::js::CheckResult r = nm::js::checkFunctionBody(nm::JsString(u"state"), body);
        const nm::js::Verdict expected = c.valid ? nm::js::Verdict::Valid : nm::js::Verdict::Invalid;
        check(r.verdict == expected, body + (c.valid ? u" -> valid " : u" -> invalid ") + r.detail);
    }

    // Nesting deeper than the checker follows is not decided (V8 itself
    // throws RangeError on deep enough input, at a stack-dependent depth).
    const nm::JsString deep = nm::JsString(300, u'(') + nm::JsString(u"a") + nm::JsString(300, u')');
    check(nm::js::checkFunctionBody(nm::JsString(u"state"), deep).verdict == nm::js::Verdict::Unsupported,
          nm::JsString(u"300 nested parentheses -> unsupported"));
    const nm::JsString deepRegex = nm::JsString(u"x = /") + nm::JsString(500, u'(') + nm::JsString(u"a")
                              + nm::JsString(500, u')') + nm::JsString(u"/");
    check(nm::js::checkFunctionBody(nm::JsString(u"state"), deepRegex).verdict == nm::js::Verdict::Unsupported,
          nm::JsString(u"500 nested regular-expression groups -> unsupported"));
    nm::JsString longChain = nm::JsString(u"x = a");
    for (int i = 0; i < 5000; ++i) longChain += nm::JsString(u" + a");
    check(nm::js::checkFunctionBody(nm::JsString(u"state"), longChain).verdict == nm::js::Verdict::Valid,
          nm::JsString(u"a 5000-term sum stays decidable (left-associative chains do not nest)"));

    if (g_failures == 0) {
        std::printf("ALL PASS (test_js_syntax)\n");
        return 0;
    }
    std::printf("%d FAILURE(S) (test_js_syntax)\n", g_failures);
    return 1;
}
