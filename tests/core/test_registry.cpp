// Unit tests for nm::EffectRegistry (qt/noisemaker/compiler/effect_registry.{h,cpp}).
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
// to link ("undefined symbols: nm::EffectRegistry::..."). GREEN: all
// checks below print PASS and the process exits 0.

#include "core/lang/effect_registry.h"


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

} // namespace

int main() {
    nm::EffectRegistry reg;
    reg.loadEmbedded();

    // ==================================================================
    // load sanity
    // ==================================================================
    {
        check(reg.getOp(nm::JsText(u"synth.noise")) != nullptr, "synth.noise op is registered");
        check(reg.getOp(nm::JsText(u"filter.adjust")) != nullptr, "filter.adjust op is registered");
        check(reg.hasEffect(nm::JsText(u"synth.noise")), "synth.noise effect lookup key present");
        check(reg.hasEffect(nm::JsText(u"synth/noise")), "synth/noise (slash) effect lookup key present");
        check(reg.hasEffect(nm::JsText(u"noise")), "bare 'noise' effect lookup key present (some effect wins it)");
    }

    // ==================================================================
    // Ops use namespaced keys; the reference registers bare starter names.
    // ==================================================================
    {
        check(reg.getOp(nm::JsText(u"noise")) == nullptr, "bare 'noise' is NOT a valid getOp() key");
        check(reg.getOp(nm::JsText(u"solid")) == nullptr, "bare 'solid' is NOT a valid getOp() key");
        check(reg.isStarterOp(nm::JsText(u"solid")), "bare 'solid' is a registered starter");
        check(reg.isStarterOp(nm::JsText(u"synth.solid")), "isStarterOp('synth.solid') (namespaced) is true");
    }

    // ==================================================================
    // starter derivation: pass-input rule, NOT the baked JSON `starter`
    // field (the one verified disagreement in the whole catalog)
    // ==================================================================
    {
        check(reg.isStarterOp(nm::JsText(u"mixer.channelCombine")),
              "mixer.channelCombine IS a starter by the pass-rule (its JSON `starter` field is false; "
              "it takes inputs via surface kwargs, not pipeline `inputs`)");
    }

    // ==================================================================
    // vec4 -> "color" registration-time rewrite (op arg `type`); the JSON
    // itself keeps "vec4" verbatim (never hand-edited/rewritten at rest)
    // ==================================================================
    {
        const nm::OpSpec* remap = reg.getOp(nm::JsText(u"synth.remap"));
        check(remap != nullptr, "synth.remap op is registered");
        bool foundZoneVert = false;
        bool typeIsColor = false;
        if (remap) {
            for (const nm::ParamDef& pd : remap->args) {
                if (pd.name == nm::JsText(u"zone0_v0")) {
                    foundZoneVert = true;
                    typeIsColor = (pd.type == nm::JsText(u"color"));
                }
            }
        }
        check(foundZoneVert, "synth.remap has a zone0_v0 global");
        check(typeIsColor, "synth.remap zone0_v0 arg type is 'color' (registry rewrote vec4->color)");
    }

    // ==================================================================
    // args positional order matches globals DECLARATION order (Qt's
    // nm::JsObject does NOT preserve parse order -- verified empirically:
    // parsing {"zebra":1,"apple":2,"mango":3,"banana":4} and iterating
    // yields alphabetical order, not source order -- so this exercises
    // the text-scanning key-order recovery directly)
    // ==================================================================
    {
        const nm::OpSpec* noise = reg.getOp(nm::JsText(u"synth.noise"));
        check(noise != nullptr, "synth.noise op is registered (for order check)");
        if (noise) {
            // synth/noise.json's globals declaration order (see the file):
            // type, octaves, scaleX, scaleY, seed, wrap, ridges,
            // loopOffset, loopScale, speed, colorMode -- alphabetical would
            // be colorMode, loopOffset, loopScale, octaves, ridges, ...
            const nm::JsList expected = {
                nm::JsText(u"type"),   nm::JsText(u"octaves"),    nm::JsText(u"scaleX"),
                nm::JsText(u"scaleY"), nm::JsText(u"seed"),       nm::JsText(u"wrap"),
                nm::JsText(u"ridges"), nm::JsText(u"loopOffset"), nm::JsText(u"loopScale"),
                nm::JsText(u"speed"),  nm::JsText(u"colorMode"),
            };
            bool orderMatches = (noise->args.size() == expected.size());
            if (orderMatches) {
                for (int i = 0; i < expected.size(); ++i) {
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
        const nm::OpSpec* noise = reg.getOp(nm::JsText(u"synth.noise"));
        check(noise != nullptr, "synth.noise op is registered (for min/max check)");
        if (noise) {
            const nm::ParamDef* octaves = nullptr;
            for (const nm::ParamDef& pd : noise->args) {
                if (pd.name == nm::JsText(u"octaves")) octaves = &pd;
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
        const nm::OpSpec* noise = reg.getOp(nm::JsText(u"synth.noise"));
        const nm::ParamDef* type = nullptr;
        if (noise) {
            for (const nm::ParamDef& pd : noise->args) {
                if (pd.name == nm::JsText(u"type")) type = &pd;
            }
        }
        check(type != nullptr, "synth.noise has a 'type' arg");
        if (type) {
            check(type->hasEnumPath() && type->enumPathString() == nm::JsText(u"synth.noise.type"),
                  "synth.noise.type synthesizes enumPath 'synth.noise.type'");
            check(type->hasChoices(), "synth.noise.type arg carries verbatim choices for the ops dump");
        }
        const nm::JsValue simplex = reg.enums().tryGetHead(nm::JsText(u"synth"));
        bool resolvesTo10 = false;
        if (simplex.isObject()) {
            const nm::JsValue noiseNs = simplex.toObject().value(nm::JsText(u"noise"));
            if (noiseNs.isObject()) {
                const nm::JsValue typeNs = noiseNs.toObject().value(nm::JsText(u"type"));
                if (typeNs.isObject()) {
                    const nm::JsValue simplexLeaf = typeNs.toObject().value(nm::JsText(u"simplex"));
                    resolvesTo10 = simplexLeaf.isObject()
                        && simplexLeaf.toObject().value(nm::JsText(u"value")).toDouble() == 10.0;
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
        const nm::OpSpec* noise = reg.getOp(nm::JsText(u"synth.noise"));
        const nm::ParamDef* loopOffset = nullptr;
        if (noise) {
            for (const nm::ParamDef& pd : noise->args) {
                if (pd.name == nm::JsText(u"loopOffset")) loopOffset = &pd;
            }
        }
        check(loopOffset != nullptr, "synth.noise has a 'loopOffset' arg");
        if (loopOffset) {
            check(loopOffset->hasChoices() && loopOffset->choicesObject().contains(nm::JsText(u"Shapes:")),
                  "loopOffset's verbatim choices still carry the 'Shapes:' group header");
        }
        const nm::JsValue synthNs = reg.enums().tryGetHead(nm::JsText(u"synth"));
        bool groupHeaderNotRegistered = true;
        if (synthNs.isObject()) {
            const nm::JsValue noiseNs = synthNs.toObject().value(nm::JsText(u"noise"));
            if (noiseNs.isObject()) {
                const nm::JsValue loopOffsetNs = noiseNs.toObject().value(nm::JsText(u"loopOffset"));
                if (loopOffsetNs.isObject()) {
                    groupHeaderNotRegistered = !loopOffsetNs.toObject().contains(nm::JsText(u"Shapes:"));
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
        const nm::JsValue oscKind = reg.enums().std().value(nm::JsText(u"oscKind"));
        check(oscKind.isObject(), "std().oscKind is a subtree");
        const double noiseVal = oscKind.toObject().value(nm::JsText(u"noise")).toObject()
                                     .value(nm::JsText(u"value")).toDouble(-1);
        const double noise1dVal = oscKind.toObject().value(nm::JsText(u"noise1d")).toObject()
                                       .value(nm::JsText(u"value")).toDouble(-2);
        check(noiseVal == 5.0 && noise1dVal == 5.0, "oscKind.noise == oscKind.noise1d == 5 (alias)");

        const nm::JsValue audioBand = reg.enums().std().value(nm::JsText(u"audioBand"));
        const double rawVal = audioBand.toObject().value(nm::JsText(u"raw")).toObject()
                                  .value(nm::JsText(u"value")).toDouble(-1);
        check(rawVal == 4.0, "audioBand.raw == 4");

        const nm::JsValue palette = reg.enums().std().value(nm::JsText(u"palette"));
        check(palette.isObject(), "std().palette is a subtree");
        const double noneIdx = palette.toObject().value(nm::JsText(u"none")).toObject()
                                    .value(nm::JsText(u"value")).toDouble(-1);
        const double solarisIdx = palette.toObject().value(nm::JsText(u"solaris")).toObject()
                                       .value(nm::JsText(u"value")).toDouble(-1);
        check(noneIdx == 0.0, "palette.none == 0 (positional index, verified against share/palettes.json)");
        check(solarisIdx == 42.0, "palette.solaris == 42 (positional index, verified against share/palettes.json)");
    }

    // ==================================================================
    // paramAliases: non-empty maps only (empty maps are inert)
    // ==================================================================
    {
        nm::JsObject kwargs;
        kwargs.insert(nm::JsText(u"noiseType"), 3.0);
        const nm::JsList warnings = reg.resolveParamAliases(nm::JsText(u"synth.noise"), kwargs);
        check(warnings.size() == 1, "resolveParamAliases produces one warning for one deprecated kwarg");
        check(!kwargs.contains(nm::JsText(u"noiseType")) && kwargs.contains(nm::JsText(u"type"))
                  && kwargs.value(nm::JsText(u"type")).toDouble() == 3.0,
              "resolveParamAliases renames noiseType -> type in place, preserving the value");

        nm::JsObject emptyAliasKwargs;
        emptyAliasKwargs.insert(nm::JsText(u"brightness"), 1.0);
        const nm::JsList noWarnings = reg.resolveParamAliases(nm::JsText(u"filter.adjust"), emptyAliasKwargs);
        check(noWarnings.isEmpty(), "an effect with an EMPTY paramAliases map ({}) produces no warnings");
    }

    // ==================================================================
    // effect aliases: hidden + deprecatedBy
    // ==================================================================
    {
        check(reg.checkEffectAlias(nm::JsText(u"filter.adjust")).isEmpty(),
              "checkEffectAlias('filter.adjust') (no alias) returns empty");
        check(reg.checkEffectAlias(nm::JsText(u"synth.noise")).isEmpty(),
              "checkEffectAlias('synth.noise') (no alias) returns empty");
    }

    // ==================================================================
    // define-map: globals carrying `define`
    // ==================================================================
    {
        const nm::JsObject dm = reg.defineMap();
        const nm::JsValue noiseDefs = dm.value(nm::JsText(u"synth.noise"));
        check(noiseDefs.isObject() && noiseDefs.toObject().value(nm::JsText(u"type")).toString()
                  == nm::JsText(u"NOISE_TYPE"),
              "defineMap['synth.noise'].type == 'NOISE_TYPE'");
    }

    // ==================================================================
    // dumpSummary() surface shape (the check_registry.mjs comparison keys)
    // ==================================================================
    {
        const nm::JsObject dump = reg.dumpSummary();
        check(dump.contains(nm::JsText(u"ops")) && dump.contains(nm::JsText(u"enums"))
                  && dump.contains(nm::JsText(u"paramAliases")) && dump.contains(nm::JsText(u"effectAliases"))
                  && dump.contains(nm::JsText(u"effectKeys")),
              "dumpSummary() has exactly the five gate keys");
        check(dump.value(nm::JsText(u"ops")).toObject().size() == 210, "dumpSummary().ops has 210 entries");
        const nm::JsObject remapArg = dump.value(nm::JsText(u"ops")).toObject().value(nm::JsText(u"synth.remap"))
                                          .toObject();
        check(!remapArg.isEmpty(), "dumpSummary().ops has a synth.remap entry");
        const nm::JsObject adjustArg = dump.value(nm::JsText(u"ops")).toObject().value(nm::JsText(u"filter.adjust")).toObject();
        bool brightnessHasNoEnumOrChoices = true;
        bool brightnessHasUniformMinMax = false;
        for (const nm::JsValue& a : adjustArg.value(nm::JsText(u"args")).toArray()) {
            const nm::JsObject ao = a.toObject();
            if (ao.value(nm::JsText(u"name")).toString() == nm::JsText(u"brightness")) {
                brightnessHasNoEnumOrChoices = !ao.contains(nm::JsText(u"enum"))
                    && !ao.contains(nm::JsText(u"enumPath")) && !ao.contains(nm::JsText(u"choices"));
                brightnessHasUniformMinMax = ao.contains(nm::JsText(u"uniform")) && ao.contains(nm::JsText(u"min"))
                    && ao.contains(nm::JsText(u"max"));
            }
        }
        check(brightnessHasNoEnumOrChoices,
              "filter.adjust's brightness arg omits enum/enumPath/choices entirely (absent, not null)");
        check(brightnessHasUniformMinMax, "filter.adjust's brightness arg carries uniform/min/max (all present)");
        check(dump.value(nm::JsText(u"effectKeys")).toObject().value(nm::JsText(u"synth.noise")).toString()
                  == nm::JsText(u"synth.noise"),
              "effectKeys['synth.noise'] fingerprints to 'synth.noise'");
    }

    if (g_failures == 0) {
        std::printf("ALL PASS (test_registry)\n");
        return 0;
    }
    std::printf("%d FAILURE(S) (test_registry)\n", g_failures);
    return 1;
}
