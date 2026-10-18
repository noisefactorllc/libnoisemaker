// Unit tests for libnoisemaker::EffectRegistry (qt/noisemaker/compiler/effect_registry.{h,cpp}).
// Plain assert-style checks, no test framework dependency (matches
// test_lexer.cpp / test_parser.cpp convention).
//
// Every non-obvious assertion here traces back to a fact independently
// verified against the live reference oracle (NM_REFERENCE_ROOT tools/
// dump-registry.mjs) or a direct read of tools/dump-validate.mjs /
// tools/export-graph.mjs / the reference shaders/src/renderer/canvas.js
// before being hard-coded -- see task report "hazards encountered":
//   - starter status is the PASS-INPUT rule, not the JSON `starter` field
//     (mixer.channelCombine disagrees between the two: baked false,
//     derived true -- the one case in the whole 210-effect catalog).
//   - ops use namespaced keys; starter registration also accepts bare names.
//   - vec4 -> "color" is the ONLY globals[key].type rewrite, applied at
//     REGISTRATION time (the JSON itself keeps "vec4" verbatim, e.g.
//     synth/remap.json's zone*_v* uniforms).
//
// RED (before effect_registry.cpp/enums.cpp existed): this binary failed
// to link ("undefined symbols: libnoisemaker::EffectRegistry::..."). GREEN: all
// checks below print PASS and the process exits 0.

#include "core/lang/effect_registry.h"


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

} // namespace

