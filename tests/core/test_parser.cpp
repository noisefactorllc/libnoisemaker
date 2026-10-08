// Unit tests for nm::parse (core/lang/parser.{h,cpp}).
// Plain assert-style checks, no test framework dependency (matches
// test_graph_load.cpp / test_lexer.cpp convention).
//
// Covers the T8 brief's named parser basics (chain, subchain,
// named/positional exclusivity, vec/bracket literals) plus the other
// hazards read out of shaders/src/lang/parser.js and empirically
// cross-checked against the reference oracle (NM_REFERENCE_ROOT
// tools/dump-ast.mjs) before being hard-coded -- see task report.
//
// RED (before parser.cpp has a real definition): this binary fails to
// LINK ("undefined symbols: parseJson(...)"). GREEN: all checks below
// print PASS and the process exits 0.

#include "core/lang/ast.h"
#include "core/lang/diagnostics.h"
#include "core/lang/lexer.h"
#include "core/lang/parser.h"


#include <utility>

#include <cmath>
#include <cstdio>

using namespace nm;
namespace {
JsonArray lexJson(const JsString& src) {
    JsonArray out;
    for (const auto& token : lex(src)) out.append(toJson(token));
    return out;
}
JsonObject parseJson(const JsonArray& arr, const JsonObject& options = JsonObject()) {
    std::vector<Token> tokens;
    for (const auto& entry : arr) tokens.push_back(tokenFromJson(entry.native()));
    return JsonValue(parse(tokens, options.native())).toObject();
}


int g_failures = 0;

void check(bool condition, const char* description) {
    if (condition) {
        std::printf("PASS: %s\n", description);
    } else {
        std::printf("FAIL: %s\n", description);
        ++g_failures;
    }
}

JsonObject parseSrc(const LangString& src) {
    return parseJson(lexJson(src));
}

bool numEq(double a, double b) {
    return std::abs(a - b) <= 1e-12 * std::max({1.0, std::abs(a), std::abs(b)});
}

} // namespace

