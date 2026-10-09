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
        const JsonObject prog = parseSrc(LangString(u"search synth\nsolid(0.1,0.2,0.3).write(o0)\nrender(o0)\n"));
        check(prog.value(LangString(u"type")).toString() == nm::NodeKind::Program, "Program root type");
        const JsonArray plans = prog.value(LangString(u"plans")).toArray();
        check(plans.size() == 1, "one plan (chain statement)");
        const JsonObject stmt = plans.at(0).toObject();
        check(!stmt.contains(LangString(u"type")), "chain-statement wrapper has NO type key");
        const JsonArray chain = stmt.value(LangString(u"chain")).toArray();
        check(chain.size() == 2, "chain has 2 elements: Call + Write");
        check(chain.at(0).toObject().value(LangString(u"type")).toString() == nm::NodeKind::Call, "chain[0] is Call");
        check(chain.at(0).toObject().value(LangString(u"name")).toString() == LangString(u"solid"),
              "chain[0].name == solid");
        check(chain.at(1).toObject().value(LangString(u"type")).toString() == nm::NodeKind::Write, "chain[1] is Write");
        check(stmt.value(LangString(u"write")).toObject().value(LangString(u"name")).toString()
                  == LangString(u"o0"),
              "top-level write shortcut == chain's terminal Write surface");
        check(stmt.value(LangString(u"write3d")).isNull(), "write3d is null when chain has no write3d");
        check(prog.value(LangString(u"render")).toObject().value(LangString(u"name")).toString()
                  == LangString(u"o0"),
              "render(o0) parsed");

        const JsonObject nsMeta = prog.value(LangString(u"namespace")).toObject();
        check(nsMeta.value(LangString(u"searchOrder")).toArray().size() == 1
                  && nsMeta.value(LangString(u"searchOrder")).toArray().at(0).toString() == LangString(u"synth"),
              "namespace.searchOrder == ['synth']");
        check(nsMeta.value(LangString(u"default")).toObject().value(LangString(u"name")).toString()
                  == LangString(u"synth"),
              "namespace.default.name == synth");
    }
    {
        // Only the LAST write()/write3d() in a chain sets the top-level
        // shortcut; every write() call still appears as its own chain
        // element regardless of position (empirically verified).
        const JsonObject prog =
            parseSrc(LangString(u"search synth,filter\nnoise().write(o0).invert().write(o1)\nrender(o1)\n"));
        const JsonArray chain = prog.value(LangString(u"plans")).toArray().at(0).toObject().value(LangString(u"chain")).toArray();
        check(chain.size() == 4, "mid-chain write: 4 chain elements (Call,Write,Call,Write)");
        check(chain.at(1).toObject().value(LangString(u"type")).toString() == nm::NodeKind::Write, "chain[1] mid-chain Write present");
        const JsonObject stmt = prog.value(LangString(u"plans")).toArray().at(0).toObject();
        check(stmt.value(LangString(u"write")).toObject().value(LangString(u"name")).toString() == LangString(u"o1"),
              "top-level write reflects the LAST write in the chain, not the first");
    }
    {
        // Comments are legal around chain dots (leadingComments), but NOT
        // inside a block before a statement (parser has no collectComments
        // call there) -- verified against the oracle: this throws.
        bool threw = false;
        try {
            parseSrc(LangString(u"search synth\nif (1) {\n  // comment\n  solid(0.1,0.2,0.3).write(o0)\n}\nrender(o0)\n"));
        } catch (const nm::DslSyntaxError&) {
            threw = true;
        }
        check(threw, "a comment immediately before a statement INSIDE a block is a syntax error (not collected there)");
    }
    {
        const JsonObject prog = parseSrc(LangString(
            u"search synth\n// leading comment\nsolid(0.1,0.2,0.3)\n  // pre-dot comment\n  .write(o0)\n// trailing comment\nrender(o0)\n// after render\n"));
        const JsonObject stmt = prog.value(LangString(u"plans")).toArray().at(0).toObject();
        check(stmt.value(LangString(u"leadingComments")).toArray().size() == 1, "top-level statement leadingComments captured");
        const JsonArray chain = stmt.value(LangString(u"chain")).toArray();
        check(chain.at(1).toObject().value(LangString(u"leadingComments")).toArray().at(0).toString()
                  == LangString(u"// pre-dot comment"),
              "pre-dot comment attaches to the following chain element");
        check(prog.value(LangString(u"render")).toObject().value(LangString(u"leadingComments")).toArray().size() == 1,
              "comment immediately before render() attaches to the render node");
        check(prog.value(LangString(u"trailingComments")).toArray().at(0).toString() == LangString(u"// after render"),
              "comment after render() lands in Program.trailingComments");
    }

    // ==================================================================
    // subchain
    // ==================================================================
    {
        const JsonObject prog = parseSrc(
            LangString(u"search filter\nnoise().subchain(name: \"fx\", id: \"sc1\") { .invert() }.write(o0)\nrender(o0)\n"));
        const JsonArray chain = prog.value(LangString(u"plans")).toArray().at(0).toObject().value(LangString(u"chain")).toArray();
        check(chain.size() == 3, "chain: Call, Subchain, Write");
        const JsonObject sub = chain.at(1).toObject();
        check(sub.value(LangString(u"type")).toString() == nm::NodeKind::Subchain, "subchain node type");
        check(sub.value(LangString(u"name")).toString() == LangString(u"fx"), "subchain kwarg name");
        check(sub.value(LangString(u"id")).toString() == LangString(u"sc1"), "subchain kwarg id");
        check(sub.value(LangString(u"body")).toArray().size() == 1, "subchain body has 1 call");
        check(sub.value(LangString(u"body")).toArray().at(0).toObject().value(LangString(u"name")).toString()
                  == LangString(u"invert"),
              "subchain body call name");
        check(sub.contains(LangString(u"loc")), "subchain has a loc");
    }
    {
        // Positional STRING shorthand for name -- same code path as the
        // name: kwarg (verified against the oracle).
        const JsonObject prog =
            parseSrc(LangString(u"search filter\nnoise().subchain(\"myname\") { .invert() }.write(o0)\nrender(o0)\n"));
        const JsonObject sub = prog.value(LangString(u"plans")).toArray().at(0).toObject().value(LangString(u"chain")).toArray().at(1).toObject();
        check(sub.value(LangString(u"name")).toString() == LangString(u"myname"), "positional string -> subchain.name");
        check(sub.value(LangString(u"id")).isNull(), "id defaults to null when omitted");
    }
    {
        // JS falsy-OR quirk (`kwargs.name?.value || null`): an explicitly
        // empty-string name/id STILL becomes JSON null, not "". Verified
        // against the reference oracle -- do not "fix" this.
        const JsonObject prog = parseSrc(
            LangString(u"search filter\nnoise().subchain(name: \"\", id: \"x\") { .invert() }.write(o0)\nrender(o0)\n"));
        const JsonObject sub = prog.value(LangString(u"plans")).toArray().at(0).toObject().value(LangString(u"chain")).toArray().at(1).toObject();
        check(sub.value(LangString(u"name")).isNull(), "subchain name: \"\" (empty string) becomes null (JS falsy-OR quirk, ported verbatim)");
        check(sub.value(LangString(u"id")).toString() == LangString(u"x"), "non-empty id kwarg unaffected");
    }
    {
        bool threw = false;
        try {
            parseSrc(LangString(u"search filter\nnoise().subchain() {}.write(o0)\nrender(o0)\n"));
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
            parseSrc(LangString(u"search synth\nsolid(a: 1, 2).write(o0)\nrender(o0)\n"));
        } catch (const nm::DslSyntaxError&) {
            threw = true;
        }
        check(threw, "keyword arg followed by positional arg throws (cannot mix)");
    }
    {
        bool threw = false;
        try {
            parseSrc(LangString(u"search synth\nsolid(1, a: 2).write(o0)\nrender(o0)\n"));
        } catch (const nm::DslSyntaxError&) {
            threw = true;
        }
        check(threw, "positional arg followed by keyword arg throws (cannot mix)");
    }
    {
        const JsonObject prog = parseSrc(LangString(u"search synth\nsolid(r: 0.1, g: 0.2, b: 0.3).write(o0)\nrender(o0)\n"));
        const JsonObject call = prog.value(LangString(u"plans")).toArray().at(0).toObject().value(LangString(u"chain")).toArray().at(0).toObject();
        check(call.value(LangString(u"args")).toArray().isEmpty(), "all-kwargs call has an empty args array");
        check(call.value(LangString(u"kwargs")).toObject().value(LangString(u"g")).toObject().value(LangString(u"value")).toDouble() == 0.2,
              "kwargs preserved by name");
    }

    // ==================================================================
    // vec / bracket literals
    // ==================================================================
    {
        const JsonObject prog = parseSrc(LangString(u"search synth\nlet x = [1, 2, 3]\nsolid(0.1,0.2,0.3).write(o0)\nrender(o0)\n"));
        const JsonArray vars = prog.value(LangString(u"vars")).toArray();
        check(vars.size() == 1, "one var");
        const JsonObject expr = vars.at(0).toObject().value(LangString(u"expr")).toObject();
        check(expr.value(LangString(u"type")).toString() == nm::NodeKind::ArrayLiteral, "[1,2,3] -> ArrayLiteral");
        const JsonArray elements = expr.value(LangString(u"elements")).toArray();
        check(elements.size() == 3, "3 elements");
        check(elements.at(0).toObject().value(LangString(u"value")).toDouble() == 1
                  && elements.at(2).toObject().value(LangString(u"value")).toDouble() == 3,
              "element values in order");
        check(expr.contains(LangString(u"loc")), "ArrayLiteral carries a loc (position of '[')");
    }
    {
        bool threw = false;
        try {
            parseSrc(LangString(u"search synth\nlet x = [1, 2\nsolid(0.1,0.2,0.3).write(o0)\nrender(o0)\n"));
        } catch (const nm::DslSyntaxError&) {
            threw = true;
        }
        check(threw, "unterminated array literal (missing ']') throws");
    }
    {
        const JsonObject prog = parseSrc(LangString(u"search synth\nlet x = []\nsolid(0.1,0.2,0.3).write(o0)\nrender(o0)\n"));
        const JsonObject expr = prog.value(LangString(u"vars")).toArray().at(0).toObject().value(LangString(u"expr")).toObject();
        check(expr.value(LangString(u"elements")).toArray().isEmpty(), "empty array literal []");
    }

    // ==================================================================
    // numeric constant folding (parse-time, double)
    // ==================================================================
    {
        const JsonObject prog = parseSrc(
            LangString(u"search synth\nlet a = -5 + 3\nlet b = -(2+3)*4\nlet c = 0.1 + 0.2\nlet d = ---5\nsolid(a,b,c).write(o0)\nrender(o0)\n"));
        const JsonArray vars = prog.value(LangString(u"vars")).toArray();
        check(numEq(vars.at(0).toObject().value(LangString(u"expr")).toObject().value(LangString(u"value")).toDouble(), -2.0),
              "-5 + 3 == -2 (unary binds tighter than the following additive op)");
        check(numEq(vars.at(1).toObject().value(LangString(u"expr")).toObject().value(LangString(u"value")).toDouble(), -20.0),
              "-(2+3)*4 == -20");
        const double c = vars.at(2).toObject().value(LangString(u"expr")).toObject().value(LangString(u"value")).toDouble();
        check(c == 0.30000000000000004, "0.1 + 0.2 keeps the exact IEEE double artifact 0.30000000000000004 (double end-to-end)");
        check(numEq(vars.at(3).toObject().value(LangString(u"expr")).toObject().value(LangString(u"value")).toDouble(), -5.0),
              "---5 == -5 (odd number of unary minuses)");
    }
    {
        const JsonObject prog = parseSrc(LangString(u"search synth\nlet x = Math.PI\nsolid(0.1,0.2,0.3).write(o0)\nrender(o0)\n"));
        const double pi = prog.value(LangString(u"vars")).toArray().at(0).toObject().value(LangString(u"expr")).toObject().value(LangString(u"value")).toDouble();
        check(pi == 3.141592653589793, "Math.PI folds to the exact reference double literal");
    }
    {
        // HEX -> Color: int(pair,16)/255 in double; 3-digit duplication;
        // alpha defaults to 1.0; 8-digit form carries alpha.
        const JsonObject prog = parseSrc(LangString(u"search synth\nlet a = #fff\nlet b = #8040c0\nlet c = #8040c080\nsolid(0.1,0.2,0.3).write(o0)\nrender(o0)\n"));
        const JsonArray vars = prog.value(LangString(u"vars")).toArray();
        const JsonObject colA = vars.at(0).toObject().value(LangString(u"expr")).toObject();
        check(colA.value(LangString(u"type")).toString() == nm::NodeKind::Color, "#fff -> Color");
        const JsonArray a = colA.value(LangString(u"value")).toArray();
        check(numEq(a.at(0).toDouble(), 1.0) && numEq(a.at(3).toDouble(), 1.0), "#fff -> [1,1,1,1] (3-digit duplication, alpha default 1)");
        const JsonArray b = vars.at(1).toObject().value(LangString(u"expr")).toObject().value(LangString(u"value")).toArray();
        check(numEq(b.at(0).toDouble(), 0x80 / 255.0) && numEq(b.at(1).toDouble(), 0x40 / 255.0) && numEq(b.at(2).toDouble(), 0xc0 / 255.0)
                  && numEq(b.at(3).toDouble(), 1.0),
              "#8040c0 -> rgb/255, alpha defaults to 1");
        const JsonArray c = vars.at(2).toObject().value(LangString(u"expr")).toObject().value(LangString(u"value")).toArray();
        check(numEq(c.at(3).toDouble(), 0x80 / 255.0), "#8040c080 -> alpha channel is the 4th hex pair / 255");
    }

    // ==================================================================
    // osc() 4-way disambiguation heuristic
    // ==================================================================
    {
        // isBareOsc: osc() with zero args/kwargs -> Oscillator with ALL defaults.
        const JsonObject prog = parseSrc(LangString(u"search synth\nsolid(osc(), 0.2, 0.3).write(o0)\nrender(o0)\n"));
        const JsonObject arg0 = prog.value(LangString(u"plans")).toArray().at(0).toObject().value(LangString(u"chain")).toArray().at(0).toObject().value(LangString(u"args")).toArray().at(0).toObject();
        check(arg0.value(LangString(u"type")).toString() == nm::NodeKind::Oscillator, "bare osc() -> Oscillator");
        check(arg0.value(LangString(u"oscType")).toObject().value(LangString(u"path")).toArray().at(1).toString() == LangString(u"sine"),
              "default oscType is oscKind.sine");
        check(numEq(arg0.value(LangString(u"max")).toObject().value(LangString(u"value")).toDouble(), 1.0), "default max is 1");
    }
    {
        // A single positional NON-oscKind-Member arg with no kwargs matches
        // none of the 4 transform conditions -> falls through as a plain
        // Call named "osc" (the synth.osc generator effect, not the value
        // oscillator). Verified against the oracle.
        const JsonObject prog = parseSrc(LangString(u"search synth\nlet x = osc(0.2)\nsolid(0.1,0.2,0.3).write(o0)\nrender(o0)\n"));
        const JsonObject expr = prog.value(LangString(u"vars")).toArray().at(0).toObject().value(LangString(u"expr")).toObject();
        check(expr.value(LangString(u"type")).toString() == nm::NodeKind::Call, "osc(0.2) (bare positional number, no kwargs) stays a plain Call");
        check(expr.value(LangString(u"name")).toString() == LangString(u"osc"), "Call name is osc");
    }
    {
        const JsonObject prog = parseSrc(LangString(u"search synth\nlet x = osc(type: oscKind.noise, min: 0.09, max: 0.46, speed: 3, seed: 5701)\nsolid(0.1,0.2,0.3).write(o0)\nrender(o0)\n"));
        const JsonObject expr = prog.value(LangString(u"vars")).toArray().at(0).toObject().value(LangString(u"expr")).toObject();
        check(expr.value(LangString(u"type")).toString() == nm::NodeKind::Oscillator, "osc(type: ..., ...) (all-osc-kwargs) -> Oscillator");
        check(numEq(expr.value(LangString(u"seed")).toObject().value(LangString(u"value")).toDouble(), 5701.0), "osc kwarg seed resolved");
    }
    {
        bool threw = false;
        try {
            parseSrc(LangString(u"search synth\nlet x = osc(type: oscKind.noise, bogus: 1)\nsolid(0.1,0.2,0.3).write(o0)\nrender(o0)\n"));
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
            LangString(u"search synth\nlet x = midi(channel: 2, mode: midiMode.trigger, sensitivity: 0.5)\nsolid(0.1,0.2,0.3).write(o0)\nrender(o0)\n"));
        const JsonObject midi = prog.value(LangString(u"vars")).toArray().at(0).toObject().value(LangString(u"expr")).toObject();
        check(midi.value(LangString(u"type")).toString() == nm::NodeKind::Midi, "midi() -> Midi node");
        check(numEq(midi.value(LangString(u"channel")).toObject().value(LangString(u"value")).toDouble(), 2.0), "midi kwarg channel resolved");
        check(midi.value(LangString(u"mode")).toObject().value(LangString(u"path")).toArray().at(1).toString() == LangString(u"trigger"),
              "midi kwarg mode resolved");
    }
    {
        bool threw = false;
        try {
            parseSrc(LangString(u"search synth\nlet x = midi()\nsolid(0.1,0.2,0.3).write(o0)\nrender(o0)\n"));
        } catch (const nm::DslSyntaxError&) {
            threw = true;
        }
        check(threw, "midi() with no channel throws (channel is required)");
    }
    {
        const JsonObject prog = parseSrc(LangString(u"search synth\nlet x = audio(audioBand.low)\nsolid(0.1,0.2,0.3).write(o0)\nrender(o0)\n"));
        const JsonObject audio = prog.value(LangString(u"vars")).toArray().at(0).toObject().value(LangString(u"expr")).toObject();
        check(audio.value(LangString(u"type")).toString() == nm::NodeKind::Audio, "audio() -> Audio node");
        check(audio.value(LangString(u"band")).toObject().value(LangString(u"path")).toArray().at(1).toString() == LangString(u"low"),
              "audio positional band resolved");
    }
    {
        const JsonObject prog = parseSrc(LangString(
            u"search synth\nlet x = midi(channel: 2, midiMode.trigger, 0.25, 0.75, 0.5, name: \"Controller\", id: \"port-a\")\n"
            "solid(0.1,0.2,0.3).write(o0)\nrender(o0)\n"));
        const JsonObject midi = prog.value(LangString(u"vars")).toArray().at(0).toObject().value(LangString(u"expr")).toObject();
        check(midi.value(LangString(u"mode")).toObject().value(LangString(u"path")).toArray().at(1).toString()
                  == LangString(u"trigger"),
              "midi dense positionals fill fields skipped by kwargs");
        check(numEq(midi.value(LangString(u"min")).toObject().value(LangString(u"value")).toDouble(), 0.25)
                  && numEq(midi.value(LangString(u"max")).toObject().value(LangString(u"value")).toDouble(), 0.75),
              "midi mixed positional min/max resolved");
        check(midi.value(LangString(u"name")).toObject().value(LangString(u"value")).toString()
                  == LangString(u"Controller")
                  && midi.value(LangString(u"id")).toObject().value(LangString(u"value")).toString()
                         == LangString(u"port-a"),
              "midi device identity retained");
    }
    {
        const JsonObject prog = parseSrc(LangString(
            u"search synth\nlet x = audio(band: audioBand.raw, 0.25, 0.75, channel: 2, name: \"Interface\", id: \"device-b\")\n"
            "solid(0.1,0.2,0.3).write(o0)\nrender(o0)\n"));
        const JsonObject audio = prog.value(LangString(u"vars")).toArray().at(0).toObject().value(LangString(u"expr")).toObject();
        check(audio.value(LangString(u"band")).toObject().value(LangString(u"path")).toArray().at(1).toString()
                  == LangString(u"raw"),
              "audio raw band retained");
        check(numEq(audio.value(LangString(u"min")).toObject().value(LangString(u"value")).toDouble(), 0.25)
                  && numEq(audio.value(LangString(u"max")).toObject().value(LangString(u"value")).toDouble(), 0.75),
              "audio dense positionals fill fields skipped by kwargs");
        check(audio.value(LangString(u"channel")).toObject().value(LangString(u"value")).toDouble() == 2.0
                  && audio.value(LangString(u"name")).toObject().value(LangString(u"value")).toString()
                         == LangString(u"Interface")
                  && audio.value(LangString(u"id")).toObject().value(LangString(u"value")).toString()
                         == LangString(u"device-b"),
              "audio device identity and channel retained");
    }
    for (const LangString& source : {
             LangString(u"search synth\nlet x = midi(1, id: \"port-a\")\n"),
             LangString(u"search synth\nlet x = midi(1, midiMode.velocity, 0, 1, 1, \"Controller\")\n"),
             LangString(u"search synth\nlet x = midi(1, bogus: 1)\n"),
             LangString(u"search synth\nlet x = midi(1, name: Controller)\n"),
             LangString(u"search synth\nlet x = midi(1, name: \"\")\n"),
             LangString(u"search synth\nlet x = audio(audioBand.low, name: \"Interface\")\n"),
             LangString(u"search synth\nlet x = audio(audioBand.low, id: \"device-b\")\n"),
             LangString(u"search synth\nlet x = audio(audioBand.low, 0, 1, 2)\n"),
             LangString(u"search synth\nlet x = audio(audioBand.low, bogus: 1)\n"),
             LangString(u"search synth\nlet x = audio(audioBand.low, channel: 1, name: Interface)\n"),
             LangString(u"search synth\nlet x = audio(audioBand.low, channel: 1, name: \"\")\n"),
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
                 LangString(u"search synth\nlet x = midi(1, zzz: 1, aaa: 2)\n"),
                 LangString(u"midi() unknown parameter 'zzz' at line 2 col 9. Valid: channel, mode, min, max, "
                                "sensitivity, name, id, cc, nrpn, zone, members")},
             std::pair{
                 LangString(u"search synth\nlet x = audio(audioBand.low, zzz: 1, aaa: 2)\n"),
                 LangString(u"audio() unknown parameter 'zzz' at line 2 col 9. Valid: band, min, max, channel, "
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
        const JsonObject prog = parseSrc(LangString(u"search synth\nfrom(filter, blur(amount: 5)).write(o0)\nrender(o0)\n"));
        const JsonObject call = prog.value(LangString(u"plans")).toArray().at(0).toObject().value(LangString(u"chain")).toArray().at(0).toObject();
        check(call.value(LangString(u"name")).toString() == LangString(u"blur"), "from(filter, blur(...)) replaces the call with the target");
        const JsonObject ns = call.value(LangString(u"namespace")).toObject();
        check(ns.value(LangString(u"name")).toString() == LangString(u"filter") && ns.value(LangString(u"fromOverride")).toBool(),
              "from() injects a namespace override with fromOverride:true");
    }
    {
        bool threw = false;
        try {
            parseSrc(LangString(u"search synth\nfoo.bar.baz()\nrender(o0)\n"));
        } catch (const nm::DslSyntaxError&) {
            threw = true;
        }
        check(threw, "a 2+-segment dotted call (foo.bar.baz()) is a syntax error, not a namespaced call (verified against oracle: 'Expect (' )");
    }
    {
        bool threw = false;
        try {
            parseSrc(LangString(u"search synth\nnd.noise().write(o0)\nrender(o0)\n"));
        } catch (const nm::DslSyntaxError&) {
            threw = true;
        }
        check(threw, "inline single-segment namespace call (nd.noise()) is explicitly forbidden");
    }
    {
        const JsonObject prog = parseSrc(LangString(u"search synth3d\nlet a = read3d(vol0)\nread3d(vol0, geo0).write3d(vol1, geo1)\nrender(o0)\n"));
        const JsonObject single = prog.value(LangString(u"vars")).toArray().at(0).toObject().value(LangString(u"expr")).toObject();
        check(single.value(LangString(u"type")).toString() == nm::NodeKind::Read3D, "read3d(vol0) -> Read3D");
        check(single.value(LangString(u"geo")).isNull(), "single-arg read3d() leaves geo == null");
        const JsonObject stmt = prog.value(LangString(u"plans")).toArray().at(0).toObject();
        check(stmt.value(LangString(u"write3d")).toObject().value(LangString(u"tex3d")).toObject().value(LangString(u"name")).toString()
                  == LangString(u"vol1"),
              "write3d() sets the top-level write3d shortcut {tex3d,geo} (no type key)");
    }

    // ==================================================================
    // search directive requirements
    // ==================================================================
    {
        bool threw = false;
        try {
            parseSrc(LangString(u"solid(0.1,0.2,0.3).write(o0)\nrender(o0)\n"));
        } catch (const nm::DslSyntaxError& e) {
            threw = true;
            check(e.line() == 3, "missing-search error has EOF location line 3");
            check(!LangString(e.message()).contains(LangString(u"at line")), "missing-search error has no 'at line' suffix");
            check(JsonValue(e.diagnostic()).toObject().value(LangString(u"code")).toString() == LangString(u"P004"), "missing-search diagnostic code is P004");
        }
        check(threw, "missing search directive throws");
    }
    {
        bool threw = false;
        try {
            parseSrc(LangString(u"search bogus\nsolid(0.1,0.2,0.3).write(o0)\nrender(o0)\n"));
        } catch (const nm::DslSyntaxError&) {
            threw = true;
        }
        check(threw, "invalid namespace name throws");
    }
    {
        bool threw = false;
        try {
            parseSrc(LangString(u"search synth\nsearch filter\nsolid(0.1,0.2,0.3).write(o0)\nrender(o0)\n"));
        } catch (const nm::DslSyntaxError&) {
            threw = true;
        }
        check(threw, "a second search directive throws");
    }
    {
        bool threw = false;
        try {
            parseSrc(LangString(u"search synth\nsolid(0.1,0.2,0.3).write(o0)\nsearch filter\nrender(o0)\n"));
        } catch (const nm::DslSyntaxError&) {
            threw = true;
        }
        check(threw, "a search directive after other statements throws");
    }

    // ==================================================================
    // write() surface variants
    // ==================================================================
    {
        const JsonObject prog = parseSrc(LangString(
            u"search synth\nnoise().write(xyz0)\nnoise().write(vel1)\nnoise().write(rgba2)\nnoise().write(mesh3)\nnoise().write(none)\nrender(o0)\n"));
        const JsonArray plans = prog.value(LangString(u"plans")).toArray();
        check(plans.size() == 5, "5 chain statements");
        const struct { const char* type; const char* name; } expected[5] = {
            {"XyzRef", "xyz0"}, {"VelRef", "vel1"}, {"RgbaRef", "rgba2"}, {"MeshRef", "mesh3"}, {"OutputRef", "none"},
        };
        bool allMatch = true;
        for (int i = 0; i < 5; ++i) {
            const JsonObject w = plans.at(i).toObject().value(LangString(u"write")).toObject();
            allMatch = allMatch && w.value(LangString(u"type")).toString() == LangString::fromUtf8(expected[i].type)
                       && w.value(LangString(u"name")).toString() == LangString::fromUtf8(expected[i].name);
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

        const LangVector<ExpectCase> cases = {
            {"opening parenthesis", LangString(u"search synth\nrender o0"), LangString(u"P001"), LangString(u"Expect '(' at line 2 col 8"), 2, 8},
            {"closing parenthesis at EOF", LangString(u"search synth\nrender(o0"), LangString(u"P002"), LangString(u"Expect ')' at line 2 col 10"), 2, 10},
            {"identifier", LangString(u"search synth\nlet = 1"), LangString(u"P001"), LangString(u"Expected identifier at line 2 col 5"), 2, 5},
            {"assignment sign", LangString(u"search synth\nlet x 1"), LangString(u"P001"), LangString(u"Expect '=' at line 2 col 7"), 2, 7},
            {"block opening", LangString(u"search synth\nif(true) return 1"), LangString(u"P001"), LangString(u"Expect '{' at line 2 col 10"), 2, 10},
            {"end of input", LangString(u"search synth\nrender(o0) xyz"), LangString(u"P001"), LangString(u"Expected end of input at line 2 col 12"), 2, 12},
            {"call closing parenthesis", LangString(u"search synth\nfoo(1"), LangString(u"P002"), LangString(u"Expect ')' at line 2 col 6"), 2, 6},
            {"write3d separator", LangString(u"search synth\nfoo().write3d(tex3d0 geo0)"), LangString(u"P001"), LangString(u"Expect ',' between tex3d and geo in write3d() at line 2 col 22"), 2, 22},
            {"CRLF and tab", LangString::fromUtf8("// \xF0\x9F\x98\x80\r\nsearch synth\r\n\trender(o0"), LangString(u"P002"), LangString(u"Expect ')' at line 3 col 11"), 3, 11},
            {"UTF-16 column", LangString::fromUtf8("search synth\nlet x = \"\xF0\x9F\x98\x80\"; render o0"), LangString(u"P001"), LangString(u"Expect '(' at line 2 col 22"), 2, 22},
        };

        for (const auto& c : cases) {
            bool caught = false;
            try {
                parseSrc(c.source);
            } catch (const nm::DslSyntaxError& err) {
                caught = true;
                check(err.message() == c.message,
                      LangString(u"parser diagnostic message matches for %1").arg(LangString::fromUtf8(c.name)).toStdString().c_str());
                const JsonObject d = JsonValue(err.diagnostic()).toObject();
                check(!d.isEmpty(),
                      LangString(u"parser diagnostic payload present for %1").arg(LangString::fromUtf8(c.name)).toStdString().c_str());
                check(d.value(LangString(u"code")).toString() == c.code,
                      LangString(u"parser diagnostic code matches for %1").arg(LangString::fromUtf8(c.name)).toStdString().c_str());
                check(d.value(LangString(u"stage")).toString() == LangString(u"parser"),
                      LangString(u"parser diagnostic stage matches for %1").arg(LangString::fromUtf8(c.name)).toStdString().c_str());
                check(d.value(LangString(u"severity")).toString() == LangString(u"error"),
                      LangString(u"parser diagnostic severity matches for %1").arg(LangString::fromUtf8(c.name)).toStdString().c_str());
                check(d.value(LangString(u"message")).toString() == c.message,
                      LangString(u"parser diagnostic message field matches for %1").arg(LangString::fromUtf8(c.name)).toStdString().c_str());
                const JsonObject span = d.value(LangString(u"span")).toObject();
                check(span.contains(LangString(u"start")) && span.contains(LangString(u"end")),
                      LangString(u"parser diagnostic span is present for %1").arg(LangString::fromUtf8(c.name)).toStdString().c_str());

                const JsonObject loc = d.value(LangString(u"location")).toObject();
                check(loc.value(LangString(u"line")).toInt() == c.line,
                      LangString(u"parser diagnostic location line matches for %1").arg(LangString::fromUtf8(c.name)).toStdString().c_str());
                check(loc.value(LangString(u"column")).toInt() == c.column,
                      LangString(u"parser diagnostic location column matches for %1").arg(LangString::fromUtf8(c.name)).toStdString().c_str());
            }
            check(caught, LangString(u"throws DslSyntaxError for %1").arg(LangString::fromUtf8(c.name)).toStdString().c_str());
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
        const LangVector<SubchainCase> cases = {
            {"non-string argument", LangString(u"search synth\nread(o0).subchain(name: 1) { .diagProbe() }"), LangString(u"Expected string value for subchain name at line 2 col 25"), 2, 25},
            {"argument at EOF", LangString(u"search synth\nread(o0).subchain(name:"), LangString(u"Expected string value for subchain name at line 2 col 24"), 2, 24},
            {"missing body dot", LangString(u"search synth\nread(o0).subchain() { diagProbe() }"), LangString(u"Expected '.' before chain element in subchain body at line 2 col 23"), 2, 23},
            {"body at EOF", LangString(u"search synth\nread(o0).subchain() {"), LangString(u"Expected '.' before chain element in subchain body at line 2 col 22"), 2, 22},
            {"empty body", LangString(u"search synth\nread(o0).subchain() {}"), LangString(u"Subchain body cannot be empty at line 2 col 10"), 2, 10},
            {"comment-only body", LangString(u"search synth\nread(o0).subchain() { /* empty */ }"), LangString(u"Subchain body cannot be empty at line 2 col 10"), 2, 10},
            {"CRLF tab and UTF-16 argument", LangString::fromUtf8("// \xF0\x9F\x98\x80\r\nsearch synth\r\n\tread(o0).subchain(name: \"\xF0\x9F\x98\x80\", id: 1) { .diagProbe() }"), LangString(u"Expected string value for subchain id at line 3 col 36"), 3, 36},
            {"missing dot after comment", LangString::fromUtf8("search synth\nread(o0).subchain() { /* \xF0\x9F\x98\x80 */ missing() }"), LangString(u"Expected '.' before chain element in subchain body at line 2 col 32"), 2, 32},
            {"unclosed nonempty body", LangString(u"search synth\nread(o0).subchain() { .diagProbe()"), LangString(u"Expected '.' before chain element in subchain body at line 2 col 35"), 2, 35},
        };
        for (const auto& c : cases) {
            bool caught = false;
            try {
                parseSrc(c.source);
            } catch (const nm::DslSyntaxError& err) {
                caught = true;
                const JsonObject d = JsonValue(err.diagnostic()).toObject();
                const JsonObject loc = d.value(LangString(u"location")).toObject();
                const JsonObject span = d.value(LangString(u"span")).toObject();
                check(err.message() == c.message && d.value(LangString(u"code")).toString() == LangString(u"P006")
                          && d.value(LangString(u"stage")).toString() == LangString(u"parser")
                          && d.value(LangString(u"severity")).toString() == LangString(u"error")
                          && d.value(LangString(u"message")).toString() == c.message
                          && span.contains(LangString(u"start")) && span.contains(LangString(u"end"))
                          && loc.value(LangString(u"line")).toInt() == c.line
                          && loc.value(LangString(u"column")).toInt() == c.column,
                      LangString(u"subchain P006 diagnostic matches for %1").arg(LangString::fromUtf8(c.name)).toStdString().c_str());
            }
            check(caught, LangString(u"throws DslSyntaxError for subchain %1").arg(LangString::fromUtf8(c.name)).toStdString().c_str());

            // Tokens without coordinates: the same code, and a null location.
            JsonArray bare;
            for (const JsonValue& val : lexJson(c.source)) {
                JsonObject tok = val.toObject();
                tok.remove(LangString(u"line"));
                tok.remove(LangString(u"col"));
                tok.remove(LangString(u"position"));
                bare.append(tok);
            }
            bool caughtBare = false;
            try {
                parseJson(bare);
            } catch (const nm::DslSyntaxError& err) {
                caughtBare = true;
                const JsonObject d = JsonValue(err.diagnostic()).toObject();
                check(d.value(LangString(u"code")).toString() == LangString(u"P006") && d.value(LangString(u"location")).isNull()
                          && d.value(LangString(u"message")).toString() == err.message(),
                      LangString(u"subchain P006 location is null without coordinates for %1").arg(LangString::fromUtf8(c.name)).toStdString().c_str());
            }
            check(caughtBare, LangString(u"throws DslSyntaxError for coordinate-free subchain %1").arg(LangString::fromUtf8(c.name)).toStdString().c_str());
        }

        // Shared expectations inside a subchain keep their own codes.
        const LangVector<std::pair<LangString, std::pair<LangString, LangString>>> precedence = {
            {LangString(u"search synth\nread(o0).subchain(1) {}"), {LangString(u"P002"), LangString(u"Expect ')' after subchain arguments at line 2 col 19")}},
            {LangString(u"search synth\nread(o0).subchain() { . }"), {LangString(u"P001"), LangString(u"Expected identifier at line 2 col 25")}},
        };
        for (const auto& [source, expected] : precedence) {
            bool caught = false;
            try {
                parseSrc(source);
            } catch (const nm::DslSyntaxError& err) {
                caught = err.message() == expected.second
                         && JsonValue(err.diagnostic()).toObject().value(LangString(u"code")).toString() == expected.first;
            }
            check(caught, LangString(u"subchain keeps %1 for: %2").arg(expected.first, expected.second).toStdString().c_str());
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

        const LangVector<CoordCase> coordCases = {
            {false, JsonValue(), false, JsonValue(), LangString(u"Expect '(' at line undefined col undefined")},
            {true, 1, false, JsonValue(), LangString(u"Expect '(' at line 1 col undefined")},
            {true, 0, true, 1, LangString(u"Expect '(' at line 0 col 1")},
            {true, 1, true, LangString(u"NaN"), LangString(u"Expect '(' at line 1 col NaN")},
        };

        for (const auto& cc : coordCases) {
            const JsonArray origTokens = lexJson(LangString(u"search synth\nrender o0"));
            JsonArray modifiedTokens;
            for (const JsonValue& val : origTokens) {
                JsonObject tokObj = val.toObject();
                if (tokObj.value(LangString(u"type")).toString() == LangString(u"OUTPUT_REF")) {
                    tokObj.remove(LangString(u"line"));
                    tokObj.remove(LangString(u"col"));
                    tokObj.remove(LangString(u"position"));
                    if (cc.hasLine) tokObj.insert(LangString(u"line"), cc.lineVal);
                    if (cc.hasCol) tokObj.insert(LangString(u"col"), cc.colVal);
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
                check(d.value(LangString(u"code")).toString() == LangString(u"P001"), "diagnostic code P001");
                check(d.value(LangString(u"stage")).toString() == LangString(u"parser"), "diagnostic stage parser");
                check(d.value(LangString(u"severity")).toString() == LangString(u"error"), "diagnostic severity error");
                check(d.value(LangString(u"message")).toString() == cc.expectedMsg, "diagnostic message matches");
                check(d.value(LangString(u"location")).isNull(), "diagnostic location is null when coordinates unavailable");
                check(d.value(LangString(u"span")).isNull(), "diagnostic span is null");
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

        const LangVector<AutoCase> autoCases = {
            {"osc unknown param", LangString(u"search synth\nlet x = osc(type: oscKind.sine, bogus: 1)"),
             LangString(u"osc() unknown parameter 'bogus' at line 2 col 9. Valid: type, min, max, speed, offset, seed"), 2, 9},
            {"midi excess positional", LangString(u"search synth\nlet x = midi(1, 2, 3, 4, 5, 6)"),
             LangString(u"midi() name, id, cc, nrpn, zone and members are keyword-only at line 2 col 9"), 2, 9},
            {"midi unknown param", LangString(u"search synth\nlet x = midi(bogus: 1)"),
             LangString(u"midi() unknown parameter 'bogus' at line 2 col 9. Valid: channel, mode, min, max, sensitivity, name, id, cc, nrpn, zone, members"), 2, 9},
            {"midi excess positional with kwargs", LangString(u"search synth\nlet x = midi(1, 2, 3, 4, 5, channel: 1)"),
             LangString(u"midi() has an excess positional argument at line 2 col 9"), 2, 9},
            {"midi missing channel or zone", LangString(u"search synth\nlet x = midi()"),
             LangString(u"midi() requires 'channel' or 'zone' argument at line 2 col 9"), 2, 9},
            {"midi channel and zone mutually exclusive", LangString(u"search synth\nlet x = midi(1, zone: 1)"),
             LangString(u"midi() 'channel' and 'zone' are mutually exclusive at line 2 col 9"), 2, 9},
            {"midi members requires zone", LangString(u"search synth\nlet x = midi(1, members: 2)"),
             LangString(u"midi() 'members' requires 'zone' at line 2 col 9"), 2, 9},
            {"midi id requires name", LangString(u"search synth\nlet x = midi(1, id: \"port\")"),
             LangString(u"midi() 'id' requires readable 'name' at line 2 col 9"), 2, 9},
            {"midi name requires string", LangString(u"search synth\nlet x = midi(1, name: 1)"),
             LangString(u"midi() 'name' requires a quoted string at line 2 col 9"), 2, 9},
            {"midi name empty", LangString(u"search synth\nlet x = midi(1, name: \"\")"),
             LangString(u"midi() 'name' must not be empty at line 2 col 9"), 2, 9},
            {"midi id requires string", LangString(u"search synth\nlet x = midi(1, name: \"port\", id: 1)"),
             LangString(u"midi() 'id' requires a quoted string at line 2 col 9"), 2, 9},
            {"midi id empty", LangString(u"search synth\nlet x = midi(1, name: \"port\", id: \"\")"),
             LangString(u"midi() 'id' must not be empty at line 2 col 9"), 2, 9},
            {"audio excess positional", LangString(u"search synth\nlet x = audio(1, 2, 3, 4)"),
             LangString(u"audio() channel, name and id are keyword-only at line 2 col 9"), 2, 9},
            {"audio unknown param", LangString(u"search synth\nlet x = audio(bogus: 1)"),
             LangString(u"audio() unknown parameter 'bogus' at line 2 col 9. Valid: band, min, max, channel, name, id"), 2, 9},
            {"audio excess positional with kwargs", LangString(u"search synth\nlet x = audio(1, 2, 3, band: 1)"),
             LangString(u"audio() has an excess positional argument at line 2 col 9"), 2, 9},
            {"audio missing band", LangString(u"search synth\nlet x = audio()"),
             LangString(u"audio() requires 'band' argument at line 2 col 9"), 2, 9},
            {"audio id requires name", LangString(u"search synth\nlet x = audio(1, id: \"device\")"),
             LangString(u"audio() 'id' requires readable 'name' at line 2 col 9"), 2, 9},
            {"audio selected device requires name and channel", LangString(u"search synth\nlet x = audio(1, name: \"device\")"),
             LangString(u"audio() selected device requires both 'name' and 'channel' at line 2 col 9"), 2, 9},
            {"audio name requires string", LangString(u"search synth\nlet x = audio(1, channel: 1, name: 1)"),
             LangString(u"audio() 'name' requires a quoted string at line 2 col 9"), 2, 9},
            {"audio name empty", LangString(u"search synth\nlet x = audio(1, channel: 1, name: \"\")"),
             LangString(u"audio() 'name' must not be empty at line 2 col 9"), 2, 9},
            {"audio id requires string", LangString(u"search synth\nlet x = audio(1, channel: 1, name: \"device\", id: 1)"),
             LangString(u"audio() 'id' requires a quoted string at line 2 col 9"), 2, 9},
            {"audio id empty", LangString(u"search synth\nlet x = audio(1, channel: 1, name: \"device\", id: \"\")"),
             LangString(u"audio() 'id' must not be empty at line 2 col 9"), 2, 9},
        };

        for (const auto& c : autoCases) {
            bool caught = false;
            try {
                parseSrc(c.source);
            } catch (const nm::DslSyntaxError& err) {
                caught = true;
                check(err.message() == c.message,
                      LangString(u"automation diagnostic message matches for %1").arg(LangString::fromUtf8(c.name)).toStdString().c_str());
                const JsonObject d = JsonValue(err.diagnostic()).toObject();
                check(!d.isEmpty(),
                      LangString(u"automation diagnostic payload present for %1").arg(LangString::fromUtf8(c.name)).toStdString().c_str());
                check(d.value(LangString(u"code")).toString() == LangString(u"P003"),
                      LangString(u"automation diagnostic code is P003 for %1").arg(LangString::fromUtf8(c.name)).toStdString().c_str());
                check(d.value(LangString(u"stage")).toString() == LangString(u"parser"),
                      LangString(u"automation diagnostic stage matches for %1").arg(LangString::fromUtf8(c.name)).toStdString().c_str());
                check(d.value(LangString(u"severity")).toString() == LangString(u"error"),
                      LangString(u"automation diagnostic severity matches for %1").arg(LangString::fromUtf8(c.name)).toStdString().c_str());
                check(d.value(LangString(u"message")).toString() == c.message,
                      LangString(u"automation diagnostic message field matches for %1").arg(LangString::fromUtf8(c.name)).toStdString().c_str());
                const JsonObject span = d.value(LangString(u"span")).toObject();
                check(span.contains(LangString(u"start")) && span.contains(LangString(u"end")),
                      LangString(u"automation diagnostic span is present for %1").arg(LangString::fromUtf8(c.name)).toStdString().c_str());
                const JsonObject loc = d.value(LangString(u"location")).toObject();
                check(loc.value(LangString(u"line")).toInt() == c.line,
                      LangString(u"automation diagnostic location line matches for %1").arg(LangString::fromUtf8(c.name)).toStdString().c_str());
                check(loc.value(LangString(u"column")).toInt() == c.column,
                      LangString(u"automation diagnostic location column matches for %1").arg(LangString::fromUtf8(c.name)).toStdString().c_str());
            }
            check(caught, LangString(u"throws DslSyntaxError for %1").arg(LangString::fromUtf8(c.name)).toStdString().c_str());
        }
    }

    // ==================================================================
    // Structured search directive diagnostics (P004)
    // ==================================================================
    {
        const LangString missingSearchMsg = LangString(u"Missing required 'search' directive. Every program must start with 'search <namespace>, ...' to specify namespace search order.");
        struct SearchCase {
            const char* name;
            LangString source;
            LangString message;
            int line;
            int column;
        };

        const LangVector<SearchCase> searchCases = {
            {"empty program", LangString(u""), missingSearchMsg, 1, 1},
            {"missing directive after statements", LangString(u"let x = 1"), missingSearchMsg, 1, 10},
            {"duplicate directive", LangString(u"search synth search filter"),
             LangString(u"Only one search directive is allowed per program at line 1 col 14"), 1, 14},
            {"invalid namespace", LangString(u"search bogus"),
             LangString(u"Invalid namespace 'bogus' at line 1 col 8. Valid namespaces: io, classicNoisedeck, synth, mixer, filter, render, points, synth3d, filter3d, user"), 1, 8},
            {"missing first namespace", LangString(u"search"),
             LangString(u"Expected namespace identifier after search at line 1 col 7"), 1, 7},
            {"missing additional namespace", LangString(u"search synth,"),
             LangString(u"Expected namespace identifier after comma at line 1 col 14"), 1, 14},
            {"misplaced directive", LangString(u"let x = 1; search synth"),
             LangString(u"'search' directive must appear before other statements at line 1 col 12"), 1, 12},
            {"nested directive", LangString(u"search synth\nif(true) { search filter }"),
             LangString(u"'search' directive is only allowed at the start of the program at line 2 col 12"), 2, 12},
            {"CRLF and tab", LangString::fromUtf8("// \xF0\x9F\x98\x80\r\n\tsearch 1"),
             LangString(u"Expected namespace identifier after search at line 2 col 9"), 2, 9},
            {"UTF-16 column", LangString::fromUtf8("search synth\nlet x = \"\xF0\x9F\x98\x80\"; search filter"),
             LangString(u"'search' directive must appear before other statements at line 2 col 15"), 2, 15},
        };

        for (const auto& c : searchCases) {
            bool caught = false;
            try {
                parseSrc(c.source);
            } catch (const nm::DslSyntaxError& err) {
                caught = true;
                check(err.message() == c.message,
                      LangString(u"search diagnostic message matches for %1").arg(LangString::fromUtf8(c.name)).toStdString().c_str());
                const JsonObject d = JsonValue(err.diagnostic()).toObject();
                check(!d.isEmpty(),
                      LangString(u"search diagnostic payload present for %1").arg(LangString::fromUtf8(c.name)).toStdString().c_str());
                check(d.value(LangString(u"code")).toString() == LangString(u"P004"),
                      LangString(u"search diagnostic code is P004 for %1").arg(LangString::fromUtf8(c.name)).toStdString().c_str());
                check(d.value(LangString(u"stage")).toString() == LangString(u"parser"),
                      LangString(u"search diagnostic stage matches for %1").arg(LangString::fromUtf8(c.name)).toStdString().c_str());
                check(d.value(LangString(u"severity")).toString() == LangString(u"error"),
                      LangString(u"search diagnostic severity matches for %1").arg(LangString::fromUtf8(c.name)).toStdString().c_str());
                check(d.value(LangString(u"message")).toString() == c.message,
                      LangString(u"search diagnostic message field matches for %1").arg(LangString::fromUtf8(c.name)).toStdString().c_str());
                const JsonObject span = d.value(LangString(u"span")).toObject();
                check(span.contains(LangString(u"start")) && span.contains(LangString(u"end")),
                      LangString(u"search diagnostic span is present for %1").arg(LangString::fromUtf8(c.name)).toStdString().c_str());
                const JsonObject loc = d.value(LangString(u"location")).toObject();
                check(loc.value(LangString(u"line")).toInt() == c.line,
                      LangString(u"search diagnostic location line matches for %1").arg(LangString::fromUtf8(c.name)).toStdString().c_str());
                check(loc.value(LangString(u"column")).toInt() == c.column,
                      LangString(u"search diagnostic location column matches for %1").arg(LangString::fromUtf8(c.name)).toStdString().c_str());
            }
            check(caught, LangString(u"throws DslSyntaxError for %1").arg(LangString::fromUtf8(c.name)).toStdString().c_str());
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

        const LangVector<OutputCase> outputCases = {
            {"invalid render target", LangString(u"search synth\nrender(1)"),
             LangString(u"Expected output reference in render()"), 2, 8},
            {"render target at EOF", LangString(u"search synth\nrender("),
             LangString(u"Expected output reference in render()"), 2, 8},
            {"write in expression", LangString(u"search synth\nlet x = diagProbe().write(o0)"),
             LangString(u"'.write()' is only allowed in statement context at line 2 col 21"), 2, 21},
            {"write3d in expression", LangString(u"search synth\nlet x = diagProbe().write3d(vol0, geo0)"),
             LangString(u"'.write()' is only allowed in statement context at line 2 col 21"), 2, 21},
            {"missing write surface", LangString(u"search synth\ndiagProbe().write()"),
             LangString(u"write() requires an explicit surface reference (e.g., o0, o1, xyz0, vel0, rgba0, mesh0, none) at line 2 col 19"), 2, 19},
            {"write surface at EOF", LangString(u"search synth\ndiagProbe().write("),
             LangString(u"write() requires an explicit surface reference (e.g., o0, o1, xyz0, vel0, rgba0, mesh0, none) at line 2 col 19"), 2, 19},
            {"invalid write surface", LangString(u"search synth\ndiagProbe().write(1)"),
             LangString(u"write() requires an explicit surface reference (e.g., o0, o1, xyz0, vel0, rgba0, mesh0, none) at line 2 col 19"), 2, 19},
            {"invalid write3d texture", LangString(u"search synth\ndiagProbe().write3d(1, geo0)"),
             LangString(u"Expected tex3d reference in write3d() at line 2 col 21"), 2, 21},
            {"write3d texture at EOF", LangString(u"search synth\ndiagProbe().write3d("),
             LangString(u"Expected tex3d reference in write3d() at line 2 col 21"), 2, 21},
            {"invalid write3d geometry", LangString(u"search synth\ndiagProbe().write3d(vol0, 1)"),
             LangString(u"Expected geo reference in write3d() at line 2 col 27"), 2, 27},
            {"write3d geometry at EOF", LangString(u"search synth\ndiagProbe().write3d(vol0,"),
             LangString(u"Expected geo reference in write3d() at line 2 col 26"), 2, 26},
            {"CRLF and tab render target", LangString::fromUtf8("// \xF0\x9F\x98\x80\r\nsearch synth\r\n\trender(\"\xF0\x9F\x98\x80\")"),
             LangString(u"Expected output reference in render()"), 3, 9},
            {"UTF-16 render target column", LangString::fromUtf8("search synth\nlet x = \"\xF0\x9F\x98\x80\"; render(none)"),
             LangString(u"Expected output reference in render()"), 2, 22},
        };

        for (const auto& c : outputCases) {
            bool caught = false;
            try {
                parseSrc(c.source);
            } catch (const nm::DslSyntaxError& err) {
                caught = true;
                check(err.message() == c.message,
                      LangString(u"output diagnostic message matches for %1").arg(LangString::fromUtf8(c.name)).toStdString().c_str());
                const JsonObject d = JsonValue(err.diagnostic()).toObject();
                check(!d.isEmpty(),
                      LangString(u"output diagnostic payload present for %1").arg(LangString::fromUtf8(c.name)).toStdString().c_str());
                check(d.value(LangString(u"code")).toString() == LangString(u"P005"),
                      LangString(u"output diagnostic code is P005 for %1").arg(LangString::fromUtf8(c.name)).toStdString().c_str());
                check(d.value(LangString(u"stage")).toString() == LangString(u"parser"),
                      LangString(u"output diagnostic stage matches for %1").arg(LangString::fromUtf8(c.name)).toStdString().c_str());
                check(d.value(LangString(u"severity")).toString() == LangString(u"error"),
                      LangString(u"output diagnostic severity matches for %1").arg(LangString::fromUtf8(c.name)).toStdString().c_str());
                check(d.value(LangString(u"message")).toString() == c.message,
                      LangString(u"output diagnostic message field matches for %1").arg(LangString::fromUtf8(c.name)).toStdString().c_str());
                const JsonObject span = d.value(LangString(u"span")).toObject();
                check(span.contains(LangString(u"start")) && span.contains(LangString(u"end")),
                      LangString(u"output diagnostic span is present for %1").arg(LangString::fromUtf8(c.name)).toStdString().c_str());
                const JsonObject loc = d.value(LangString(u"location")).toObject();
                check(loc.value(LangString(u"line")).toInt() == c.line,
                      LangString(u"output diagnostic location line matches for %1").arg(LangString::fromUtf8(c.name)).toStdString().c_str());
                check(loc.value(LangString(u"column")).toInt() == c.column,
                      LangString(u"output diagnostic location column matches for %1").arg(LangString::fromUtf8(c.name)).toStdString().c_str());
            }
            check(caught, LangString(u"throws DslSyntaxError for %1").arg(LangString::fromUtf8(c.name)).toStdString().c_str());
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

        const LangVector<PrecedenceCase> precedenceCases = {
            {LangString(u"search synth\nrender o0"), LangString(u"P001"),
             LangString(u"Expect '(' at line 2 col 8")},
            {LangString(u"search synth\nrender(o0"), LangString(u"P002"),
             LangString(u"Expect ')' at line 2 col 10")},
            {LangString(u"search synth\nrender(o0) render(o1)"), LangString(u"P001"),
             LangString(u"Expected end of input at line 2 col 12")},
            {LangString(u"search synth\ndiagProbe().write(o0"), LangString(u"P002"),
             LangString(u"Expect ')' at line 2 col 21")},
            {LangString(u"search synth\ndiagProbe().write3d(vol0 geo0)"), LangString(u"P001"),
             LangString(u"Expect ',' between tex3d and geo in write3d() at line 2 col 26")},
        };

        for (const auto& c : precedenceCases) {
            bool caught = false;
            try {
                parseSrc(c.source);
            } catch (const nm::DslSyntaxError& err) {
                caught = true;
                check(err.message() == c.message, "precedence error message matches");
                const JsonObject d = JsonValue(err.diagnostic()).toObject();
                check(d.value(LangString(u"code")).toString() == c.code, "precedence diagnostic code matches");
            }
            check(caught, "throws DslSyntaxError for precedence case");
        }
    }

    // ==================================================================
    // Parser automation, search, and output diagnostics represent unavailable coordinates explicitly
    // ==================================================================
    {
        for (const LangString& src : {LangString(u"search synth\nlet x = midi()"),
                                   LangString(u"search bogus"),
                                   LangString(u"search synth\nrender(1)"),
                                   LangString(u"search synth\ndiagProbe().write()")}) {
            const JsonArray origTokens = lexJson(src);
            JsonArray modifiedTokens;
            for (const JsonValue& val : origTokens) {
                JsonObject tokObj = val.toObject();
                tokObj.remove(LangString(u"line"));
                tokObj.remove(LangString(u"col"));
                tokObj.remove(LangString(u"position"));
                modifiedTokens.append(tokObj);
            }
            bool caught = false;
            try {
                parseJson(modifiedTokens);
            } catch (const nm::DslSyntaxError& err) {
                caught = true;
                const JsonObject d = JsonValue(err.diagnostic()).toObject();
                check(d.value(LangString(u"code")).toString().startsWith(u'P'), "code starts with P");
                check(d.value(LangString(u"location")).isNull(), "location is null when coordinates unavailable");
            }
            check(caught, "throws DslSyntaxError for unavailable coordinates in P003/P004/P005");
        }
    }

    // P007 call-form diagnostics test suite
    {
        // 1. Inline namespace syntax
        bool caught = false;
        try {
            parseJson(lexJson(LangString(u"search synth\nlet x = synth.noise()\nrender x")));
        } catch (const nm::DslSyntaxError& err) {
            caught = true;
            const JsonObject d = JsonValue(err.diagnostic()).toObject();
            check(d.value(LangString(u"code")).toString() == LangString(u"P007"), "inline namespace emits P007");
            check(d.value(LangString(u"stage")).toString() == LangString(u"parser"), "P007 stage is parser");
            check(d.value(LangString(u"severity")).toString() == LangString(u"error"), "P007 severity is error");
            const JsonObject loc = d.value(LangString(u"location")).toObject();
            check(loc.value(LangString(u"line")).toInt() == 2, "P007 inline namespace line is 2");
            const JsonObject span = d.value(LangString(u"span")).toObject();
            check(span.contains(LangString(u"start")) && span.contains(LangString(u"end")), "P007 inline namespace has span");
        }
        check(caught, "throws P007 for inline namespace syntax");

        // 2. Mixed positional and keyword arguments
        caught = false;
        try {
            parseJson(lexJson(LangString(u"search synth\nlet x = noise(1, freq: 2)\nrender x")));
        } catch (const nm::DslSyntaxError& err) {
            caught = true;
            const JsonObject d = JsonValue(err.diagnostic()).toObject();
            check(d.value(LangString(u"code")).toString() == LangString(u"P007"), "mixed positional/keyword emits P007");
            check(d.value(LangString(u"message")).toString().contains(LangString(u"Cannot mix positional and keyword arguments")),
                  "P007 mixed positional/keyword message");
        }
        check(caught, "throws P007 for mixed positional/keyword args");

        // 3. from() call validation: named kwargs
        caught = false;
        try {
            parseJson(lexJson(LangString(u"search synth\nlet x = from(ns: synth, noise())\nrender x")));
        } catch (const nm::DslSyntaxError& err) {
            caught = true;
            const JsonObject d = JsonValue(err.diagnostic()).toObject();
            check(d.value(LangString(u"code")).toString() == LangString(u"P007"), "from() kwargs emits P007");
        }
        check(caught, "throws P007 for from() with named kwargs");

        // 4. from() call validation: arity mismatch
        caught = false;
        try {
            parseJson(lexJson(LangString(u"search synth\nlet x = from(synth)\nrender x")));
        } catch (const nm::DslSyntaxError& err) {
            caught = true;
            const JsonObject d = JsonValue(err.diagnostic()).toObject();
            check(d.value(LangString(u"code")).toString() == LangString(u"P007"), "from() arity emits P007");
            check(d.value(LangString(u"message")).toString().contains(LangString(u"'from' requires exactly two arguments")),
                  "P007 from() arity message");
        }
        check(caught, "throws P007 for from() with arity mismatch");
    }

    // P001 number coercion diagnostics test suite
    {
        // 1. Array literal coerced to number in arithmetic
        bool caught = false;
        try {
            parseJson(lexJson(LangString(u"search synth\nlet x = [1, 2] + 3\nrender x")));
        } catch (const nm::DslSyntaxError& err) {
            caught = true;
            const JsonObject d = JsonValue(err.diagnostic()).toObject();
            check(d.value(LangString(u"code")).toString() == LangString(u"P001"), "array coercion emits P001");
            check(d.value(LangString(u"message")).toString() == LangString(u"Expected number"), "P001 expected number message");
            const JsonObject loc = d.value(LangString(u"location")).toObject();
            check(loc.value(LangString(u"line")).toInt() == 2, "P001 array coercion line is 2");
            const JsonObject span = d.value(LangString(u"span")).toObject();
            check(span.contains(LangString(u"start")) && span.contains(LangString(u"end")), "P001 array coercion has span");
        }
        check(caught, "throws P001 for array coerced to number");
    }

    // Subchain argument validation test suite
    {
        // 1. Default parse accepts unknown subchain key without altering AST and attaches P008 report
        {
            const LangString src = LangString(u"search synth\nnoise().subchain(nme: \"typo\", name: \"ok\") {\n.bloom()\n}.write(o0)\n");
            const JsonObject prog = parseSrc(src);
            const JsonArray plans = prog.value(LangString(u"plans")).toArray();
            const JsonArray chain = plans.at(0).toObject().value(LangString(u"chain")).toArray();
            const JsonObject subchain = chain.at(1).toObject();
            check(subchain.value(LangString(u"type")).toString() == LangString(u"Subchain"), "chain[1] is Subchain");
            check(subchain.value(LangString(u"name")).toString() == LangString(u"ok"), "subchain name is 'ok'");
            check(subchain.value(LangString(u"id")).isNull(), "subchain id is null");
            const JsonArray diags = subchain.value(LangString(u"subchainArgumentDiagnostics")).toArray();
            check(diags.size() == 1, "subchain has 1 arg diagnostic");
            const JsonObject d0 = diags.at(0).toObject();
            check(d0.value(LangString(u"code")).toString() == LangString(u"P008"), "diag code is P008");
            check(d0.value(LangString(u"severity")).toString() == LangString(u"warning"), "P008 severity is warning");
            check(d0.value(LangString(u"message")).toString().contains(LangString(u"nme")), "P008 message mentions 'nme'");
            const JsonObject loc = d0.value(LangString(u"location")).toObject();
            check(loc.value(LangString(u"line")).toInt() == 2, "P008 location line 2");
            const JsonObject span = d0.value(LangString(u"span")).toObject();
            check(span.contains(LangString(u"start")) && span.contains(LangString(u"end")), "P008 span attached");
        }

        // 2. Default parse reports duplicate key with last value winning (P009)
        {
            const LangString src = LangString(u"search synth\nnoise().subchain(name: \"first\", name: \"second\") {\n.bloom()\n}.write(o0)\n");
            const JsonObject prog = parseSrc(src);
            const JsonArray plans = prog.value(LangString(u"plans")).toArray();
            const JsonObject subchain = plans.at(0).toObject().value(LangString(u"chain")).toArray().at(1).toObject();
            check(subchain.value(LangString(u"name")).toString() == LangString(u"second"), "last value wins for name: 'second'");
            const JsonArray diags = subchain.value(LangString(u"subchainArgumentDiagnostics")).toArray();
            check(diags.size() == 1, "subchain has 1 duplicate key diagnostic");
            const JsonObject d0 = diags.at(0).toObject();
            check(d0.value(LangString(u"code")).toString() == LangString(u"P009"), "diag code is P009");
            check(d0.value(LangString(u"severity")).toString() == LangString(u"warning"), "P009 severity is warning");
            check(d0.value(LangString(u"message")).toString().contains(LangString(u"name")), "P009 message mentions 'name'");
        }

        // 3. Default parse reports missing comma separator (P010)
        {
            const LangString src = LangString(u"search synth\nnoise().subchain(name: \"a\" id: \"b\") {\n.bloom()\n}.write(o0)\n");
            const JsonObject prog = parseSrc(src);
            const JsonArray plans = prog.value(LangString(u"plans")).toArray();
            const JsonObject subchain = plans.at(0).toObject().value(LangString(u"chain")).toArray().at(1).toObject();
            check(subchain.value(LangString(u"name")).toString() == LangString(u"a"), "subchain name is 'a'");
            check(subchain.value(LangString(u"id")).toString() == LangString(u"b"), "subchain id is 'b'");
            const JsonArray diags = subchain.value(LangString(u"subchainArgumentDiagnostics")).toArray();
            check(diags.size() == 1, "subchain has 1 missing separator diagnostic");
            const JsonObject d0 = diags.at(0).toObject();
            check(d0.value(LangString(u"code")).toString() == LangString(u"P010"), "diag code is P010");
            check(d0.value(LangString(u"severity")).toString() == LangString(u"warning"), "P010 severity is warning");
        }

        // 4. Co-occurring violations are reported in source order: P008, P010, P009
        {
            const LangString src = LangString(u"search synth\nnoise().subchain(nme: \"x\", name: \"a\" name: \"b\") {\n.bloom()\n}.write(o0)\n");
            const JsonObject prog = parseSrc(src);
            const JsonArray plans = prog.value(LangString(u"plans")).toArray();
            const JsonObject subchain = plans.at(0).toObject().value(LangString(u"chain")).toArray().at(1).toObject();
            const JsonArray diags = subchain.value(LangString(u"subchainArgumentDiagnostics")).toArray();
            check(diags.size() == 3, "co-occurring violations report 3 diagnostics");
            check(diags.at(0).toObject().value(LangString(u"code")).toString() == LangString(u"P008"), "diag[0] is P008");
            check(diags.at(1).toObject().value(LangString(u"code")).toString() == LangString(u"P010"), "diag[1] is P010");
            check(diags.at(2).toObject().value(LangString(u"code")).toString() == LangString(u"P009"), "diag[2] is P009");
        }

        // 5. Repeated unknown keys report P008 once per occurrence and never P009
        {
            const LangString src = LangString(u"search synth\nnoise().subchain(nme: \"x\", nme: \"y\", name: \"ok\") {\n.bloom()\n}.write(o0)\n");
            const JsonObject prog = parseSrc(src);
            const JsonArray plans = prog.value(LangString(u"plans")).toArray();
            const JsonObject subchain = plans.at(0).toObject().value(LangString(u"chain")).toArray().at(1).toObject();
            const JsonArray diags = subchain.value(LangString(u"subchainArgumentDiagnostics")).toArray();
            check(diags.size() == 2, "repeated unknown key reports 2 diagnostics");
            check(diags.at(0).toObject().value(LangString(u"code")).toString() == LangString(u"P008"), "diag[0] is P008");
            check(diags.at(1).toObject().value(LangString(u"code")).toString() == LangString(u"P008"), "diag[1] is P008");
        }

        // 6. Strict mode rejects unknown subchain key with P008
        {
            JsonObject opts;
            opts.insert(LangString(u"subchainArguments"), LangString(u"strict"));
            bool caught = false;
            try {
                parseJson(lexJson(LangString(u"search synth\nnoise().subchain(nme: \"typo\", name: \"ok\") {\n.bloom()\n}.write(o0)\n")), opts);
            } catch (const nm::DslSyntaxError& err) {
                caught = true;
                const JsonObject d = JsonValue(err.diagnostic()).toObject();
                check(d.value(LangString(u"code")).toString() == LangString(u"P008"), "strict unknown key emits P008");
                check(d.value(LangString(u"stage")).toString() == LangString(u"parser"), "P008 stage parser");
                check(d.value(LangString(u"severity")).toString() == LangString(u"error"), "P008 strict severity error");
                check(d.value(LangString(u"message")).toString().contains(LangString(u"nme")), "P008 mentions 'nme'");
                check(d.value(LangString(u"span")).toObject().contains(LangString(u"start")), "P008 strict has span");
            }
            check(caught, "strict mode throws for unknown subchain key");
        }

        // 7. Strict mode rejects duplicate subchain key with P009
        {
            JsonObject opts;
            opts.insert(LangString(u"subchainArguments"), LangString(u"strict"));
            bool caught = false;
            try {
                parseJson(lexJson(LangString(u"search synth\nnoise().subchain(name: \"a\", name: \"b\") {\n.bloom()\n}.write(o0)\n")), opts);
            } catch (const nm::DslSyntaxError& err) {
                caught = true;
                const JsonObject d = JsonValue(err.diagnostic()).toObject();
                check(d.value(LangString(u"code")).toString() == LangString(u"P009"), "strict duplicate key emits P009");
                check(d.value(LangString(u"severity")).toString() == LangString(u"error"), "P009 strict severity error");
            }
            check(caught, "strict mode throws for duplicate subchain key");
        }

        // 8. Strict mode rejects missing separator with P010
        {
            JsonObject opts;
            opts.insert(LangString(u"subchainArguments"), LangString(u"strict"));
            bool caught = false;
            try {
                parseJson(lexJson(LangString(u"search synth\nnoise().subchain(name: \"a\" id: \"b\") {\n.bloom()\n}.write(o0)\n")), opts);
            } catch (const nm::DslSyntaxError& err) {
                caught = true;
                const JsonObject d = JsonValue(err.diagnostic()).toObject();
                check(d.value(LangString(u"code")).toString() == LangString(u"P010"), "strict missing separator emits P010");
                check(d.value(LangString(u"severity")).toString() == LangString(u"error"), "P010 strict severity error");
            }
            check(caught, "strict mode throws for missing separator");
        }

        // 9. Caller-supplied mock tokens without positions preserve location without span
        {
            JsonArray tokens = lexJson(LangString(u"search synth\nnoise().subchain(nme: \"typo\") {\n.bloom()\n}.write(o0)\n"));
            // Strip position from the tokens
            JsonArray stripped;
            for (const JsonValue& v : tokens) {
                JsonObject obj = v.toObject();
                obj.remove(LangString(u"position"));
                stripped.append(obj);
            }
            const JsonObject prog = parseJson(stripped);
            const JsonArray plans = prog.value(LangString(u"plans")).toArray();
            const JsonObject subchain = plans.at(0).toObject().value(LangString(u"chain")).toArray().at(1).toObject();
            const JsonArray diags = subchain.value(LangString(u"subchainArgumentDiagnostics")).toArray();
            check(diags.size() == 1, "mock tokens emit 1 arg diagnostic");
            const JsonObject d0 = diags.at(0).toObject();
            check(d0.value(LangString(u"code")).toString() == LangString(u"P008"), "mock token diag code is P008");
            check(d0.contains(LangString(u"location")), "mock token diag has location");
            check(!d0.contains(LangString(u"span")), "mock token diag does NOT have span");
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
