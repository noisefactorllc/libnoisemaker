// Unit tests for nm::validateEffectDefinition (qt/noisemaker/compiler/
// effect_validator.{h,cpp}): the port of shaders/src/runtime/effect-validator.js
// validateEffectDefinition() (upstream commits ba87ffae + 9d3474df).
// Plain assert-style checks, no test framework dependency.
//
// Cases mirror shaders/tests/test_effect_definition_validation.js, minus the
// shapes the port's JSON grammar cannot represent (Effect instances and
// subclass constructors are documented deviations in effect_validator.h).
// The upstream corpus walk (every tracked definition validates cleanly) is
// mirrored here over the port's generated definition JSONs under
// qt/noisemaker/effects/ — the port's shape contract (starter key accepted,
// hooks absent, builtinMeshes array form) applies to all of them.

#include "core/lang/effect_validator.h"
#include "core/catalog/catalog.h"
#include "core/value/json.h"


#include <cmath>
#include <fstream>
#include <sstream>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

namespace {

using nm::validateEffectDefinition;

int g_failures = 0;

std::vector<std::string> validate(const nm::JsObject& def) {
    return validateEffectDefinition(def);
}

nm::JsObject parseDefinition(const char* json, const char* source) {
    try { return nm::JsValue(nm::json::parse(json)).toObject(); } catch (const nm::json::ParseError&) {
        std::printf("FAIL: %s is not valid JSON\n", source);
        ++g_failures;
    }
    return {};
}

// A complete, valid plain-object definition exercising the schema surface
// (the reference test's validDefinition()).
nm::JsObject validDefinition() {
    return parseDefinition(R"json({
    "name": "Validator Probe",
    "namespace": "synth",
    "func": "validatorProbe",
    "description": "Used by the definition-validator tests",
    "tags": ["noise", "util"],
    "openCategories": ["general"],
    "defaultProgram": "search synth\nvalidatorProbe().write(o0)",
    "hidden": false,
    "uniformLayout": {
        "resolution": { "slot": 0, "components": "xy" },
        "time": { "slot": 0, "components": "z" }
    },
    "uniformLayouts": {
        "alt": { "amount": { "slot": 0, "components": "x" } }
    },
    "paramAliases": { "amt": "amount" },
    "textures": {
        "scratch": { "width": 64, "height": "100%", "format": "rgba16f" },
        "scaled": { "width": { "param": "volumeSize", "power": 2, "default": 1024 }, "height": { "screenDivide": "zoom", "default": 8 } }
    },
    "globals": {
        "amount": {
            "type": "float", "default": 0.5, "uniform": "amount",
            "min": 0, "max": 1, "step": 0.01, "zero": 0,
            "ui": { "label": "amount", "control": "slider", "category": "effect" }
        },
        "mode": {
            "type": "int", "default": 1, "uniform": "mode",
            "define": "PROBE_MODE",
            "choices": { "off": 0, "on": 1, "Group:": null },
            "ui": { "label": "mode", "control": "dropdown", "enabledBy": "amount" }
        },
        "flag": {
            "type": "boolean", "default": true, "uniform": "flag",
            "ui": { "label": "flag", "control": "checkbox", "enabledBy": { "param": "mode", "eq": 1 } }
        },
        "tint": {
            "type": "color", "default": [1, 0, 0], "uniform": "tint",
            "ui": { "label": "tint", "control": "color" }
        },
        "point": {
            "type": "vec3", "default": [0, 0, 0], "uniform": "point",
            "min": [-1, -1, -1], "max": [1, 1, 1],
            "ui": { "label": "point", "control": "vector3", "format": "x/y/z" }
        },
        "table": {
            "type": "int", "default": 7,
            "choices": { "a": 7, "b": 9 },
            "ui": { "label": "table", "control": "dropdown", "hidden": true }
        },
        "surfaceIn": {
            "type": "surface", "default": "none",
            "colorModeUniform": "surfaceActive",
            "ui": { "label": "surface", "control": false }
        }
    },
    "passes": [
        {
            "name": "render",
            "program": "probe",
            "type": "compute",
            "drawMode": "points",
            "count": "input",
            "countUniform": "mode",
            "repeat": 2,
            "blend": ["ONE", "ONE_MINUS_SRC_ALPHA"],
            "drawBuffers": 2,
            "workgroups": [8, 8, 1],
            "viewport": { "width": { "param": "volumeSize", "paramDefault": 64 }, "height": 32 },
            "conditions": {
                "runIf": [{ "uniform": "mode", "equals": 1 }],
                "skipIf": [{ "uniform": "flag", "equals": false }]
            },
            "uniforms": { "amount": "amount", "literal": 3 },
            "inputs": { "srcTex": "inputTex", "scratchTex": "scratch", "paramTex": "surfaceIn" },
            "outputs": { "fragColor": "outputTex" }
        }
    ]
})json",
                              "validDefinition");
}

