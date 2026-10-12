// Unit tests for libnoisemaker::validate (qt/noisemaker/compiler/validator.{h,cpp}).
// Plain assert-style checks, no test framework dependency (matches
// test_lexer.cpp / test_parser.cpp / test_registry.cpp convention).
//
// Every non-trivial expected value here traces back to a small .dsl probe
// run through the LIVE reference oracle (NM_REFERENCE_ROOT=... node
// tools/dump-validate.mjs qt/noisemaker/effects <probe>.dsl) before being
// hard-coded -- see task report "TDD RED/GREEN evidence" and "hazards
// encountered". Several hazards this suite specifically locks in:
//   - bare and namespaced starter chains with no write() produce S006.
//   - "member"-typed params (filter.channel et al.) silently fall back to
//     default on an unresolved value -- NO diagnostic. Every OTHER
//     enum/choices-bearing param (declared plain "int"/"float"/"palette")
//     falls through to the numeric resolver's branches, pushing S003 on failure.
//   - diagnostic `location` is `{line, column}` when loc is present (upstream e5bd2013).
//   - `nodeId` is present (possibly null) only for Subchain-triggered
//     diagnostics (the only AST node type with a literal `id` field).
//
// RED (before validator.cpp existed): this binary failed to link
// ("undefined symbols: libnoisemaker::validate(...)"). GREEN: all checks below print
// PASS and the process exits 0.

#include "core/lang/ast.h"
#include "core/lang/diagnostics.h"
#include "core/lang/effect_registry.h"
#include "core/lang/lexer.h"
#include "core/lang/parser.h"
#include "core/lang/validator.h"


#include <cstdio>
#include <iterator>
#include <stdexcept>

namespace {

int g_failures = 0;

void check(bool condition, const char* description) {
    if (condition) {
        std::printf("PASS: %s\n", description);
    } else {
        std::printf("FAIL: %s\n", description);
        ++g_failures;
    }
}

libnoisemaker::EffectRegistry& registry() {
    static libnoisemaker::EffectRegistry reg;
    static bool loaded = false;
    if (!loaded) {
        reg.loadEmbedded();
        loaded = true;
    }
    return reg;
}

libnoisemaker::JsObject validateSrc(const libnoisemaker::JsText& src) {
    return libnoisemaker::JsObject(libnoisemaker::validate(libnoisemaker::parse(libnoisemaker::lexTokens(src)), registry()));
}

// One chain step out of plans[0].chain, by 0-based index.
libnoisemaker::JsObject step(const libnoisemaker::JsObject& out, int planIdx, int stepIdx) {
    return out.value(libnoisemaker::JsText(u"plans")).toArray().at(planIdx).toObject()
        .value(libnoisemaker::JsText(u"chain")).toArray().at(stepIdx).toObject();
}

libnoisemaker::JsObject args(const libnoisemaker::JsObject& out, int planIdx, int stepIdx) {
    return step(out, planIdx, stepIdx).value(libnoisemaker::JsText(u"args")).toObject();
}

libnoisemaker::JsArray diags(const libnoisemaker::JsObject& out) { return out.value(libnoisemaker::JsText(u"diagnostics")).toArray(); }

bool anyDiagCode(const libnoisemaker::JsObject& out, const libnoisemaker::JsText& code) {
    for (const libnoisemaker::JsValue& d : diags(out)) {
        if (d.toObject().value(libnoisemaker::JsText(u"code")).toString() == code) return true;
    }
    return false;
}

} // namespace