int main() {
    libnoisemaker::EffectRegistry reg;
    reg.loadEmbedded();

    // ==================================================================
    // load sanity
    // ==================================================================
    {
        check(reg.getOp(libnoisemaker::JsText(u"synth.noise")) != nullptr, "synth.noise op is registered");
        check(reg.getOp(libnoisemaker::JsText(u"filter.adjust")) != nullptr, "filter.adjust op is registered");
        check(reg.hasEffect(libnoisemaker::JsText(u"synth.noise")), "synth.noise effect lookup key present");
        check(reg.hasEffect(libnoisemaker::JsText(u"synth/noise")), "synth/noise (slash) effect lookup key present");
        check(reg.hasEffect(libnoisemaker::JsText(u"noise")), "bare 'noise' effect lookup key present (some effect wins it)");
    }

    // ==================================================================
    // Ops use namespaced keys; the reference registers bare starter names.
    // ==================================================================
    {
        check(reg.getOp(libnoisemaker::JsText(u"noise")) == nullptr, "bare 'noise' is NOT a valid getOp() key");
        check(reg.getOp(libnoisemaker::JsText(u"solid")) == nullptr, "bare 'solid' is NOT a valid getOp() key");
        check(reg.isStarterOp(libnoisemaker::JsText(u"solid")), "bare 'solid' is a registered starter");
        check(reg.isStarterOp(libnoisemaker::JsText(u"synth.solid")), "isStarterOp('synth.solid') (namespaced) is true");
    }

    // ==================================================================
    // starter derivation: pass-input rule, NOT the baked JSON `starter`
    // field (the one verified disagreement in the whole catalog)
    // ==================================================================
    {
        check(reg.isStarterOp(libnoisemaker::JsText(u"mixer.channelCombine")),
              "mixer.channelCombine IS a starter by the pass-rule (its JSON `starter` field is false; "
              "it takes inputs via surface kwargs, not pipeline `inputs`)");
    }

    // ==================================================================
    // vec4 -> "color" registration-time rewrite (op arg `type`); the JSON
    // itself keeps "vec4" verbatim (never hand-edited/rewritten at rest)
    // ==================================================================
    {
        const libnoisemaker::OpSpec* remap = reg.getOp(libnoisemaker::JsText(u"synth.remap"));
        check(remap != nullptr, "synth.remap op is registered");
        bool foundZoneVert = false;
        bool typeIsColor = false;
        if (remap) {
            for (const libnoisemaker::ParamDef& pd : remap->args) {
                if (pd.name == libnoisemaker::JsText(u"zone0_v0")) {
                    foundZoneVert = true;
                    typeIsColor = (pd.type == libnoisemaker::JsText(u"color"));
                }
            }
        }
        check(foundZoneVert, "synth.remap has a zone0_v0 global");
        check(typeIsColor, "synth.remap zone0_v0 arg type is 'color' (registry rewrote vec4->color)");
    }

    // ==================================================================
    // args positional order matches globals DECLARATION order (Qt's
    // libnoisemaker::JsObject does NOT preserve parse order -- verified empirically:
    // parsing {"zebra":1,"apple":2,"mango":3,"banana":4} and iterating
    // yields alphabetical order, not source order -- so this exercises
    // the text-scanning key-order recovery directly)
    // ==================================================================
    {
        const libnoisemaker::OpSpec* noise = reg.getOp(libnoisemaker::JsText(u"synth.noise"));
        check(noise != nullptr, "synth.noise op is registered (for order check)");
        if (noise) {
            // synth/noise.json's globals declaration order (see the file):
            // type, octaves, scaleX, scaleY, seed, wrap, ridges,
            // loopOffset, loopScale, speed, colorMode -- alphabetical would
            // be colorMode, loopOffset, loopScale, octaves, ridges, ...
            const libnoisemaker::JsList expected = {
                libnoisemaker::JsText(u"type"),   libnoisemaker::JsText(u"octaves"),    libnoisemaker::JsText(u"scaleX"),
                libnoisemaker::JsText(u"scaleY"), libnoisemaker::JsText(u"seed"),       libnoisemaker::JsText(u"wrap"),
                libnoisemaker::JsText(u"ridges"), libnoisemaker::JsText(u"loopOffset"), libnoisemaker::JsText(u"loopScale"),
                libnoisemaker::JsText(u"speed"),  libnoisemaker::JsText(u"colorMode"),
            };
            bool orderMatches = (noise->args.size() == expected.size());
            if (orderMatches) {
                for (int i = 0; i < std::ssize(expected); ++i) {
                    if (noise->args.at(i).name != expected.at(i)) {
                        orderMatches = false;
                        break;
                    }
                }
            }
            check(orderMatches, "synth.noise args preserve globals DECLARATION order (not alphabetical)");
        }
    }

    // ==================================================================
    // min/max/default/uniform presence (Undefined when absent)
    // ==================================================================
    {
        const libnoisemaker::OpSpec* noise = reg.getOp(libnoisemaker::JsText(u"synth.noise"));
        check(noise != nullptr, "synth.noise op is registered (for min/max check)");
        if (noise) {
            const libnoisemaker::ParamDef* octaves = nullptr;
            for (const libnoisemaker::ParamDef& pd : noise->args) {
                if (pd.name == libnoisemaker::JsText(u"octaves")) octaves = &pd;
            }
            check(octaves != nullptr, "synth.noise has an 'octaves' arg");
            if (octaves) {
                check(octaves->hasMin() && octaves->minAsDouble() == 1.0, "octaves.min == 1");
                check(octaves->hasMax() && octaves->maxAsDouble() == 8.0, "octaves.max == 8");
                check(octaves->hasDefault() && octaves->defaultValue.toDouble() == 2.0, "octaves.default == 2");
            }
        }
    }

    // ==================================================================
    // choices with no explicit enum synthesize "<ns>.<func>.<key>" and
    // register into the PROJECT enum tree (canvas.js parity)
    // ==================================================================
    {
        const libnoisemaker::OpSpec* noise = reg.getOp(libnoisemaker::JsText(u"synth.noise"));
        const libnoisemaker::ParamDef* type = nullptr;
        if (noise) {
            for (const libnoisemaker::ParamDef& pd : noise->args) {
                if (pd.name == libnoisemaker::JsText(u"type")) type = &pd;
            }
        }
        check(type != nullptr, "synth.noise has a 'type' arg");
        if (type) {
            check(type->hasEnumPath() && type->enumPathString() == libnoisemaker::JsText(u"synth.noise.type"),
                  "synth.noise.type synthesizes enumPath 'synth.noise.type'");
            check(type->hasChoices(), "synth.noise.type arg carries verbatim choices for the ops dump");
        }
        const libnoisemaker::JsValue simplex = reg.enums().tryGetHead(libnoisemaker::JsText(u"synth"));
        bool resolvesTo10 = false;
        if (simplex.isObject()) {
            const libnoisemaker::JsValue noiseNs = simplex.toObject().value(libnoisemaker::JsText(u"noise"));
            if (noiseNs.isObject()) {
                const libnoisemaker::JsValue typeNs = noiseNs.toObject().value(libnoisemaker::JsText(u"type"));
                if (typeNs.isObject()) {
                    const libnoisemaker::JsValue simplexLeaf = typeNs.toObject().value(libnoisemaker::JsText(u"simplex"));
                    resolvesTo10 = simplexLeaf.isObject()
                        && simplexLeaf.toObject().value(libnoisemaker::JsText(u"value")).toDouble() == 10.0;
                }
            }
        }
        check(resolvesTo10, "project enum tree resolves synth.noise.type.simplex -> 10");
    }

    // ==================================================================
    // choices ':' group-header entries are skipped for enum REGISTRATION
    // (loopOffset's choices include "Shapes:": null, "Directional:": null,
    // "Misc:": null) but kept VERBATIM in the dumped/stored `choices`
    // field (two different consumers, two different filtering rules)
    // ==================================================================
    {
        const libnoisemaker::OpSpec* noise = reg.getOp(libnoisemaker::JsText(u"synth.noise"));
        const libnoisemaker::ParamDef* loopOffset = nullptr;
        if (noise) {
            for (const libnoisemaker::ParamDef& pd : noise->args) {
                if (pd.name == libnoisemaker::JsText(u"loopOffset")) loopOffset = &pd;
            }
        }
        check(loopOffset != nullptr, "synth.noise has a 'loopOffset' arg");
        if (loopOffset) {
            check(loopOffset->hasChoices() && loopOffset->choicesObject().contains(libnoisemaker::JsText(u"Shapes:")),
                  "loopOffset's verbatim choices still carry the 'Shapes:' group header");
        }
        const libnoisemaker::JsValue synthNs = reg.enums().tryGetHead(libnoisemaker::JsText(u"synth"));
        bool groupHeaderNotRegistered = true;
        if (synthNs.isObject()) {
            const libnoisemaker::JsValue noiseNs = synthNs.toObject().value(libnoisemaker::JsText(u"noise"));
            if (noiseNs.isObject()) {
                const libnoisemaker::JsValue loopOffsetNs = noiseNs.toObject().value(libnoisemaker::JsText(u"loopOffset"));
                if (loopOffsetNs.isObject()) {
                    groupHeaderNotRegistered = !loopOffsetNs.toObject().contains(libnoisemaker::JsText(u"Shapes:"));
                }
            }
        }
        check(groupHeaderNotRegistered, "'Shapes:' group header is NOT registered as an enum leaf");
    }

    // ==================================================================
    // std enum tree: fixed, always present regardless of any effect load
    // (channel/color/oscType/oscKind/midiMode/audioBand/palette)
    // ==================================================================
    {
        const libnoisemaker::JsValue oscKind = reg.enums().std().value(libnoisemaker::JsText(u"oscKind"));
        check(oscKind.isObject(), "std().oscKind is a subtree");
        const double noiseVal = oscKind.toObject().value(libnoisemaker::JsText(u"noise")).toObject()
                                     .value(libnoisemaker::JsText(u"value")).toDouble(-1);
        const double noise1dVal = oscKind.toObject().value(libnoisemaker::JsText(u"noise1d")).toObject()
                                       .value(libnoisemaker::JsText(u"value")).toDouble(-2);
        check(noiseVal == 5.0 && noise1dVal == 5.0, "oscKind.noise == oscKind.noise1d == 5 (alias)");

        const libnoisemaker::JsValue audioBand = reg.enums().std().value(libnoisemaker::JsText(u"audioBand"));
        const double rawVal = audioBand.toObject().value(libnoisemaker::JsText(u"raw")).toObject()
                                  .value(libnoisemaker::JsText(u"value")).toDouble(-1);
        check(rawVal == 4.0, "audioBand.raw == 4");

        const libnoisemaker::JsValue palette = reg.enums().std().value(libnoisemaker::JsText(u"palette"));
        check(palette.isObject(), "std().palette is a subtree");
        const double noneIdx = palette.toObject().value(libnoisemaker::JsText(u"none")).toObject()
                                    .value(libnoisemaker::JsText(u"value")).toDouble(-1);
        const double solarisIdx = palette.toObject().value(libnoisemaker::JsText(u"solaris")).toObject()
                                       .value(libnoisemaker::JsText(u"value")).toDouble(-1);
        check(noneIdx == 0.0, "palette.none == 0 (positional index, verified against share/palettes.json)");
        check(solarisIdx == 42.0, "palette.solaris == 42 (positional index, verified against share/palettes.json)");
    }

    // ==================================================================
    // effect aliases: hidden + deprecatedBy
    // ==================================================================
    {
        check(reg.checkEffectAlias(libnoisemaker::JsText(u"filter.adjust")).isEmpty(),
              "checkEffectAlias('filter.adjust') (no alias) returns empty");
        check(reg.checkEffectAlias(libnoisemaker::JsText(u"synth.noise")).isEmpty(),
              "checkEffectAlias('synth.noise') (no alias) returns empty");
    }

    // ==================================================================
    // define-map: globals carrying `define`
    // ==================================================================
    {
        const libnoisemaker::JsObject dm = reg.defineMap();
        const libnoisemaker::JsValue noiseDefs = dm.value(libnoisemaker::JsText(u"synth.noise"));
        check(noiseDefs.isObject() && noiseDefs.toObject().value(libnoisemaker::JsText(u"type")).toString()
                  == libnoisemaker::JsText(u"NOISE_TYPE"),
              "defineMap['synth.noise'].type == 'NOISE_TYPE'");
    }

    // ==================================================================
    // dumpSummary() surface shape (the check_registry.mjs comparison keys)
    // ==================================================================
    {
        const libnoisemaker::JsObject dump = reg.dumpSummary();
        check(dump.contains(libnoisemaker::JsText(u"ops")) && dump.contains(libnoisemaker::JsText(u"enums"))
                  && dump.contains(libnoisemaker::JsText(u"effectAliases"))
                  && dump.contains(libnoisemaker::JsText(u"effectKeys")) && dump.size() == 4,
              "dumpSummary() has exactly the four gate keys");
        check(dump.value(libnoisemaker::JsText(u"ops")).toObject().size() == 210, "dumpSummary().ops has 210 entries");
        const libnoisemaker::JsObject remapArg = dump.value(libnoisemaker::JsText(u"ops")).toObject().value(libnoisemaker::JsText(u"synth.remap"))
                                          .toObject();
        check(!remapArg.isEmpty(), "dumpSummary().ops has a synth.remap entry");
        const libnoisemaker::JsObject adjustArg = dump.value(libnoisemaker::JsText(u"ops")).toObject().value(libnoisemaker::JsText(u"filter.adjust")).toObject();
        bool brightnessHasNoEnumOrChoices = true;
        bool brightnessHasUniformMinMax = false;
        for (const libnoisemaker::JsValue& a : adjustArg.value(libnoisemaker::JsText(u"args")).toArray()) {
            const libnoisemaker::JsObject ao = a.toObject();
            if (ao.value(libnoisemaker::JsText(u"name")).toString() == libnoisemaker::JsText(u"brightness")) {
                brightnessHasNoEnumOrChoices = !ao.contains(libnoisemaker::JsText(u"enum"))
                    && !ao.contains(libnoisemaker::JsText(u"enumPath")) && !ao.contains(libnoisemaker::JsText(u"choices"));
                brightnessHasUniformMinMax = ao.contains(libnoisemaker::JsText(u"uniform")) && ao.contains(libnoisemaker::JsText(u"min"))
                    && ao.contains(libnoisemaker::JsText(u"max"));
            }
        }
        check(brightnessHasNoEnumOrChoices,
              "filter.adjust's brightness arg omits enum/enumPath/choices entirely (absent, not null)");
        check(brightnessHasUniformMinMax, "filter.adjust's brightness arg carries uniform/min/max (all present)");
        check(dump.value(libnoisemaker::JsText(u"effectKeys")).toObject().value(libnoisemaker::JsText(u"synth.noise")).toString()
                  == libnoisemaker::JsText(u"synth.noise"),
              "effectKeys['synth.noise'] fingerprints to 'synth.noise'");
    }

    if (g_failures == 0) {
        std::printf("ALL PASS (test_registry)\n");
        return 0;
    }
    std::printf("%d FAILURE(S) (test_registry)\n", g_failures);
    return 1;
}