int main() {
    // ==================================================================
    // chain
    // ==================================================================
    {
        const JsonObject prog = parseSrc(QStringLiteral("search synth\nsolid(0.1,0.2,0.3).write(o0)\nrender(o0)\n"));
        check(prog.value(QStringLiteral("type")).toString() == nm::NodeKind::Program, "Program root type");
        const JsonArray plans = prog.value(QStringLiteral("plans")).toArray();
        check(plans.size() == 1, "one plan (chain statement)");
        const JsonObject stmt = plans.at(0).toObject();
        check(!stmt.contains(QStringLiteral("type")), "chain-statement wrapper has NO type key");
        const JsonArray chain = stmt.value(QStringLiteral("chain")).toArray();
        check(chain.size() == 2, "chain has 2 elements: Call + Write");
        check(chain.at(0).toObject().value(QStringLiteral("type")).toString() == nm::NodeKind::Call, "chain[0] is Call");
        check(chain.at(0).toObject().value(QStringLiteral("name")).toString() == QStringLiteral("solid"),
              "chain[0].name == solid");
        check(chain.at(1).toObject().value(QStringLiteral("type")).toString() == nm::NodeKind::Write, "chain[1] is Write");
        check(stmt.value(QStringLiteral("write")).toObject().value(QStringLiteral("name")).toString()
                  == QStringLiteral("o0"),
              "top-level write shortcut == chain's terminal Write surface");
        check(stmt.value(QStringLiteral("write3d")).isNull(), "write3d is null when chain has no write3d");
        check(prog.value(QStringLiteral("render")).toObject().value(QStringLiteral("name")).toString()
                  == QStringLiteral("o0"),
              "render(o0) parsed");

        const JsonObject nsMeta = prog.value(QStringLiteral("namespace")).toObject();
        check(nsMeta.value(QStringLiteral("searchOrder")).toArray().size() == 1
                  && nsMeta.value(QStringLiteral("searchOrder")).toArray().at(0).toString() == QStringLiteral("synth"),
              "namespace.searchOrder == ['synth']");
        check(nsMeta.value(QStringLiteral("default")).toObject().value(QStringLiteral("name")).toString()
                  == QStringLiteral("synth"),
              "namespace.default.name == synth");
    }
    {
        // Only the LAST write()/write3d() in a chain sets the top-level
        // shortcut; every write() call still appears as its own chain
        // element regardless of position (empirically verified).
        const JsonObject prog =
            parseSrc(QStringLiteral("search synth,filter\nnoise().write(o0).invert().write(o1)\nrender(o1)\n"));
        const JsonArray chain = prog.value(QStringLiteral("plans")).toArray().at(0).toObject().value(QStringLiteral("chain")).toArray();
        check(chain.size() == 4, "mid-chain write: 4 chain elements (Call,Write,Call,Write)");
        check(chain.at(1).toObject().value(QStringLiteral("type")).toString() == nm::NodeKind::Write, "chain[1] mid-chain Write present");
        const JsonObject stmt = prog.value(QStringLiteral("plans")).toArray().at(0).toObject();
        check(stmt.value(QStringLiteral("write")).toObject().value(QStringLiteral("name")).toString() == QStringLiteral("o1"),
              "top-level write reflects the LAST write in the chain, not the first");
    }
    {
        // Comments are legal around chain dots (leadingComments), but NOT
        // inside a block before a statement (parser has no collectComments
        // call there) -- verified against the oracle: this throws.
        bool threw = false;
        try {
            parseSrc(QStringLiteral("search synth\nif (1) {\n  // comment\n  solid(0.1,0.2,0.3).write(o0)\n}\nrender(o0)\n"));
        } catch (const nm::DslSyntaxError&) {
            threw = true;
        }
        check(threw, "a comment immediately before a statement INSIDE a block is a syntax error (not collected there)");
    }
    {
        const JsonObject prog = parseSrc(QStringLiteral(
            "search synth\n// leading comment\nsolid(0.1,0.2,0.3)\n  // pre-dot comment\n  .write(o0)\n// trailing comment\nrender(o0)\n// after render\n"));
        const JsonObject stmt = prog.value(QStringLiteral("plans")).toArray().at(0).toObject();
        check(stmt.value(QStringLiteral("leadingComments")).toArray().size() == 1, "top-level statement leadingComments captured");
        const JsonArray chain = stmt.value(QStringLiteral("chain")).toArray();
        check(chain.at(1).toObject().value(QStringLiteral("leadingComments")).toArray().at(0).toString()
                  == QStringLiteral("// pre-dot comment"),
              "pre-dot comment attaches to the following chain element");
        check(prog.value(QStringLiteral("render")).toObject().value(QStringLiteral("leadingComments")).toArray().size() == 1,
              "comment immediately before render() attaches to the render node");
        check(prog.value(QStringLiteral("trailingComments")).toArray().at(0).toString() == QStringLiteral("// after render"),
              "comment after render() lands in Program.trailingComments");
    }

    // ==================================================================
    // subchain
    // ==================================================================
    {
        const JsonObject prog = parseSrc(
            QStringLiteral("search filter\nnoise().subchain(name: \"fx\", id: \"sc1\") { .invert() }.write(o0)\nrender(o0)\n"));
        const JsonArray chain = prog.value(QStringLiteral("plans")).toArray().at(0).toObject().value(QStringLiteral("chain")).toArray();
        check(chain.size() == 3, "chain: Call, Subchain, Write");
        const JsonObject sub = chain.at(1).toObject();
        check(sub.value(QStringLiteral("type")).toString() == nm::NodeKind::Subchain, "subchain node type");
        check(sub.value(QStringLiteral("name")).toString() == QStringLiteral("fx"), "subchain kwarg name");
        check(sub.value(QStringLiteral("id")).toString() == QStringLiteral("sc1"), "subchain kwarg id");
        check(sub.value(QStringLiteral("body")).toArray().size() == 1, "subchain body has 1 call");
        check(sub.value(QStringLiteral("body")).toArray().at(0).toObject().value(QStringLiteral("name")).toString()
                  == QStringLiteral("invert"),
              "subchain body call name");
        check(sub.contains(QStringLiteral("loc")), "subchain has a loc");
    }
    {
        // Positional STRING shorthand for name -- same code path as the
        // name: kwarg (verified against the oracle).
        const JsonObject prog =
            parseSrc(QStringLiteral("search filter\nnoise().subchain(\"myname\") { .invert() }.write(o0)\nrender(o0)\n"));
        const JsonObject sub = prog.value(QStringLiteral("plans")).toArray().at(0).toObject().value(QStringLiteral("chain")).toArray().at(1).toObject();
        check(sub.value(QStringLiteral("name")).toString() == QStringLiteral("myname"), "positional string -> subchain.name");
        check(sub.value(QStringLiteral("id")).isNull(), "id defaults to null when omitted");
    }
    {
        // JS falsy-OR quirk (`kwargs.name?.value || null`): an explicitly
        // empty-string name/id STILL becomes JSON null, not "". Verified
        // against the reference oracle -- do not "fix" this.
        const JsonObject prog = parseSrc(
            QStringLiteral("search filter\nnoise().subchain(name: \"\", id: \"x\") { .invert() }.write(o0)\nrender(o0)\n"));
        const JsonObject sub = prog.value(QStringLiteral("plans")).toArray().at(0).toObject().value(QStringLiteral("chain")).toArray().at(1).toObject();
        check(sub.value(QStringLiteral("name")).isNull(), "subchain name: \"\" (empty string) becomes null (JS falsy-OR quirk, ported verbatim)");
        check(sub.value(QStringLiteral("id")).toString() == QStringLiteral("x"), "non-empty id kwarg unaffected");
    }
    {
        bool threw = false;
        try {
            parseSrc(QStringLiteral("search filter\nnoise().subchain() {}.write(o0)\nrender(o0)\n"));
        } catch (const nm::DslSyntaxError&) {
            threw = true;
        }
        check(threw, "empty subchain body throws DslSyntaxError");
    }

    // ==================================================================
    // named/positional exclusivity
    // ==================================================================
    {
        bool threw = false;
        try {
            parseSrc(QStringLiteral("search synth\nsolid(a: 1, 2).write(o0)\nrender(o0)\n"));
        } catch (const nm::DslSyntaxError&) {
            threw = true;
        }
        check(threw, "keyword arg followed by positional arg throws (cannot mix)");
    }
    {
        bool threw = false;
        try {
            parseSrc(QStringLiteral("search synth\nsolid(1, a: 2).write(o0)\nrender(o0)\n"));
        } catch (const nm::DslSyntaxError&) {
            threw = true;
        }
        check(threw, "positional arg followed by keyword arg throws (cannot mix)");
    }
    {
        const JsonObject prog = parseSrc(QStringLiteral("search synth\nsolid(r: 0.1, g: 0.2, b: 0.3).write(o0)\nrender(o0)\n"));
        const JsonObject call = prog.value(QStringLiteral("plans")).toArray().at(0).toObject().value(QStringLiteral("chain")).toArray().at(0).toObject();
        check(call.value(QStringLiteral("args")).toArray().isEmpty(), "all-kwargs call has an empty args array");
        check(call.value(QStringLiteral("kwargs")).toObject().value(QStringLiteral("g")).toObject().value(QStringLiteral("value")).toDouble() == 0.2,
              "kwargs preserved by name");
    }

    // ==================================================================
    // vec / bracket literals
    // ==================================================================
    {
        const JsonObject prog = parseSrc(QStringLiteral("search synth\nlet x = [1, 2, 3]\nsolid(0.1,0.2,0.3).write(o0)\nrender(o0)\n"));
        const JsonArray vars = prog.value(QStringLiteral("vars")).toArray();
        check(vars.size() == 1, "one var");
        const JsonObject expr = vars.at(0).toObject().value(QStringLiteral("expr")).toObject();
        check(expr.value(QStringLiteral("type")).toString() == nm::NodeKind::ArrayLiteral, "[1,2,3] -> ArrayLiteral");
        const JsonArray elements = expr.value(QStringLiteral("elements")).toArray();
        check(elements.size() == 3, "3 elements");
        check(elements.at(0).toObject().value(QStringLiteral("value")).toDouble() == 1
                  && elements.at(2).toObject().value(QStringLiteral("value")).toDouble() == 3,
              "element values in order");
        check(expr.contains(QStringLiteral("loc")), "ArrayLiteral carries a loc (position of '[')");
    }
    {
        bool threw = false;
        try {
            parseSrc(QStringLiteral("search synth\nlet x = [1, 2\nsolid(0.1,0.2,0.3).write(o0)\nrender(o0)\n"));
        } catch (const nm::DslSyntaxError&) {
            threw = true;
        }
        check(threw, "unterminated array literal (missing ']') throws");
    }
    {
        const JsonObject prog = parseSrc(QStringLiteral("search synth\nlet x = []\nsolid(0.1,0.2,0.3).write(o0)\nrender(o0)\n"));
        const JsonObject expr = prog.value(QStringLiteral("vars")).toArray().at(0).toObject().value(QStringLiteral("expr")).toObject();
        check(expr.value(QStringLiteral("elements")).toArray().isEmpty(), "empty array literal []");
    }

    // ==================================================================
    // numeric constant folding (parse-time, double)
    // ==================================================================
    {
        const JsonObject prog = parseSrc(
            QStringLiteral("search synth\nlet a = -5 + 3\nlet b = -(2+3)*4\nlet c = 0.1 + 0.2\nlet d = ---5\nsolid(a,b,c).write(o0)\nrender(o0)\n"));
        const JsonArray vars = prog.value(QStringLiteral("vars")).toArray();
        check(numEq(vars.at(0).toObject().value(QStringLiteral("expr")).toObject().value(QStringLiteral("value")).toDouble(), -2.0),
              "-5 + 3 == -2 (unary binds tighter than the following additive op)");
        check(numEq(vars.at(1).toObject().value(QStringLiteral("expr")).toObject().value(QStringLiteral("value")).toDouble(), -20.0),
              "-(2+3)*4 == -20");
        const double c = vars.at(2).toObject().value(QStringLiteral("expr")).toObject().value(QStringLiteral("value")).toDouble();
        check(c == 0.30000000000000004, "0.1 + 0.2 keeps the exact IEEE double artifact 0.30000000000000004 (double end-to-end)");
        check(numEq(vars.at(3).toObject().value(QStringLiteral("expr")).toObject().value(QStringLiteral("value")).toDouble(), -5.0),
              "---5 == -5 (odd number of unary minuses)");
    }
    {
        const JsonObject prog = parseSrc(QStringLiteral("search synth\nlet x = Math.PI\nsolid(0.1,0.2,0.3).write(o0)\nrender(o0)\n"));
        const double pi = prog.value(QStringLiteral("vars")).toArray().at(0).toObject().value(QStringLiteral("expr")).toObject().value(QStringLiteral("value")).toDouble();
        check(pi == 3.141592653589793, "Math.PI folds to the exact reference double literal");
    }
    {
        // HEX -> Color: int(pair,16)/255 in double; 3-digit duplication;
        // alpha defaults to 1.0; 8-digit form carries alpha.
        const JsonObject prog = parseSrc(QStringLiteral("search synth\nlet a = #fff\nlet b = #8040c0\nlet c = #8040c080\nsolid(0.1,0.2,0.3).write(o0)\nrender(o0)\n"));
        const JsonArray vars = prog.value(QStringLiteral("vars")).toArray();
        const JsonObject colA = vars.at(0).toObject().value(QStringLiteral("expr")).toObject();
        check(colA.value(QStringLiteral("type")).toString() == nm::NodeKind::Color, "#fff -> Color");
        const JsonArray a = colA.value(QStringLiteral("value")).toArray();
        check(numEq(a.at(0).toDouble(), 1.0) && numEq(a.at(3).toDouble(), 1.0), "#fff -> [1,1,1,1] (3-digit duplication, alpha default 1)");
        const JsonArray b = vars.at(1).toObject().value(QStringLiteral("expr")).toObject().value(QStringLiteral("value")).toArray();
        check(numEq(b.at(0).toDouble(), 0x80 / 255.0) && numEq(b.at(1).toDouble(), 0x40 / 255.0) && numEq(b.at(2).toDouble(), 0xc0 / 255.0)
                  && numEq(b.at(3).toDouble(), 1.0),
              "#8040c0 -> rgb/255, alpha defaults to 1");
        const JsonArray c = vars.at(2).toObject().value(QStringLiteral("expr")).toObject().value(QStringLiteral("value")).toArray();
        check(numEq(c.at(3).toDouble(), 0x80 / 255.0), "#8040c080 -> alpha channel is the 4th hex pair / 255");
    }

    // ==================================================================
    // osc() 4-way disambiguation heuristic
    // ==================================================================
    {
        // isBareOsc: osc() with zero args/kwargs -> Oscillator with ALL defaults.
        const JsonObject prog = parseSrc(QStringLiteral("search synth\nsolid(osc(), 0.2, 0.3).write(o0)\nrender(o0)\n"));
        const JsonObject arg0 = prog.value(QStringLiteral("plans")).toArray().at(0).toObject().value(QStringLiteral("chain")).toArray().at(0).toObject().value(QStringLiteral("args")).toArray().at(0).toObject();
        check(arg0.value(QStringLiteral("type")).toString() == nm::NodeKind::Oscillator, "bare osc() -> Oscillator");
        check(arg0.value(QStringLiteral("oscType")).toObject().value(QStringLiteral("path")).toArray().at(1).toString() == QStringLiteral("sine"),
              "default oscType is oscKind.sine");
        check(numEq(arg0.value(QStringLiteral("max")).toObject().value(QStringLiteral("value")).toDouble(), 1.0), "default max is 1");
    }
    {
        // A single positional NON-oscKind-Member arg with no kwargs matches
        // none of the 4 transform conditions -> falls through as a plain
        // Call named "osc" (the synth.osc generator effect, not the value
        // oscillator). Verified against the oracle.
        const JsonObject prog = parseSrc(QStringLiteral("search synth\nlet x = osc(0.2)\nsolid(0.1,0.2,0.3).write(o0)\nrender(o0)\n"));
        const JsonObject expr = prog.value(QStringLiteral("vars")).toArray().at(0).toObject().value(QStringLiteral("expr")).toObject();
        check(expr.value(QStringLiteral("type")).toString() == nm::NodeKind::Call, "osc(0.2) (bare positional number, no kwargs) stays a plain Call");
        check(expr.value(QStringLiteral("name")).toString() == QStringLiteral("osc"), "Call name is osc");
    }
    {
        const JsonObject prog = parseSrc(QStringLiteral("search synth\nlet x = osc(type: oscKind.noise, min: 0.09, max: 0.46, speed: 3, seed: 5701)\nsolid(0.1,0.2,0.3).write(o0)\nrender(o0)\n"));
        const JsonObject expr = prog.value(QStringLiteral("vars")).toArray().at(0).toObject().value(QStringLiteral("expr")).toObject();
        check(expr.value(QStringLiteral("type")).toString() == nm::NodeKind::Oscillator, "osc(type: ..., ...) (all-osc-kwargs) -> Oscillator");
        check(numEq(expr.value(QStringLiteral("seed")).toObject().value(QStringLiteral("value")).toDouble(), 5701.0), "osc kwarg seed resolved");
    }
    {
        bool threw = false;
        try {
            parseSrc(QStringLiteral("search synth\nlet x = osc(type: oscKind.noise, bogus: 1)\nsolid(0.1,0.2,0.3).write(o0)\nrender(o0)\n"));
        } catch (const nm::DslSyntaxError&) {
            threw = true;
        }
        check(threw, "osc() with an unknown kwarg name throws");
    }

    // ==================================================================
    // midi() / audio() / from() / read() / read3d()
    // ==================================================================
    {
        // NOTE: channel/mode/sensitivity must be ALL-keyword here -- a
        // positional channel followed by keyword mode/sensitivity would hit
        // the same "cannot mix positional and keyword" rule tested above
        // (verified against the oracle: midi(2, mode: ..., sensitivity: ...)
        // throws at parseCall(), before the midi-specific transform ever runs).
        const JsonObject prog = parseSrc(
            QStringLiteral("search synth\nlet x = midi(channel: 2, mode: midiMode.trigger, sensitivity: 0.5)\nsolid(0.1,0.2,0.3).write(o0)\nrender(o0)\n"));
        const JsonObject midi = prog.value(QStringLiteral("vars")).toArray().at(0).toObject().value(QStringLiteral("expr")).toObject();
        check(midi.value(QStringLiteral("type")).toString() == nm::NodeKind::Midi, "midi() -> Midi node");
        check(numEq(midi.value(QStringLiteral("channel")).toObject().value(QStringLiteral("value")).toDouble(), 2.0), "midi kwarg channel resolved");
        check(midi.value(QStringLiteral("mode")).toObject().value(QStringLiteral("path")).toArray().at(1).toString() == QStringLiteral("trigger"),
              "midi kwarg mode resolved");
    }
    {
        bool threw = false;
        try {
            parseSrc(QStringLiteral("search synth\nlet x = midi()\nsolid(0.1,0.2,0.3).write(o0)\nrender(o0)\n"));
        } catch (const nm::DslSyntaxError&) {
            threw = true;
        }
        check(threw, "midi() with no channel throws (channel is required)");
    }
    {
        const JsonObject prog = parseSrc(QStringLiteral("search synth\nlet x = audio(audioBand.low)\nsolid(0.1,0.2,0.3).write(o0)\nrender(o0)\n"));
        const JsonObject audio = prog.value(QStringLiteral("vars")).toArray().at(0).toObject().value(QStringLiteral("expr")).toObject();
        check(audio.value(QStringLiteral("type")).toString() == nm::NodeKind::Audio, "audio() -> Audio node");
        check(audio.value(QStringLiteral("band")).toObject().value(QStringLiteral("path")).toArray().at(1).toString() == QStringLiteral("low"),
              "audio positional band resolved");
    }
    {
        const JsonObject prog = parseSrc(QStringLiteral(
            "search synth\nlet x = midi(channel: 2, midiMode.trigger, 0.25, 0.75, 0.5, name: \"Controller\", id: \"port-a\")\n"
            "solid(0.1,0.2,0.3).write(o0)\nrender(o0)\n"));
        const JsonObject midi = prog.value(QStringLiteral("vars")).toArray().at(0).toObject().value(QStringLiteral("expr")).toObject();
        check(midi.value(QStringLiteral("mode")).toObject().value(QStringLiteral("path")).toArray().at(1).toString()
                  == QStringLiteral("trigger"),
              "midi dense positionals fill fields skipped by kwargs");
        check(numEq(midi.value(QStringLiteral("min")).toObject().value(QStringLiteral("value")).toDouble(), 0.25)
                  && numEq(midi.value(QStringLiteral("max")).toObject().value(QStringLiteral("value")).toDouble(), 0.75),
              "midi mixed positional min/max resolved");
        check(midi.value(QStringLiteral("name")).toObject().value(QStringLiteral("value")).toString()
                  == QStringLiteral("Controller")
                  && midi.value(QStringLiteral("id")).toObject().value(QStringLiteral("value")).toString()
                         == QStringLiteral("port-a"),
              "midi device identity retained");
    }
    {
        const JsonObject prog = parseSrc(QStringLiteral(
            "search synth\nlet x = audio(band: audioBand.raw, 0.25, 0.75, channel: 2, name: \"Interface\", id: \"device-b\")\n"
            "solid(0.1,0.2,0.3).write(o0)\nrender(o0)\n"));
        const JsonObject audio = prog.value(QStringLiteral("vars")).toArray().at(0).toObject().value(QStringLiteral("expr")).toObject();
        check(audio.value(QStringLiteral("band")).toObject().value(QStringLiteral("path")).toArray().at(1).toString()
                  == QStringLiteral("raw"),
              "audio raw band retained");
        check(numEq(audio.value(QStringLiteral("min")).toObject().value(QStringLiteral("value")).toDouble(), 0.25)
                  && numEq(audio.value(QStringLiteral("max")).toObject().value(QStringLiteral("value")).toDouble(), 0.75),
              "audio dense positionals fill fields skipped by kwargs");
        check(audio.value(QStringLiteral("channel")).toObject().value(QStringLiteral("value")).toDouble() == 2.0
                  && audio.value(QStringLiteral("name")).toObject().value(QStringLiteral("value")).toString()
                         == QStringLiteral("Interface")
                  && audio.value(QStringLiteral("id")).toObject().value(QStringLiteral("value")).toString()
                         == QStringLiteral("device-b"),
              "audio device identity and channel retained");
    }
    for (const LangString& source : {
             QStringLiteral("search synth\nlet x = midi(1, id: \"port-a\")\n"),
             QStringLiteral("search synth\nlet x = midi(1, midiMode.velocity, 0, 1, 1, \"Controller\")\n"),
             QStringLiteral("search synth\nlet x = midi(1, bogus: 1)\n"),
             QStringLiteral("search synth\nlet x = midi(1, name: Controller)\n"),
             QStringLiteral("search synth\nlet x = midi(1, name: \"\")\n"),
             QStringLiteral("search synth\nlet x = audio(audioBand.low, name: \"Interface\")\n"),
             QStringLiteral("search synth\nlet x = audio(audioBand.low, id: \"device-b\")\n"),
             QStringLiteral("search synth\nlet x = audio(audioBand.low, 0, 1, 2)\n"),
             QStringLiteral("search synth\nlet x = audio(audioBand.low, bogus: 1)\n"),
             QStringLiteral("search synth\nlet x = audio(audioBand.low, channel: 1, name: Interface)\n"),
             QStringLiteral("search synth\nlet x = audio(audioBand.low, channel: 1, name: \"\")\n"),
         }) {
        bool threw = false;
        try {
            parseSrc(source);
        } catch (const nm::DslSyntaxError&) {
            threw = true;
        }
        check(threw, "midi/audio selector invariants reject incomplete identity");
    }
    for (const auto& testCase : {
             std::pair{
                 QStringLiteral("search synth\nlet x = midi(1, zzz: 1, aaa: 2)\n"),
                 QStringLiteral("midi() unknown parameter 'zzz' at line 2 col 9. Valid: channel, mode, min, max, "
                                "sensitivity, name, id, cc, nrpn, zone, members")},
             std::pair{
                 QStringLiteral("search synth\nlet x = audio(audioBand.low, zzz: 1, aaa: 2)\n"),
                 QStringLiteral("audio() unknown parameter 'zzz' at line 2 col 9. Valid: band, min, max, channel, "
                                "name, id")},
         }) {
        LangString message;
        try {
            parseSrc(testCase.first);
        } catch (const nm::DslSyntaxError& error) {
            message = error.message();
        }
        check(message == testCase.second, "midi/audio reports the first unknown keyword in source order");
    }
    {
        const JsonObject prog = parseSrc(QStringLiteral("search synth\nfrom(filter, blur(amount: 5)).write(o0)\nrender(o0)\n"));
        const JsonObject call = prog.value(QStringLiteral("plans")).toArray().at(0).toObject().value(QStringLiteral("chain")).toArray().at(0).toObject();
        check(call.value(QStringLiteral("name")).toString() == QStringLiteral("blur"), "from(filter, blur(...)) replaces the call with the target");
        const JsonObject ns = call.value(QStringLiteral("namespace")).toObject();
        check(ns.value(QStringLiteral("name")).toString() == QStringLiteral("filter") && ns.value(QStringLiteral("fromOverride")).toBool(),
              "from() injects a namespace override with fromOverride:true");
    }
    {
        bool threw = false;
        try {
            parseSrc(QStringLiteral("search synth\nfoo.bar.baz()\nrender(o0)\n"));
        } catch (const nm::DslSyntaxError&) {
            threw = true;
        }
        check(threw, "a 2+-segment dotted call (foo.bar.baz()) is a syntax error, not a namespaced call (verified against oracle: 'Expect (' )");
    }
    {
        bool threw = false;
        try {
            parseSrc(QStringLiteral("search synth\nnd.noise().write(o0)\nrender(o0)\n"));
        } catch (const nm::DslSyntaxError&) {
            threw = true;
        }
        check(threw, "inline single-segment namespace call (nd.noise()) is explicitly forbidden");
    }
    {
        const JsonObject prog = parseSrc(QStringLiteral("search synth3d\nlet a = read3d(vol0)\nread3d(vol0, geo0).write3d(vol1, geo1)\nrender(o0)\n"));
        const JsonObject single = prog.value(QStringLiteral("vars")).toArray().at(0).toObject().value(QStringLiteral("expr")).toObject();
        check(single.value(QStringLiteral("type")).toString() == nm::NodeKind::Read3D, "read3d(vol0) -> Read3D");
        check(single.value(QStringLiteral("geo")).isNull(), "single-arg read3d() leaves geo == null");
        const JsonObject stmt = prog.value(QStringLiteral("plans")).toArray().at(0).toObject();
        check(stmt.value(QStringLiteral("write3d")).toObject().value(QStringLiteral("tex3d")).toObject().value(QStringLiteral("name")).toString()
                  == QStringLiteral("vol1"),
              "write3d() sets the top-level write3d shortcut {tex3d,geo} (no type key)");
    }

    // ==================================================================
    // search directive requirements
    // ==================================================================
    {
        bool threw = false;
        try {
            parseSrc(QStringLiteral("solid(0.1,0.2,0.3).write(o0)\nrender(o0)\n"));
        } catch (const nm::DslSyntaxError& e) {
            threw = true;
            check(e.line() == 3, "missing-search error has EOF location line 3");
            check(!LangString(e.message()).contains(QStringLiteral("at line")), "missing-search error has no 'at line' suffix");
            check(JsonValue(e.diagnostic()).toObject().value(QStringLiteral("code")).toString() == QStringLiteral("P004"), "missing-search diagnostic code is P004");
        }
        check(threw, "missing search directive throws");
    }
    {
        bool threw = false;
        try {
            parseSrc(QStringLiteral("search bogus\nsolid(0.1,0.2,0.3).write(o0)\nrender(o0)\n"));
        } catch (const nm::DslSyntaxError&) {
            threw = true;
        }
        check(threw, "invalid namespace name throws");
    }
    {
        bool threw = false;
        try {
            parseSrc(QStringLiteral("search synth\nsearch filter\nsolid(0.1,0.2,0.3).write(o0)\nrender(o0)\n"));
        } catch (const nm::DslSyntaxError&) {
            threw = true;
        }
        check(threw, "a second search directive throws");
    }
    {
        bool threw = false;
        try {
            parseSrc(QStringLiteral("search synth\nsolid(0.1,0.2,0.3).write(o0)\nsearch filter\nrender(o0)\n"));
        } catch (const nm::DslSyntaxError&) {
            threw = true;
        }
        check(threw, "a search directive after other statements throws");
    }

    // ==================================================================
    // write() surface variants
    // ==================================================================
    {
        const JsonObject prog = parseSrc(QStringLiteral(
            "search synth\nnoise().write(xyz0)\nnoise().write(vel1)\nnoise().write(rgba2)\nnoise().write(mesh3)\nnoise().write(none)\nrender(o0)\n"));
        const JsonArray plans = prog.value(QStringLiteral("plans")).toArray();
        check(plans.size() == 5, "5 chain statements");
        const struct { const char* type; const char* name; } expected[5] = {
            {"XyzRef", "xyz0"}, {"VelRef", "vel1"}, {"RgbaRef", "rgba2"}, {"MeshRef", "mesh3"}, {"OutputRef", "none"},
        };
        bool allMatch = true;
        for (int i = 0; i < 5; ++i) {
            const JsonObject w = plans.at(i).toObject().value(QStringLiteral("write")).toObject();
            allMatch = allMatch && w.value(QStringLiteral("type")).toString() == LangString::fromUtf8(expected[i].type)
                       && w.value(QStringLiteral("name")).toString() == LangString::fromUtf8(expected[i].name);
        }
        check(allMatch, "write() accepts xyz/vel/rgba/mesh refs and the literal 'none' ident");
    }

    // ==================================================================
    // Structured parser expectation diagnostics (P001 / P002)
    // ==================================================================
    {
        struct ExpectCase {
            const char* name;
            LangString source;
            LangString code;
            LangString message;
            int line;
            int column;
        };

        const QVector<ExpectCase> cases = {
            {"opening parenthesis", QStringLiteral("search synth\nrender o0"), QStringLiteral("P001"), QStringLiteral("Expect '(' at line 2 col 8"), 2, 8},
            {"closing parenthesis at EOF", QStringLiteral("search synth\nrender(o0"), QStringLiteral("P002"), QStringLiteral("Expect ')' at line 2 col 10"), 2, 10},
            {"identifier", QStringLiteral("search synth\nlet = 1"), QStringLiteral("P001"), QStringLiteral("Expected identifier at line 2 col 5"), 2, 5},
            {"assignment sign", QStringLiteral("search synth\nlet x 1"), QStringLiteral("P001"), QStringLiteral("Expect '=' at line 2 col 7"), 2, 7},
            {"block opening", QStringLiteral("search synth\nif(true) return 1"), QStringLiteral("P001"), QStringLiteral("Expect '{' at line 2 col 10"), 2, 10},
            {"end of input", QStringLiteral("search synth\nrender(o0) xyz"), QStringLiteral("P001"), QStringLiteral("Expected end of input at line 2 col 12"), 2, 12},
            {"call closing parenthesis", QStringLiteral("search synth\nfoo(1"), QStringLiteral("P002"), QStringLiteral("Expect ')' at line 2 col 6"), 2, 6},
            {"write3d separator", QStringLiteral("search synth\nfoo().write3d(tex3d0 geo0)"), QStringLiteral("P001"), QStringLiteral("Expect ',' between tex3d and geo in write3d() at line 2 col 22"), 2, 22},
            {"CRLF and tab", LangString::fromUtf8("// \xF0\x9F\x98\x80\r\nsearch synth\r\n\trender(o0"), QStringLiteral("P002"), QStringLiteral("Expect ')' at line 3 col 11"), 3, 11},
            {"UTF-16 column", LangString::fromUtf8("search synth\nlet x = \"\xF0\x9F\x98\x80\"; render o0"), QStringLiteral("P001"), QStringLiteral("Expect '(' at line 2 col 22"), 2, 22},
        };

        for (const auto& c : cases) {
            bool caught = false;
            try {
                parseSrc(c.source);
            } catch (const nm::DslSyntaxError& err) {
                caught = true;
                check(err.message() == c.message,
                      QStringLiteral("parser diagnostic message matches for %1").arg(LangString::fromUtf8(c.name)).toStdString().c_str());
                const JsonObject d = JsonValue(err.diagnostic()).toObject();
                check(!d.isEmpty(),
                      QStringLiteral("parser diagnostic payload present for %1").arg(LangString::fromUtf8(c.name)).toStdString().c_str());
                check(d.value(QStringLiteral("code")).toString() == c.code,
                      QStringLiteral("parser diagnostic code matches for %1").arg(LangString::fromUtf8(c.name)).toStdString().c_str());
                check(d.value(QStringLiteral("stage")).toString() == QStringLiteral("parser"),
                      QStringLiteral("parser diagnostic stage matches for %1").arg(LangString::fromUtf8(c.name)).toStdString().c_str());
                check(d.value(QStringLiteral("severity")).toString() == QStringLiteral("error"),
                      QStringLiteral("parser diagnostic severity matches for %1").arg(LangString::fromUtf8(c.name)).toStdString().c_str());
                check(d.value(QStringLiteral("message")).toString() == c.message,
                      QStringLiteral("parser diagnostic message field matches for %1").arg(LangString::fromUtf8(c.name)).toStdString().c_str());
                const JsonObject span = d.value(QStringLiteral("span")).toObject();
                check(span.contains(QStringLiteral("start")) && span.contains(QStringLiteral("end")),
                      QStringLiteral("parser diagnostic span is present for %1").arg(LangString::fromUtf8(c.name)).toStdString().c_str());

                const JsonObject loc = d.value(QStringLiteral("location")).toObject();
                check(loc.value(QStringLiteral("line")).toInt() == c.line,
                      QStringLiteral("parser diagnostic location line matches for %1").arg(LangString::fromUtf8(c.name)).toStdString().c_str());
                check(loc.value(QStringLiteral("column")).toInt() == c.column,
                      QStringLiteral("parser diagnostic location column matches for %1").arg(LangString::fromUtf8(c.name)).toStdString().c_str());
            }
            check(caught, QStringLiteral("throws DslSyntaxError for %1").arg(LangString::fromUtf8(c.name)).toStdString().c_str());
        }
    }

    // ==================================================================
    // Structured subchain diagnostics (P006): the reference's
    // shaders/tests/test_diagnostic_locations.js cases at 30c47030
    // ==================================================================
    {
        struct SubchainCase {
            const char* name;
            LangString source;
            LangString message;
            int line;
            int column;
        };
        const QVector<SubchainCase> cases = {
            {"non-string argument", QStringLiteral("search synth\nread(o0).subchain(name: 1) { .diagProbe() }"), QStringLiteral("Expected string value for subchain name at line 2 col 25"), 2, 25},
            {"argument at EOF", QStringLiteral("search synth\nread(o0).subchain(name:"), QStringLiteral("Expected string value for subchain name at line 2 col 24"), 2, 24},
            {"missing body dot", QStringLiteral("search synth\nread(o0).subchain() { diagProbe() }"), QStringLiteral("Expected '.' before chain element in subchain body at line 2 col 23"), 2, 23},
            {"body at EOF", QStringLiteral("search synth\nread(o0).subchain() {"), QStringLiteral("Expected '.' before chain element in subchain body at line 2 col 22"), 2, 22},
            {"empty body", QStringLiteral("search synth\nread(o0).subchain() {}"), QStringLiteral("Subchain body cannot be empty at line 2 col 10"), 2, 10},
            {"comment-only body", QStringLiteral("search synth\nread(o0).subchain() { /* empty */ }"), QStringLiteral("Subchain body cannot be empty at line 2 col 10"), 2, 10},
            {"CRLF tab and UTF-16 argument", LangString::fromUtf8("// \xF0\x9F\x98\x80\r\nsearch synth\r\n\tread(o0).subchain(name: \"\xF0\x9F\x98\x80\", id: 1) { .diagProbe() }"), QStringLiteral("Expected string value for subchain id at line 3 col 36"), 3, 36},
            {"missing dot after comment", LangString::fromUtf8("search synth\nread(o0).subchain() { /* \xF0\x9F\x98\x80 */ missing() }"), QStringLiteral("Expected '.' before chain element in subchain body at line 2 col 32"), 2, 32},
            {"unclosed nonempty body", QStringLiteral("search synth\nread(o0).subchain() { .diagProbe()"), QStringLiteral("Expected '.' before chain element in subchain body at line 2 col 35"), 2, 35},
        };
        for (const auto& c : cases) {
            bool caught = false;
            try {
                parseSrc(c.source);
            } catch (const nm::DslSyntaxError& err) {
                caught = true;
                const JsonObject d = JsonValue(err.diagnostic()).toObject();
                const JsonObject loc = d.value(QStringLiteral("location")).toObject();
                const JsonObject span = d.value(QStringLiteral("span")).toObject();
                check(err.message() == c.message && d.value(QStringLiteral("code")).toString() == QStringLiteral("P006")
                          && d.value(QStringLiteral("stage")).toString() == QStringLiteral("parser")
                          && d.value(QStringLiteral("severity")).toString() == QStringLiteral("error")
                          && d.value(QStringLiteral("message")).toString() == c.message
                          && span.contains(QStringLiteral("start")) && span.contains(QStringLiteral("end"))
                          && loc.value(QStringLiteral("line")).toInt() == c.line
                          && loc.value(QStringLiteral("column")).toInt() == c.column,
                      QStringLiteral("subchain P006 diagnostic matches for %1").arg(LangString::fromUtf8(c.name)).toStdString().c_str());
            }
            check(caught, QStringLiteral("throws DslSyntaxError for subchain %1").arg(LangString::fromUtf8(c.name)).toStdString().c_str());

            // Tokens without coordinates: the same code, and a null location.
            JsonArray bare;
            for (const JsonValue& val : lexJson(c.source)) {
                JsonObject tok = val.toObject();
                tok.remove(QStringLiteral("line"));
                tok.remove(QStringLiteral("col"));
                tok.remove(QStringLiteral("position"));
                bare.append(tok);
            }
            bool caughtBare = false;
            try {
                parseJson(bare);
            } catch (const nm::DslSyntaxError& err) {
                caughtBare = true;
                const JsonObject d = JsonValue(err.diagnostic()).toObject();
                check(d.value(QStringLiteral("code")).toString() == QStringLiteral("P006") && d.value(QStringLiteral("location")).isNull()
                          && d.value(QStringLiteral("message")).toString() == err.message(),
                      QStringLiteral("subchain P006 location is null without coordinates for %1").arg(LangString::fromUtf8(c.name)).toStdString().c_str());
            }
            check(caughtBare, QStringLiteral("throws DslSyntaxError for coordinate-free subchain %1").arg(LangString::fromUtf8(c.name)).toStdString().c_str());
        }

        // Shared expectations inside a subchain keep their own codes.
        const QVector<std::pair<LangString, std::pair<LangString, LangString>>> precedence = {
            {QStringLiteral("search synth\nread(o0).subchain(1) {}"), {QStringLiteral("P002"), QStringLiteral("Expect ')' after subchain arguments at line 2 col 19")}},
            {QStringLiteral("search synth\nread(o0).subchain() { . }"), {QStringLiteral("P001"), QStringLiteral("Expected identifier at line 2 col 25")}},
        };
        for (const auto& [source, expected] : precedence) {
            bool caught = false;
            try {
                parseSrc(source);
            } catch (const nm::DslSyntaxError& err) {
                caught = err.message() == expected.second
                         && JsonValue(err.diagnostic()).toObject().value(QStringLiteral("code")).toString() == expected.first;
            }
            check(caught, QStringLiteral("subchain keeps %1 for: %2").arg(expected.first, expected.second).toStdString().c_str());
        }
    }

    // ==================================================================
    // Parser expectation diagnostics represent unavailable caller-token coordinates explicitly
    // ==================================================================
    {
        struct CoordCase {
            bool hasLine;
            JsonValue lineVal;
            bool hasCol;
            JsonValue colVal;
            LangString expectedMsg;
        };

        const QVector<CoordCase> coordCases = {
            {false, JsonValue(), false, JsonValue(), QStringLiteral("Expect '(' at line undefined col undefined")},
            {true, 1, false, JsonValue(), QStringLiteral("Expect '(' at line 1 col undefined")},
            {true, 0, true, 1, QStringLiteral("Expect '(' at line 0 col 1")},
            {true, 1, true, QStringLiteral("NaN"), QStringLiteral("Expect '(' at line 1 col NaN")},
        };

        for (const auto& cc : coordCases) {
            const JsonArray origTokens = lexJson(QStringLiteral("search synth\nrender o0"));
            JsonArray modifiedTokens;
            for (const JsonValue& val : origTokens) {
                JsonObject tokObj = val.toObject();
                if (tokObj.value(QStringLiteral("type")).toString() == QStringLiteral("OUTPUT_REF")) {
                    tokObj.remove(QStringLiteral("line"));
                    tokObj.remove(QStringLiteral("col"));
                    tokObj.remove(QStringLiteral("position"));
                    if (cc.hasLine) tokObj.insert(QStringLiteral("line"), cc.lineVal);
                    if (cc.hasCol) tokObj.insert(QStringLiteral("col"), cc.colVal);
                }
                modifiedTokens.append(tokObj);
            }

            bool caught = false;
            try {
                parseJson(modifiedTokens);
            } catch (const nm::DslSyntaxError& err) {
                caught = true;
                check(err.message() == cc.expectedMsg, "parser error message with unavailable coords matches");
                const JsonObject d = JsonValue(err.diagnostic()).toObject();
                check(d.value(QStringLiteral("code")).toString() == QStringLiteral("P001"), "diagnostic code P001");
                check(d.value(QStringLiteral("stage")).toString() == QStringLiteral("parser"), "diagnostic stage parser");
                check(d.value(QStringLiteral("severity")).toString() == QStringLiteral("error"), "diagnostic severity error");
                check(d.value(QStringLiteral("message")).toString() == cc.expectedMsg, "diagnostic message matches");
                check(d.value(QStringLiteral("location")).isNull(), "diagnostic location is null when coordinates unavailable");
                check(d.value(QStringLiteral("span")).isNull(), "diagnostic span is null");
            }
            check(caught, "throws DslSyntaxError for unavailable coordinates");
        }
    }

    // ==================================================================
    // Structured automation argument diagnostics (P003)
    // ==================================================================
    {
        struct AutoCase {
            const char* name;
            LangString source;
            LangString message;
            int line;
            int column;
        };

        const QVector<AutoCase> autoCases = {
            {"osc unknown param", QStringLiteral("search synth\nlet x = osc(type: oscKind.sine, bogus: 1)"),
             QStringLiteral("osc() unknown parameter 'bogus' at line 2 col 9. Valid: type, min, max, speed, offset, seed"), 2, 9},
            {"midi excess positional", QStringLiteral("search synth\nlet x = midi(1, 2, 3, 4, 5, 6)"),
             QStringLiteral("midi() name, id, cc, nrpn, zone and members are keyword-only at line 2 col 9"), 2, 9},
            {"midi unknown param", QStringLiteral("search synth\nlet x = midi(bogus: 1)"),
             QStringLiteral("midi() unknown parameter 'bogus' at line 2 col 9. Valid: channel, mode, min, max, sensitivity, name, id, cc, nrpn, zone, members"), 2, 9},
            {"midi excess positional with kwargs", QStringLiteral("search synth\nlet x = midi(1, 2, 3, 4, 5, channel: 1)"),
             QStringLiteral("midi() has an excess positional argument at line 2 col 9"), 2, 9},
            {"midi missing channel or zone", QStringLiteral("search synth\nlet x = midi()"),
             QStringLiteral("midi() requires 'channel' or 'zone' argument at line 2 col 9"), 2, 9},
            {"midi channel and zone mutually exclusive", QStringLiteral("search synth\nlet x = midi(1, zone: 1)"),
             QStringLiteral("midi() 'channel' and 'zone' are mutually exclusive at line 2 col 9"), 2, 9},
            {"midi members requires zone", QStringLiteral("search synth\nlet x = midi(1, members: 2)"),
             QStringLiteral("midi() 'members' requires 'zone' at line 2 col 9"), 2, 9},
            {"midi id requires name", QStringLiteral("search synth\nlet x = midi(1, id: \"port\")"),
             QStringLiteral("midi() 'id' requires readable 'name' at line 2 col 9"), 2, 9},
            {"midi name requires string", QStringLiteral("search synth\nlet x = midi(1, name: 1)"),
             QStringLiteral("midi() 'name' requires a quoted string at line 2 col 9"), 2, 9},
            {"midi name empty", QStringLiteral("search synth\nlet x = midi(1, name: \"\")"),
             QStringLiteral("midi() 'name' must not be empty at line 2 col 9"), 2, 9},
            {"midi id requires string", QStringLiteral("search synth\nlet x = midi(1, name: \"port\", id: 1)"),
             QStringLiteral("midi() 'id' requires a quoted string at line 2 col 9"), 2, 9},
            {"midi id empty", QStringLiteral("search synth\nlet x = midi(1, name: \"port\", id: \"\")"),
             QStringLiteral("midi() 'id' must not be empty at line 2 col 9"), 2, 9},
            {"audio excess positional", QStringLiteral("search synth\nlet x = audio(1, 2, 3, 4)"),
             QStringLiteral("audio() channel, name and id are keyword-only at line 2 col 9"), 2, 9},
            {"audio unknown param", QStringLiteral("search synth\nlet x = audio(bogus: 1)"),
             QStringLiteral("audio() unknown parameter 'bogus' at line 2 col 9. Valid: band, min, max, channel, name, id"), 2, 9},
            {"audio excess positional with kwargs", QStringLiteral("search synth\nlet x = audio(1, 2, 3, band: 1)"),
             QStringLiteral("audio() has an excess positional argument at line 2 col 9"), 2, 9},
            {"audio missing band", QStringLiteral("search synth\nlet x = audio()"),
             QStringLiteral("audio() requires 'band' argument at line 2 col 9"), 2, 9},
            {"audio id requires name", QStringLiteral("search synth\nlet x = audio(1, id: \"device\")"),
             QStringLiteral("audio() 'id' requires readable 'name' at line 2 col 9"), 2, 9},
            {"audio selected device requires name and channel", QStringLiteral("search synth\nlet x = audio(1, name: \"device\")"),
             QStringLiteral("audio() selected device requires both 'name' and 'channel' at line 2 col 9"), 2, 9},
            {"audio name requires string", QStringLiteral("search synth\nlet x = audio(1, channel: 1, name: 1)"),
             QStringLiteral("audio() 'name' requires a quoted string at line 2 col 9"), 2, 9},
            {"audio name empty", QStringLiteral("search synth\nlet x = audio(1, channel: 1, name: \"\")"),
             QStringLiteral("audio() 'name' must not be empty at line 2 col 9"), 2, 9},
            {"audio id requires string", QStringLiteral("search synth\nlet x = audio(1, channel: 1, name: \"device\", id: 1)"),
             QStringLiteral("audio() 'id' requires a quoted string at line 2 col 9"), 2, 9},
            {"audio id empty", QStringLiteral("search synth\nlet x = audio(1, channel: 1, name: \"device\", id: \"\")"),
             QStringLiteral("audio() 'id' must not be empty at line 2 col 9"), 2, 9},
        };

        for (const auto& c : autoCases) {
            bool caught = false;
            try {
                parseSrc(c.source);
            } catch (const nm::DslSyntaxError& err) {
                caught = true;
                check(err.message() == c.message,
                      QStringLiteral("automation diagnostic message matches for %1").arg(LangString::fromUtf8(c.name)).toStdString().c_str());
                const JsonObject d = JsonValue(err.diagnostic()).toObject();
                check(!d.isEmpty(),
                      QStringLiteral("automation diagnostic payload present for %1").arg(LangString::fromUtf8(c.name)).toStdString().c_str());
                check(d.value(QStringLiteral("code")).toString() == QStringLiteral("P003"),
                      QStringLiteral("automation diagnostic code is P003 for %1").arg(LangString::fromUtf8(c.name)).toStdString().c_str());
                check(d.value(QStringLiteral("stage")).toString() == QStringLiteral("parser"),
                      QStringLiteral("automation diagnostic stage matches for %1").arg(LangString::fromUtf8(c.name)).toStdString().c_str());
                check(d.value(QStringLiteral("severity")).toString() == QStringLiteral("error"),
                      QStringLiteral("automation diagnostic severity matches for %1").arg(LangString::fromUtf8(c.name)).toStdString().c_str());
                check(d.value(QStringLiteral("message")).toString() == c.message,
                      QStringLiteral("automation diagnostic message field matches for %1").arg(LangString::fromUtf8(c.name)).toStdString().c_str());
                const JsonObject span = d.value(QStringLiteral("span")).toObject();
                check(span.contains(QStringLiteral("start")) && span.contains(QStringLiteral("end")),
                      QStringLiteral("automation diagnostic span is present for %1").arg(LangString::fromUtf8(c.name)).toStdString().c_str());
                const JsonObject loc = d.value(QStringLiteral("location")).toObject();
                check(loc.value(QStringLiteral("line")).toInt() == c.line,
                      QStringLiteral("automation diagnostic location line matches for %1").arg(LangString::fromUtf8(c.name)).toStdString().c_str());
                check(loc.value(QStringLiteral("column")).toInt() == c.column,
                      QStringLiteral("automation diagnostic location column matches for %1").arg(LangString::fromUtf8(c.name)).toStdString().c_str());
            }
            check(caught, QStringLiteral("throws DslSyntaxError for %1").arg(LangString::fromUtf8(c.name)).toStdString().c_str());
        }
    }

    // ==================================================================
    // Structured search directive diagnostics (P004)
    // ==================================================================
    {
        const LangString missingSearchMsg = QStringLiteral("Missing required 'search' directive. Every program must start with 'search <namespace>, ...' to specify namespace search order.");
        struct SearchCase {
            const char* name;
            LangString source;
            LangString message;
            int line;
            int column;
        };

        const QVector<SearchCase> searchCases = {
            {"empty program", QStringLiteral(""), missingSearchMsg, 1, 1},
            {"missing directive after statements", QStringLiteral("let x = 1"), missingSearchMsg, 1, 10},
            {"duplicate directive", QStringLiteral("search synth search filter"),
             QStringLiteral("Only one search directive is allowed per program at line 1 col 14"), 1, 14},
            {"invalid namespace", QStringLiteral("search bogus"),
             QStringLiteral("Invalid namespace 'bogus' at line 1 col 8. Valid namespaces: io, classicNoisedeck, synth, mixer, filter, render, points, synth3d, filter3d, user"), 1, 8},
            {"missing first namespace", QStringLiteral("search"),
             QStringLiteral("Expected namespace identifier after search at line 1 col 7"), 1, 7},
            {"missing additional namespace", QStringLiteral("search synth,"),
             QStringLiteral("Expected namespace identifier after comma at line 1 col 14"), 1, 14},
            {"misplaced directive", QStringLiteral("let x = 1; search synth"),
             QStringLiteral("'search' directive must appear before other statements at line 1 col 12"), 1, 12},
            {"nested directive", QStringLiteral("search synth\nif(true) { search filter }"),
             QStringLiteral("'search' directive is only allowed at the start of the program at line 2 col 12"), 2, 12},
            {"CRLF and tab", LangString::fromUtf8("// \xF0\x9F\x98\x80\r\n\tsearch 1"),
             QStringLiteral("Expected namespace identifier after search at line 2 col 9"), 2, 9},
            {"UTF-16 column", LangString::fromUtf8("search synth\nlet x = \"\xF0\x9F\x98\x80\"; search filter"),
             QStringLiteral("'search' directive must appear before other statements at line 2 col 15"), 2, 15},
        };

        for (const auto& c : searchCases) {
            bool caught = false;
            try {
                parseSrc(c.source);
            } catch (const nm::DslSyntaxError& err) {
                caught = true;
                check(err.message() == c.message,
                      QStringLiteral("search diagnostic message matches for %1").arg(LangString::fromUtf8(c.name)).toStdString().c_str());
                const JsonObject d = JsonValue(err.diagnostic()).toObject();
                check(!d.isEmpty(),
                      QStringLiteral("search diagnostic payload present for %1").arg(LangString::fromUtf8(c.name)).toStdString().c_str());
                check(d.value(QStringLiteral("code")).toString() == QStringLiteral("P004"),
                      QStringLiteral("search diagnostic code is P004 for %1").arg(LangString::fromUtf8(c.name)).toStdString().c_str());
                check(d.value(QStringLiteral("stage")).toString() == QStringLiteral("parser"),
                      QStringLiteral("search diagnostic stage matches for %1").arg(LangString::fromUtf8(c.name)).toStdString().c_str());
                check(d.value(QStringLiteral("severity")).toString() == QStringLiteral("error"),
                      QStringLiteral("search diagnostic severity matches for %1").arg(LangString::fromUtf8(c.name)).toStdString().c_str());
                check(d.value(QStringLiteral("message")).toString() == c.message,
                      QStringLiteral("search diagnostic message field matches for %1").arg(LangString::fromUtf8(c.name)).toStdString().c_str());
                const JsonObject span = d.value(QStringLiteral("span")).toObject();
                check(span.contains(QStringLiteral("start")) && span.contains(QStringLiteral("end")),
                      QStringLiteral("search diagnostic span is present for %1").arg(LangString::fromUtf8(c.name)).toStdString().c_str());
                const JsonObject loc = d.value(QStringLiteral("location")).toObject();
                check(loc.value(QStringLiteral("line")).toInt() == c.line,
                      QStringLiteral("search diagnostic location line matches for %1").arg(LangString::fromUtf8(c.name)).toStdString().c_str());
                check(loc.value(QStringLiteral("column")).toInt() == c.column,
                      QStringLiteral("search diagnostic location column matches for %1").arg(LangString::fromUtf8(c.name)).toStdString().c_str());
            }
            check(caught, QStringLiteral("throws DslSyntaxError for %1").arg(LangString::fromUtf8(c.name)).toStdString().c_str());
        }
    }

    // ==================================================================
    // Structured output validation diagnostics (P005)
    // ==================================================================
    {
        struct OutputCase {
            const char* name;
            LangString source;
            LangString message;
            int line;
            int column;
        };

        const QVector<OutputCase> outputCases = {
            {"invalid render target", QStringLiteral("search synth\nrender(1)"),
             QStringLiteral("Expected output reference in render()"), 2, 8},
            {"render target at EOF", QStringLiteral("search synth\nrender("),
             QStringLiteral("Expected output reference in render()"), 2, 8},
            {"write in expression", QStringLiteral("search synth\nlet x = diagProbe().write(o0)"),
             QStringLiteral("'.write()' is only allowed in statement context at line 2 col 21"), 2, 21},
            {"write3d in expression", QStringLiteral("search synth\nlet x = diagProbe().write3d(vol0, geo0)"),
             QStringLiteral("'.write()' is only allowed in statement context at line 2 col 21"), 2, 21},
            {"missing write surface", QStringLiteral("search synth\ndiagProbe().write()"),
             QStringLiteral("write() requires an explicit surface reference (e.g., o0, o1, xyz0, vel0, rgba0, mesh0, none) at line 2 col 19"), 2, 19},
            {"write surface at EOF", QStringLiteral("search synth\ndiagProbe().write("),
             QStringLiteral("write() requires an explicit surface reference (e.g., o0, o1, xyz0, vel0, rgba0, mesh0, none) at line 2 col 19"), 2, 19},
            {"invalid write surface", QStringLiteral("search synth\ndiagProbe().write(1)"),
             QStringLiteral("write() requires an explicit surface reference (e.g., o0, o1, xyz0, vel0, rgba0, mesh0, none) at line 2 col 19"), 2, 19},
            {"invalid write3d texture", QStringLiteral("search synth\ndiagProbe().write3d(1, geo0)"),
             QStringLiteral("Expected tex3d reference in write3d() at line 2 col 21"), 2, 21},
            {"write3d texture at EOF", QStringLiteral("search synth\ndiagProbe().write3d("),
             QStringLiteral("Expected tex3d reference in write3d() at line 2 col 21"), 2, 21},
            {"invalid write3d geometry", QStringLiteral("search synth\ndiagProbe().write3d(vol0, 1)"),
             QStringLiteral("Expected geo reference in write3d() at line 2 col 27"), 2, 27},
            {"write3d geometry at EOF", QStringLiteral("search synth\ndiagProbe().write3d(vol0,"),
             QStringLiteral("Expected geo reference in write3d() at line 2 col 26"), 2, 26},
            {"CRLF and tab render target", LangString::fromUtf8("// \xF0\x9F\x98\x80\r\nsearch synth\r\n\trender(\"\xF0\x9F\x98\x80\")"),
             QStringLiteral("Expected output reference in render()"), 3, 9},
            {"UTF-16 render target column", LangString::fromUtf8("search synth\nlet x = \"\xF0\x9F\x98\x80\"; render(none)"),
             QStringLiteral("Expected output reference in render()"), 2, 22},
        };

        for (const auto& c : outputCases) {
            bool caught = false;
            try {
                parseSrc(c.source);
            } catch (const nm::DslSyntaxError& err) {
                caught = true;
                check(err.message() == c.message,
                      QStringLiteral("output diagnostic message matches for %1").arg(LangString::fromUtf8(c.name)).toStdString().c_str());
                const JsonObject d = JsonValue(err.diagnostic()).toObject();
                check(!d.isEmpty(),
                      QStringLiteral("output diagnostic payload present for %1").arg(LangString::fromUtf8(c.name)).toStdString().c_str());
                check(d.value(QStringLiteral("code")).toString() == QStringLiteral("P005"),
                      QStringLiteral("output diagnostic code is P005 for %1").arg(LangString::fromUtf8(c.name)).toStdString().c_str());
                check(d.value(QStringLiteral("stage")).toString() == QStringLiteral("parser"),
                      QStringLiteral("output diagnostic stage matches for %1").arg(LangString::fromUtf8(c.name)).toStdString().c_str());
                check(d.value(QStringLiteral("severity")).toString() == QStringLiteral("error"),
                      QStringLiteral("output diagnostic severity matches for %1").arg(LangString::fromUtf8(c.name)).toStdString().c_str());
                check(d.value(QStringLiteral("message")).toString() == c.message,
                      QStringLiteral("output diagnostic message field matches for %1").arg(LangString::fromUtf8(c.name)).toStdString().c_str());
                const JsonObject span = d.value(QStringLiteral("span")).toObject();
                check(span.contains(QStringLiteral("start")) && span.contains(QStringLiteral("end")),
                      QStringLiteral("output diagnostic span is present for %1").arg(LangString::fromUtf8(c.name)).toStdString().c_str());
                const JsonObject loc = d.value(QStringLiteral("location")).toObject();
                check(loc.value(QStringLiteral("line")).toInt() == c.line,
                      QStringLiteral("output diagnostic location line matches for %1").arg(LangString::fromUtf8(c.name)).toStdString().c_str());
                check(loc.value(QStringLiteral("column")).toInt() == c.column,
                      QStringLiteral("output diagnostic location column matches for %1").arg(LangString::fromUtf8(c.name)).toStdString().c_str());
            }
            check(caught, QStringLiteral("throws DslSyntaxError for %1").arg(LangString::fromUtf8(c.name)).toStdString().c_str());
        }
    }

    // ==================================================================
    // Output syntax preserves shared expectation diagnostic precedence
    // ==================================================================
    {
        struct PrecedenceCase {
            LangString source;
            LangString code;
            LangString message;
        };

        const QVector<PrecedenceCase> precedenceCases = {
            {QStringLiteral("search synth\nrender o0"), QStringLiteral("P001"),
             QStringLiteral("Expect '(' at line 2 col 8")},
            {QStringLiteral("search synth\nrender(o0"), QStringLiteral("P002"),
             QStringLiteral("Expect ')' at line 2 col 10")},
            {QStringLiteral("search synth\nrender(o0) render(o1)"), QStringLiteral("P001"),
             QStringLiteral("Expected end of input at line 2 col 12")},
            {QStringLiteral("search synth\ndiagProbe().write(o0"), QStringLiteral("P002"),
             QStringLiteral("Expect ')' at line 2 col 21")},
            {QStringLiteral("search synth\ndiagProbe().write3d(vol0 geo0)"), QStringLiteral("P001"),
             QStringLiteral("Expect ',' between tex3d and geo in write3d() at line 2 col 26")},
        };

        for (const auto& c : precedenceCases) {
            bool caught = false;
            try {
                parseSrc(c.source);
            } catch (const nm::DslSyntaxError& err) {
                caught = true;
                check(err.message() == c.message, "precedence error message matches");
                const JsonObject d = JsonValue(err.diagnostic()).toObject();
                check(d.value(QStringLiteral("code")).toString() == c.code, "precedence diagnostic code matches");
            }
            check(caught, "throws DslSyntaxError for precedence case");
        }
    }

    // ==================================================================
    // Parser automation, search, and output diagnostics represent unavailable coordinates explicitly
    // ==================================================================
    {
        for (const LangString& src : {QStringLiteral("search synth\nlet x = midi()"),
                                   QStringLiteral("search bogus"),
                                   QStringLiteral("search synth\nrender(1)"),
                                   QStringLiteral("search synth\ndiagProbe().write()")}) {
            const JsonArray origTokens = lexJson(src);
            JsonArray modifiedTokens;
            for (const JsonValue& val : origTokens) {
                JsonObject tokObj = val.toObject();
                tokObj.remove(QStringLiteral("line"));
                tokObj.remove(QStringLiteral("col"));
                tokObj.remove(QStringLiteral("position"));
                modifiedTokens.append(tokObj);
            }
            bool caught = false;
            try {
                parseJson(modifiedTokens);
            } catch (const nm::DslSyntaxError& err) {
                caught = true;
                const JsonObject d = JsonValue(err.diagnostic()).toObject();
                check(d.value(QStringLiteral("code")).toString().startsWith(QLatin1Char('P')), "code starts with P");
                check(d.value(QStringLiteral("location")).isNull(), "location is null when coordinates unavailable");
            }
            check(caught, "throws DslSyntaxError for unavailable coordinates in P003/P004/P005");
        }
    }

    // P007 call-form diagnostics test suite
    {
        // 1. Inline namespace syntax
        bool caught = false;
        try {
            parseJson(lexJson(QStringLiteral("search synth\nlet x = synth.noise()\nrender x")));
        } catch (const nm::DslSyntaxError& err) {
            caught = true;
            const JsonObject d = JsonValue(err.diagnostic()).toObject();
            check(d.value(QStringLiteral("code")).toString() == QStringLiteral("P007"), "inline namespace emits P007");
            check(d.value(QStringLiteral("stage")).toString() == QStringLiteral("parser"), "P007 stage is parser");
            check(d.value(QStringLiteral("severity")).toString() == QStringLiteral("error"), "P007 severity is error");
            const JsonObject loc = d.value(QStringLiteral("location")).toObject();
            check(loc.value(QStringLiteral("line")).toInt() == 2, "P007 inline namespace line is 2");
            const JsonObject span = d.value(QStringLiteral("span")).toObject();
            check(span.contains(QStringLiteral("start")) && span.contains(QStringLiteral("end")), "P007 inline namespace has span");
        }
        check(caught, "throws P007 for inline namespace syntax");

        // 2. Mixed positional and keyword arguments
        caught = false;
        try {
            parseJson(lexJson(QStringLiteral("search synth\nlet x = noise(1, freq: 2)\nrender x")));
        } catch (const nm::DslSyntaxError& err) {
            caught = true;
            const JsonObject d = JsonValue(err.diagnostic()).toObject();
            check(d.value(QStringLiteral("code")).toString() == QStringLiteral("P007"), "mixed positional/keyword emits P007");
            check(d.value(QStringLiteral("message")).toString().contains(QStringLiteral("Cannot mix positional and keyword arguments")),
                  "P007 mixed positional/keyword message");
        }
        check(caught, "throws P007 for mixed positional/keyword args");

        // 3. from() call validation: named kwargs
        caught = false;
        try {
            parseJson(lexJson(QStringLiteral("search synth\nlet x = from(ns: synth, noise())\nrender x")));
        } catch (const nm::DslSyntaxError& err) {
            caught = true;
            const JsonObject d = JsonValue(err.diagnostic()).toObject();
            check(d.value(QStringLiteral("code")).toString() == QStringLiteral("P007"), "from() kwargs emits P007");
        }
        check(caught, "throws P007 for from() with named kwargs");

        // 4. from() call validation: arity mismatch
        caught = false;
        try {
            parseJson(lexJson(QStringLiteral("search synth\nlet x = from(synth)\nrender x")));
        } catch (const nm::DslSyntaxError& err) {
            caught = true;
            const JsonObject d = JsonValue(err.diagnostic()).toObject();
            check(d.value(QStringLiteral("code")).toString() == QStringLiteral("P007"), "from() arity emits P007");
            check(d.value(QStringLiteral("message")).toString().contains(QStringLiteral("'from' requires exactly two arguments")),
                  "P007 from() arity message");
        }
        check(caught, "throws P007 for from() with arity mismatch");
    }

    // P001 number coercion diagnostics test suite
    {
        // 1. Array literal coerced to number in arithmetic
        bool caught = false;
        try {
            parseJson(lexJson(QStringLiteral("search synth\nlet x = [1, 2] + 3\nrender x")));
        } catch (const nm::DslSyntaxError& err) {
            caught = true;
            const JsonObject d = JsonValue(err.diagnostic()).toObject();
            check(d.value(QStringLiteral("code")).toString() == QStringLiteral("P001"), "array coercion emits P001");
            check(d.value(QStringLiteral("message")).toString() == QStringLiteral("Expected number"), "P001 expected number message");
            const JsonObject loc = d.value(QStringLiteral("location")).toObject();
            check(loc.value(QStringLiteral("line")).toInt() == 2, "P001 array coercion line is 2");
            const JsonObject span = d.value(QStringLiteral("span")).toObject();
            check(span.contains(QStringLiteral("start")) && span.contains(QStringLiteral("end")), "P001 array coercion has span");
        }
        check(caught, "throws P001 for array coerced to number");
    }

    // Subchain argument validation test suite
    {
        // 1. Default parse accepts unknown subchain key without altering AST and attaches P008 report
        {
            const LangString src = QStringLiteral("search synth\nnoise().subchain(nme: \"typo\", name: \"ok\") {\n.bloom()\n}.write(o0)\n");
            const JsonObject prog = parseSrc(src);
            const JsonArray plans = prog.value(QStringLiteral("plans")).toArray();
            const JsonArray chain = plans.at(0).toObject().value(QStringLiteral("chain")).toArray();
            const JsonObject subchain = chain.at(1).toObject();
            check(subchain.value(QStringLiteral("type")).toString() == QStringLiteral("Subchain"), "chain[1] is Subchain");
            check(subchain.value(QStringLiteral("name")).toString() == QStringLiteral("ok"), "subchain name is 'ok'");
            check(subchain.value(QStringLiteral("id")).isNull(), "subchain id is null");
            const JsonArray diags = subchain.value(QStringLiteral("subchainArgumentDiagnostics")).toArray();
            check(diags.size() == 1, "subchain has 1 arg diagnostic");
            const JsonObject d0 = diags.at(0).toObject();
            check(d0.value(QStringLiteral("code")).toString() == QStringLiteral("P008"), "diag code is P008");
            check(d0.value(QStringLiteral("severity")).toString() == QStringLiteral("warning"), "P008 severity is warning");
            check(d0.value(QStringLiteral("message")).toString().contains(QStringLiteral("nme")), "P008 message mentions 'nme'");
            const JsonObject loc = d0.value(QStringLiteral("location")).toObject();
            check(loc.value(QStringLiteral("line")).toInt() == 2, "P008 location line 2");
            const JsonObject span = d0.value(QStringLiteral("span")).toObject();
            check(span.contains(QStringLiteral("start")) && span.contains(QStringLiteral("end")), "P008 span attached");
        }

        // 2. Default parse reports duplicate key with last value winning (P009)
        {
            const LangString src = QStringLiteral("search synth\nnoise().subchain(name: \"first\", name: \"second\") {\n.bloom()\n}.write(o0)\n");
            const JsonObject prog = parseSrc(src);
            const JsonArray plans = prog.value(QStringLiteral("plans")).toArray();
            const JsonObject subchain = plans.at(0).toObject().value(QStringLiteral("chain")).toArray().at(1).toObject();
            check(subchain.value(QStringLiteral("name")).toString() == QStringLiteral("second"), "last value wins for name: 'second'");
            const JsonArray diags = subchain.value(QStringLiteral("subchainArgumentDiagnostics")).toArray();
            check(diags.size() == 1, "subchain has 1 duplicate key diagnostic");
            const JsonObject d0 = diags.at(0).toObject();
            check(d0.value(QStringLiteral("code")).toString() == QStringLiteral("P009"), "diag code is P009");
            check(d0.value(QStringLiteral("severity")).toString() == QStringLiteral("warning"), "P009 severity is warning");
            check(d0.value(QStringLiteral("message")).toString().contains(QStringLiteral("name")), "P009 message mentions 'name'");
        }

        // 3. Default parse reports missing comma separator (P010)
        {
            const LangString src = QStringLiteral("search synth\nnoise().subchain(name: \"a\" id: \"b\") {\n.bloom()\n}.write(o0)\n");
            const JsonObject prog = parseSrc(src);
            const JsonArray plans = prog.value(QStringLiteral("plans")).toArray();
            const JsonObject subchain = plans.at(0).toObject().value(QStringLiteral("chain")).toArray().at(1).toObject();
            check(subchain.value(QStringLiteral("name")).toString() == QStringLiteral("a"), "subchain name is 'a'");
            check(subchain.value(QStringLiteral("id")).toString() == QStringLiteral("b"), "subchain id is 'b'");
            const JsonArray diags = subchain.value(QStringLiteral("subchainArgumentDiagnostics")).toArray();
            check(diags.size() == 1, "subchain has 1 missing separator diagnostic");
            const JsonObject d0 = diags.at(0).toObject();
            check(d0.value(QStringLiteral("code")).toString() == QStringLiteral("P010"), "diag code is P010");
            check(d0.value(QStringLiteral("severity")).toString() == QStringLiteral("warning"), "P010 severity is warning");
        }

        // 4. Co-occurring violations are reported in source order: P008, P010, P009
        {
            const LangString src = QStringLiteral("search synth\nnoise().subchain(nme: \"x\", name: \"a\" name: \"b\") {\n.bloom()\n}.write(o0)\n");
            const JsonObject prog = parseSrc(src);
            const JsonArray plans = prog.value(QStringLiteral("plans")).toArray();
            const JsonObject subchain = plans.at(0).toObject().value(QStringLiteral("chain")).toArray().at(1).toObject();
            const JsonArray diags = subchain.value(QStringLiteral("subchainArgumentDiagnostics")).toArray();
            check(diags.size() == 3, "co-occurring violations report 3 diagnostics");
            check(diags.at(0).toObject().value(QStringLiteral("code")).toString() == QStringLiteral("P008"), "diag[0] is P008");
            check(diags.at(1).toObject().value(QStringLiteral("code")).toString() == QStringLiteral("P010"), "diag[1] is P010");
            check(diags.at(2).toObject().value(QStringLiteral("code")).toString() == QStringLiteral("P009"), "diag[2] is P009");
        }

        // 5. Repeated unknown keys report P008 once per occurrence and never P009
        {
            const LangString src = QStringLiteral("search synth\nnoise().subchain(nme: \"x\", nme: \"y\", name: \"ok\") {\n.bloom()\n}.write(o0)\n");
            const JsonObject prog = parseSrc(src);
            const JsonArray plans = prog.value(QStringLiteral("plans")).toArray();
            const JsonObject subchain = plans.at(0).toObject().value(QStringLiteral("chain")).toArray().at(1).toObject();
            const JsonArray diags = subchain.value(QStringLiteral("subchainArgumentDiagnostics")).toArray();
            check(diags.size() == 2, "repeated unknown key reports 2 diagnostics");
            check(diags.at(0).toObject().value(QStringLiteral("code")).toString() == QStringLiteral("P008"), "diag[0] is P008");
            check(diags.at(1).toObject().value(QStringLiteral("code")).toString() == QStringLiteral("P008"), "diag[1] is P008");
        }

        // 6. Strict mode rejects unknown subchain key with P008
        {
            JsonObject opts;
            opts.insert(QStringLiteral("subchainArguments"), QStringLiteral("strict"));
            bool caught = false;
            try {
                parseJson(lexJson(QStringLiteral("search synth\nnoise().subchain(nme: \"typo\", name: \"ok\") {\n.bloom()\n}.write(o0)\n")), opts);
            } catch (const nm::DslSyntaxError& err) {
                caught = true;
                const JsonObject d = JsonValue(err.diagnostic()).toObject();
                check(d.value(QStringLiteral("code")).toString() == QStringLiteral("P008"), "strict unknown key emits P008");
                check(d.value(QStringLiteral("stage")).toString() == QStringLiteral("parser"), "P008 stage parser");
                check(d.value(QStringLiteral("severity")).toString() == QStringLiteral("error"), "P008 strict severity error");
                check(d.value(QStringLiteral("message")).toString().contains(QStringLiteral("nme")), "P008 mentions 'nme'");
                check(d.value(QStringLiteral("span")).toObject().contains(QStringLiteral("start")), "P008 strict has span");
            }
            check(caught, "strict mode throws for unknown subchain key");
        }

        // 7. Strict mode rejects duplicate subchain key with P009
        {
            JsonObject opts;
            opts.insert(QStringLiteral("subchainArguments"), QStringLiteral("strict"));
            bool caught = false;
            try {
                parseJson(lexJson(QStringLiteral("search synth\nnoise().subchain(name: \"a\", name: \"b\") {\n.bloom()\n}.write(o0)\n")), opts);
            } catch (const nm::DslSyntaxError& err) {
                caught = true;
                const JsonObject d = JsonValue(err.diagnostic()).toObject();
                check(d.value(QStringLiteral("code")).toString() == QStringLiteral("P009"), "strict duplicate key emits P009");
                check(d.value(QStringLiteral("severity")).toString() == QStringLiteral("error"), "P009 strict severity error");
            }
            check(caught, "strict mode throws for duplicate subchain key");
        }

        // 8. Strict mode rejects missing separator with P010
        {
            JsonObject opts;
            opts.insert(QStringLiteral("subchainArguments"), QStringLiteral("strict"));
            bool caught = false;
            try {
                parseJson(lexJson(QStringLiteral("search synth\nnoise().subchain(name: \"a\" id: \"b\") {\n.bloom()\n}.write(o0)\n")), opts);
            } catch (const nm::DslSyntaxError& err) {
                caught = true;
                const JsonObject d = JsonValue(err.diagnostic()).toObject();
                check(d.value(QStringLiteral("code")).toString() == QStringLiteral("P010"), "strict missing separator emits P010");
                check(d.value(QStringLiteral("severity")).toString() == QStringLiteral("error"), "P010 strict severity error");
            }
            check(caught, "strict mode throws for missing separator");
        }

        // 9. Caller-supplied mock tokens without positions preserve location without span
        {
            JsonArray tokens = lexJson(QStringLiteral("search synth\nnoise().subchain(nme: \"typo\") {\n.bloom()\n}.write(o0)\n"));
            // Strip position from the tokens
            JsonArray stripped;
            for (const JsonValue& v : tokens) {
                JsonObject obj = v.toObject();
                obj.remove(QStringLiteral("position"));
                stripped.append(obj);
            }
            const JsonObject prog = parseJson(stripped);
            const JsonArray plans = prog.value(QStringLiteral("plans")).toArray();
            const JsonObject subchain = plans.at(0).toObject().value(QStringLiteral("chain")).toArray().at(1).toObject();
            const JsonArray diags = subchain.value(QStringLiteral("subchainArgumentDiagnostics")).toArray();
            check(diags.size() == 1, "mock tokens emit 1 arg diagnostic");
            const JsonObject d0 = diags.at(0).toObject();
            check(d0.value(QStringLiteral("code")).toString() == QStringLiteral("P008"), "mock token diag code is P008");
            check(d0.contains(QStringLiteral("location")), "mock token diag has location");
            check(!d0.contains(QStringLiteral("span")), "mock token diag does NOT have span");
        }
    }

    {
        const nm::Value native = nm::parse(nm::lex(u"search synth\nnoise().write(o0)\nrender(o0)\n"));
        const nm::Value* plans = native.is_object() ? native.as_object().find(u"plans") : nullptr;
        check(plans && plans->is_array() && plans->as_array().size() == 1,
              "native parse API returns a Value AST with one plan");
    }
    {
        bool matched = false;
        try { nm::parse(nm::lex(u"search synth\nnoise(constructor)")); }
        catch (const nm::DslSyntaxError& err) {
            matched = err.message() == u"Unexpected token function Object() { [native code] } at line 2 col 7";
        }
        check(matched, "inherited constructor token preserves JS SyntaxError text");
    }
    if (g_failures == 0) {
        std::printf("ALL PASS (test_parser)\n");
        return 0;
    }
    std::printf("%d FAILURE(S) (test_parser)\n", g_failures);
    return 1;
}
