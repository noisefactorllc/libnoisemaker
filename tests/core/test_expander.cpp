// Unit tests for nm::expand (qt/noisemaker/compiler/expander.{h,cpp}) and
// nm::compileGraphJson (dsl_compiler.{h,cpp}). Plain assert-style checks,
// no test framework dependency (matches test_lexer.cpp/test_registry.cpp
// convention).
//
// Every non-obvious numeric fixture here was ground-truthed against the
// live reference oracle (NM_REFERENCE_ROOT=... node tools/dump-expand.mjs
// qt/noisemaker/effects <probe>.dsl) BEFORE being hard-coded — see task
// report "TDD RED/GREEN evidence" for the exact probe commands and their
// raw output. In particular the define-SORT-ORDER test (classicNoisedeck/
// noise.json) empirically DISPROVES the task brief's own claim that
// defines are "sorted by define name" — the live oracle's program suffix
// is `__COLOR_MODE_6__LOOP_OFFSET_300__METRIC_0__REFRACT_MODE_2__NOISE_TYPE_10`,
// i.e. REFRACT_MODE before NOISE_TYPE, which is the GLOBAL-name order
// (colorMode<loopOffset<metric<refractMode<type), not the define-name
// order (which would put NOISE_TYPE before REFRACT_MODE). See expander.h's
// file header for the full analysis.
//
// RED (before dim.cpp/resources.cpp/expander.cpp/dsl_compiler.cpp
// existed): this binary failed to link ("undefined symbols: nm::expand,
// nm::compileGraphJson, ..."). GREEN: all checks below print PASS and the
// process exits 0.

#include "core/graph/dsl_compiler.h"
#include "core/graph/expander.h"
#include "core/catalog/catalog.h"
#include "core/lang/effect_registry.h"
#include "core/lang/lexer.h"
#include "core/lang/parser.h"
#include "core/lang/validator.h"

using QString = nm::JsText;
using QJsonObject = nm::JsObject;
using QJsonArray = nm::JsArray;
using QJsonValue = nm::JsValue;
#undef QStringLiteral
#define QStringLiteral(x) nm::JsText(u##x)

#include <cmath>
#include <cstdio>
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

bool numEq(double a, double b) { return std::fabs(a - b) <= 1e-9 * std::max(1.0, std::max(std::fabs(a), std::fabs(b))); }

nm::EffectRegistry& registry() {
    static nm::EffectRegistry reg;
    static bool loaded = false;
    if (!loaded) {
        reg.loadEmbedded();
        loaded = true;
    }
    return reg;
}

nm::ExpandResult expandSrc(const QString& src) {
    const auto tokens = nm::lex(src);
    const QJsonObject ast(nm::parse(tokens));
    const QJsonObject validated = nm::validate(ast, registry());
    return nm::expand(validated, registry());
}

const nm::ExpandedPass* findPass(const nm::ExpandResult& r, const QString& idSuffix) {
    for (const nm::ExpandedPass& p : r.passes) {
        if (p.id.endsWith(idSuffix)) return &p;
    }
    return nullptr;
}

QString findInput(const nm::ExpandedPass& pass, const QString& uniformName) {
    for (const auto& kv : pass.inputs) {
        if (kv.first == uniformName) return kv.second;
    }
    return QString();
}

QString findOutput(const nm::ExpandedPass& pass, const QString& attachment) {
    for (const auto& kv : pass.outputs) {
        if (kv.first == attachment) return kv.second;
    }
    return QString();
}

} // namespace