int main() {
    // ==================================================================
    // numeric arg resolution: clamp + S002, and pass-through when in range
    // ==================================================================
    {
        const libnoisemaker::JsObject out = validateSrc(libnoisemaker::JsText(u"search synth\nnoise(octaves: 20).write(o0)\nrender(o0)\n"));
        check(args(out, 0, 0).value(libnoisemaker::JsText(u"octaves")).toDouble() == 8.0, "octaves: 20 clamps to max 8");
        const libnoisemaker::JsArray ds = diags(out);
        check(ds.size() == 1 && ds.at(0).toObject().value(libnoisemaker::JsText(u"code")).toString() == libnoisemaker::JsText(u"S002"),
              "exactly one S002 diagnostic for the clamp");
        check(ds.at(0).toObject().value(libnoisemaker::JsText(u"message")).toString()
                  == libnoisemaker::JsText(u"Argument out of range for 'octaves' in noise() (got 20, clamped to 8)"),
              "S002 message text matches the oracle exactly (JS-style number formatting)");

        const libnoisemaker::JsObject ok = validateSrc(libnoisemaker::JsText(u"search synth\nnoise(octaves: 5).write(o0)\nrender(o0)\n"));
        check(args(ok, 0, 0).value(libnoisemaker::JsText(u"octaves")).toDouble() == 5.0, "octaves: 5 (in range) passes through unclamped");
        check(diags(ok).isEmpty(), "no diagnostics when the value is in range");
    }

    // ==================================================================
    // enum resolution: numeric-with-synthesized-enum path (int-typed
    // globals with `choices`, e.g. noise.type) -- DIFFERENT from the
    // dedicated "member" type (see below): resolves via def.enum on the
    // numeric resolver, pushes S003 on failure.
    // ==================================================================
    {
        const libnoisemaker::JsObject out = validateSrc(libnoisemaker::JsText(u"search synth\nnoise(type: simplex).write(o0)\nrender(o0)\n"));
        check(args(out, 0, 0).value(libnoisemaker::JsText(u"type")).toDouble() == 10.0, "type: simplex resolves to choices value 10");
        check(diags(out).isEmpty(), "no diagnostics on a valid enum choice");

        const libnoisemaker::JsObject bad = validateSrc(libnoisemaker::JsText(u"search synth\nnoise(type: bogus).write(o0)\nrender(o0)\n"));
        check(args(bad, 0, 0).value(libnoisemaker::JsText(u"type")).toDouble() == 10.0, "type: bogus (unresolvable) falls back to default 10");
        const libnoisemaker::JsArray bd = diags(bad);
        check(bd.size() == 1 && bd.at(0).toObject().value(libnoisemaker::JsText(u"code")).toString() == libnoisemaker::JsText(u"S003")
                  && bd.at(0).toObject().value(libnoisemaker::JsText(u"message")).toString()
                         == libnoisemaker::JsText(u"Variable used before assignment: 'bogus'"),
              "numeric-with-enum path pushes S003 on an unresolvable Ident (matches the oracle's exact message)");
    }

    // ==================================================================
    // dedicated "member" type (filter.channel, filter.palette,
    // filter3d.palette3d, synth.osc2d ONLY) -- resolves against a STD
    // enum via an explicit `enum` field; SILENTLY falls back on failure,
    // no diagnostic at all (verified against the oracle).
    // ==================================================================
    {
        const libnoisemaker::JsObject out = validateSrc(
            libnoisemaker::JsText(u"search filter, synth\nnoise().channel(channel: g).write(o0)\nrender(o0)\n"));
        check(args(out, 0, 1).value(libnoisemaker::JsText(u"channel")).toDouble() == 1.0, "channel: g resolves via stdEnums.channel.g == 1");
        check(diags(out).isEmpty(), "no diagnostics on a valid member value");

        const libnoisemaker::JsObject bad = validateSrc(
            libnoisemaker::JsText(u"search filter, synth\nnoise().channel(channel: bogus).write(o0)\nrender(o0)\n"));
        check(args(bad, 0, 1).value(libnoisemaker::JsText(u"channel")).toDouble() == 0.0,
              "channel: bogus (unresolvable) SILENTLY falls back to default 'channel.r' == 0");
        check(diags(bad).isEmpty(),
              "member-type dispatch pushes NO diagnostic on failure (unlike the numeric-with-enum path above)");
    }

    // ==================================================================
    // A bare name the parameter defines itself, as an inline choice or a
    // member of its enum, wins over the DSL's state values (reference
    // 29e76468 validator.js isOwnChoice, upstream
    // shaders/tests/test_reserved_choice_names.mjs): the unparser writes
    // choices by bare name, so `geometry: seed` and `channel: a` have to
    // compile back to the choice, not to the `seed` or `a` state value.
    // ==================================================================
    {
        const libnoisemaker::JsObject out = validateSrc(
            libnoisemaker::JsText(u"search synth\nsacredGeometry(geometry: seed).write(o0)\nrender(o0)\n"));
        check(args(out, 0, 0).value(libnoisemaker::JsText(u"geometry")).toDouble() == 4.0,
              "an inline choice named `seed` selects the choice (sacredGeometry geometry: seed == 4)");
        check(diags(out).isEmpty(), "no diagnostics when the bare name is the param's own choice");

        const libnoisemaker::JsObject member = validateSrc(
            libnoisemaker::JsText(u"search synth, filter\nsacredGeometry().channel(channel: a).write(o0)\nrender(o0)\n"));
        check(args(member, 0, 1).value(libnoisemaker::JsText(u"channel")).toDouble() == 3.0,
              "an enum member named `a` selects the member (filter.channel channel: a == 3)");
        check(diags(member).isEmpty(), "no diagnostics when the bare member name shadows a state value");

        const libnoisemaker::JsObject state = validateSrc(
            libnoisemaker::JsText(u"search synth\nsacredGeometry(scale: seed).write(o0)\nrender(o0)\n"));
        const libnoisemaker::JsObject scale = args(state, 0, 0).value(libnoisemaker::JsText(u"scale")).toObject();
        check(scale.value(libnoisemaker::JsText(u"min")).toDouble() == 1.0 && scale.value(libnoisemaker::JsText(u"max")).toDouble() == 20.0
                  && scale.value(libnoisemaker::JsText(u"_ast")).toObject().value(libnoisemaker::JsText(u"name")).toString() == libnoisemaker::JsText(u"seed"),
              "a state value still binds a parameter that has no such choice (sacredGeometry scale: seed)");
    }

    // ==================================================================
    // vec3 arg resolution (+ default passthrough)
    // ==================================================================
    {
        const libnoisemaker::JsObject out = validateSrc(
            libnoisemaker::JsText(u"search classicNoisedeck\nnoise(paletteOffset: vec3(0.1,0.2,0.3)).write(o0)\nrender(o0)\n"));
        const libnoisemaker::JsArray po = args(out, 0, 0).value(libnoisemaker::JsText(u"paletteOffset")).toArray();
        check(po.size() == 3 && po.at(0).toDouble() == 0.1 && po.at(1).toDouble() == 0.2 && po.at(2).toDouble() == 0.3,
              "vec3(0.1,0.2,0.3) resolves to [0.1,0.2,0.3]");

        const libnoisemaker::JsObject dflt = validateSrc(libnoisemaker::JsText(u"search classicNoisedeck\nnoise().write(o0)\nrender(o0)\n"));
        const libnoisemaker::JsArray poDflt = args(dflt, 0, 0).value(libnoisemaker::JsText(u"paletteOffset")).toArray();
        check(poDflt.size() == 3 && poDflt.at(0).toDouble() == 0.5 && poDflt.at(1).toDouble() == 0.5 && poDflt.at(2).toDouble() == 0.5,
              "paletteOffset defaults to [0.5,0.5,0.5] when omitted");
    }

    // ==================================================================
    // "numeric catch-all" for types outside the 8 explicitly dispatched
    // ones (vec2/mat3/palette/...): an array default passes through
    // verbatim; a bare Number override clamps as a SCALAR even though the
    // default is an array (verified against the oracle: synth.media).
    // ==================================================================
    {
        const libnoisemaker::JsObject dflt = validateSrc(libnoisemaker::JsText(u"search synth\nmedia().write(o0)\nrender(o0)\n"));
        const libnoisemaker::JsArray sz = args(dflt, 0, 0).value(libnoisemaker::JsText(u"imageSize")).toArray();
        check(sz.size() == 2 && sz.at(0).toDouble() == 1024.0 && sz.at(1).toDouble() == 1024.0,
              "vec2-typed imageSize keeps its [1024,1024] array default verbatim (falls through to the numeric resolver)");

        const libnoisemaker::JsObject scalar = validateSrc(libnoisemaker::JsText(u"search synth\nmedia(imageSize: 5).write(o0)\nrender(o0)\n"));
        check(args(scalar, 0, 0).value(libnoisemaker::JsText(u"imageSize")).toDouble() == 5.0,
              "a bare Number override on a vec2-typed param clamps as a SCALAR, not an array");
    }

    // ==================================================================
    // boolean arg resolution
    // ==================================================================
    {
        const libnoisemaker::JsObject out = validateSrc(libnoisemaker::JsText(u"search synth\nnoise(wrap: false).write(o0)\nrender(o0)\n"));
        check(args(out, 0, 0).value(libnoisemaker::JsText(u"wrap")).toBool(true) == false, "wrap: false resolves to false");
        check(diags(out).isEmpty(), "no diagnostics for a valid boolean literal");
    }

    // ==================================================================
    // color arg resolution (hex literal -> RGBA array; T8's parser never
    // emits Color.hex, so this always goes through Color.value)
    // ==================================================================
    {
        const libnoisemaker::JsObject out = validateSrc(libnoisemaker::JsText(u"search synth\nsolid(color: #ff0000).write(o0)\nrender(o0)\n"));
        const libnoisemaker::JsArray c = args(out, 0, 0).value(libnoisemaker::JsText(u"color")).toArray();
        check(c.size() == 4 && c.at(0).toDouble() == 1.0 && c.at(1).toDouble() == 0.0 && c.at(2).toDouble() == 0.0 && c.at(3).toDouble() == 1.0,
              "#ff0000 resolves to color [1,0,0,1]");

        // A bare Number positional arg cannot satisfy a color-typed param
        // (solid's sole global is "color", not separate r/g/b) -- falls
        // back to the default with S002 (verified against the oracle).
        const libnoisemaker::JsObject positional = validateSrc(libnoisemaker::JsText(u"search synth\nsolid(0.1,0.2,0.3).write(o0)\nrender(o0)\n"));
        const libnoisemaker::JsArray pc = args(positional, 0, 0).value(libnoisemaker::JsText(u"color")).toArray();
        check(pc.size() == 3 && pc.at(0).toDouble() == 0.5 && pc.at(1).toDouble() == 0.5 && pc.at(2).toDouble() == 0.5,
              "solid(0.1,0.2,0.3): a bare Number can't satisfy the single color-typed param, falls back to default [0.5,0.5,0.5]");
        check(anyDiagCode(positional, libnoisemaker::JsText(u"S002")), "S002 pushed for the out-of-type positional color arg");
    }

    // ==================================================================
    // surface arg resolution: read() passthrough AND an inline nested
    // chain (a second effect call used directly as a surface argument,
    // flattened into its own temp step)
    // ==================================================================
    {
        const libnoisemaker::JsObject viaRead = validateSrc(libnoisemaker::JsText(
            "search mixer, synth\nnoise().blendMode(tex: read(o0), mode: hardLight, mix: 0.5).write(o1)\nrender(o1)\n"));
        check(step(viaRead, 0, 1).value(libnoisemaker::JsText(u"op")).toString() == libnoisemaker::JsText(u"mixer.blendMode"), "blendMode step present");
        const libnoisemaker::JsObject texArg = args(viaRead, 0, 1).value(libnoisemaker::JsText(u"tex")).toObject();
        check(texArg.value(libnoisemaker::JsText(u"kind")).toString() == libnoisemaker::JsText(u"output") && texArg.value(libnoisemaker::JsText(u"name")).toString() == libnoisemaker::JsText(u"o0"),
              "tex: read(o0) resolves to {kind:output,name:o0} (no extra chain step)");

        const libnoisemaker::JsObject inline_ = validateSrc(libnoisemaker::JsText(
            "search mixer, synth\nnoise().blendMode(tex: noise(seed: 2), mode: hardLight, mix: 0.5).write(o1)\nrender(o1)\n"));
        // chain: [0]=noise(), [1]=noise(seed:2) (flattened FIRST, since arg
        // resolution recurses via processChain before the blendMode step's
        // own temp index is allocated), [2]=blendMode, [3]=_write.
        check(step(inline_, 0, 0).value(libnoisemaker::JsText(u"chain")).isUndefined(), "sanity: step() indexes the flattened chain array directly");
        const libnoisemaker::JsArray chain = inline_.value(libnoisemaker::JsText(u"plans")).toArray().first().toObject().value(libnoisemaker::JsText(u"chain")).toArray();
        check(chain.size() == 4, "inline surface chain: noise() + noise(seed:2) + blendMode + _write == 4 steps");
        check(chain.at(1).toObject().value(libnoisemaker::JsText(u"op")).toString() == libnoisemaker::JsText(u"synth.noise")
                  && chain.at(1).toObject().value(libnoisemaker::JsText(u"args")).toObject().value(libnoisemaker::JsText(u"seed")).toDouble() == 2.0,
              "the inline noise(seed:2) becomes its OWN temp step, allocated before blendMode's");
        const libnoisemaker::JsObject blendStep = chain.at(2).toObject();
        check(blendStep.value(libnoisemaker::JsText(u"op")).toString() == libnoisemaker::JsText(u"mixer.blendMode"), "blendMode is temp index 2");
        const libnoisemaker::JsObject texObj = blendStep.value(libnoisemaker::JsText(u"args")).toObject().value(libnoisemaker::JsText(u"tex")).toObject();
        check(texObj.value(libnoisemaker::JsText(u"kind")).toString() == libnoisemaker::JsText(u"temp") && texObj.value(libnoisemaker::JsText(u"index")).toInt(-1) == 1,
              "tex: noise(seed:2) resolves to {kind:temp,index:1} (points at the inline chain's own step)");
    }

    // ==================================================================
    // search-order resolution: a bare call resolves to the FIRST
    // namespace (in program order) that registers it
    // ==================================================================
    {
        const libnoisemaker::JsObject first = validateSrc(libnoisemaker::JsText(u"search classicNoisedeck, synth\nnoise().write(o0)\nrender(o0)\n"));
        check(step(first, 0, 0).value(libnoisemaker::JsText(u"op")).toString() == libnoisemaker::JsText(u"classicNoisedeck.noise"),
              "search classicNoisedeck, synth -> bare noise() resolves to classicNoisedeck.noise (first match)");

        const libnoisemaker::JsObject second = validateSrc(libnoisemaker::JsText(u"search synth, classicNoisedeck\nnoise().write(o0)\nrender(o0)\n"));
        check(step(second, 0, 0).value(libnoisemaker::JsText(u"op")).toString() == libnoisemaker::JsText(u"synth.noise"),
              "search synth, classicNoisedeck -> bare noise() resolves to synth.noise (order flipped, resolution follows)");
    }

    // ==================================================================
    // S006 (starter chain missing write()): the reference registers both
    // bare and namespaced starter names.
    // ==================================================================
    {
        const libnoisemaker::JsObject bare = validateSrc(libnoisemaker::JsText(u"search synth\nsolid(0.1,0.2,0.3)\n"));
        check(anyDiagCode(bare, libnoisemaker::JsText(u"S006")), "a bare starter call with no write() produces S006");
        check(anyDiagCode(bare, libnoisemaker::JsText(u"S001")), "it also produces S001 'Chain must have explicit write()...'");

        const libnoisemaker::JsObject viaFromStmt = validateSrc(libnoisemaker::JsText(u"search filter\nfrom(synth, solid()).noise()\nrender(o0)\n"));
        check(anyDiagCode(viaFromStmt, libnoisemaker::JsText(u"S006")),
              "a STATEMENT-level chain starting with a from()-resolved starter DOES produce S006 "
              "(compileChainStatement checks the raw, un-substituted chain)");

        const libnoisemaker::JsObject viaFromLet = validateSrc(libnoisemaker::JsText(u"search filter\nlet x = from(synth, solid()).noise()\nrender(o0)\n"));
        check(anyDiagCode(viaFromLet, libnoisemaker::JsText(u"S006")),
              "a let-bound starter chain produces S006 with bare starter registration");
    }

    // ==================================================================
    // diagnostic shape: location is {line, column}; nodeId is
    // present (possibly null) ONLY for a Subchain-triggered diagnostic
    // ==================================================================
    {
        const libnoisemaker::JsObject out = validateSrc(
            libnoisemaker::JsText(u"search filter\nbogus().subchain(){.solid()}.write(o0)\n"));
        const libnoisemaker::JsArray ds = diags(out);
        check(ds.size() == 3, "bogus().subchain(){...}.write(o0): 3 diagnostics (unknown effect, subchain-no-input, write-no-input)");
        bool foundSubchainDiag = false, foundWriteDiag = false;
        for (const libnoisemaker::JsValue& dv : ds) {
            const libnoisemaker::JsObject d = dv.toObject();
            if (d.value(libnoisemaker::JsText(u"code")).toString() == libnoisemaker::JsText(u"S005")
                && d.value(libnoisemaker::JsText(u"identifier")).toString() == libnoisemaker::JsText(u"[Subchain]")) {
                foundSubchainDiag = true;
                check(d.contains(libnoisemaker::JsText(u"nodeId")) && d.value(libnoisemaker::JsText(u"nodeId")).isNull(),
                      "Subchain-triggered diagnostic has nodeId:null present (Subchain nodes carry a literal 'id' field)");
                check(d.value(libnoisemaker::JsText(u"location")).toObject().value(libnoisemaker::JsText(u"line")).toInt() == 2
                          && d.value(libnoisemaker::JsText(u"location")).toObject().value(libnoisemaker::JsText(u"column")).toInt() == 9,
                      "Subchain location preserves line 2, column 9 (preserving source column from loc)");
            }
            if (d.value(libnoisemaker::JsText(u"code")).toString() == libnoisemaker::JsText(u"S005")
                && d.value(libnoisemaker::JsText(u"identifier")).toString() == libnoisemaker::JsText(u"[Write]")) {
                foundWriteDiag = true;
                check(!d.contains(libnoisemaker::JsText(u"nodeId")), "Write-triggered diagnostic has NO nodeId key at all (Write nodes have no 'id' field)");
                check(d.value(libnoisemaker::JsText(u"location")).toObject().value(libnoisemaker::JsText(u"line")).toInt() == 2
                          && d.value(libnoisemaker::JsText(u"location")).toObject().value(libnoisemaker::JsText(u"column")).toInt() == 30,
                      "Write location preserves line 2, column 30 (preserving source column from loc)");
            }
        }
        check(foundSubchainDiag && foundWriteDiag, "both the subchain-no-input and write-no-input diagnostics were found");
    }

    // ==================================================================
    // column coordinate precedence and fallback (upstream e5bd2013)
    // ==================================================================
    {
        auto makeAstWithLoc = [](const libnoisemaker::JsObject& loc, bool includeLoc) {
            libnoisemaker::JsObject ast;
            libnoisemaker::JsObject nsObj;
            libnoisemaker::JsArray searchOrder;
            searchOrder.append(libnoisemaker::JsText(u"synth"));
            nsObj.insert(libnoisemaker::JsText(u"searchOrder"), searchOrder);
            ast.insert(libnoisemaker::JsText(u"namespace"), nsObj);

            libnoisemaker::JsArray plans;
            libnoisemaker::JsObject stmt;
            libnoisemaker::JsArray chain;
            libnoisemaker::JsObject writeNode;
            writeNode.insert(libnoisemaker::JsText(u"type"), libnoisemaker::NodeKind::Write);
            libnoisemaker::JsObject surf;
            surf.insert(libnoisemaker::JsText(u"type"), libnoisemaker::NodeKind::OutputRef);
            surf.insert(libnoisemaker::JsText(u"name"), libnoisemaker::JsText(u"o0"));
            writeNode.insert(libnoisemaker::JsText(u"surface"), surf);
            if (includeLoc) {
                writeNode.insert(libnoisemaker::JsText(u"loc"), loc);
            }
            chain.append(writeNode);
            stmt.insert(libnoisemaker::JsText(u"chain"), chain);
            plans.append(stmt);
            ast.insert(libnoisemaker::JsText(u"plans"), plans);
            return ast;
        };

        // loc with explicit column
        libnoisemaker::JsObject loc1;
        loc1.insert(libnoisemaker::JsText(u"line"), 4);
        loc1.insert(libnoisemaker::JsText(u"column"), 12);
        const libnoisemaker::JsObject out1 = libnoisemaker::validate(makeAstWithLoc(loc1, true), registry());
        const libnoisemaker::JsArray ds1 = diags(out1);
        check(ds1.size() == 1, "AST Write node with loc.column produces 1 diagnostic");
        check(ds1.first().toObject().value(libnoisemaker::JsText(u"location")).toObject().value(libnoisemaker::JsText(u"line")).toInt() == 4
                  && ds1.first().toObject().value(libnoisemaker::JsText(u"location")).toObject().value(libnoisemaker::JsText(u"column")).toInt() == 12,
              "diagnostic preserves loc.column coordinate");

        // loc with column and col: column takes precedence
        libnoisemaker::JsObject loc2;
        loc2.insert(libnoisemaker::JsText(u"line"), 7);
        loc2.insert(libnoisemaker::JsText(u"column"), 15);
        loc2.insert(libnoisemaker::JsText(u"col"), 99);
        const libnoisemaker::JsObject out2 = libnoisemaker::validate(makeAstWithLoc(loc2, true), registry());
        const libnoisemaker::JsArray ds2 = diags(out2);
        check(ds2.first().toObject().value(libnoisemaker::JsText(u"location")).toObject().value(libnoisemaker::JsText(u"column")).toInt() == 15,
              "diagnostic prefers loc.column over loc.col fallback");

        // loc with col only: falls back to col
        libnoisemaker::JsObject locFallback;
        locFallback.insert(libnoisemaker::JsText(u"line"), 5);
        locFallback.insert(libnoisemaker::JsText(u"col"), 42);
        const libnoisemaker::JsObject outFallback = libnoisemaker::validate(makeAstWithLoc(locFallback, true), registry());
        const libnoisemaker::JsArray dsFallback = diags(outFallback);
        check(dsFallback.first().toObject().value(libnoisemaker::JsText(u"location")).toObject().value(libnoisemaker::JsText(u"column")).toInt() == 42,
              "diagnostic falls back to loc.col when loc.column is absent");

        // unlocated node: no location object
        const libnoisemaker::JsObject out3 = libnoisemaker::validate(makeAstWithLoc(libnoisemaker::JsObject(), false), registry());
        const libnoisemaker::JsArray ds3 = diags(out3);
        check(ds3.size() == 1, "unlocated AST node produces 1 diagnostic");
        check(!ds3.first().toObject().contains(libnoisemaker::JsText(u"location")),
              "unlocated AST node produces diagnostic without location object");
    }

    // ==================================================================
    // Func (`() => expr`) values: the reference compiles the body with
    // `new Function('state', `with(state){ return ${src}; }`)`. Accepted
    // bodies compile to {min, max} (numeric) or {} (boolean, condition);
    // rejected ones push S001 and keep the default (oracle-gated by
    // parity/corpus/func_*.dsl). Body verdicts here were checked in V8.
    // ==================================================================
    {
        const libnoisemaker::JsObject valid = validateSrc(libnoisemaker::JsText(
            "search synth\nnoise(octaves: () => Math.sin(time) * 4, wrap: () => frame > 3).write(o0)\nrender(o0)\n"));
        check(diags(valid).isEmpty(), "valid Func bodies: no diagnostics");
        const libnoisemaker::JsObject numeric_func = args(valid, 0, 0).value(libnoisemaker::JsText(u"octaves")).toObject();
        check(numeric_func.value(libnoisemaker::JsText(u"fn")).raw().is_function()
                  && numeric_func.value(libnoisemaker::JsText(u"min")) == libnoisemaker::JsValue(1)
                  && numeric_func.value(libnoisemaker::JsText(u"max")) == libnoisemaker::JsValue(8),
              "valid numeric Func preserves fn and numeric bounds");
        check(args(valid, 0, 0).value(libnoisemaker::JsText(u"wrap")).toObject()
                  .value(libnoisemaker::JsText(u"fn")).raw().is_function(),
              "valid boolean Func preserves fn");

        const libnoisemaker::JsObject invalid = validateSrc(libnoisemaker::JsText(
            "search synth\nnoise(octaves: () => time +, wrap: () => time // note\n).write(o0)\nrender(o0)\n"));
        const libnoisemaker::JsArray ds = diags(invalid);
        check(ds.size() == 2 && ds.at(0).toObject().value(libnoisemaker::JsText(u"message")).toString()
                                    == libnoisemaker::JsText(u"Invalid function for 'octaves': 'time +'"),
              "invalid numeric Func -> S001 \"Invalid function for 'octaves': 'time +'\"");
        check(args(invalid, 0, 0).value(libnoisemaker::JsText(u"octaves")).toDouble() == 2.0
                  && args(invalid, 0, 0).value(libnoisemaker::JsText(u"wrap")) == libnoisemaker::JsValue(true),
              "invalid Func keeps the defaults (octaves 2, wrap true)");
        check(ds.size() == 2 && ds.at(1).toObject().value(libnoisemaker::JsText(u"identifier")).toString()
                                    == libnoisemaker::JsText(u"{time // note}"),
              "a trailing // comment joins the body and swallows the reference's `; }`: S001");

        const libnoisemaker::JsObject cond = validateSrc(libnoisemaker::JsText(
            "search synth\nif (() => time > 1) {\n  noise().write(o0)\n} elif (() => time >) {\n}\n"));
        const libnoisemaker::JsObject branch = cond.value(libnoisemaker::JsText(u"plans")).toArray().first().toObject();
        check(branch.value(libnoisemaker::JsText(u"cond")).toObject()
                  .value(libnoisemaker::JsText(u"fn")).raw().is_function(),
              "valid Func condition preserves fn");
        check(branch.value(libnoisemaker::JsText(u"elif")).toArray().first().toObject().value(libnoisemaker::JsText(u"cond")) == libnoisemaker::JsValue(false)
                  && diags(cond).size() == 1
                  && diags(cond).first().toObject().value(libnoisemaker::JsText(u"message")).toString()
                         == libnoisemaker::JsText(u"Invalid function expression: 'time >'"),
              "invalid Func condition -> false and S001 \"Invalid function expression\"");

        bool threw = false;
        try {
            validateSrc(libnoisemaker::JsText(u"search synth\nnoise(octaves: () => ") + libnoisemaker::JsText(200, u'(')
                        + libnoisemaker::JsText(u"1") + libnoisemaker::JsText(200, u')') + libnoisemaker::JsText(u").write(o0)\n"));
        } catch (const libnoisemaker::UnsupportedDsl&) {
            threw = true;
        }
        check(threw, "a Func body nested deeper than the checker follows -> UnsupportedDsl");
    }

    // Control flow compiles to the reference's Branch/Break/Continue/Return
    // plan entries (oracle-gated by parity/corpus/control_flow_*.dsl).
    {
        const libnoisemaker::JsObject out = validateSrc(libnoisemaker::JsText(
            "search synth\nlet depth = 4\nnoise().write(o0)\n"
            "if (depth) {\n  noise().write(o1)\n  break\n} elif (time) {\n  continue\n} elif (bogus) {\n} else {\n  solid().write(o2)\n  return 5\n}\n"
            "if (0 / 0) {} elif (foo.bar) {}\nreturn\nrender(o0)\n"));
        const libnoisemaker::JsArray plans = out.value(libnoisemaker::JsText(u"plans")).toArray();
        check(plans.size() == 4, "control flow: 4 plans (chain, Branch, Branch, Return)");
        const libnoisemaker::JsObject branch = plans.at(1).toObject();
        check(branch.value(libnoisemaker::JsText(u"type")).toString() == libnoisemaker::JsText(u"Branch")
                  && branch.value(libnoisemaker::JsText(u"cond")) == libnoisemaker::JsValue(true),
              "if (depth): Branch whose cond is true (let-bound Number 4)");
        const libnoisemaker::JsArray thenBranch = branch.value(libnoisemaker::JsText(u"then")).toArray();
        check(thenBranch.size() == 2 && thenBranch.at(0).toObject().value(libnoisemaker::JsText(u"final")).toInt() == 3
                  && thenBranch.at(1).toObject().value(libnoisemaker::JsText(u"type")).toString() == libnoisemaker::JsText(u"Break"),
              "then block: a chain plan continuing the shared temp counter, then Break");
        const libnoisemaker::JsArray elif = branch.value(libnoisemaker::JsText(u"elif")).toArray();
        check(elif.size() == 2 && elif.at(0).toObject().value(libnoisemaker::JsText(u"cond")).toObject()
                                     .value(libnoisemaker::JsText(u"fn")).raw().is_function()
                  && elif.at(1).toObject().value(libnoisemaker::JsText(u"cond")) == libnoisemaker::JsValue(false),
              "elif (time) preserves fn; elif (bogus) -> false");
        const libnoisemaker::JsArray elseBranch = branch.value(libnoisemaker::JsText(u"else")).toArray();
        check(elseBranch.size() == 2 && elseBranch.at(1).toObject().value(libnoisemaker::JsText(u"value")).toObject()
                                             .value(libnoisemaker::JsText(u"value")).toDouble() == 5.0,
              "else block: chain plan, then Return with a Number value");
        const libnoisemaker::JsObject second = plans.at(2).toObject();
        check(second.value(libnoisemaker::JsText(u"cond")) == libnoisemaker::JsValue(false)
                  && second.value(libnoisemaker::JsText(u"else")).toArray().isEmpty(),
              "if (0 / 0): the clone turns NaN into null, so cond is false; no else -> []");
        check(plans.at(3).toObject() == libnoisemaker::JsObject{{libnoisemaker::JsText(u"type"), libnoisemaker::JsText(u"Return")}},
              "bare return -> {type: Return}");
        const libnoisemaker::JsArray ds = diags(out);
        check(ds.size() == 2 && ds.at(0).toObject().value(libnoisemaker::JsText(u"code")).toString() == libnoisemaker::JsText(u"S003")
                  && ds.at(1).toObject().value(libnoisemaker::JsText(u"message")).toString() == libnoisemaker::JsText(u"Unknown enum path: 'foo.bar'"),
              "diagnostics: S003 for bogus, S001 'Unknown enum path' for foo.bar, in evaluation order");

        libnoisemaker::JsText letError;
        try {
            validateSrc(libnoisemaker::JsText(u"search synth\nif (1) {\n  let x = 1\n}\n"));
        } catch (const std::runtime_error& e) {
            letError = libnoisemaker::JsText::fromUtf8(e.what());
        }
        check(letError == libnoisemaker::JsText(u"Cannot read properties of undefined (reading '0')"),
              "let inside a block fails validation with the reference's TypeError text");
    }

    // Bare state values (time/frame/...) compile to the values the reference
    // graph JSON omits function fields, but raw validated values preserve them.
    {
        const libnoisemaker::JsObject boolean = validateSrc(libnoisemaker::JsText(u"search synth\nnoise(wrap: time).write(o0)\nrender(o0)\n"));
        check(args(boolean, 0, 0).value(libnoisemaker::JsText(u"wrap")).toObject()
                  .value(libnoisemaker::JsText(u"fn")).raw().is_function(),
              "state-value boolean param preserves fn");
        const libnoisemaker::JsObject member = validateSrc(libnoisemaker::JsText(u"search synth\nosc2d(oscType: time).write(o0)\nrender(o0)\n"));
        check(args(member, 0, 0).value(libnoisemaker::JsText(u"oscType")).toObject()
                  .value(libnoisemaker::JsText(u"fn")).raw().is_function(),
              "state-value member param preserves fn");
        const libnoisemaker::JsObject numeric = validateSrc(libnoisemaker::JsText(u"search synth\nnoise(octaves: frame).write(o0)\nrender(o0)\n"));
        const libnoisemaker::JsObject octaves = args(numeric, 0, 0).value(libnoisemaker::JsText(u"octaves")).toObject();
        check(octaves.value(libnoisemaker::JsText(u"fn")).raw().is_function()
                  && octaves.value(libnoisemaker::JsText(u"min")).toDouble() == 1.0 && octaves.value(libnoisemaker::JsText(u"max")).toDouble() == 8.0
                  && octaves.value(libnoisemaker::JsText(u"_ast")).toObject().value(libnoisemaker::JsText(u"name")).toString() == libnoisemaker::JsText(u"frame"),
              "state-value numeric param preserves fn, bounds, and AST");
    }

    // Oscillator (osc()) is explicitly NOT in the UnsupportedDsl set --
    // its descriptor is compiled for deterministic runtime evaluation.
    {
        const libnoisemaker::JsObject out = validateSrc(
            libnoisemaker::JsText(u"search synth\nnoise(scaleX: osc(min: 10, max: 90, speed: 2)).write(o0)\nrender(o0)\n"));
        const libnoisemaker::JsObject osc = args(out, 0, 0).value(libnoisemaker::JsText(u"scaleX")).toObject();
        check(osc.value(libnoisemaker::JsText(u"type")).toString() == libnoisemaker::JsText(u"Oscillator") && osc.value(libnoisemaker::JsText(u"oscType")).toDouble() == 0.0
                  && osc.value(libnoisemaker::JsText(u"min")).toDouble() == 1.0 && osc.value(libnoisemaker::JsText(u"max")).toDouble() == 1.0
                  && osc.value(libnoisemaker::JsText(u"speed")).toDouble() == 2.0,
              "osc(min:10,max:90,speed:2) resolves fully (NOT UnsupportedDsl); min/max clamp into 0..1 (both -> 1)");
        check(osc.value(libnoisemaker::JsText(u"_ast")).isObject(), "the resolved Oscillator value embeds its _ast subtree");
    }

    // ==================================================================
    // _varRef propagation: a let-bound variable used as a numeric arg
    // wraps the clamped value in {_varRef,value}; used as an Oscillator
    // source, the marker appears BOTH at the value's top level AND nested
    // inside its _ast copy (verified against the oracle).
    // ==================================================================
    {
        const libnoisemaker::JsObject out = validateSrc(libnoisemaker::JsText(u"search synth\nlet n = 33\nnoise(octaves: n).write(o0)\nrender(o0)\n"));
        const libnoisemaker::JsObject wrapped = args(out, 0, 0).value(libnoisemaker::JsText(u"octaves")).toObject();
        check(wrapped.value(libnoisemaker::JsText(u"_varRef")).toString() == libnoisemaker::JsText(u"n") && wrapped.value(libnoisemaker::JsText(u"value")).toDouble() == 8.0,
              "let n = 33; noise(octaves: n) -> {_varRef:'n', value:8} (clamped)");

        const libnoisemaker::JsObject viaOsc = validateSrc(
            libnoisemaker::JsText(u"search synth\nlet o = osc(min: 0.2, max: 0.8)\nnoise(scaleX: o).write(o0)\nrender(o0)\n"));
        const libnoisemaker::JsObject oscVal = args(viaOsc, 0, 0).value(libnoisemaker::JsText(u"scaleX")).toObject();
        check(oscVal.value(libnoisemaker::JsText(u"_varRef")).toString() == libnoisemaker::JsText(u"o"), "_varRef present at the Oscillator value's top level");
        check(oscVal.value(libnoisemaker::JsText(u"_ast")).toObject().value(libnoisemaker::JsText(u"_varRef")).toString() == libnoisemaker::JsText(u"o"),
              "_varRef is ALSO present nested inside _ast (same underlying node object in the reference)");
    }

    // ==================================================================
    // Nested automation descriptors: numeric fields may themselves be
    // oscillator/MIDI/audio sources; enum/string selector fields may not.
    // ==================================================================
    {
        const libnoisemaker::JsObject out = validateSrc(libnoisemaker::JsText(
            "search synth\n"
            "let rate = osc(type: oscKind.sine, min: 0.25, max: 0.75)\n"
            "let carrier = osc(type: oscKind.saw, speed: rate)\n"
            "noise(scaleX: carrier).write(o0)\nrender(o0)\n"));
        const libnoisemaker::JsObject carrier = args(out, 0, 0).value(libnoisemaker::JsText(u"scaleX")).toObject();
        const libnoisemaker::JsObject rate = carrier.value(libnoisemaker::JsText(u"speed")).toObject();
        check(diags(out).isEmpty(), "nested oscillator compiles without diagnostics");
        check(carrier.value(libnoisemaker::JsText(u"type")).toString() == libnoisemaker::JsText(u"Oscillator")
                  && rate.value(libnoisemaker::JsText(u"type")).toString() == libnoisemaker::JsText(u"Oscillator"),
              "oscillator speed preserves its nested automation descriptor");
        check(rate.value(libnoisemaker::JsText(u"_varRef")).toString() == libnoisemaker::JsText(u"rate"),
              "nested automation preserves the referenced variable name");
    }

    {
        const libnoisemaker::JsObject out = validateSrc(libnoisemaker::JsText(
            "search synth\n"
            "let movement = midi(channel: 1, name: \"Controller\", id: \"port-a\")\n"
            "let gate = audio(band: audioBand.vol, min: movement, channel: 2, "
            "name: \"Interface\", id: \"device-b\")\n"
            "noise(scaleX: gate).write(o0)\nrender(o0)\n"));
        const libnoisemaker::JsObject audio = args(out, 0, 0).value(libnoisemaker::JsText(u"scaleX")).toObject();
        check(diags(out).isEmpty(), "nested selected MIDI/audio descriptors compile without diagnostics");
        check(audio.value(libnoisemaker::JsText(u"type")).toString() == libnoisemaker::JsText(u"Audio")
                  && audio.value(libnoisemaker::JsText(u"min")).toObject().value(libnoisemaker::JsText(u"type")).toString()
                         == libnoisemaker::JsText(u"Midi"),
              "audio numeric bounds preserve a nested MIDI source");
        check(audio.value(libnoisemaker::JsText(u"name")).toString() == libnoisemaker::JsText(u"Interface")
                  && audio.value(libnoisemaker::JsText(u"id")).toString() == libnoisemaker::JsText(u"device-b")
                  && audio.value(libnoisemaker::JsText(u"channel")).toInt() == 2,
              "compiled audio descriptor preserves its selected-device identity");
    }

    {
        const libnoisemaker::JsObject invalidBand = validateSrc(libnoisemaker::JsText(
            "search synth\nlet movement = midi(channel: 1)\n"
            "noise(scaleX: audio(band: movement)).write(o0)\nrender(o0)\n"));
        check(anyDiagCode(invalidBand, libnoisemaker::JsText(u"S002")),
              "automation remains invalid for literal-only audio band");

        const libnoisemaker::JsObject invalidMin = validateSrc(libnoisemaker::JsText(
            "search synth\nnoise(scaleX: audio(band: audioBand.vol, min: \"bad\"))"
            ".write(o0)\nrender(o0)\n"));
        const libnoisemaker::JsObject audio = args(invalidMin, 0, 0).value(libnoisemaker::JsText(u"scaleX")).toObject();
        check(anyDiagCode(invalidMin, libnoisemaker::JsText(u"S001"))
                  && audio.value(libnoisemaker::JsText(u"_invalid")).toBool(),
              "invalid audio numeric fields diagnose and mark the descriptor invalid");
    }

    {
        const libnoisemaker::JsObject cycle = validateSrc(libnoisemaker::JsText(
            "search synth\n"
            "let first = osc(type: oscKind.sine, speed: second)\n"
            "let second = osc(type: oscKind.tri, speed: first)\n"
            "noise(scaleX: first).write(o0)\nrender(o0)\n"));
        bool foundCycle = false;
        for (const libnoisemaker::JsValue& diagnostic : diags(cycle)) {
            if (diagnostic.toObject().value(libnoisemaker::JsText(u"message")).toString().contains(
                    libnoisemaker::JsText(u"Automation cycle detected"))) {
                foundCycle = true;
            }
        }
        check(foundCycle, "automation reference cycle produces a diagnostic instead of recursing");
    }

    {
        const libnoisemaker::JsObject tooDeep = validateSrc(libnoisemaker::JsText(
            "search synth\n"
            "let rate9 = osc(type: oscKind.sine)\n"
            "let rate8 = osc(type: oscKind.sine, speed: rate9)\n"
            "let rate7 = osc(type: oscKind.sine, speed: rate8)\n"
            "let rate6 = osc(type: oscKind.sine, speed: rate7)\n"
            "let rate5 = osc(type: oscKind.sine, speed: rate6)\n"
            "let rate4 = osc(type: oscKind.sine, speed: rate5)\n"
            "let rate3 = osc(type: oscKind.sine, speed: rate4)\n"
            "let rate2 = osc(type: oscKind.sine, speed: rate3)\n"
            "let rate1 = osc(type: oscKind.sine, speed: rate2)\n"
            "let carrier = osc(type: oscKind.saw, speed: rate1)\n"
            "noise(scaleX: carrier).write(o0)\nrender(o0)\n"));
        bool foundDepth = false;
        for (const libnoisemaker::JsValue& diagnostic : diags(tooDeep)) {
            if (diagnostic.toObject().value(libnoisemaker::JsText(u"message")).toString().contains(
                    libnoisemaker::JsText(u"maximum depth of 8"))) {
                foundDepth = true;
            }
        }
        check(foundDepth, "automation nesting beyond eight levels produces the reference diagnostic");
    }

    // ==================================================================
    // vars / searchNamespaces / render pass through in the top-level shape
    // ==================================================================
    {
        const libnoisemaker::JsObject out = validateSrc(libnoisemaker::JsText(u"search synth\nlet n = 33\nnoise(octaves: n).write(o0)\nrender(o0)\n"));
        check(out.value(libnoisemaker::JsText(u"render")).toString() == libnoisemaker::JsText(u"o0"), "render name passes through");
        check(out.value(libnoisemaker::JsText(u"searchNamespaces")).toArray().size() == 1
                  && out.value(libnoisemaker::JsText(u"searchNamespaces")).toArray().first().toString() == libnoisemaker::JsText(u"synth"),
              "searchNamespaces echoes the program's search order");
        const libnoisemaker::JsArray vars = out.value(libnoisemaker::JsText(u"vars")).toArray();
        check(vars.size() == 1 && vars.first().toObject().value(libnoisemaker::JsText(u"name")).toString() == libnoisemaker::JsText(u"n"),
              "vars carries the original (unresolved) var declarations");

        const libnoisemaker::JsObject noVars = validateSrc(libnoisemaker::JsText(u"search synth\nnoise().write(o0)\nrender(o0)\n"));
        check(noVars.value(libnoisemaker::JsText(u"vars")).toArray().isEmpty(), "vars is [] (present, empty) when the program has no `let` bindings");
    }

    {
        try {
            const libnoisemaker::JsList modes{"cc", "cc14", "nrpn", "pitchBend", "pressure", "polyPressure"};
            for (int i = 0; i < std::ssize(modes); ++i) {
                const auto out = validateSrc(libnoisemaker::JsText(u"search synth\nnoise(scaleX: midi(1, midiMode.%1, nrpn: 42)).write(o0)\nrender(o0)\n").arg(modes.at(i)));
                const auto midi = args(out, 0, 0).value("scaleX").toObject();
                check(diags(out).isEmpty() && midi.value("mode").toInt() == i + 5,
                      "expression MIDI mode compiles to its canonical number");
            }
            const auto zone = validateSrc(libnoisemaker::JsText(u"search synth\nnoise(scaleX: midi(zone: midiZone.upper, members: 3, mode: midiMode.pressure)).write(o0)\nrender(o0)\n"));
            const auto midi = args(zone, 0, 0).value("scaleX").toObject();
            check(diags(zone).isEmpty() && midi.value("zone").toInt() == 1 && !midi.contains("channel"),
                  "MPE zone compiles without an implicit channel");
            for (const libnoisemaker::JsText& expression : {libnoisemaker::JsText(u"midi(17, midiMode.cc)"), libnoisemaker::JsText(u"midi(1, midiMode.cc14, cc: 32)"), libnoisemaker::JsText(u"midi(1, midiMode.nrpn)"), libnoisemaker::JsText(u"midi(zone: midiZone.lower, members: 0)")}) {
                const auto invalid = validateSrc(libnoisemaker::JsText(u"search synth\nnoise(scaleX: %1).write(o0)\nrender(o0)\n").arg(expression));
                check(!diags(invalid).isEmpty() && args(invalid, 0, 0).value("scaleX").toObject().value("_invalid").toBool(),
                      "invalid static MIDI selectors fail closed");
            }
            const libnoisemaker::JsList noteModes{"noteChange", "gateNote", "gateVelocity", "triggerNote", "velocity"};
            for (const libnoisemaker::JsText& m : noteModes) {
                for (int ch : {1, 16}) {
                    const auto valid = validateSrc(libnoisemaker::JsText(u"search synth\nnoise(scaleX: midi(%1, midiMode.%2)).write(o0)\nrender(o0)\n").arg(ch).arg(m));
                    const auto validMidi = args(valid, 0, 0).value("scaleX").toObject();
                    check(diags(valid).isEmpty() && validMidi.value("channel").toInt() == ch && !validMidi.value("_invalid").toBool(),
                          "legacy note mode accepts static channel within 1..16");
                }
                for (const libnoisemaker::JsText& badCh : {libnoisemaker::JsText(u"0"), libnoisemaker::JsText(u"17"), libnoisemaker::JsText(u"1.5"), libnoisemaker::JsText(u"true")}) {
                    const auto invalid = validateSrc(libnoisemaker::JsText(u"search synth\nnoise(scaleX: midi(%1, midiMode.%2)).write(o0)\nrender(o0)\n").arg(badCh).arg(m));
                    check(!diags(invalid).isEmpty() && args(invalid, 0, 0).value("scaleX").toObject().value("_invalid").toBool(),
                          "legacy note mode rejects non-integer or out-of-range static channel");
                }
            }
            const auto audio = validateSrc(libnoisemaker::JsText(u"search synth\nnoise(scaleX: audio(audioBand.raw, channel: 32)).write(o0)\nrender(o0)\n"));
            check(diags(audio).isEmpty() && args(audio, 0, 0).value("scaleX").toObject().value("channel").toInt() == 32,
                  "default-device audio accepts its highest supported channel");
            const auto badAudio = validateSrc(libnoisemaker::JsText(u"search synth\nnoise(scaleX: audio(audioBand.raw, channel: 33)).write(o0)\nrender(o0)\n"));
            check(args(badAudio, 0, 0).value("scaleX").toObject().value("_invalid").toBool(),
                  "audio rejects channels above 32");
        } catch (const std::exception&) {
            check(false, "new MIDI and audio selectors parse successfully");
        }
    }

    {
        const libnoisemaker::JsObject out = validateSrc(libnoisemaker::JsText(
            "search synth, filter\nlet eff = rotate(1, 0.1)\nnoise().eff().write(o0)\nrender(o0)\n"));
        check(diags(out).isEmpty(), "chained variable alias: no validator diagnostics");
        const libnoisemaker::JsArray plans = out.value(libnoisemaker::JsText(u"plans")).toArray();
        check(plans.size() == 1, "chained variable alias: exactly 1 plan");
        if (!plans.isEmpty()) {
            const libnoisemaker::JsArray chain = plans.first().toObject().value(libnoisemaker::JsText(u"chain")).toArray();
            check(chain.size() == 3, "chained variable alias: plan chain has exactly 3 steps");
            if (chain.size() >= 3) {
                check(chain.at(0).toObject().value(libnoisemaker::JsText(u"op")).toString() == libnoisemaker::JsText(u"synth.noise"),
                      "chained variable alias: step 0 is synth.noise");
                check(chain.at(1).toObject().value(libnoisemaker::JsText(u"op")).toString() == libnoisemaker::JsText(u"filter.rotate"),
                      "chained variable alias: step 1 is filter.rotate");
                check(chain.at(2).toObject().value(libnoisemaker::JsText(u"op")).toString() == libnoisemaker::JsText(u"_write"),
                      "chained variable alias: step 2 is terminal _write");
            }
        }
    }

    // Member paths walk own properties like the reference's
    // hasOwnProperty check: arrays and strings expose `length` (oracle-gated
    // by parity/corpus/member_own_properties.dsl).
    {
        const libnoisemaker::JsObject out = validateSrc(libnoisemaker::JsText(
            "search synth\nlet n = noise(3)\nlet c = #ff8800\nlet label = n.name\n"
            "noise(octaves: c.value.length, seed: label.length, scaleX: n.args.length).write(o0)\nrender(o0)\n"));
        check(diags(out).isEmpty(), "member own properties: no diagnostics");
        check(args(out, 0, 0).value(libnoisemaker::JsText(u"octaves")).toDouble() == 4.0,
              "array length: c.value.length resolves to 4");
        check(args(out, 0, 0).value(libnoisemaker::JsText(u"seed")).toDouble() == 5.0,
              "string length: label.length resolves to 5 (\"noise\")");
        check(args(out, 0, 0).value(libnoisemaker::JsText(u"scaleX")).toDouble() == 1.0,
              "array length: n.args.length resolves to 1");
        const libnoisemaker::JsObject missing = validateSrc(libnoisemaker::JsText(
            "search synth\nlet c = #ff8800\nnoise(octaves: c.value.size).write(o0)\nrender(o0)\n"));
        check(diags(missing).size() == 1 && diags(missing).first().toObject().value(libnoisemaker::JsText(u"code")).toString() == libnoisemaker::JsText(u"S001"),
              "an array has no own `size`: S001, as before");
    }

    // Subchain argument reports surfaced by validate()
    {
        const libnoisemaker::JsObject out = validateSrc(libnoisemaker::JsText(
            "search synth\nnoise().subchain(nme: \"typo\", name: \"ok\") {\n.bloom()\n}.write(o0)\nrender(o0)\n"));
        const libnoisemaker::JsArray ds = diags(out);
        bool foundP008 = false;
        for (const libnoisemaker::JsValue& dv : ds) {
            const libnoisemaker::JsObject d = dv.toObject();
            if (d.value(libnoisemaker::JsText(u"code")).toString() == libnoisemaker::JsText(u"P008")) {
                foundP008 = true;
                check(d.value(libnoisemaker::JsText(u"severity")).toString() == libnoisemaker::JsText(u"warning"), "surfaced P008 severity is warning");
                check(d.contains(libnoisemaker::JsText(u"nodeId")), "surfaced P008 contains nodeId");
                check(d.value(libnoisemaker::JsText(u"nodeId")).isNull(), "surfaced P008 nodeId is null");
                check(d.value(libnoisemaker::JsText(u"message")).toString().contains(libnoisemaker::JsText(u"nme")), "surfaced P008 message mentions 'nme'");
                check(d.contains(libnoisemaker::JsText(u"location")), "surfaced P008 contains location");
                const libnoisemaker::JsObject loc = d.value(libnoisemaker::JsText(u"location")).toObject();
                check(loc.value(libnoisemaker::JsText(u"line")).toInt() == 2, "surfaced P008 location line is 2");
            }
        }
        check(foundP008, "validate() surfaces P008 diagnostic from Subchain node");


    }

    if (g_failures == 0) {
        std::printf("ALL PASS (test_validator)\n");
        return 0;
    }
    std::printf("%d FAILURE(S) (test_validator)\n", g_failures);
    return 1;
}