void checkValid(const nm::JsObject& def, const char* source) {
    const std::vector<std::string> errors = validate(def);
    if (!errors.empty()) {
        std::printf("FAIL: %s expected [], got:", source);
        for (const std::string& e : errors) std::printf(" [%s]", e.c_str());
        std::printf("\n");
        ++g_failures;
    } else {
        std::printf("PASS: %s -> []\n", source);
    }
}

void checkAnyError(const nm::JsObject& def, const char* source) {
    const std::vector<std::string> errors = validate(def);
    if (errors.empty()) {
        std::printf("FAIL: %s expected >=1 error, got []\n", source);
        ++g_failures;
    } else {
        std::printf("PASS: %s -> %zu error(s)\n", source, errors.size());
    }
}

void checkErrors(const nm::JsObject& def, size_t expected, const char* source) {
    const std::vector<std::string> errors = validate(def);
    if (errors.size() != expected) {
        std::printf("FAIL: %s expected %zu errors, got %zu:", source, expected, errors.size());
        for (const std::string& e : errors) std::printf(" [%s]", e.c_str());
        std::printf("\n");
        ++g_failures;
    } else {
        std::printf("PASS: %s -> %zu error(s)\n", source, errors.size());
    }
}

void checkMessage(const nm::JsObject& def, const char* substring, const char* source) {
    const std::vector<std::string> errors = validate(def);
    for (const std::string& e : errors) {
        if (e.find(substring) != std::string::npos) {
            std::printf("PASS: %s -> %s\n", source, e.c_str());
            return;
        }
    }
    std::printf("FAIL: %s expected an error containing \"%s\", got:", source, substring);
    for (const std::string& e : errors) std::printf(" [%s]", e.c_str());
    std::printf("\n");
    ++g_failures;
}

void checkMessageCount(const nm::JsObject& def, const char* substring, size_t expected,
                       const char* source) {
    size_t count = 0;
    for (const std::string& e : validate(def)) {
        if (e.find(substring) != std::string::npos) ++count;
    }
    if (count != expected) {
        std::printf("FAIL: %s expected %zu occurrences of \"%s\", got %zu\n", source, expected,
                    substring, count);
        ++g_failures;
    } else {
        std::printf("PASS: %s\n", source);
    }
}

// Mutators that must produce at least one error (the reference test's
// malformed-container, type-constraint, and unsupported-reference cases).

nm::JsObject getObject(nm::JsObject& host, const nm::JsText& key) {
    return host.value(key).toObject();
}

// Merge a key into a nested object, replacing it wholesale.
template <typename T>
void nest(nm::JsObject& def, const nm::JsText& container, const nm::JsText& key,
          const T& value) {
    nm::JsObject host = def.value(container).toObject();
    host.insert(key, value);
    def.insert(container, host);
}

void nestInGlobals(nm::JsObject& def, const nm::JsText& global, const nm::JsValue& spec) {
    nm::JsObject globals = def.value("globals").toObject();
    globals.insert(global, spec);
    def.insert("globals", globals);
}

void mutateFirstPass(nm::JsObject& def, const nm::JsText& key, const nm::JsValue& value) {
    nm::JsArray passes = def.value("passes").toArray();
    nm::JsObject pass = passes.at(0).toObject();
    pass.insert(key, value);
    passes.replace(0, pass);
    def.insert("passes", passes);
}

void mutateFirstPassNested(nm::JsObject& def, const nm::JsText& container, const nm::JsText& key,
                           const nm::JsValue& value) {
    nm::JsArray passes = def.value("passes").toArray();
    nm::JsObject pass = passes.at(0).toObject();
    nm::JsObject host = pass.value(container).toObject();
    host.insert(key, value);
    pass.insert(container, host);
    passes.replace(0, pass);
    def.insert("passes", passes);
}

} // namespace

