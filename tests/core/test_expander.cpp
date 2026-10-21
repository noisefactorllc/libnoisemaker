// Unit tests for libnoisemaker::expand (qt/noisemaker/compiler/expander.{h,cpp}) and
// libnoisemaker::compileGraphJson (dsl_compiler.{h,cpp}). Plain assert-style checks,
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
// existed): this binary failed to link ("undefined symbols: libnoisemaker::expand,
// libnoisemaker::compileGraphJson, ..."). GREEN: all checks below print PASS and the
// process exits 0.

#include "core/graph/dsl_compiler.h"
#include "core/graph/expander.h"
#include "core/catalog/catalog.h"
#include "core/lang/effect_registry.h"
#include "core/lang/lexer.h"
#include "core/lang/parser.h"
#include "core/lang/validator.h"


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

libnoisemaker::EffectRegistry& registry() {
    static libnoisemaker::EffectRegistry reg;
    static bool loaded = false;
    if (!loaded) {
        reg.loadEmbedded();
        loaded = true;
    }
    return reg;
}

libnoisemaker::ExpandResult expandSrc(const libnoisemaker::JsText& src) {
    const auto tokens = libnoisemaker::lexTokens(src);
    const libnoisemaker::JsObject ast(libnoisemaker::parse(tokens));
    const libnoisemaker::JsObject validated = libnoisemaker::validate(ast, registry());
    return libnoisemaker::expand(validated, registry());
}

const libnoisemaker::ExpandedPass* findPass(const libnoisemaker::ExpandResult& r, const libnoisemaker::JsText& idSuffix) {
    for (const libnoisemaker::ExpandedPass& p : r.passes) {
        if (p.id.endsWith(idSuffix)) return &p;
    }
    return nullptr;
}

libnoisemaker::JsText findInput(const libnoisemaker::ExpandedPass& pass, const libnoisemaker::JsText& uniformName) {
    for (const auto& kv : pass.inputs) {
        if (kv.first == uniformName) return kv.second;
    }
    return libnoisemaker::JsText();
}

libnoisemaker::JsText findOutput(const libnoisemaker::ExpandedPass& pass, const libnoisemaker::JsText& attachment) {
    for (const auto& kv : pass.outputs) {
        if (kv.first == attachment) return kv.second;
    }
    return libnoisemaker::JsText();
}

} // namespace