int main() {
    // ======================================================================
    // Define collection + SORT ORDER (reference/03 §4.5; expander.h file
    // header "PARITY-CRITICAL DEVIATION FROM THE TASK BRIEF").
    // Oracle probe: `search classicNoisedeck\nnoise().write(o0)\nrender(o0)`
    // ======================================================================
    {
        const QString src = QStringLiteral("search classicNoisedeck\nnoise().write(o0)\nrender(o0)\n");
        const nm::ExpandResult r = expandSrc(src);
        check(r.errors.isEmpty(), "classicNoisedeck.noise(): no expand errors");
        const nm::ExpandedPass* p0 = findPass(r, QStringLiteral("_pass_0"));
        check(p0 != nullptr, "classicNoisedeck.noise(): effect pass 0 exists");
        if (p0) {
            check(p0->program == QStringLiteral("node_0_noise__COLOR_MODE_6__LOOP_OFFSET_300__METRIC_0__REFRACT_MODE_2__NOISE_TYPE_10"),
                  "define suffix is sorted by GLOBAL name (colorMode<loopOffset<metric<refractMode<type), "
                  "NOT by define name (which would put NOISE_TYPE before REFRACT_MODE) -- oracle-verified");
            check(p0->uniforms.value(QStringLiteral("type")).toDouble() == 10.0,
                  "RAW pass.uniforms still carries 'type' (validator pre-fills step.args with EVERY "
                  "global's default, so the args-processing loop always sees it) -- only the "
                  "orchestrator's normalize step promotes/removes it");
        }
    }

    // ======================================================================
    // Define-vs-uniform routing (brief Step 1's named test): synth.noise
    // NOISE_TYPE (define, no .uniform) vs colorMode (choices+.uniform,
    // stays a runtime uniform, no .define). Oracle probe:
    // `search synth\nnoise(type: simplex, colorMode: mono).write(o0)\nrender(o0)`
    // ======================================================================
    {
        const QString src = QStringLiteral("search synth\nnoise(type: simplex, colorMode: mono).write(o0)\nrender(o0)\n");
        const nm::ExpandResult r = expandSrc(src);
        check(r.errors.isEmpty(), "synth.noise(type:simplex,colorMode:mono): no expand errors");
        const nm::ExpandedPass* p0 = findPass(r, QStringLiteral("_pass_0"));
        check(p0 != nullptr, "synth.noise: effect pass 0 exists");
        if (p0) {
            check(p0->program == QStringLiteral("node_0_noise__LOOP_OFFSET_300__NOISE_TYPE_10"),
                  "synth.noise program suffix: only NOISE_TYPE/LOOP_OFFSET (define-carrying globals) "
                  "contribute, colorMode does NOT (it has no .define)");
            check(p0->uniforms.value(QStringLiteral("type")).toDouble() == 10.0,
                  "raw pass.uniforms['type'] == 10 (simplex) -- present pre-normalization");
            check(p0->uniforms.value(QStringLiteral("colorMode")).toDouble() == 0.0,
                  "raw pass.uniforms['colorMode'] == 0 (mono) -- a genuine runtime uniform, "
                  "resolved via its OWN choices, never define-suffix-relevant");
            const nm::CatalogFile* shader = nm::catalog_find("wgsl/synth/noise/noise.wgsl");
            const QJsonObject program = r.programs.value(p0->program).toObject();
            check(shader != nullptr && program.value(QStringLiteral("wgsl")).toString() == nm::JsText::fromUtf8(std::string(shader->bytes)),
                  "expanded program retains exact embedded WGSL source");
        }

        // The reference compiler retains raw pass uniforms and stores
        // compile-time defines on the program entry.
        try {
            const QJsonObject graph(nm::Value(nm::compileGraphJson(src, registry())));
            const QJsonArray passes = graph.value(QStringLiteral("passes")).toArray();
            const QJsonObject effectPass = passes.at(0).toObject();
            const QJsonObject program = graph.value(QStringLiteral("programs")).toObject()
                .value(effectPass.value(QStringLiteral("program")).toString()).toObject();
            const QJsonObject defines = program.value(QStringLiteral("defines")).toObject();
            const QJsonObject uniforms = effectPass.value(QStringLiteral("uniforms")).toObject();
            check(defines.value(QStringLiteral("NOISE_TYPE")).toInt(-1) == 10,
                  "graph program: defines.NOISE_TYPE == 10");
            check(defines.value(QStringLiteral("LOOP_OFFSET")).toInt(-1) == 300,
                  "graph program: defines.LOOP_OFFSET == 300");
            check(uniforms.value(QStringLiteral("type")).toInt(-1) == 10, "graph pass retains type uniform");
            check(uniforms.value(QStringLiteral("loopOffset")).toInt(-1) == 300, "graph pass retains loopOffset uniform");
            check(uniforms.contains(QStringLiteral("colorMode")) && uniforms.value(QStringLiteral("colorMode")).toDouble() == 0.0,
                  "graph pass retains runtime colorMode uniform");
            check(!defines.contains(QStringLiteral("colorMode")) && !defines.contains(QStringLiteral("COLOR_MODE")),
                  "graph program has no colorMode define");
            const nm::Graph runtime = nm::compileGraph(src, registry());
            check(!runtime.passes.empty() && runtime.passes.front().passType == QStringLiteral("effect")
                      && runtime.passes.front().defines.value(QStringLiteral("NOISE_TYPE")).toInt(-1) == 10,
                  "runtime Graph accepts reference graph passes and program defines");
        } catch (const std::exception& e) {
            check(false, e.what());
        }
    }

    // A per-step shader override replaces the embedded program source.
    {
        const QString src = QStringLiteral("search synth\nnoise().write(o0)\nrender(o0)\n");
        const QString overriddenWgsl = QStringLiteral("// shader override for node 0");
        const QJsonObject overrideShader{{QStringLiteral("wgsl"), overriddenWgsl}};
        const QJsonObject overridePrograms{{QStringLiteral("noise"), overrideShader}};
        const QJsonObject overrideSteps{{QStringLiteral("0"), overridePrograms}};
        const QJsonObject options{{QStringLiteral("shaderOverrides"), overrideSteps}};
        const QJsonObject graph(nm::Value(nm::compileGraphJson(src, registry(), options.raw().as_object())));
        const QJsonObject programs = graph.value(QStringLiteral("programs")).toObject();
        const QString programName = graph.value(QStringLiteral("passes")).toArray().first().toObject()
                                        .value(QStringLiteral("program")).toString();
        check(programs.value(programName).toObject().value(QStringLiteral("wgsl")) == overriddenWgsl,
              "compileGraphJson uses shaderOverrides keyed by the step temp index");
    }

    // ======================================================================
    // compileGraph failures carry what compiler.js throws: the code plus the
    // diagnostics (or expand errors), and what() is compiler.js
    // formatError's text. Expected strings were minted from the reference's
    // own formatError applied to compileGraph's throw (reference c9ee8a04).
    // ======================================================================
    {
        QString code;
        QString text;
        QJsonArray diagnostics;
        try {
            nm::compileGraphJson(QStringLiteral("search synth, filter\nnoise().write(o0)\nread(o0).blur().read(o1).write(o2)\n"
                                                "noise(octaves: [1, 2]).write(o3)\nnoise(seed: bogus).write(o4)\nrender(o0)\n"),
                                 registry());
        } catch (const nm::CompilationError& e) {
            code = e.code();
            text = QString::fromUtf8(std::string(e.what()));
            diagnostics = e.diagnostics();
        }
        check(code == QStringLiteral("ERR_COMPILATION_FAILED"), "compile failure: code ERR_COMPILATION_FAILED");
        check(diagnostics.size() == 2 && diagnostics.at(0).toObject().value(QStringLiteral("code")).toString() == QStringLiteral("S001")
                  && diagnostics.at(1).toObject().value(QStringLiteral("code")).toString() == QStringLiteral("S003"),
              "compile failure: carries the validate() diagnostics (S001, S003)");
        check(text == QStringLiteral("read() is a starter node and cannot be chained inline. Use standalone read() to start a "
                                     "new chain.: '[Read]' (line 3, col 17); Variable used before assignment: 'bogus'"),
              "compile failure: what() is the reference formatError text, with line and column");

        code.clear();
        text.clear();
        QJsonArray errors;
        try {
            nm::compileGraphJson(QStringLiteral("search synth\nlet x = 1\n"), registry());
        } catch (const nm::CompilationError& e) {
            code = e.code();
            text = QString::fromUtf8(std::string(e.what()));
            errors = e.errors();
        }
        check(code == QStringLiteral("ERR_EXPANSION_FAILED") && errors.size() == 1,
              "expansion failure: code ERR_EXPANSION_FAILED with the expand() errors");
        check(text == QStringLiteral("No render surface specified and no write() found - add render(oN) or write(oN)"),
              "expansion failure: what() is the reference formatError text");
    }

    // ======================================================================
    // Palette expansion (reference/03 §7). classicNoisedeck.noise()'s
    // default `palette:2` (1-based) -> PALETTES[1] = fiveG. Distinctive
    // non-round floats rule out "still just the plain vec3 default"
    // (paletteOffset/Amp/Freq/Phase/Mode are ALSO independently-declared
    // vec3/int globals with their OWN, different, defaults).
    // ======================================================================
    {
        const QString src = QStringLiteral("search classicNoisedeck\nnoise().write(o0)\nrender(o0)\n");
        const nm::ExpandResult r = expandSrc(src);
        const nm::ExpandedPass* p0 = findPass(r, QStringLiteral("_pass_0"));
        check(p0 != nullptr, "palette test: effect pass 0 exists");
        if (p0) {
            const QJsonArray amp = p0->uniforms.value(QStringLiteral("paletteAmp")).toArray();
            const QJsonArray phase = p0->uniforms.value(QStringLiteral("palettePhase")).toArray();
            check(amp.size() == 3 && numEq(amp.at(0).toDouble(), 0.56851584) && numEq(amp.at(1).toDouble(), 0.7740668)
                      && numEq(amp.at(2).toDouble(), 0.23485267),
                  "palette index 2 expands to fiveG's paletteAmp [.56851584,.7740668,.23485267] "
                  "(overwrites the plain vec3 default [.5,.5,.5])");
            check(phase.size() == 3 && numEq(phase.at(0).toDouble(), 0.727029) && numEq(phase.at(1).toDouble(), 0.08039695)
                      && numEq(phase.at(2).toDouble(), 0.10427457),
                  "palette index 2 expands to fiveG's palettePhase [.727029,.08039695,.10427457]");
            check(p0->uniforms.value(QStringLiteral("paletteMode")).toDouble() == 3.0,
                  "palette index 2 (fiveG) has classicNoisedeck mode 3 (rgb)");
        }
    }

    // ======================================================================
    // Input-lane threading (2D chain) — noise().blur() must thread
    // noise's node output into blur's inputTex, not a bare placeholder.
    // ======================================================================
    {
        const QString src = QStringLiteral("search synth, filter\nnoise().blur().write(o0)\nrender(o0)\n");
        const nm::ExpandResult r = expandSrc(src);
        check(r.errors.isEmpty(), "noise().blur(): no expand errors");
        const nm::ExpandedPass* noisePass = findPass(r, QStringLiteral("node_0_pass_0"));
        const nm::ExpandedPass* blurPass = findPass(r, QStringLiteral("node_1_pass_0"));
        check(noisePass != nullptr && blurPass != nullptr, "noise().blur(): both effect passes exist");
        if (noisePass && blurPass) {
            const QString noiseOut = findOutput(*noisePass, QStringLiteral("fragColor"));
            const QString blurIn = findInput(*blurPass, QStringLiteral("inputTex"));
            check(!noiseOut.isEmpty() && noiseOut == QStringLiteral("node_0_out"),
                  "noise's fragColor output registers as node_0_out (not the last step -- write() "
                  "builtin follows, so no last-pass-to-surface fusion here)");
            check(!blurIn.isEmpty() && blurIn == noiseOut,
                  "blur's inputTex resolves to EXACTLY noise's output virtual texture id (2D lane threading)");
        }
    }

    // ======================================================================
    // Agent-lane threading + texture spec expansion + scoped params
    // (reference/03 §6.1/§6.3). Oracle probe:
    // `search points, synth, render\nsolid().pointsEmit(stateSize: 128).physarum().pointsRender().write(o0)\nrender(o0)`
    // ======================================================================
    {
        const QString src = QStringLiteral(
            "search points, synth, render\nsolid().pointsEmit(stateSize: 128).physarum().pointsRender().write(o0)\nrender(o0)\n");
        const nm::ExpandResult r = expandSrc(src);
        check(r.errors.isEmpty(), "physarum agent chain: no expand errors");
        check(r.textureSpecs.contains(QStringLiteral("global_xyz_node_1")),
              "physarum agent chain: global_xyz scoped to its creating node (pointsEmit == node_1)");
        if (r.textureSpecs.contains(QStringLiteral("global_xyz_node_1"))) {
            const QJsonObject spec = r.textureSpecs.value(QStringLiteral("global_xyz_node_1")).toObject();
            const QJsonObject width = spec.value(QStringLiteral("width")).toObject();
            check(width.value(QStringLiteral("param")).toString() == QStringLiteral("stateSize_node_1"),
                  "global_xyz_node_1's width.param is scoped: 'stateSize' -> 'stateSize_node_1'");
            check(width.value(QStringLiteral("default")).toInt(-1) == 256,
                  "scoping rewrites ONLY the param name, not other DimSpec fields (default:256 preserved)");
            check(spec.value(QStringLiteral("format")).toString() == QStringLiteral("rgba32f"),
                  "global_xyz_node_1 format passes through unchanged (rgba32f)");
        }
        // Chain-scoped (non-particle) global texture: physarum's own pheromone trail.
        check(r.textureSpecs.contains(QStringLiteral("global_physarum_pheromone_chain_0")),
              "physarum's own global_physarum_pheromone texture is CHAIN-scoped (not particle-scoped: "
              "the name doesn't match the particle regex xyz|vel|rgba|points_trail|life_data)");
    }

    // ======================================================================
    // Control flow: the reference expander iterates `plan.chain` with
    // for...of, so a Branch/Break/Continue/Return plan raises a TypeError
    // (oracle-gated by parity/corpus/control_flow_*.dsl).
    // ======================================================================
    {
        QString error;
        try {
            expandSrc(QStringLiteral("search synth\nnoise().write(o0)\nif (1) {\n  noise().write(o1)\n}\nrender(o0)\n"));
        } catch (const std::runtime_error& e) {
            error = QString::fromUtf8(std::string(e.what()));
        }
        check(error == QStringLiteral("plan.chain is not iterable"),
              "a Branch plan fails expansion with the reference's TypeError text");
        error.clear();
        try {
            expandSrc(QStringLiteral("search synth\nnoise().write(o0)\nreturn 3\nrender(o0)\n"));
        } catch (const std::runtime_error& e) {
            error = QString::fromUtf8(std::string(e.what()));
        }
        check(error == QStringLiteral("plan.chain is not iterable"), "a Return plan fails expansion the same way");
    }

    // ======================================================================
    // Chained variable alias expansion
    // ======================================================================
    {
        const QString src = QStringLiteral(
            "search synth, filter\nlet eff = rotate(1, 0.1)\nnoise().eff().write(o0)\nrender(o0)\n");
        const nm::ExpandResult r = expandSrc(src);
        check(r.errors.isEmpty(), "chained variable alias: no expand errors");
        check(r.passes.size() == 3, "chained variable alias: exactly 3 passes (noise + rotate + write blit)");
        const nm::ExpandedPass* noisePass = findPass(r, QStringLiteral("node_0_pass_0"));
        const nm::ExpandedPass* rotatePass = findPass(r, QStringLiteral("node_1_pass_0"));
        check(noisePass != nullptr && rotatePass != nullptr, "chained variable alias: both effect passes exist");
        const nm::ExpandedPass* writePass = findPass(r, QStringLiteral("node_2_write_blit"));
        check(writePass != nullptr, "chained variable alias: terminal write blit pass exists");
        if (writePass) {
            check(writePass->isBlit, "chained variable alias: terminal pass is blit");
            check(findInput(*writePass, QStringLiteral("src")) == QStringLiteral("node_1_out"),
                  "chained variable alias: blit reads node_1_out");
            check(findOutput(*writePass, QStringLiteral("color")) == QStringLiteral("global_o0"),
                  "chained variable alias: blit writes global_o0");
        }
    }

    // ======================================================================
    // Pass-level uniform aliases (reference bd773801 expander.js +
    // runtime/uniform-aliases.js): a pass definition that feeds a shader
    // uniform from a differently named global (`uniforms: { layoutMode:
    // "layout" }`) records `{ shaderUniform: globalName }` on the expanded
    // pass; a pass that maps a global under its own name (attrition) and
    // blit passes record nothing. The normalized graph JSON carries the
    // mapping through (null when empty) for the runtime.
    // ======================================================================
    {
        const QString src = QStringLiteral(
            "search synth, filter, render, points, mixer\n"
            "perlin().subchain(name: \"emit\", id: \"e1\") {\n"
            "  .pointsEmit(stateSize: x256)\n"
            "  .pointsRender()\n"
            "}\n"
            ".write(o0)\n"
            "render(o0)\n");
        const nm::ExpandResult r = expandSrc(src);
        check(r.errors.isEmpty(), "pointsEmit subchain: no expand errors");
        const nm::ExpandedPass* aliasPass = nullptr;
        const nm::ExpandedPass* selfPass = nullptr;
        for (const nm::ExpandedPass& p : r.passes) {
            if (p.isBlit) {
                check(p.uniformAliases.isEmpty(), "pointsEmit subchain: blit pass records no uniform aliases");
                continue;
            }
            if (p.uniforms.contains(QStringLiteral("layoutMode"))) aliasPass = &p;
            if (p.uniforms.contains(QStringLiteral("attrition"))) selfPass = &p;
        }
        check(aliasPass != nullptr && selfPass != nullptr,
              "pointsEmit subchain: the init pass (layoutMode) and the step pass (attrition) both exist");
        if (aliasPass) {
            check(aliasPass->uniformAliases.value(QStringLiteral("layoutMode")).toString() == QStringLiteral("layout"),
                  "the renamed init-pass uniform records uniformAliases { layoutMode: layout }");
            check(!aliasPass->uniformAliases.contains(QStringLiteral("attrition"))
                      && !aliasPass->uniformAliases.contains(QStringLiteral("resetState")),
                  "self-mapped pass uniforms record no alias entry");
        }
        if (selfPass) {
            check(selfPass->uniformAliases.isEmpty(),
                  "a pass whose uniforms all map globals under their own name records no aliases");
        }
        const QJsonObject graph(nm::Value(nm::compileGraphJson(src, registry())));
        const QJsonArray graphPasses = graph.value(QStringLiteral("passes")).toArray();
        bool sawAliasField = false;
        for (const QJsonValue& pv : graphPasses) {
            const QJsonObject gp = pv.toObject();
            const QJsonValue ua = gp.value(QStringLiteral("uniformAliases"));
            if (gp.value(QStringLiteral("uniforms")).toObject().contains(QStringLiteral("layoutMode"))) {
                sawAliasField = ua.isObject()
                    && ua.toObject().value(QStringLiteral("layoutMode")).toString() == QStringLiteral("layout");
            } else {
                check(ua.isUndefined(), "graph JSON: a pass without aliases omits uniformAliases");
            }
        }
        check(sawAliasField, "graph JSON: the aliased pass serializes uniformAliases { layoutMode: layout }");
    }

    {
        const nm::ExpandResult r = expandSrc(QStringLiteral("search synth\nnoise().write(o0)\nrender(o0)\n"));
        const QJsonObject blit = r.programs.value(QStringLiteral("blit")).toObject();
        check(blit.value(QStringLiteral("fragmentEntryPoint")).toString() == QStringLiteral("main")
                  && blit.value(QStringLiteral("fragment")).toString().startsWith(u"#version 300 es")
                  && blit.value(QStringLiteral("wgsl")).toString().find(u"textureSample") != QString::npos
                  && !blit.contains(QStringLiteral("defines")),
              "built-in blit program retains source and the reference field set");
        const QJsonObject graph(nm::Value(nm::compileGraphJson(
            QStringLiteral("search synth\nnoise().write(o0)\nrender(o0)\n"), registry())));
        check(graph.value(QStringLiteral("programs")).toObject().value(QStringLiteral("blit")).toObject().contains(QStringLiteral("fragment")),
              "compileGraphJson retains shader source for callers");
        check(nm::hashSource(u"\U0001F600") == u"11zz7", "source hash uses both UTF-16 surrogate units");
    }

    if (g_failures == 0) {
        std::printf("ALL PASS (test_expander)\n");
        return 0;
    }
    std::printf("%d FAILURE(S) (test_expander)\n", g_failures);
    return 1;
}