int main() {
    // --- null and non-object containers (exact reference messages) ---
    // null/undefined: the port receives them as a nm::JsValue, so exercise via
    // validateEffectDefinition(nm::JsValue) directly.
    {
        const std::vector<std::string> errors =
            nm::validateEffectDefinition(nm::JsValue(nm::JsValue::Null));
        const bool ok = errors.size() == 1 &&
                        errors.at(0) == "Effect definition is null or undefined";
        std::printf("%s: null -> exact message\n", ok ? "PASS" : "FAIL");
        if (!ok) ++g_failures;
    }
    {
        const std::vector<std::string> errors = nm::validateEffectDefinition(nm::JsArray());
        const bool ok = errors.size() == 1 &&
                        errors.at(0) ==
                            "Effect definition must be a plain object or Effect instance, "
                            "not an array";
        std::printf("%s: array -> exact message\n", ok ? "PASS" : "FAIL");
        if (!ok) ++g_failures;
    }
    for (const nm::JsValue& bad : {nm::JsValue("string"), nm::JsValue(42), nm::JsValue(true)}) {
        const std::vector<std::string> errors = nm::validateEffectDefinition(bad);
        const bool ok = !errors.empty();
        std::printf("%s: primitive diagnosed\n", ok ? "PASS" : "FAIL");
        if (!ok) ++g_failures;
    }

    // --- valid plain-object definition returns no errors ---
    checkValid(validDefinition(), "valid plain-object definition");

    // --- unknown top-level declarative fields are diagnosed ---
    {
        nm::JsObject def = validDefinition();
        def.insert("globalz", def.value("globals"));
        checkMessageCount(def, "globalz", 1, "unknown top-level field diagnosed");
    }

    // --- unknown nested fields are diagnosed ---
    {
        nm::JsObject def = validDefinition();
        nm::JsObject globals = getObject(def, "globals");
        nm::JsObject amount = getObject(globals, "amount");
        amount.insert("unkown", 1);
        globals.insert("amount", amount);
        def.insert("globals", globals);
        checkMessageCount(def, "unkown", 1, "unknown global-spec field diagnosed");
    }
    {
        nm::JsObject def = validDefinition();
        nm::JsObject globals = getObject(def, "globals");
        nm::JsObject mode = getObject(globals, "mode");
        nm::JsObject ui = getObject(mode, "ui");
        ui.insert("colour", "red");
        mode.insert("ui", ui);
        globals.insert("mode", mode);
        def.insert("globals", globals);
        checkMessageCount(def, "colour", 1, "unknown ui field diagnosed");
    }
    {
        nm::JsObject def = validDefinition();
        mutateFirstPass(def, "progam", "typo");
        checkMessageCount(def, "progam", 1, "unknown pass field diagnosed");
    }
    // --- ui.resetOnChange is accepted as a boolean and diagnosed otherwise
    // (reference 3c1e47e5, upstream test_effect_definition_validation.js) ---
    {
        nm::JsObject def = validDefinition();
        nm::JsObject globals = getObject(def, "globals");
        nm::JsObject mode = getObject(globals, "mode");
        nm::JsObject ui = getObject(mode, "ui");
        ui.insert("resetOnChange", true);
        mode.insert("ui", ui);
        globals.insert("mode", mode);
        def.insert("globals", globals);
        checkValid(def, "ui.resetOnChange true accepted");
    }
    {
        nm::JsObject def = validDefinition();
        nm::JsObject globals = getObject(def, "globals");
        nm::JsObject mode = getObject(globals, "mode");
        nm::JsObject ui = getObject(mode, "ui");
        ui.insert("resetOnChange", "yes");
        mode.insert("ui", ui);
        globals.insert("mode", mode);
        def.insert("globals", globals);
        checkMessageCount(def, "resetOnChange", 1, "non-boolean ui.resetOnChange diagnosed");
    }
    {
        nm::JsObject def = validDefinition();
        nm::JsObject textures = getObject(def, "textures");
        nm::JsObject scratch = getObject(textures, "scratch");
        scratch.insert("widht", 8);
        textures.insert("scratch", scratch);
        def.insert("textures", textures);
        checkMessageCount(def, "widht", 1, "unknown texture-spec field diagnosed");
    }

    // --- malformed containers are reported without throwing ---
    {
        struct Case {
            const char* name;
            void (*mutate)(nm::JsObject&);
        };
        const Case cases[] = {
            {"globals is an array", [](nm::JsObject& d) { d.insert("globals", nm::JsArray{"nope"}); }},
            {"globals is a string", [](nm::JsObject& d) { d.insert("globals", "nope"); }},
            {"globals entry null", [](nm::JsObject& d) {
                 nm::JsObject g; g.insert("g", nm::JsValue());
                 d.insert("globals", g);
             }},
            {"globals entry array", [](nm::JsObject& d) {
                 nm::JsObject g; g.insert("g", nm::JsArray());
                 d.insert("globals", g);
             }},
            {"ui is a string", [](nm::JsObject& d) {
                 nestInGlobals(d, "g", nm::JsObject{{"type", "float"}, {"default", 0.5},
                                                   {"ui", "slider"}});
             }},
            {"passes is an object", [](nm::JsObject& d) {
                 nm::JsObject p; p.insert("program", "x");
                 d.insert("passes", p);
             }},
            {"passes contains null", [](nm::JsObject& d) {
                 nm::JsArray a; a.append(nm::JsValue());
                 d.insert("passes", a);
             }},
            {"inputs is a string", [](nm::JsObject& d) {
                 nm::JsArray a; nm::JsObject p; p.insert("program", "p"); p.insert("inputs", "x");
                 a.append(p); d.insert("passes", a);
             }},
            {"outputs is a number", [](nm::JsObject& d) {
                 nm::JsArray a; nm::JsObject p; p.insert("program", "p"); p.insert("outputs", 3);
                 a.append(p); d.insert("passes", a);
             }},
            {"uniforms is a string", [](nm::JsObject& d) {
                 nm::JsArray a; nm::JsObject p; p.insert("program", "p"); p.insert("uniforms", "x");
                 a.append(p); d.insert("passes", a);
             }},
            {"conditions is a string", [](nm::JsObject& d) {
                 nm::JsArray a; nm::JsObject p; p.insert("program", "p");
                 p.insert("conditions", "always");
                 a.append(p); d.insert("passes", a);
             }},
            {"runIf entry null", [](nm::JsObject& d) {
                 nm::JsArray a; nm::JsObject p; p.insert("program", "p");
                 nm::JsObject c; nm::JsArray r; r.append(nm::JsValue());
                 c.insert("runIf", r); p.insert("conditions", c);
                 a.append(p); d.insert("passes", a);
             }},
            {"condition uniform is a number", [](nm::JsObject& d) {
                 nm::JsArray a; nm::JsObject p; p.insert("program", "p");
                 nm::JsObject c; nm::JsArray r; nm::JsObject cond; cond.insert("uniform", 7);
                 r.append(cond); c.insert("runIf", r); p.insert("conditions", c);
                 a.append(p); d.insert("passes", a);
             }},
            {"condition missing equals", [](nm::JsObject& d) {
                 nm::JsArray a; nm::JsObject p; p.insert("program", "p");
                 nm::JsObject c; nm::JsArray r; nm::JsObject cond; cond.insert("uniform", "mode");
                 r.append(cond); c.insert("runIf", r); p.insert("conditions", c);
                 a.append(p); d.insert("passes", a);
             }},
            {"textures is an array", [](nm::JsObject& d) {
                 nm::JsArray t; t.append("x"); d.insert("textures", t);
             }},
            {"texture spec is a string", [](nm::JsObject& d) {
                 nm::JsObject t; t.insert("t", "big"); d.insert("textures", t);
             }},
            {"texture width is a word", [](nm::JsObject& d) {
                 nm::JsObject t; nm::JsObject s; s.insert("width", "banana");
                 t.insert("t", s); d.insert("textures", t);
             }},
            {"texture width param is a number", [](nm::JsObject& d) {
                 nm::JsObject t; nm::JsObject s; nm::JsObject w; w.insert("param", 42);
                 s.insert("width", w); t.insert("t", s); d.insert("textures", t);
             }},
            {"texture width has an unknown field", [](nm::JsObject& d) {
                 nm::JsObject t; nm::JsObject s; nm::JsObject w; w.insert("wrong", 1);
                 s.insert("width", w); t.insert("t", s); d.insert("textures", t);
             }},
            {"texture width is negative", [](nm::JsObject& d) {
                 nm::JsObject t; nm::JsObject s; s.insert("width", -4);
                 t.insert("t", s); d.insert("textures", t);
             }},
            {"texture format is unknown", [](nm::JsObject& d) {
                 nm::JsObject t; nm::JsObject s; s.insert("format", "rgba999");
                 t.insert("t", s); d.insert("textures", t);
             }},
            {"uniformLayout slot is a string", [](nm::JsObject& d) {
                 nm::JsObject l; nm::JsObject u; u.insert("slot", "x"); u.insert("components", "x");
                 l.insert("u", u); d.insert("uniformLayout", l);
             }},
            {"uniformLayout components are out of the grammar", [](nm::JsObject& d) {
                 nm::JsObject l; nm::JsObject u; u.insert("slot", 0);
                 u.insert("components", "xyzwq");
                 l.insert("u", u); d.insert("uniformLayout", l);
             }},
            {"uniformLayout components are out of order", [](nm::JsObject& d) {
                 nm::JsObject l; nm::JsObject u; u.insert("slot", 0); u.insert("components", "zy");
                 l.insert("u", u); d.insert("uniformLayout", l);
             }},
            {"uniformLayout slot is fractional", [](nm::JsObject& d) {
                 nm::JsObject l; nm::JsObject u; u.insert("slot", 0.5); u.insert("components", "x");
                 l.insert("u", u); d.insert("uniformLayout", l);
             }},
            {"uniformLayout conflicts at one slot", [](nm::JsObject& d) {
                 nm::JsObject l;
                 nm::JsObject u; u.insert("slot", 0); u.insert("components", "x");
                 nm::JsObject v; v.insert("slot", 0); v.insert("components", "y");
                 nm::JsObject w; w.insert("slot", 0); w.insert("components", "x");
                 l.insert("u", u); l.insert("v", v); l.insert("w", w);
                 d.insert("uniformLayout", l);
             }},
            {"uniformLayouts value is a string", [](nm::JsObject& d) {
                 nm::JsObject ls; ls.insert("p", "layout"); d.insert("uniformLayouts", ls);
             }},
            {"paramAliases is a string", [](nm::JsObject& d) { d.insert("paramAliases", "aliases"); }},
            {"paramAliases target is unknown", [](nm::JsObject& d) {
                 nm::JsObject a; a.insert("amt", "nosuchglobal"); d.insert("paramAliases", a);
             }},
            {"tags is a string", [](nm::JsObject& d) { d.insert("tags", "noise"); }},
            {"tags contains an unknown tag", [](nm::JsObject& d) {
                 nm::JsArray t; t.append("not-a-tag"); d.insert("tags", t);
             }},
            {"tags contains a number", [](nm::JsObject& d) {
                 nm::JsArray t; t.append(42); d.insert("tags", t);
             }},
            {"openCategories contains a number", [](nm::JsObject& d) {
                 nm::JsArray t; t.append(7); d.insert("openCategories", t);
             }},
            {"onInit present (JSON cannot encode functions)", [](nm::JsObject& d) {
                 d.insert("onInit", "nope");
             }},
            {"onUpdate present (JSON cannot encode functions)", [](nm::JsObject& d) {
                 d.insert("onUpdate", 42);
             }},
            {"onDestroy present (JSON cannot encode functions)", [](nm::JsObject& d) {
                 d.insert("onDestroy", nm::JsObject());
             }},
            {"asyncInit present (JSON cannot encode functions)", [](nm::JsObject& d) {
                 d.insert("asyncInit", true);
             }},
            {"shaders is a string", [](nm::JsObject& d) { d.insert("shaders", "inline"); }},
            {"shaders value is a string", [](nm::JsObject& d) {
                 nm::JsObject s; s.insert("main", "source"); d.insert("shaders", s);
             }},
            {"deprecatedBy is a number", [](nm::JsObject& d) { d.insert("deprecatedBy", 5); }},
            {"externalTexture is a number", [](nm::JsObject& d) { d.insert("externalTexture", 7); }},
        };
        for (const Case& c : cases) {
            nm::JsObject def = validDefinition();
            c.mutate(def);
            checkAnyError(def, c.name);
        }
    }

    // --- global spec type and primitive constraints are enforced ---
    {
        struct Case {
            const char* name;
            nm::JsText global;
            nm::JsText key;
            nm::JsValue value;
        };
        const Case cases[] = {
            {"float type is vec9", "amount", "type", "vec9"},
            {"float type is a number", "amount", "type", 3},
            {"float default is a string", "amount", "default", "high"},
            {"int default is fractional", "mode", "default", 1.5},
            {"int default is a string", "mode", "default", "one"},
            {"boolean default is a number", "flag", "default", 1},
            {"color default is short", "tint", "default", nm::JsArray{1, 0}},
            {"color default has a string", "tint", "default", nm::JsArray{1, 0, "a"}},
            {"vec3 default is short", "point", "default", nm::JsArray{0, 0}},
            {"vec3 default is out of min/max", "point", "default", nm::JsArray{0, 0, 2}},
            {"vec3 min is short", "point", "min", nm::JsArray{-1, -1}},
            {"vec3 max is a number", "point", "max", 1},
            {"float min is a string", "amount", "min", "low"},
            {"float min exceeds max", "amount", "min", 0.9},
            {"float max is below min", "amount", "max", 0.4},
            {"float step is a string", "amount", "step", "x"},
            {"choices is an array", "mode", "choices", nm::JsArray{0, 1}},
            {"choices value is a string", "mode", "choices", nm::JsObject{{"bad", "x"}}},
            {"int default not among choices", "mode", "default", 5},
            {"dropdown default not among choices", "table", "default", 8},
            {"colorModeUniform is a number", "surfaceIn", "colorModeUniform", 3},
            {"uniform is a number", "amount", "uniform", 9},
            {"define is a number", "mode", "define", 5},
        };
        for (const Case& c : cases) {
            nm::JsObject def = validDefinition();
            nm::JsObject globals = getObject(def, "globals");
            nm::JsObject spec = getObject(globals, c.global);
            spec.insert(c.key, c.value);
            globals.insert(c.global, spec);
            def.insert("globals", globals);
            checkAnyError(def, c.name);
        }
    }
    // NaN and Infinity defaults (JSON cannot carry them; nm::JsValue can hold
    // the doubles, which is how callers passing runtime-computed values
    // exercise the reference's !isFinite branch).
    for (const double bad : {std::nan(""), std::numeric_limits<double>::infinity()}) {
        nm::JsObject def = validDefinition();
        nm::JsObject globals = getObject(def, "globals");
        nm::JsObject amount = getObject(globals, "amount");
        amount.insert("default", bad);
        globals.insert("amount", amount);
        def.insert("globals", globals);
        checkAnyError(def, "float default is NaN/Infinity");
    }
    {
        nm::JsObject def = validDefinition();
        nm::JsObject globals = getObject(def, "globals");
        nm::JsObject amount = getObject(globals, "amount");
        amount.insert("max", std::numeric_limits<double>::infinity());
        globals.insert("amount", amount);
        def.insert("globals", globals);
        checkAnyError(def, "float max is Infinity");
    }

    // --- member-typed globals must resolve through the std enum tables ---
    {
        nm::JsObject spec{
            {"type", "member"}, {"default", "noSuchTable.member"}, {"enum", "noSuchTable"},
            {"ui", nm::JsObject{{"label", "member"}, {"control", "dropdown"}}}};
        nm::JsObject def = validDefinition();
        nestInGlobals(def, "memberProbe", spec);
        checkMessage(def, "noSuchTable", "member enum path unresolved");
    }
    {
        nm::JsObject spec{
            {"type", "member"}, {"default", "oscType.sine"}, {"enum", "oscType"},
            {"ui", nm::JsObject{{"label", "member"}, {"control", "dropdown"}}}};
        nm::JsObject def = validDefinition();
        nestInGlobals(def, "memberProbe", spec);
        checkValid(def, "member enum resolves through std tables");
    }

    // --- duplicate and conflicting bindings and layouts are diagnosed ---
    {
        nm::JsObject def = validDefinition();
        nestInGlobals(def, "other",
                      nm::JsObject{{"type", "float"}, {"default", 0}, {"uniform", "amount"}});
        checkMessage(def, "amount", "duplicate uniform binding diagnosed");
    }

    // --- template interpolation of non-string values follows JS String(value) ---
    {
        nm::JsObject def = validDefinition();
        mutateFirstPass(def, "type", 5);
        const std::string want = "Pass 0: unknown pass type '5'";
        const std::vector<std::string> errors = validate(def);
        bool ok = false;
        for (const std::string& e : errors) {
            if (e == want) ok = true;
        }
        std::printf("%s: non-string pass type interpolates as String(5)\n", ok ? "PASS" : "FAIL");
        if (!ok) {
            for (const std::string& e : errors) std::printf("  [%s]\n", e.c_str());
            ++g_failures;
        }
    }
    {
        nm::JsObject def = validDefinition();
        mutateFirstPass(def, "drawMode", true);
        const std::string want = "Pass 0: unknown drawMode 'true'";
        const std::vector<std::string> errors = validate(def);
        bool ok = false;
        for (const std::string& e : errors) {
            if (e == want) ok = true;
        }
        std::printf("%s: non-string drawMode interpolates as String(true)\n", ok ? "PASS" : "FAIL");
        if (!ok) {
            for (const std::string& e : errors) std::printf("  [%s]\n", e.c_str());
            ++g_failures;
        }
    }
    {
        nm::JsObject def = validDefinition();
        nm::JsObject t; nm::JsObject s; s.insert("format", 1);
        t.insert("t", s); def.insert("textures", t);
        const std::string want = "Texture 't': unknown format '1'";
        const std::vector<std::string> errors = validate(def);
        bool ok = false;
        for (const std::string& e : errors) {
            if (e == want) ok = true;
        }
        std::printf("%s: non-string texture format interpolates as String(1)\n", ok ? "PASS" : "FAIL");
        if (!ok) {
            for (const std::string& e : errors) std::printf("  [%s]\n", e.c_str());
            ++g_failures;
        }
    }
    {
        // The reference emits paramAliases['<alias>'] with BOTH quotes.
        nm::JsObject def = validDefinition();
        nm::JsObject a; a.insert("amt", "nosuchglobal"); def.insert("paramAliases", a);
        const std::string want = "paramAliases['amt'] references unknown global 'nosuchglobal'";
        const std::vector<std::string> errors = validate(def);
        bool ok = false;
        for (const std::string& e : errors) {
            if (e == want) ok = true;
        }
        std::printf("%s: paramAliases unknown-global message is exact\n", ok ? "PASS" : "FAIL");
        if (!ok) {
            for (const std::string& e : errors) std::printf("  [%s]\n", e.c_str());
            ++g_failures;
        }
    }
    {
        nm::JsObject def = validDefinition();
        nm::JsObject l;
        nm::JsObject a; a.insert("slot", 1); a.insert("components", "x");
        nm::JsObject b; b.insert("slot", 1); b.insert("components", "xy");
        l.insert("a", a); l.insert("b", b);
        def.insert("uniformLayout", l);
        checkMessage(def, "slot 1", "overlapping layout diagnosed");
    }
    {
        nm::JsObject def = validDefinition();
        nm::JsObject l;
        nm::JsObject a; a.insert("slot", 1); a.insert("components", "x");
        nm::JsObject b; b.insert("slot", 1); b.insert("components", "x");
        l.insert("a", a); l.insert("b", b);
        def.insert("uniformLayout", l);
        checkAnyError(def, "duplicate layout entries diagnosed");
    }

    // --- unsupported binding references are diagnosed ---
    {
        struct Case {
            const char* name;
            void (*mutate)(nm::JsObject&);
        };
        const Case cases[] = {
            {"pass input is a number", [](nm::JsObject& d) { mutateFirstPassNested(d, "inputs", "bad", 7); }},
            {"pass input references an undeclared texture", [](nm::JsObject& d) { mutateFirstPassNested(d, "inputs", "bad", "notDeclaredAnywhere"); }},
            {"pass output references an undeclared texture", [](nm::JsObject& d) { mutateFirstPassNested(d, "outputs", "bad", "notDeclaredAnywhere"); }},
            {"pass uniform is an array", [](nm::JsObject& d) { mutateFirstPassNested(d, "uniforms", "bad", nm::JsArray()); }},
            {"pass uniform is an object", [](nm::JsObject& d) { mutateFirstPassNested(d, "uniforms", "bad", nm::JsObject{{"ref", 1}}); }},
            {"condition uniform is undeclared", [](nm::JsObject& d) { mutateFirstPassNested(d, "conditions", "runIf", nm::JsArray{nm::JsObject{{"uniform", "nosuch"}, {"equals", 1}}}); }},
            {"countUniform is undeclared", [](nm::JsObject& d) { mutateFirstPass(d, "countUniform", "nosuch"); }},
            {"count is a word", [](nm::JsObject& d) { mutateFirstPass(d, "count", "banana"); }},
            {"count is negative", [](nm::JsObject& d) { mutateFirstPass(d, "count", -1); }},
            {"repeat is an array", [](nm::JsObject& d) { mutateFirstPass(d, "repeat", nm::JsArray()); }},
            {"drawMode is unknown", [](nm::JsObject& d) { mutateFirstPass(d, "drawMode", "hexagons"); }},
            {"type is unknown", [](nm::JsObject& d) { mutateFirstPass(d, "type", "vertex"); }},
            {"drawBuffers is zero", [](nm::JsObject& d) { mutateFirstPass(d, "drawBuffers", 0); }},
            {"blend is a string", [](nm::JsObject& d) { mutateFirstPass(d, "blend", "on"); }},
            {"blend has one factor", [](nm::JsObject& d) { mutateFirstPass(d, "blend", nm::JsArray{"ONE"}); }},
            {"workgroups is a string", [](nm::JsObject& d) { mutateFirstPass(d, "workgroups", "8"); }},
            {"viewport is a number", [](nm::JsObject& d) { mutateFirstPass(d, "viewport", 32); }},
            {"entryPoint is a number", [](nm::JsObject& d) { mutateFirstPass(d, "entryPoint", 7); }},
            {"enabledBy param is undeclared", [](nm::JsObject& d) {
                 nm::JsObject globals = getObject(d, "globals");
                 nm::JsObject flag = getObject(globals, "flag");
                 nm::JsObject ui = getObject(flag, "ui");
                 ui.insert("enabledBy", nm::JsObject{{"param", "nosuch"}, {"eq", 1}});
                 flag.insert("ui", ui);
                 globals.insert("flag", flag);
                 d.insert("globals", globals);
             }},
            {"enabledBy is an undeclared name", [](nm::JsObject& d) {
                 nm::JsObject globals = getObject(d, "globals");
                 nm::JsObject flag = getObject(globals, "flag");
                 nm::JsObject ui = getObject(flag, "ui");
                 ui.insert("enabledBy", "nosuch");
                 flag.insert("ui", ui);
                 globals.insert("flag", flag);
                 d.insert("globals", globals);
             }},
            {"enabledBy has no eq", [](nm::JsObject& d) {
                 nm::JsObject globals = getObject(d, "globals");
                 nm::JsObject flag = getObject(globals, "flag");
                 nm::JsObject ui = getObject(flag, "ui");
                 ui.insert("enabledBy", nm::JsObject{{"param", "mode"}});
                 flag.insert("ui", ui);
                 globals.insert("flag", flag);
                 d.insert("globals", globals);
             }},
            {"enabledBy has an unknown container", [](nm::JsObject& d) {
                 nm::JsObject globals = getObject(d, "globals");
                 nm::JsObject flag = getObject(globals, "flag");
                 nm::JsObject ui = getObject(flag, "ui");
                 ui.insert("enabledBy", nm::JsObject{{"and", "x"}});
                 flag.insert("ui", ui);
                 globals.insert("flag", flag);
                 d.insert("globals", globals);
             }},
            {"ui control is unknown", [](nm::JsObject& d) {
                 nm::JsObject globals = getObject(d, "globals");
                 nm::JsObject mode = getObject(globals, "mode");
                 nm::JsObject ui = getObject(mode, "ui");
                 ui.insert("control", "dropdownx");
                 mode.insert("ui", ui);
                 globals.insert("mode", mode);
                 d.insert("globals", globals);
             }},
            {"ui label is a number", [](nm::JsObject& d) {
                 nm::JsObject globals = getObject(d, "globals");
                 nm::JsObject mode = getObject(globals, "mode");
                 nm::JsObject ui = getObject(mode, "ui");
                 ui.insert("label", 7);
                 mode.insert("ui", ui);
                 globals.insert("mode", mode);
                 d.insert("globals", globals);
             }},
        };
        for (const Case& c : cases) {
            nm::JsObject def = validDefinition();
            c.mutate(def);
            checkAnyError(def, c.name);
        }
    }

    // --- numeric literals and supported dimension expressions are preserved ---
    {
        nm::JsObject def = validDefinition();
        mutateFirstPassNested(def, "uniforms", "literal", 0);
        mutateFirstPassNested(def, "uniforms", "another", 12.5);
        checkValid(def, "numeric pass-uniform literals preserved");
    }
    {
        nm::JsObject def = validDefinition();
        nm::JsObject textures = getObject(def, "textures");
        nm::JsObject expressions;
        nm::JsObject width;
        width.insert("scale", 0.5);
        width.insert("clamp", nm::JsObject{{"min", 8}, {"max", 256}});
        nm::JsObject height;
        height.insert("param", "volumeSize");
        height.insert("multiply", 2);
        height.insert("paramDefault", 64);
        expressions.insert("width", width);
        expressions.insert("height", height);
        textures.insert("expressions", expressions);
        def.insert("textures", textures);
        checkValid(def, "supported dimension expressions preserved");
    }

    // --- lifecycle hooks: JSON cannot encode functions, so a present hook is
    // reported (documented deviation; the reference's valid case is
    // unreachable in this grammar) ---
    {
        nm::JsObject def = validDefinition();
        def.insert("onInit", 1);
        def.insert("onUpdate", "x");
        def.insert("onDestroy", nm::JsObject());
        def.insert("asyncInit", true);
        checkErrors(def, 4, "four present hooks each reported");
    }

    // --- validation errors follow declaration order ---
    {
        nm::JsObject def = validDefinition();
        nm::JsObject globals;
        nm::JsObject zzz; zzz.insert("type", "float"); zzz.insert("default", "bad");
        nm::JsObject aaa; aaa.insert("type", "float"); aaa.insert("default", "bad");
        globals.insert("zzz", zzz);
        globals.insert("aaa", aaa);
        def.insert("globals", globals);
        const std::vector<std::string> errors = validate(def);
        ptrdiff_t z = -1, a = -1;
        for (size_t i = 0; i < errors.size(); ++i) {
            if (errors[i].find("'zzz'") != std::string::npos) z = ptrdiff_t(i);
            if (errors[i].find("'aaa'") != std::string::npos) a = ptrdiff_t(i);
        }
        const bool ok = z != -1 && a != -1 && z < a;
        std::printf("%s: both global errors reported in declaration order\n",
                    ok ? "PASS" : "FAIL");
        if (!ok) ++g_failures;
    }

    // Differential probes from the pinned reference.
    {
        std::ifstream input("parity/effect-validator-corpus.json");
        if (!input) {
            std::printf("FAIL: differential corpus missing\n");
            ++g_failures;
        } else {
            std::ostringstream bytes;
            bytes << input.rdbuf();
            const nm::JsObject corpus = nm::JsValue(nm::json::parse(bytes.str())).toObject();
            const nm::JsArray probes = corpus.value(u"probes").toArray();
            const nm::JsText commit = corpus.value(u"provenance").toObject().value(u"referenceCommit").toString();
            int mismatches = 0;
            for (const nm::JsValue& probeValue : probes) {
                const nm::JsObject probe = probeValue.toObject();
                const nm::JsObject def = probe.value(u"def").toObject();
                nm::JsList want;
                for (const nm::JsValue& e : probe.value(u"referenceErrors").toArray()) want.append(e.toString());
                const std::vector<std::string> got = validateEffectDefinition(def);
                nm::JsList actual;
                for (const std::string& e : got) actual.append(nm::JsText::fromUtf8(e));
                if (want != actual) {
                    ++mismatches;
                    std::printf("FAIL: corpus probe '%s': want %s; got %s\n",
                                probe.value(u"id").toString().toUtf8().c_str(),
                                want.join(u'|').toUtf8().c_str(), actual.join(u'|').toUtf8().c_str());
                }
            }
            std::printf("[differential] probes=%zu mismatch=%d reference=%s\n",
                        probes.size(), mismatches, commit.toUtf8().c_str());
            if (probes.empty() || commit.empty() || mismatches) ++g_failures;
        }
    }

    // Every embedded definition's reference-authored fields must validate.
    {
        int expected = 0, passed = 0;
        for (const auto& file : nm::catalog_files()) {
            if (!file.path.starts_with("effects/") || !file.path.ends_with(".json")) continue;
            ++expected;
            try {
                nm::Value definition = nm::json::parse(file.bytes);
                // The converter adds these metadata fields after the
                // reference definition grammar is applied.
                definition.as_object().erase(u"starter");
                definition.as_object().erase(u"jsHooks");
                definition.as_object().erase(u"sourceNamespace");
                const auto errors = validateEffectDefinition(definition);
                if (errors.empty()) ++passed;
                else {
                    std::printf("FAIL: %.*s: %zu validation errors\n",
                                int(file.path.size()), file.path.data(), errors.size());
                    ++g_failures;
                }
            } catch (const nm::json::ParseError&) {
                std::printf("FAIL: %.*s: invalid JSON\n", int(file.path.size()), file.path.data());
                ++g_failures;
            }
        }
        std::printf("[definition corpus] expected=%d pass=%d\n", expected, passed);
        if (expected != 210 || passed != expected) ++g_failures;
    }

    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASS" : "FAILURES", g_failures,
                g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