int main() {
    // ======================================================================
    // Define collection + SORT ORDER (reference/03 §4.5; expander.h file
    // header "PARITY-CRITICAL DEVIATION FROM THE TASK BRIEF").
    // Oracle probe: `search classicNoisedeck\nnoise().write(o0)\nrender(o0)`
    // ======================================================================
    {
        const libnoisemaker::JsText src = libnoisemaker::JsText(u"search classicNoisedeck\nnoise().write(o0)\nrender(o0)\n");
        const libnoisemaker::ExpandResult r = expandSrc(src);
        check(r.errors.isEmpty(), "classicNoisedeck.noise(): no expand errors");
        const libnoisemaker::ExpandedPass* p0 = findPass(r, libnoisemaker::JsText(u"_pass_0"));
        check(p0 != nullptr, "classicNoisedeck.noise(): effect pass 0 exists");
        if (p0) {
            check(p0->program == libnoisemaker::JsText(u"node_0_noise__COLOR_MODE_6__LOOP_OFFSET_300__METRIC_0__REFRACT_MODE_2__NOISE_TYPE_10"),
                  "define suffix is sorted by GLOBAL name (colorMode<loopOffset<metric<refractMode<type), "
                  "NOT by define name (which would put NOISE_TYPE before REFRACT_MODE) -- oracle-verified");
            check(p0->uniforms.value(libnoisemaker::JsText(u"type")).toDouble() == 10.0,
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
        const libnoisemaker::JsText src = libnoisemaker::JsText(u"search synth\nnoise(type: simplex, colorMode: mono).write(o0)\nrender(o0)\n");
        const libnoisemaker::ExpandResult r = expandSrc(src);
        check(r.errors.isEmpty(), "synth.noise(type:simplex,colorMode:mono): no expand errors");
        const libnoisemaker::ExpandedPass* p0 = findPass(r, libnoisemaker::JsText(u"_pass_0"));
        check(p0 != nullptr, "synth.noise: effect pass 0 exists");
        if (p0) {
            check(p0->program == libnoisemaker::JsText(u"node_0_noise__LOOP_OFFSET_300__NOISE_TYPE_10"),
                  "synth.noise program suffix: only NOISE_TYPE/LOOP_OFFSET (define-carrying globals) "
                  "contribute, colorMode does NOT (it has no .define)");
            check(p0->uniforms.value(libnoisemaker::JsText(u"type")).toDouble() == 10.0,
                  "raw pass.uniforms['type'] == 10 (simplex) -- present pre-normalization");
            check(p0->uniforms.value(libnoisemaker::JsText(u"colorMode")).toDouble() == 0.0,
                  "raw pass.uniforms['colorMode'] == 0 (mono) -- a genuine runtime uniform, "
                  "resolved via its OWN choices, never define-suffix-relevant");
            const libnoisemaker::CatalogFile* shader = libnoisemaker::catalog_find("wgsl/synth/noise/noise.wgsl");
            const libnoisemaker::JsObject program = r.programs.value(p0->program).toObject();
            check(shader != nullptr && program.value(libnoisemaker::JsText(u"wgsl")).toString() == libnoisemaker::JsText::fromUtf8(std::string(shader->bytes)),
                  "expanded program retains exact embedded WGSL source");
        }

        // The reference compiler retains raw pass uniforms and stores
        // compile-time defines on the program entry.
        try {
            const libnoisemaker::JsObject graph(libnoisemaker::Value(libnoisemaker::compileGraphJson(src, registry())));
            const libnoisemaker::JsArray passes = graph.value(libnoisemaker::JsText(u"passes")).toArray();
            const libnoisemaker::JsObject effectPass = passes.at(0).toObject();
            const libnoisemaker::JsObject program = graph.value(libnoisemaker::JsText(u"programs")).toObject()
                .value(effectPass.value(libnoisemaker::JsText(u"program")).toString()).toObject();
            const libnoisemaker::JsObject defines = program.value(libnoisemaker::JsText(u"defines")).toObject();
            const libnoisemaker::JsObject uniforms = effectPass.value(libnoisemaker::JsText(u"uniforms")).toObject();
            check(defines.value(libnoisemaker::JsText(u"NOISE_TYPE")).toInt(-1) == 10,
                  "graph program: defines.NOISE_TYPE == 10");
            check(defines.value(libnoisemaker::JsText(u"LOOP_OFFSET")).toInt(-1) == 300,
                  "graph program: defines.LOOP_OFFSET == 300");
            check(uniforms.value(libnoisemaker::JsText(u"type")).toInt(-1) == 10, "graph pass retains type uniform");
            check(uniforms.value(libnoisemaker::JsText(u"loopOffset")).toInt(-1) == 300, "graph pass retains loopOffset uniform");
            check(uniforms.contains(libnoisemaker::JsText(u"colorMode")) && uniforms.value(libnoisemaker::JsText(u"colorMode")).toDouble() == 0.0,
                  "graph pass retains runtime colorMode uniform");
            check(!defines.contains(libnoisemaker::JsText(u"colorMode")) && !defines.contains(libnoisemaker::JsText(u"COLOR_MODE")),
                  "graph program has no colorMode define");
            const libnoisemaker::Graph runtime = libnoisemaker::compileGraph(src, registry());
            check(!runtime.passes.empty() && runtime.passes.front().passType == libnoisemaker::JsText(u"effect")
                      && runtime.passes.front().defines.value(libnoisemaker::JsText(u"NOISE_TYPE")).toInt(-1) == 10,
                  "runtime Graph accepts reference graph passes and program defines");
        } catch (const std::exception& e) {
            check(false, e.what());
        }
    }

    // A per-step shader override replaces the embedded program source.
    {
        const libnoisemaker::JsText src = libnoisemaker::JsText(u"search synth\nnoise().write(o0)\nrender(o0)\n");
        const libnoisemaker::JsText overriddenWgsl = libnoisemaker::JsText(u"// shader override for node 0");
        const libnoisemaker::JsObject overrideShader{{libnoisemaker::JsText(u"wgsl"), overriddenWgsl}};
        const libnoisemaker::JsObject overridePrograms{{libnoisemaker::JsText(u"noise"), overrideShader}};
        const libnoisemaker::JsObject overrideSteps{{libnoisemaker::JsText(u"0"), overridePrograms}};
        const libnoisemaker::JsObject options{{libnoisemaker::JsText(u"shaderOverrides"), overrideSteps}};
        const libnoisemaker::JsObject graph(libnoisemaker::Value(libnoisemaker::compileGraphJson(src, registry(), options.raw().as_object())));
        const libnoisemaker::JsObject programs = graph.value(libnoisemaker::JsText(u"programs")).toObject();
        const libnoisemaker::JsText programName = graph.value(libnoisemaker::JsText(u"passes")).toArray().first().toObject()
                                        .value(libnoisemaker::JsText(u"program")).toString();
        check(programs.value(programName).toObject().value(libnoisemaker::JsText(u"wgsl")) == overriddenWgsl,
              "compileGraphJson uses shaderOverrides keyed by the step temp index");
    }

    // ======================================================================
    // compileGraph failures carry what compiler.js throws: the code plus the
    // diagnostics (or expand errors), and what() is compiler.js
    // formatError's text. Expected strings were minted from the reference's
    // own formatError applied to compileGraph's throw (reference c9ee8a04).
    // ======================================================================
    {
        libnoisemaker::JsText code;
        libnoisemaker::JsText text;
        libnoisemaker::JsArray diagnostics;
        try {
            libnoisemaker::compileGraphJson(libnoisemaker::JsText(u"search synth, filter\nnoise().write(o0)\nread(o0).blur().read(o1).write(o2)\n"
                                                "noise(octaves: [1, 2]).write(o3)\nnoise(seed: bogus).write(o4)\nrender(o0)\n"),
                                 registry());
        } catch (const libnoisemaker::CompilationError& e) {
            code = e.code();
            text = libnoisemaker::JsText::fromUtf8(std::string(e.what()));
            diagnostics = e.diagnostics();
        }
        check(code == libnoisemaker::JsText(u"ERR_COMPILATION_FAILED"), "compile failure: code ERR_COMPILATION_FAILED");
        check(diagnostics.size() == 2 && diagnostics.at(0).toObject().value(libnoisemaker::JsText(u"code")).toString() == libnoisemaker::JsText(u"S001")
                  && diagnostics.at(1).toObject().value(libnoisemaker::JsText(u"code")).toString() == libnoisemaker::JsText(u"S003"),
              "compile failure: carries the validate() diagnostics (S001, S003)");
        check(text == libnoisemaker::JsText(u"read() is a starter node and cannot be chained inline. Use standalone read() to start a "
                                     "new chain.: '[Read]' (line 3, col 17); Variable used before assignment: 'bogus'"),
              "compile failure: what() is the reference formatError text, with line and column");

        code.clear();
        text.clear();
        libnoisemaker::JsArray errors;
        try {
            libnoisemaker::compileGraphJson(libnoisemaker::JsText(u"search synth\nlet x = 1\n"), registry());
        } catch (const libnoisemaker::CompilationError& e) {
            code = e.code();
            text = libnoisemaker::JsText::fromUtf8(std::string(e.what()));
            errors = e.errors();
        }
        check(code == libnoisemaker::JsText(u"ERR_EXPANSION_FAILED") && errors.size() == 1,
              "expansion failure: code ERR_EXPANSION_FAILED with the expand() errors");
        check(text == libnoisemaker::JsText(u"No render surface specified and no write() found - add render(oN) or write(oN)"),
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
        const libnoisemaker::JsText src = libnoisemaker::JsText(u"search classicNoisedeck\nnoise().write(o0)\nrender(o0)\n");
        const libnoisemaker::ExpandResult r = expandSrc(src);
        const libnoisemaker::ExpandedPass* p0 = findPass(r, libnoisemaker::JsText(u"_pass_0"));
        check(p0 != nullptr, "palette test: effect pass 0 exists");
        if (p0) {
            const libnoisemaker::JsArray amp = p0->uniforms.value(libnoisemaker::JsText(u"paletteAmp")).toArray();
            const libnoisemaker::JsArray phase = p0->uniforms.value(libnoisemaker::JsText(u"palettePhase")).toArray();
            check(amp.size() == 3 && numEq(amp.at(0).toDouble(), 0.56851584) && numEq(amp.at(1).toDouble(), 0.7740668)
                      && numEq(amp.at(2).toDouble(), 0.23485267),
                  "palette index 2 expands to fiveG's paletteAmp [.56851584,.7740668,.23485267] "
                  "(overwrites the plain vec3 default [.5,.5,.5])");
            check(phase.size() == 3 && numEq(phase.at(0).toDouble(), 0.727029) && numEq(phase.at(1).toDouble(), 0.08039695)
                      && numEq(phase.at(2).toDouble(), 0.10427457),
                  "palette index 2 expands to fiveG's palettePhase [.727029,.08039695,.10427457]");
            check(p0->uniforms.value(libnoisemaker::JsText(u"paletteMode")).toDouble() == 3.0,
                  "palette index 2 (fiveG) has classicNoisedeck mode 3 (rgb)");
        }
    }

    // ======================================================================
    // Input-lane threading (2D chain) — noise().blur() must thread
    // noise's node output into blur's inputTex, not a bare placeholder.
    // ======================================================================
    {
        const libnoisemaker::JsText src = libnoisemaker::JsText(u"search synth, filter\nnoise().blur().write(o0)\nrender(o0)\n");
        const libnoisemaker::ExpandResult r = expandSrc(src);
        check(r.errors.isEmpty(), "noise().blur(): no expand errors");
        const libnoisemaker::ExpandedPass* noisePass = findPass(r, libnoisemaker::JsText(u"node_0_pass_0"));
        const libnoisemaker::ExpandedPass* blurPass = findPass(r, libnoisemaker::JsText(u"node_1_pass_0"));
        check(noisePass != nullptr && blurPass != nullptr, "noise().blur(): both effect passes exist");
        if (noisePass && blurPass) {
            const libnoisemaker::JsText noiseOut = findOutput(*noisePass, libnoisemaker::JsText(u"fragColor"));
            const libnoisemaker::JsText blurIn = findInput(*blurPass, libnoisemaker::JsText(u"inputTex"));
            check(!noiseOut.isEmpty() && noiseOut == libnoisemaker::JsText(u"node_0_out"),
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
        const libnoisemaker::JsText src = libnoisemaker::JsText(
            u"search points, synth, render\nsolid().pointsEmit(stateSize: 128).physarum().pointsRender().write(o0)\nrender(o0)\n");
        const libnoisemaker::ExpandResult r = expandSrc(src);
        check(r.errors.isEmpty(), "physarum agent chain: no expand errors");
        check(r.textureSpecs.contains(libnoisemaker::JsText(u"global_xyz_node_1")),
              "physarum agent chain: global_xyz scoped to its creating node (pointsEmit == node_1)");
        if (r.textureSpecs.contains(libnoisemaker::JsText(u"global_xyz_node_1"))) {
            const libnoisemaker::JsObject spec = r.textureSpecs.value(libnoisemaker::JsText(u"global_xyz_node_1")).toObject();
            const libnoisemaker::JsObject width = spec.value(libnoisemaker::JsText(u"width")).toObject();
            check(width.value(libnoisemaker::JsText(u"param")).toString() == libnoisemaker::JsText(u"stateSize_node_1"),
                  "global_xyz_node_1's width.param is scoped: 'stateSize' -> 'stateSize_node_1'");
            check(width.value(libnoisemaker::JsText(u"default")).toInt(-1) == 256,
                  "scoping rewrites ONLY the param name, not other DimSpec fields (default:256 preserved)");
            check(spec.value(libnoisemaker::JsText(u"format")).toString() == libnoisemaker::JsText(u"rgba32f"),
                  "global_xyz_node_1 format passes through unchanged (rgba32f)");
        }
        // Chain-scoped (non-particle) global texture: physarum's own pheromone trail.
        check(r.textureSpecs.contains(libnoisemaker::JsText(u"global_physarum_pheromone_chain_0")),
              "physarum's own global_physarum_pheromone texture is CHAIN-scoped (not particle-scoped: "
              "the name doesn't match the particle regex xyz|vel|rgba|points_trail|life_data)");
    }

    // ======================================================================
    // Control flow: nothing executes Branch/Break/Continue/Return plans.
    // The expander records the reference error for each and expands no
    // passes for them (oracle-gated by parity/corpus/control_flow_*.dsl).
    // ======================================================================
    {
        const libnoisemaker::ExpandResult r = expandSrc(
            libnoisemaker::JsText(u"search synth\nnoise().write(o0)\nif (1) {\n  noise().write(o1)\n}\nrender(o0)\n"));
        check(r.errors.size() == 1
                  && r.errors.first().toObject().value(libnoisemaker::JsText(u"message")).toString()
                         == libnoisemaker::JsText(u"The runtime does not yet execute control flow ('if')"),
              "a Branch plan records the reference control-flow error and expands nothing for it");
        const libnoisemaker::ExpandResult returned = expandSrc(
            libnoisemaker::JsText(u"search synth\nnoise().write(o0)\nreturn 3\nrender(o0)\n"));
        check(returned.errors.size() == 1
                  && returned.errors.first().toObject().value(libnoisemaker::JsText(u"message")).toString()
                         == libnoisemaker::JsText(u"The runtime does not yet execute control flow ('return')"),
              "a Return plan records the same control-flow error");
        const libnoisemaker::ExpandedPass* noisePass = findPass(r, libnoisemaker::JsText(u"node_0_pass_0"));
        check(noisePass != nullptr, "the plan before the branch still expands");
    }

    // ======================================================================
    // Chained variable alias expansion
    // ======================================================================
    {
        const libnoisemaker::JsText src = libnoisemaker::JsText(
            u"search synth, filter\nlet eff = rotate(1, 0.1)\nnoise().eff().write(o0)\nrender(o0)\n");
        const libnoisemaker::ExpandResult r = expandSrc(src);
        check(r.errors.isEmpty(), "chained variable alias: no expand errors");
        check(r.passes.size() == 3, "chained variable alias: exactly 3 passes (noise + rotate + write blit)");
        const libnoisemaker::ExpandedPass* noisePass = findPass(r, libnoisemaker::JsText(u"node_0_pass_0"));
        const libnoisemaker::ExpandedPass* rotatePass = findPass(r, libnoisemaker::JsText(u"node_1_pass_0"));
        check(noisePass != nullptr && rotatePass != nullptr, "chained variable alias: both effect passes exist");
        const libnoisemaker::ExpandedPass* writePass = findPass(r, libnoisemaker::JsText(u"node_2_write_blit"));
        check(writePass != nullptr, "chained variable alias: terminal write blit pass exists");
        if (writePass) {
            check(writePass->isBlit, "chained variable alias: terminal pass is blit");
            check(findInput(*writePass, libnoisemaker::JsText(u"src")) == libnoisemaker::JsText(u"node_1_out"),
                  "chained variable alias: blit reads node_1_out");
            check(findOutput(*writePass, libnoisemaker::JsText(u"color")) == libnoisemaker::JsText(u"global_o0"),
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
        const libnoisemaker::JsText src = libnoisemaker::JsText(
            u"search synth, filter, render, points, mixer\n"
            "perlin().subchain(name: \"emit\", id: \"e1\") {\n"
            "  .pointsEmit(stateSize: x256)\n"
            "  .pointsRender()\n"
            "}\n"
            ".write(o0)\n"
            "render(o0)\n");
        const libnoisemaker::ExpandResult r = expandSrc(src);
        check(r.errors.isEmpty(), "pointsEmit subchain: no expand errors");
        const libnoisemaker::ExpandedPass* aliasPass = nullptr;
        const libnoisemaker::ExpandedPass* selfPass = nullptr;
        for (const libnoisemaker::ExpandedPass& p : r.passes) {
            if (p.isBlit) {
                check(p.uniformAliases.isEmpty(), "pointsEmit subchain: blit pass records no uniform aliases");
                continue;
            }
            if (p.uniforms.contains(libnoisemaker::JsText(u"layoutMode"))) aliasPass = &p;
            if (p.uniforms.contains(libnoisemaker::JsText(u"attrition"))) selfPass = &p;
        }
        check(aliasPass != nullptr && selfPass != nullptr,
              "pointsEmit subchain: the init pass (layoutMode) and the step pass (attrition) both exist");
        if (aliasPass) {
            check(aliasPass->uniformAliases.value(libnoisemaker::JsText(u"layoutMode")).toString() == libnoisemaker::JsText(u"layout"),
                  "the renamed init-pass uniform records uniformAliases { layoutMode: layout }");
            check(!aliasPass->uniformAliases.contains(libnoisemaker::JsText(u"attrition"))
                      && !aliasPass->uniformAliases.contains(libnoisemaker::JsText(u"resetState")),
                  "self-mapped pass uniforms record no alias entry");
        }
        if (selfPass) {
            check(selfPass->uniformAliases.isEmpty(),
                  "a pass whose uniforms all map globals under their own name records no aliases");
        }
        const libnoisemaker::JsObject graph(libnoisemaker::Value(libnoisemaker::compileGraphJson(src, registry())));
        const libnoisemaker::JsArray graphPasses = graph.value(libnoisemaker::JsText(u"passes")).toArray();
        bool sawAliasField = false;
        for (const libnoisemaker::JsValue& pv : graphPasses) {
            const libnoisemaker::JsObject gp = pv.toObject();
            const libnoisemaker::JsValue ua = gp.value(libnoisemaker::JsText(u"uniformAliases"));
            if (gp.value(libnoisemaker::JsText(u"uniforms")).toObject().contains(libnoisemaker::JsText(u"layoutMode"))) {
                sawAliasField = ua.isObject()
                    && ua.toObject().value(libnoisemaker::JsText(u"layoutMode")).toString() == libnoisemaker::JsText(u"layout");
            } else {
                check(ua.isUndefined(), "graph JSON: a pass without aliases omits uniformAliases");
            }
        }
        check(sawAliasField, "graph JSON: the aliased pass serializes uniformAliases { layoutMode: layout }");
    }

    {
        const libnoisemaker::ExpandResult r = expandSrc(libnoisemaker::JsText(u"search synth\nnoise().write(o0)\nrender(o0)\n"));
        const libnoisemaker::JsObject blit = r.programs.value(libnoisemaker::JsText(u"blit")).toObject();
        check(blit.value(libnoisemaker::JsText(u"fragmentEntryPoint")).toString() == libnoisemaker::JsText(u"main")
                  && blit.value(libnoisemaker::JsText(u"fragment")).toString().startsWith(u"#version 300 es")
                  && blit.value(libnoisemaker::JsText(u"wgsl")).toString().find(u"textureSample") != libnoisemaker::JsText::npos
                  && !blit.contains(libnoisemaker::JsText(u"defines")),
              "built-in blit program retains source and the reference field set");
        const libnoisemaker::JsObject graph(libnoisemaker::Value(libnoisemaker::compileGraphJson(
            libnoisemaker::JsText(u"search synth\nnoise().write(o0)\nrender(o0)\n"), registry())));
        check(graph.value(libnoisemaker::JsText(u"programs")).toObject().value(libnoisemaker::JsText(u"blit")).toObject().contains(libnoisemaker::JsText(u"fragment")),
              "compileGraphJson retains shader source for callers");
        check(libnoisemaker::hashSource(u"\U0001F600") == u"11zz7", "source hash uses both UTF-16 surrogate units");
    }

    if (g_failures == 0) {
        std::printf("ALL PASS (test_expander)\n");
        return 0;
    }
    std::printf("%d FAILURE(S) (test_expander)\n", g_failures);
    return 1;
}
