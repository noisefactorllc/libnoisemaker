// effect_validator.cpp -- port of the REFERENCE
// shaders/src/runtime/effect-validator.js validateEffectDefinition()
// (upstream commits ba87ffae + 9d3474df, byte-for-byte in behavior
// for every shape representable in the port's JSON definition grammar; see
// effect_validator.h for the documented deviations).
//
// Error strings are byte-identical to the reference's template literals.
// Numbers inside messages format through nm::js::numberToString (ECMAScript
// Number::toString), matching the reference's `${number}` interpolation.
// Object keys retain declaration order, as the reference's Object.entries()
// does.
//
// Deterministic and side-effect-free for definition data: it never mutates
// the input, invokes lifecycle hooks, or sorts runtime globals. String
// coercion can throw the reference's object-to-primitive TypeError.

#include "effect_validator.h"

#include "enums.h"
#include "core/value/js_number.h"


#include <cmath>
#include <map>
#include <set>
#include <vector>

namespace nm {

namespace {

using Errors = std::vector<std::string>;

// --- tag inventory (shaders/src/runtime/tags.js VALID_TAGS, in declaration
// order) ---
const JsList& validTags() {
    static const JsList tags = {
        JsText(u"color"),  JsText(u"distort"),      JsText(u"edges"),
        JsText(u"geometric"), JsText(u"lens"),      JsText(u"noise"),
        JsText(u"transform"), JsText(u"util"),      JsText(u"sim"),
        JsText(u"3d"),     JsText(u"audio"),        JsText(u"agents"),
        JsText(u"antialiasing"), JsText(u"artist"), JsText(u"blend"),
        JsText(u"blur"),   JsText(u"fractal"),      JsText(u"geometry"),
        JsText(u"glitch"), JsText(u"image"),        JsText(u"mesh"),
        JsText(u"midi"),   JsText(u"palette"),      JsText(u"pattern"),
        JsText(u"pixel"),  JsText(u"text"),         JsText(u"tiling"),
        JsText(u"video"),
    };
    return tags;
}

const JsList& globalTypes() {
    static const JsList types = {
        JsText(u"float"), JsText(u"int"),     JsText(u"boolean"),
        JsText(u"vec2"),  JsText(u"vec3"),   JsText(u"vec4"),
        JsText(u"mat3"),  JsText(u"color"),  JsText(u"surface"),
        JsText(u"volume"), JsText(u"geometry"), JsText(u"member"),
        JsText(u"palette"), JsText(u"button"), JsText(u"string"),
    };
    return types;
}

const JsList& uiControls() {
    static const JsList controls = {
        JsText(u"slider"), JsText(u"checkbox"), JsText(u"dropdown"),
        JsText(u"color"),  JsText(u"button"),   JsText(u"vector3"),
        JsText(u"vec3"),
    };
    return controls;
}

const JsList& uiKeys() {
    static const JsList keys = {
        JsText(u"label"), JsText(u"control"),   JsText(u"category"),
        JsText(u"hidden"), JsText(u"hint"),    JsText(u"format"),
        JsText(u"buttonLabel"), JsText(u"enabledBy"), JsText(u"multiline"),
        JsText(u"resetOnChange"),
    };
    return keys;
}

const JsList& enabledByOps() {
    static const JsList ops = {
        JsText(u"eq"), JsText(u"neq"), JsText(u"lt"),
        JsText(u"gt"), JsText(u"gte"), JsText(u"lte"),
        JsText(u"in"), JsText(u"notIn"),
    };
    return ops;
}

const JsList& globalSpecKeys() {
    static const JsList keys = {
        JsText(u"type"), JsText(u"default"), JsText(u"uniform"),
        JsText(u"define"), JsText(u"choices"), JsText(u"enum"),
        JsText(u"min"), JsText(u"max"), JsText(u"step"),
        JsText(u"zero"), JsText(u"randMin"), JsText(u"randMax"),
        JsText(u"randChance"), JsText(u"randChoices"),
        JsText(u"colorModeUniform"), JsText(u"ui"),
    };
    return keys;
}

const JsList& passKeys() {
    static const JsList keys = {
        JsText(u"name"), JsText(u"program"), JsText(u"type"),
        JsText(u"entryPoint"), JsText(u"drawMode"), JsText(u"drawBuffers"),
        JsText(u"count"), JsText(u"countUniform"), JsText(u"repeat"),
        JsText(u"blend"), JsText(u"workgroups"),
        JsText(u"storageBuffers"), JsText(u"storageTextures"),
        JsText(u"viewport"), JsText(u"conditions"),
        JsText(u"defines"), JsText(u"uniforms"),
        JsText(u"inputs"), JsText(u"outputs"),
        JsText(u"clear"), JsText(u"samplerTypes"),
    };
    return keys;
}

// Sampler names the WebGPU backend creates; pass.samplerTypes picks one per sampler.
const JsList& samplerTypes() {
    static const JsList types = {
        JsText(u"default"), JsText(u"nearest"), JsText(u"repeat"),
        JsText(u"mipmap"),
    };
    return types;
}

const JsList& textureSpecKeys() {
    static const JsList keys = {
        JsText(u"width"), JsText(u"height"), JsText(u"depth"),
        JsText(u"format"), JsText(u"is3D"),
        // Authorable texture policies (reference 2f47612c).
        JsText(u"filter"), JsText(u"mipmaps"), JsText(u"persistent"),
    };
    return keys;
}

const JsList& textureFilters() {
    static const JsList filters = { JsText(u"nearest"), JsText(u"linear") };
    return filters;
}

const JsList& conditionContainerKeys() {
    static const JsList keys = { JsText(u"runIf"), JsText(u"skipIf") };
    return keys;
}

const JsList& dimKeywords() {
    static const JsList keywords = {
        JsText(u"screen"), JsText(u"auto"),
        JsText(u"input"),  JsText(u"resolution"),
    };
    return keywords;
}

const JsList& formats() {
    static const JsList formats = {
        JsText(u"rgba16f"), JsText(u"rgba16float"), JsText(u"rgba8"),
        JsText(u"rgba8unorm"), JsText(u"rgba32f"), JsText(u"rgba32float"),
        JsText(u"r8"), JsText(u"r8unorm"), JsText(u"r16f"),
        JsText(u"r16float"), JsText(u"r32f"), JsText(u"r32float"),
    };
    return formats;
}

const JsList& drawModes() {
    static const JsList modes = {
        JsText(u"points"), JsText(u"triangles"), JsText(u"billboards"),
    };
    return modes;
}

const JsList& passTypes() {
    static const JsList types = { JsText(u"render"), JsText(u"compute") };
    return types;
}

const JsList& layoutEntryKeys() {
    static const JsList keys = {
        JsText(u"name"), JsText(u"slot"), JsText(u"components"),
    };
    return keys;
}

const JsList& byteLayoutKeys() {
    static const JsList keys = {
        JsText(u"name"), JsText(u"offset"),
        JsText(u"size"), JsText(u"type"),
    };
    return keys;
}

const JsList& dimSpecKeys() {
    static const JsList keys = {
        JsText(u"param"), JsText(u"power"), JsText(u"multiply"),
        JsText(u"default"), JsText(u"paramDefault"),
        JsText(u"screenDivide"), JsText(u"scale"), JsText(u"clamp"),
        JsText(u"inputOverride"),
    };
    return keys;
}

// Top-level grammar keys (reference TOP_LEVEL_KEYS) plus catalog metadata.
const JsList& topLevelKeys() {
    static const JsList keys = {
        JsText(u"name"), JsText(u"namespace"), JsText(u"func"),
        JsText(u"description"), JsText(u"tags"), JsText(u"globals"),
        JsText(u"passes"), JsText(u"textures"), JsText(u"textures3d"),
        JsText(u"shaders"), JsText(u"uniformLayout"), JsText(u"uniformLayouts"),
        JsText(u"paramAliases"), JsText(u"openCategories"),
        JsText(u"defaultProgram"), JsText(u"hidden"), JsText(u"deprecatedBy"),
        JsText(u"externalTexture"), JsText(u"externalMesh"),
        JsText(u"builtinMeshes"), JsText(u"outputTex3d"), JsText(u"outputGeo"),
        JsText(u"state"), JsText(u"uniforms"),
        JsText(u"onInit"), JsText(u"onUpdate"), JsText(u"onDestroy"),
        JsText(u"asyncInit"),
    };
    return keys;
}

const JsList& pipelineInputs() {
    static const JsList inputs = {
        JsText(u"inputTex"), JsText(u"inputTex3d"), JsText(u"inputGeo"),
        JsText(u"inputXyz"), JsText(u"inputVel"),  JsText(u"inputRgba"),
        JsText(u"noise"),    JsText(u"midiNoteGrid"), JsText(u"feedback"),
        JsText(u"selfTex"),  JsText(u"outputTex"), JsText(u"none"),
    };
    return inputs;
}

const JsList& pipelineOutputs() {
    static const JsList outputs = {
        JsText(u"outputTex"), JsText(u"outputTex3d"), JsText(u"outputXyz"),
        JsText(u"outputVel"), JsText(u"outputRgba"),
    };
    return outputs;
}

// Semantic component order consumed by the backends' uniform packing
// ({x: 0, y: 1, z: 2, w: 3}); ASCII codes do not follow xyzw order.
int componentOrder(char16_t c) {
    switch (c) {
        case 'x': return 0;
        case 'y': return 1;
        case 'z': return 2;
        case 'w': return 3;
        default:  return -1;
    }
}

bool isFiniteNumber(const JsValue& value) {
    return value.isDouble() && std::isfinite(value.toDouble());
}

// Number.isInteger: a finite double with an integral value.
bool isJsInteger(const JsValue& value) {
    if (!value.isDouble()) return false;
    const double d = value.toDouble();
    return std::isfinite(d) && d == std::floor(d);
}

bool isNonEmptyString(const JsValue& value) {
    return value.isString() && !value.toString().isEmpty();
}

// The reference's `!def` falsy check, including NaN.
bool isFalsy(const JsValue& value) {
    switch (value.type()) {
        case JsValue::Undefined: return true;
        case JsValue::Null:      return true;
        case JsValue::Bool:      return !value.toBool();
        case JsValue::Double:    return value.toDouble() == 0.0 || std::isnan(value.toDouble());
        case JsValue::String:    return value.toString().isEmpty();
        case JsValue::Array:     return false; // [] is truthy
        case JsValue::Object:    return false;
        case JsValue::Function:  return false;
        default:                    return true;
    }
}

// ECMAScript `typeof` name for a Value, including tagged function inputs.
const char* jsTypeOf(const JsValue& value) {
    switch (value.type()) {
        case JsValue::Array:   return "object"; // typeof [] is "object"
        case JsValue::Bool:    return "boolean";
        case JsValue::Double:  return "number";
        case JsValue::String:  return "string";
        case JsValue::Function: return "function";
        case JsValue::Object:  return "object";
        case JsValue::Null:    return "object"; // typeof null is "object"
        case JsValue::Undefined: return "undefined";
        default:                  return "unknown";
    }
}

std::string toStd(const JsText& s) { return s.toStdString(); }

// Number interpolation: the reference's `${number}` uses String(number).
std::string numToStd(double value) { return toStd(js::numberToString(value)); }

// JS String(value) over a JSON value (the reference's `${expr}` interpolation
// where the operand is not statically known to be a string).
std::string jsStringOf(const JsValue& value) {
    return toStd(value.toVariant());
}

// --- std enum resolution (reference resolveStdEnum over std_enums.js) ---

// Injectable std-enum tree, stored BY VALUE (setEffectValidatorStdEnums
// copies its argument, so no caller-object lifetime is retained). Defaults
// to the live nm::Enums::std() table until overridden (the reference
// imports std_enums.js directly).
JsObject g_stdEnumsOverride;

const JsObject& stdEnumTable() {
    static const JsObject defaultTable = Enums().std();
    return g_stdEnumsOverride.isEmpty() ? defaultTable : g_stdEnumsOverride;
}

struct StdEnumResult {
    enum Kind { None, Leaf, Table } kind = None;
};

// Walks the dotted path through the std enum tree. Leaves carry a `value` key
// exactly like the reference's {type:'Number', value} entries (the port's
// Enums::leaf() shape, see enums.h).
StdEnumResult resolveStdEnum(const JsText& pathStr) {
    StdEnumResult result;
    if (pathStr.isEmpty()) return result;
    // The reference indexes a normal JS object, so its inherited prototype
    // is itself a table for this specific path.
    if (pathStr == JsText(u"__proto__")) {
        result.kind = StdEnumResult::Table;
        return result;
    }
    JsValue node(stdEnumTable());
    const JsList parts = pathStr.split(u'.', false);
    for (const JsText& part : parts) {
        if (!node.isObject()) return result;
        const JsObject nodeObj = node.toObject();
        if (!nodeObj.contains(part)) return result;
        node = nodeObj.value(part);
    }
    if (node.isObject() && node.toObject().contains(JsText(u"value"))) {
        result.kind = StdEnumResult::Leaf;
        return result;
    }
    if (node.isObject()) {
        result.kind = StdEnumResult::Table;
        return result;
    }
    return result;
}

// --- context (declared globals + their uniform names) ---

struct ValidatorContext {
    JsSet<JsText> globalKeys;
    JsSet<JsText> globalUniformNames;
};

bool referencesGlobal(const JsText& name, const ValidatorContext& context) {
    return context.globalKeys.contains(name) || context.globalUniformNames.contains(name);
}

// Forward declarations for the mutually recursive ui/enabledBy pair.
void validateEnabledBy(const JsValue& cond, Errors& errors, const std::string& label,
                       const ValidatorContext& context);
void validateUi(const JsValue& uiValue, Errors& errors, const std::string& label,
                const ValidatorContext& context);

// --- validateDimSpec: reference lines 117-200 ---
void validateDimSpec(const JsValue& spec, Errors& errors, const std::string& label) {
    if (spec.isDouble()) {
        if (!isFiniteNumber(spec) || spec.toDouble() <= 0) {
            errors.push_back(label + ": dimension must be a positive finite number, keyword, percentage, or dimension expression");
        }
        return;
    }
    if (spec.isString()) {
        const JsText s = spec.toString();
        if (dimKeywords().contains(s)) return;
        // PERCENT_PATTERN ^[\d.]+$ on the head before '%'; parseFloat over the
        // whole spec then re-checks finiteness/positivity.
        if (s.endsWith(u'%')) {
            const JsText head = s.left(s.size() - 1);
            bool percentPattern = !head.isEmpty();
            for (const char16_t c : head) {
                if (!(c >= u'0' && c <= u'9') && c != u'.') { percentPattern = false; break; }
            }
            if (percentPattern) {
                // parseFloat(s) stops at the '%'; the head matched ^[\d.]+$, so
                // parseFloat parses the longest numeric prefix of the head
                // (e.g. "0.5.3%" -> 0.5, NaN cases can't occur here).
                double percent = 0;
                bool ok = false;
                for (int cut = head.size(); cut > 0 && !ok; --cut) {
                    percent = head.left(cut).toDouble(&ok);
                }
                if (!ok || !std::isfinite(percent) || percent <= 0) {
                    errors.push_back(label + ": invalid percentage '" + toStd(s) + "'");
                }
                return;
            }
        }
        errors.push_back(label + ": invalid dimension '" + toStd(s) + "'");
        return;
    }
    if (spec.isObject()) {
        const JsObject specObj = spec.toObject();
        for (const JsText& key : specObj.keys()) {
            if (!dimSpecKeys().contains(key)) {
                errors.push_back(label + ": unknown dimension field '" + toStd(key) + "'");
            }
        }
        if (!specObj.value(JsText(u"param")).isUndefined()) {
            const JsValue param = specObj.value(JsText(u"param"));
            if (!isNonEmptyString(param)) {
                errors.push_back(label + ": \"param\" must be a non-empty string");
            }
            for (const char* field : {"power", "multiply", "default", "paramDefault"}) {
                const JsValue v = specObj.value(JsText::fromLatin1(field));
                if (!v.isUndefined() && !isFiniteNumber(v)) {
                    errors.push_back(label + ": \"" + field + "\" must be a finite number");
                }
            }
            const JsValue inputOverride = specObj.value(JsText(u"inputOverride"));
            if (!inputOverride.isUndefined() && !isNonEmptyString(inputOverride)) {
                errors.push_back(label + ": \"inputOverride\" must be a non-empty string");
            }
            return;
        }
        if (!specObj.value(JsText(u"screenDivide")).isUndefined()) {
            const JsValue screenDivide = specObj.value(JsText(u"screenDivide"));
            if (!isNonEmptyString(screenDivide)) {
                errors.push_back(label + ": \"screenDivide\" must be a non-empty string");
            }
            const JsValue dflt = specObj.value(JsText(u"default"));
            if (!dflt.isUndefined() && !isFiniteNumber(dflt)) {
                errors.push_back(label + ": \"default\" must be a finite number");
            }
            return;
        }
        if (!specObj.value(JsText(u"scale")).isUndefined()) {
            const JsValue scale = specObj.value(JsText(u"scale"));
            if (!isFiniteNumber(scale)) {
                errors.push_back(label + ": \"scale\" must be a finite number");
            }
            const JsValue clamp = specObj.value(JsText(u"clamp"));
            if (!clamp.isUndefined()) {
                if (!clamp.isObject()) {
                    errors.push_back(label + ": \"clamp\" must be an object");
                } else {
                    const JsObject clampObj = clamp.toObject();
                    const JsValue clampMin = clampObj.value(JsText(u"min"));
                    if (!clampMin.isUndefined() && !isFiniteNumber(clampMin)) {
                        errors.push_back(label + ": \"clamp.min\" must be a finite number");
                    }
                    const JsValue clampMax = clampObj.value(JsText(u"max"));
                    if (!clampMax.isUndefined() && !isFiniteNumber(clampMax)) {
                        errors.push_back(label + ": \"clamp.max\" must be a finite number");
                    }
                    for (const JsText& key : clampObj.keys()) {
                        if (key != JsText(u"min") && key != JsText(u"max")) {
                            errors.push_back(label + ": unknown clamp field '" + toStd(key) + "'");
                        }
                    }
                }
            }
            return;
        }
        errors.push_back(label + ": dimension object must reference \"param\", \"screenDivide\", or \"scale\"");
        return;
    }
    errors.push_back(label + ": invalid dimension specification");
}

// --- validateLayoutEntry: reference lines 285-311 ---
void validateLayoutEntry(const JsObject& entry, Errors& errors, const std::string& label) {
    for (const JsText& key : entry.keys()) {
        if (!layoutEntryKeys().contains(key)) {
            errors.push_back(label + ": unknown field '" + toStd(key) + "'");
        }
    }
    const JsValue name = entry.value(JsText(u"name"));
    if (!isNonEmptyString(name)) {
        errors.push_back(label + ": missing \"name\" string");
    }
    const JsValue slot = entry.value(JsText(u"slot"));
    if (!isJsInteger(slot) || slot.toDouble() < 0) {
        errors.push_back(label + ": \"slot\" must be a non-negative integer");
    }
    const JsValue components = entry.value(JsText(u"components"));
    if (!components.isString()) {
        errors.push_back(label + ": \"components\" must be 1-4 characters from xyzw");
        return;
    }
    const JsText cs = components.toString();
    // ^[xyzw]{1,4}$
    if (cs.size() < 1 || cs.size() > 4) {
        errors.push_back(label + ": \"components\" must be 1-4 characters from xyzw");
        return;
    }
    for (const char16_t c : cs) {
        if (componentOrder(c) < 0) {
            errors.push_back(label + ": \"components\" must be 1-4 characters from xyzw");
            return;
        }
    }
    for (int i = 1; i < cs.size(); ++i) {
        if (componentOrder(cs.at(i)) <= componentOrder(cs.at(i - 1))) {
            errors.push_back(label + ": \"components\" '" + toStd(cs) + "' must be in ascending xyzw order");
            break;
        }
    }
}

// A normalized slot/components claim used by checkLayoutConflicts.
struct SlotClaim {
    JsValue name;
    int slot = 0;
    JsText components;
};

void checkLayoutConflicts(const std::vector<SlotClaim>& entries, Errors& errors,
                          const std::string& label) {
    for (size_t i = 0; i < entries.size(); ++i) {
        for (size_t j = i + 1; j < entries.size(); ++j) {
            const SlotClaim& a = entries[i];
            const SlotClaim& b = entries[j];
            if (a.slot != b.slot) continue;
            bool overlap = false;
            for (const char16_t c : a.components) {
                if (b.components.contains(c)) { overlap = true; break; }
            }
            if (a.components == b.components) {
                errors.push_back(label + ": duplicate layout entries '" + toStd(a.name.toVariant()) + "' and '"
                                 + toStd(b.name.toVariant()) + "' claim slot " + numToStd(a.slot)
                                 + " components '" + toStd(a.components) + "'");
            } else if (overlap) {
                errors.push_back(label + ": layout conflict at slot " + numToStd(a.slot) + ": '"
                                 + toStd(a.name.toVariant()) + "' (" + toStd(a.components) + ") overlaps '"
                                 + toStd(b.name.toVariant()) + "' (" + toStd(b.components) + ")");
            }
        }
    }
}

// --- validateUniformLayout: reference lines 208-279 ---
void validateUniformLayout(const JsValue& layout, Errors& errors, const std::string& label) {
    if (layout.isArray()) {
        const JsArray arr = layout.toArray();
        std::vector<SlotClaim> entries;
        for (int i = 0; i < arr.size(); ++i) {
            const JsValue entry = arr.at(i);
            const std::string entryLabel = label + "[" + numToStd(i) + "]";
            if (entry.isObject()) {
                const JsObject obj = entry.toObject();
                validateLayoutEntry(obj, errors, entryLabel);
                const JsValue slot = obj.value(JsText(u"slot"));
                if (isJsInteger(slot)) {
                    SlotClaim claim;
                    claim.name = obj.value(JsText(u"name"));
                    claim.slot = static_cast<int>(slot.toDouble());
                    claim.components = obj.value(JsText(u"components")).toString();
                    entries.push_back(claim);
                }
            } else {
                // validateLayoutEntry's non-object message.
                errors.push_back(entryLabel + ": layout entry must be an object");
            }
        }
        checkLayoutConflicts(entries, errors, label);
        return;
    }
    if (!layout.isObject()) {
        errors.push_back(label + ": must be an object or array layout");
        return;
    }
    const JsObject layoutObj = layout.toObject();
    if (layoutObj.value(JsText(u"type")) == JsText(u"byte")) {
        const JsValue inner = layoutObj.value(JsText(u"layout"));
        if (!inner.isArray()) {
            errors.push_back(label + ": byte layout requires a \"layout\" array");
            return;
        }
        for (const JsText& key : layoutObj.keys()) {
            if (key != JsText(u"type") && key != JsText(u"layout")) {
                errors.push_back(label + ": unknown byte-layout field '" + toStd(key) + "'");
            }
        }
        const JsArray arr = inner.toArray();
        for (int i = 0; i < arr.size(); ++i) {
            const JsValue entry = arr.at(i);
            const std::string entryLabel = label + ".layout[" + numToStd(i) + "]";
            if (!entry.isObject()) {
                errors.push_back(entryLabel + ": entry must be an object");
                continue;
            }
            const JsObject obj = entry.toObject();
            for (const JsText& key : obj.keys()) {
                if (!byteLayoutKeys().contains(key) && key != JsText(u"components")) {
                    errors.push_back(entryLabel + ": unknown field '" + toStd(key) + "'");
                }
            }
            const JsValue name = obj.value(JsText(u"name"));
            if (!isNonEmptyString(name)) {
                errors.push_back(entryLabel + ": missing \"name\" string");
            }
            const JsValue offset = obj.value(JsText(u"offset"));
            if (!isJsInteger(offset) || offset.toDouble() < 0) {
                errors.push_back(entryLabel + ": \"offset\" must be a non-negative integer");
            }
            const JsValue size = obj.value(JsText(u"size"));
            if (!isJsInteger(size) || size.toDouble() <= 0) {
                errors.push_back(entryLabel + ": \"size\" must be a positive integer");
            }
            const JsValue type = obj.value(JsText(u"type"));
            if (!isNonEmptyString(type)) {
                errors.push_back(entryLabel + ": missing \"type\" string");
            }
        }
        // Byte-layout duplicate/overlap diagnosis over fully valid entries
        // (reference checkByteLayoutConflicts).
        for (int i = 0; i < arr.size(); ++i) {
            if (!arr.at(i).isObject()) continue;
            const JsObject a = arr.at(i).toObject();
            const JsValue aName = a.value(JsText(u"name"));
            const JsValue aOffset = a.value(JsText(u"offset"));
            const JsValue aSize = a.value(JsText(u"size"));
            if (!isNonEmptyString(aName) || !isJsInteger(aOffset) || aOffset.toDouble() < 0 ||
                !isJsInteger(aSize) || aSize.toDouble() <= 0) continue;
            for (int j = i + 1; j < arr.size(); ++j) {
                if (!arr.at(j).isObject()) continue;
                const JsObject b = arr.at(j).toObject();
                const JsValue bName = b.value(JsText(u"name"));
                const JsValue bOffset = b.value(JsText(u"offset"));
                const JsValue bSize = b.value(JsText(u"size"));
                if (!isNonEmptyString(bName) || !isJsInteger(bOffset) || bOffset.toDouble() < 0 ||
                    !isJsInteger(bSize) || bSize.toDouble() <= 0) continue;
                if (aName.toString() == bName.toString()) {
                    errors.push_back(label + ": duplicate byte-layout entries '" + toStd(aName.toString())
                                     + "' (offsets " + numToStd(aOffset.toDouble()) + " and "
                                     + numToStd(bOffset.toDouble()) + ")");
                    continue;
                }
                const double aStart = aOffset.toDouble();
                const double aEnd = aStart + aSize.toDouble();
                const double bStart = bOffset.toDouble();
                const double bEnd = bStart + bSize.toDouble();
                if (aStart < bEnd && bStart < aEnd) {
                    errors.push_back(label + ": byte layout conflict: '" + toStd(aName.toString())
                                     + "' (offset " + numToStd(aStart) + ", size " + numToStd(aSize.toDouble())
                                     + ") overlaps '" + toStd(bName.toString()) + "' (offset "
                                     + numToStd(bStart) + ", size " + numToStd(bSize.toDouble()) + ")");
                }
            }
        }
        return;
    }
    // Named-key map form: validateLayoutEntry over {name, ...spec}.
    std::vector<SlotClaim> entries;
    for (auto it = layoutObj.begin(); it != layoutObj.end(); ++it) {
        const std::string entryLabel = label + "['" + toStd(it.key()) + "']";
        if (!it.value().isObject()) {
            errors.push_back(entryLabel + ": layout entry must be an object");
            continue;
        }
        JsObject spec;
        spec.insert(JsText(u"name"), it.key());
        const JsObject sourceSpec = it.value().toObject();
        for (auto field = sourceSpec.constBegin(); field != sourceSpec.constEnd(); ++field) {
            spec.insert(field.key(), field.value());
        }
        validateLayoutEntry(spec, errors, entryLabel);
        const JsValue slot = it.value().toObject().value(JsText(u"slot"));
        if (isJsInteger(slot)) {
            SlotClaim claim;
            claim.name = it.key();
            claim.slot = static_cast<int>(slot.toDouble());
            claim.components = it.value().toObject().value(JsText(u"components")).toString();
            entries.push_back(claim);
        }
    }
    checkLayoutConflicts(entries, errors, label);
}

// --- validateEnabledBy: reference lines 355-406 ---
void validateEnabledBy(const JsValue& cond, Errors& errors, const std::string& label,
                       const ValidatorContext& context) {
    if (cond.isString()) {
        const JsText s = cond.toString();
        if (!context.globalKeys.contains(s)) {
            errors.push_back(label + ": enabledBy references unknown global '" + toStd(s) + "'");
        }
        return;
    }
    if (!cond.isObject()) {
        errors.push_back(label + ": \"enabledBy\" must be a global name or condition object");
        return;
    }
    const JsObject obj = cond.toObject();
    if (!obj.value(JsText(u"not")).isUndefined()) {
        for (const JsText& key : obj.keys()) {
            if (key != JsText(u"not")) {
                errors.push_back(label + ": unknown enabledBy field '" + toStd(key) + "'");
            }
        }
        validateEnabledBy(obj.value(JsText(u"not")), errors, label, context);
        return;
    }
    if (!obj.value(JsText(u"and")).isUndefined() ||
        !obj.value(JsText(u"or")).isUndefined()) {
        for (const JsText& key : obj.keys()) {
            if (key != JsText(u"and") && key != JsText(u"or")) {
                errors.push_back(label + ": unknown enabledBy field '" + toStd(key) + "'");
            }
        }
        for (const char* branch : {"and", "or"}) {
            const JsValue v = obj.value(JsText::fromLatin1(branch));
            if (!v.isUndefined()) {
                if (!v.isArray()) {
                    errors.push_back(label + ": \"enabledBy." + branch + "\" must be an array");
                } else {
                    for (const JsValue& sub : v.toArray()) {
                        validateEnabledBy(sub, errors, label, context);
                    }
                }
            }
        }
        return;
    }
    for (const JsText& key : obj.keys()) {
        if (key != JsText(u"param") && !enabledByOps().contains(key)) {
            errors.push_back(label + ": unknown enabledBy field '" + toStd(key) + "'");
        }
    }
    const JsValue param = obj.value(JsText(u"param"));
    if (!isNonEmptyString(param)) {
        errors.push_back(label + ": \"enabledBy\" requires a \"param\" string");
        return;
    }
    if (!context.globalKeys.contains(param.toString())) {
        errors.push_back(label + ": enabledBy references unknown global '" + toStd(param.toString()) + "'");
    }
    bool hasOp = false;
    for (const JsText& op : enabledByOps()) {
        if (!obj.value(op).isUndefined()) { hasOp = true; break; }
    }
    if (!hasOp) {
        errors.push_back(label + ": \"enabledBy\" requires one of eq/neq/lt/gt/in/notIn");
    }
    const JsValue in = obj.value(JsText(u"in"));
    if (!in.isUndefined() && !in.isArray()) {
        errors.push_back(label + ": \"enabledBy.in\" must be an array");
    }
    const JsValue notIn = obj.value(JsText(u"notIn"));
    if (!notIn.isUndefined() && !notIn.isArray()) {
        errors.push_back(label + ": \"enabledBy.notIn\" must be an array");
    }
}

// --- validateUi: reference lines 408-441 ---
void validateUi(const JsValue& uiValue, Errors& errors, const std::string& label,
                const ValidatorContext& context) {
    if (!uiValue.isObject()) {
        errors.push_back(label + ": must be an object");
        return;
    }
    const JsObject ui = uiValue.toObject();
    for (const JsText& key : ui.keys()) {
        if (!uiKeys().contains(key)) {
            errors.push_back(label + ": unknown field '" + toStd(key) + "'");
        }
    }
    const JsValue labelV = ui.value(JsText(u"label"));
    if (!labelV.isUndefined() && !isNonEmptyString(labelV)) {
        errors.push_back(label + ": \"label\" must be a non-empty string");
    }
    const JsValue control = ui.value(JsText(u"control"));
    if (!control.isUndefined() && !(control.isBool() && !control.toBool()) &&
        !uiControls().contains(control.toString())) {
        errors.push_back(label + ": unknown control '" + jsStringOf(control) + "'");
    }
    const JsValue category = ui.value(JsText(u"category"));
    if (!category.isUndefined() && !isNonEmptyString(category)) {
        errors.push_back(label + ": \"category\" must be a non-empty string");
    }
    const JsValue hidden = ui.value(JsText(u"hidden"));
    if (!hidden.isUndefined() && !hidden.isBool()) {
        errors.push_back(label + ": \"hidden\" must be a boolean");
    }
    const JsValue multiline = ui.value(JsText(u"multiline"));
    if (!multiline.isUndefined() && !multiline.isBool()) {
        errors.push_back(label + ": \"multiline\" must be a boolean");
    }
    const JsValue resetOnChange = ui.value(JsText(u"resetOnChange"));
    if (!resetOnChange.isUndefined() && !resetOnChange.isBool()) {
        errors.push_back(label + ": \"resetOnChange\" must be a boolean");
    }
    for (const char* key : {"hint", "format", "buttonLabel"}) {
        const JsValue v = ui.value(JsText::fromLatin1(key));
        if (!v.isUndefined() && !isNonEmptyString(v)) {
            errors.push_back(label + ": \"" + key + "\" must be a non-empty string");
        }
    }
    const JsValue enabledBy = ui.value(JsText(u"enabledBy"));
    if (!enabledBy.isUndefined()) {
        validateEnabledBy(enabledBy, errors, label, context);
    }
}

// HEX_COLOR ^#[0-9a-fA-F]{6}$
bool isHexColor(const JsText& s) {
    if (s.size() != 7 || s.at(0) != u'#') return false;
    for (int i = 1; i < 7; ++i) {
        const char16_t c = s.at(i);
        const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        if (!hex) return false;
    }
    return true;
}

// --- validateDefault: reference lines 443-500 ---
void validateDefault(const JsObject& spec, Errors& errors, const std::string& label) {
    const JsValue typeV = spec.value(JsText(u"type"));
    const JsText type = typeV.isString() ? typeV.toString() : JsText();
    const JsValue value = spec.value(JsText(u"default"));

    if (type == JsText(u"float") || type == JsText(u"palette") ||
        type == JsText(u"button")) {
        if (!isFiniteNumber(value)) {
            errors.push_back(label + ": \"default\" must be a finite number");
        }
    } else if (type == JsText(u"int")) {
        if (!isFiniteNumber(value) || !isJsInteger(value)) {
            errors.push_back(label + ": \"default\" must be a finite integer");
        }
    } else if (type == JsText(u"boolean")) {
        if (!value.isBool()) {
            errors.push_back(label + ": \"default\" must be a boolean");
        }
    } else if (type == JsText(u"vec2") || type == JsText(u"vec3") ||
               type == JsText(u"vec4") || type == JsText(u"mat3")) {
        const int dims = type == JsText(u"vec2") ? 2 : type == JsText(u"vec3") ? 3
                       : type == JsText(u"vec4") ? 4 : 9;
        bool ok = value.isArray();
        if (ok) {
            const JsArray arr = value.toArray();
            ok = arr.size() == dims;
            for (const JsValue& v : arr) {
                if (!isFiniteNumber(v)) { ok = false; break; }
            }
        }
        if (!ok) {
            errors.push_back(label + ": \"default\" must be an array of " + std::to_string(dims)
                             + " finite numbers");
        }
    } else if (type == JsText(u"color")) {
        if (value.isArray()) {
            const JsArray arr = value.toArray();
            bool ok = arr.size() == 3;
            for (const JsValue& v : arr) {
                if (!isFiniteNumber(v)) { ok = false; break; }
            }
            if (!ok) {
                errors.push_back(label + ": \"default\" must be a 3-component color array");
            }
        } else if (!isNonEmptyString(value) || !isHexColor(value.toString())) {
            errors.push_back(label + ": \"default\" must be a 3-component color array or '#rrggbb' string");
        }
    } else if (type == JsText(u"string")) {
        if (!value.isString()) {
            errors.push_back(label + ": \"default\" must be a string");
        }
    } else if (type == JsText(u"surface") || type == JsText(u"volume") ||
               type == JsText(u"geometry") || type == JsText(u"member")) {
        if (!value.isString()) {
            errors.push_back(label + ": \"default\" must be a string");
        } else if (type == JsText(u"member")) {
            const StdEnumResult resolved = resolveStdEnum(value.toString());
            if (resolved.kind != StdEnumResult::Leaf) {
                errors.push_back(label + ": \"default\" '" + toStd(value.toString())
                                 + "' does not resolve to a std enum value");
            }
        }
    }
    // Unknown type already reported separately (validateGlobals).
}

// rangeDims: reference lines 507-513.
int rangeDims(const JsText& type) {
    if (type == JsText(u"vec2")) return 2;
    if (type == JsText(u"vec3") || type == JsText(u"color")) return 3;
    if (type == JsText(u"vec4")) return 4;
    if (type == JsText(u"mat3")) return 9;
    return 1;
}

// --- validateRangeBounds: reference lines 522-576 ---
void validateRangeBounds(const JsObject& spec, Errors& errors, const std::string& label) {
    const JsValue typeV = spec.value(JsText(u"type"));
    const JsText type = typeV.isString() ? typeV.toString() : JsText();
    const int dims = rangeDims(type);

    // min/max bounds: scalar broadcasts, arrays must match dims exactly.
    bool haveMin = false, haveMax = false;
    std::vector<double> minVals, maxVals;
    for (const char* field : {"min", "max"}) {
        const JsValue value = spec.value(JsText::fromLatin1(field));
        if (value.isUndefined()) continue;
        if (isFiniteNumber(value)) {
            if (field[0] == 'm' && field[1] == 'i') { haveMin = true; minVals = {value.toDouble()}; }
            else { haveMax = true; maxVals = {value.toDouble()}; }
        } else if (value.isArray()) {
            const JsArray arr = value.toArray();
            bool allFinite = true;
            for (const JsValue& v : arr) {
                if (!isFiniteNumber(v)) { allFinite = false; break; }
            }
            if (static_cast<int>(arr.size()) == dims && allFinite) {
                std::vector<double> vals;
                for (const JsValue& v : arr) vals.push_back(v.toDouble());
                if (field[0] == 'm' && field[1] == 'i') { haveMin = true; minVals = vals; }
                else { haveMax = true; maxVals = vals; }
            } else {
                errors.push_back(label + ": \"" + field + "\" must be an array of " + std::to_string(dims)
                                 + " finite numbers for type '"
                                 + (type.isEmpty() ? "unknown" : toStd(type)) + "'");
            }
        } else {
            errors.push_back(label + ": \"" + field + "\" must be a finite number or an array of "
                             + std::to_string(dims) + " finite numbers");
        }
    }

    if (haveMin && haveMax) {
        const bool sameForm = (minVals.size() == 1) == (maxVals.size() == 1);
        if (!sameForm) {
            errors.push_back(label + ": \"min\" and \"max\" must both be scalars or both be arrays");
        } else {
            for (size_t i = 0; i < minVals.size(); ++i) {
                if (minVals[i] > maxVals[i]) {
                    errors.push_back(label + ": \"min\" must not exceed \"max\"");
                    break;
                }
            }
        }
    }

    // Default containment: broadcast scalar bounds, compare componentwise.
    const JsValue dflt = spec.value(JsText(u"default"));
    if (dflt.isArray() && haveMin && haveMax) {
        const JsArray arr = dflt.toArray();
        bool allFinite = true;
        for (const JsValue& v : arr) {
            if (!isFiniteNumber(v)) { allFinite = false; break; }
        }
        if (allFinite) {
            for (int i = 0; i < arr.size(); ++i) {
                const double mn = minVals.size() == 1 ? minVals[0] : minVals[i];
                const double mx = maxVals.size() == 1 ? maxVals[0] : maxVals[i];
                if (arr.at(i).toDouble() < mn || arr.at(i).toDouble() > mx) {
                    JsList parts;
                    for (const JsValue& v : arr) parts << js::numberToString(v.toDouble());
                    errors.push_back(label + ": default [" + toStd(parts.join(JsText(u", ")))
                                     + "] is outside the declared range");
                    break;
                }
            }
        }
    } else if (isFiniteNumber(dflt) && haveMin && haveMax &&
               minVals.size() == 1 && maxVals.size() == 1) {
        if (dflt.toDouble() < minVals[0] || dflt.toDouble() > maxVals[0]) {
            errors.push_back(label + ": default " + numToStd(dflt.toDouble()) + " is outside the declared range ["
                             + numToStd(minVals[0]) + ", " + numToStd(maxVals[0]) + "]");
        }
    }
}

// --- validateGlobals: reference lines 578-688 ---
void validateGlobals(const JsValue& globals, Errors& errors, const ValidatorContext& context) {
    if (globals.isUndefined() || globals.isNull()) {
        return;
    }
    if (!globals.isObject()) {
        errors.push_back("\"globals\" must be an object");
        return;
    }
    const JsObject globalsObj = globals.toObject();

    std::map<std::string, std::string> uniformOwners; // uniform name -> global key
    for (auto it = globalsObj.begin(); it != globalsObj.end(); ++it) {
        const std::string key = toStd(it.key());
        const std::string label = "Global '" + key + "'";
        if (!it.value().isObject()) {
            errors.push_back(label + ": must be an object");
            continue;
        }
        const JsObject spec = it.value().toObject();

        for (const JsText& field : spec.keys()) {
            if (!globalSpecKeys().contains(field)) {
                errors.push_back(label + ": unknown field '" + toStd(field) + "'");
            }
        }

        const JsValue typeV = spec.value(JsText(u"type"));
        if (typeV.isUndefined() || typeV.isNull() ||
            (typeV.isBool() && !typeV.toBool()) ||
            (typeV.isString() && typeV.toString().isEmpty()) ||
            (typeV.isDouble() && typeV.toDouble() == 0.0)) {
            errors.push_back(label + ": Missing \"type\"");
        } else if (!typeV.isString() || !globalTypes().contains(typeV.toString())) {
            // String(spec.type) over a JSON value.
            errors.push_back(label + ": Unknown type '" + jsStringOf(typeV) + "'");
        }

        if (!spec.value(JsText(u"default")).isUndefined()) {
            validateDefault(spec, errors, label);
        }

        if (!spec.value(JsText(u"min")).isUndefined() ||
            !spec.value(JsText(u"max")).isUndefined()) {
            validateRangeBounds(spec, errors, label);
        }

        for (const char* field : {"step", "zero", "randMin", "randMax", "randChance"}) {
            const JsValue v = spec.value(JsText::fromLatin1(field));
            if (!v.isUndefined() && !isFiniteNumber(v)) {
                errors.push_back(label + ": \"" + field + "\" must be a finite number");
            }
        }
        const JsValue randChoices = spec.value(JsText(u"randChoices"));
        if (!randChoices.isUndefined()) {
            bool ok = randChoices.isArray();
            if (ok) {
                for (const JsValue& v : randChoices.toArray()) {
                    if (!isFiniteNumber(v)) { ok = false; break; }
                }
            }
            if (!ok) {
                errors.push_back(label + ": \"randChoices\" must be an array of finite numbers");
            }
        }

        const JsValue uniform = spec.value(JsText(u"uniform"));
        if (!uniform.isUndefined() && !isNonEmptyString(uniform)) {
            errors.push_back(label + ": \"uniform\" must be a non-empty string");
        } else if (isNonEmptyString(uniform)) {
            const std::string uniformName = toStd(uniform.toString());
            const auto owner = uniformOwners.find(uniformName);
            if (owner != uniformOwners.end() && !owner->second.empty()) {
                errors.push_back(label + ": uniform '" + uniformName + "' conflicts with global '"
                                 + owner->second + "'");
            } else {
                uniformOwners[uniformName] = key;
            }
        }

        const JsValue define = spec.value(JsText(u"define"));
        if (!define.isUndefined() && !isNonEmptyString(define)) {
            errors.push_back(label + ": \"define\" must be a non-empty string");
        }

        const JsValue colorModeUniform = spec.value(JsText(u"colorModeUniform"));
        if (!colorModeUniform.isUndefined() && !isNonEmptyString(colorModeUniform)) {
            errors.push_back(label + ": \"colorModeUniform\" must be a non-empty string");
        }

        const JsValue choices = spec.value(JsText(u"choices"));
        if (!choices.isUndefined()) {
            if (!choices.isObject()) {
                errors.push_back(label + ": \"choices\" must be an object mapping names to values");
            } else {
                const JsObject choicesObj = choices.toObject();
                std::vector<double> numeric;
                const bool stringType = typeV.toString() == JsText(u"string");
                for (auto cit = choicesObj.begin(); cit != choicesObj.end(); ++cit) {
                    const JsValue value = cit.value();
                    if (value.isNull()) continue; // Section headers in dropdown menus.
                    if (stringType) {
                        if (!value.isString()) {
                            errors.push_back(label + ": choices['" + toStd(cit.key())
                                             + "'] must be a string for type 'string'");
                        }
                        continue;
                    }
                    if (!isFiniteNumber(value)) {
                        errors.push_back(label + ": choices['" + toStd(cit.key())
                                         + "'] must be a number or null");
                    } else {
                        numeric.push_back(value.toDouble());
                    }
                }
                const JsValue dflt = spec.value(JsText(u"default"));
                if (!numeric.empty() && isFiniteNumber(dflt)) {
                    bool found = false;
                    for (const double v : numeric) {
                        if (v == dflt.toDouble()) { found = true; break; }
                    }
                    if (!found) {
                        errors.push_back(label + ": default " + numToStd(dflt.toDouble())
                                         + " is not among the declared choice values");
                    }
                }
            }
        }

        const JsValue enumPath = spec.value(JsText(u"enum"));
        if (!enumPath.isUndefined()) {
            if (!isNonEmptyString(enumPath)) {
                errors.push_back(label + ": \"enum\" must be a non-empty string");
            } else {
                const StdEnumResult resolved = resolveStdEnum(enumPath.toString());
                if (resolved.kind != StdEnumResult::Table) {
                    errors.push_back(label + ": enum '" + toStd(enumPath.toString())
                                     + "' does not resolve to a std enum table");
                }
            }
        }

        const JsValue ui = spec.value(JsText(u"ui"));
        if (!ui.isUndefined()) {
            validateUi(ui, errors, label + ".ui", context);
        }
    }
}

// --- validateTextureMap: reference lines 690-726 ---
void validateTextureMap(const JsValue& textures, Errors& errors, const std::string& containerName) {
    if (textures.isUndefined()) {
        return;
    }
    if (!textures.isObject()) {
        errors.push_back("\"" + containerName + "\" must be an object");
        return;
    }
    const JsObject texturesObj = textures.toObject();
    for (auto it = texturesObj.begin(); it != texturesObj.end(); ++it) {
        const std::string label = "Texture '" + toStd(it.key()) + "'";
        if (!it.value().isObject()) {
            errors.push_back(label + ": must be an object");
            continue;
        }
        const JsObject spec = it.value().toObject();
        for (const JsText& key : spec.keys()) {
            if (!textureSpecKeys().contains(key)) {
                errors.push_back(label + ": unknown field '" + toStd(key) + "'");
            }
        }
        for (const char* dim : {"width", "height"}) {
            const JsValue v = spec.value(JsText::fromLatin1(dim));
            if (!v.isUndefined()) {
                validateDimSpec(v, errors, label + "." + dim);
            }
        }
        const JsValue depth = spec.value(JsText(u"depth"));
        if (!depth.isUndefined() && (!isFiniteNumber(depth) || depth.toDouble() <= 0)) {
            errors.push_back(label + ": \"depth\" must be a positive finite number");
        }
        const JsValue format = spec.value(JsText(u"format"));
        if (!format.isUndefined() &&
            (!format.isString() || !formats().contains(format.toString()))) {
            errors.push_back(label + ": unknown format '" + jsStringOf(format) + "'");
        }
        const JsValue is3D = spec.value(JsText(u"is3D"));
        if (!is3D.isUndefined() && !is3D.isBool()) {
            errors.push_back(label + ": \"is3D\" must be a boolean");
        }
        // Authorable texture policies (reference 2f47612c):
        // filtering policies are authorable on 3D textures only; mipmap and
        // persistence policies are authorable on 2D textures only.
        const JsValue filter = spec.value(JsText(u"filter"));
        if (!filter.isUndefined()) {
            if (containerName != "textures3d") {
                errors.push_back(label + ": \"filter\" is only supported on 3D texture specs (\"textures3d\")");
            } else if (!filter.isString() || !textureFilters().contains(filter.toString())) {
                errors.push_back(label + ": unknown filter '" + jsStringOf(filter)
                                 + "' (expected 'nearest' or 'linear')");
            }
        }
        const JsValue mipmaps = spec.value(JsText(u"mipmaps"));
        if (!mipmaps.isUndefined()) {
            if (containerName == "textures3d") {
                errors.push_back(label + ": \"mipmaps\" is only supported on 2D texture specs (\"textures\")");
            } else if (!mipmaps.isBool()) {
                errors.push_back(label + ": \"mipmaps\" must be a boolean");
            }
        }
        const JsValue persistent = spec.value(JsText(u"persistent"));
        if (!persistent.isUndefined()) {
            if (containerName == "textures3d") {
                errors.push_back(label + ": \"persistent\" is only supported on 2D texture specs (\"textures\")");
            } else if (!persistent.isBool()) {
                errors.push_back(label + ": \"persistent\" must be a boolean");
            }
        }
    }
}

// --- validatePass: reference lines 732-931 ---
void validatePass(const JsObject& source, const JsObject& pass, int index, Errors& errors,
                  const ValidatorContext& context) {
    const std::string label = "Pass " + numToStd(index);

    if (!pass.contains(JsText(u"program")) || !isNonEmptyString(pass.value(JsText(u"program")))) {
        errors.push_back(label + ": Missing \"program\" string");
    }

    for (const JsText& key : pass.keys()) {
        if (!passKeys().contains(key)) {
            errors.push_back(label + ": unknown field '" + toStd(key) + "'");
        }
    }

    const JsValue name = pass.value(JsText(u"name"));
    if (!name.isUndefined() && !isNonEmptyString(name)) {
        errors.push_back(label + ": \"name\" must be a non-empty string");
    }
    const JsValue entryPoint = pass.value(JsText(u"entryPoint"));
    if (!entryPoint.isUndefined() && !isNonEmptyString(entryPoint)) {
        errors.push_back(label + ": \"entryPoint\" must be a non-empty string");
    }
    const JsValue type = pass.value(JsText(u"type"));
    if (!type.isUndefined() && !passTypes().contains(type.toString())) {
        errors.push_back(label + ": unknown pass type '" + jsStringOf(type) + "'");
    }
    const JsValue drawMode = pass.value(JsText(u"drawMode"));
    if (!drawMode.isUndefined() && !drawModes().contains(drawMode.toString())) {
        errors.push_back(label + ": unknown drawMode '" + jsStringOf(drawMode) + "'");
    }
    const JsValue drawBuffers = pass.value(JsText(u"drawBuffers"));
    if (!drawBuffers.isUndefined() && (!isJsInteger(drawBuffers) || drawBuffers.toDouble() < 1)) {
        errors.push_back(label + ": \"drawBuffers\" must be a positive integer");
    }
    const JsValue count = pass.value(JsText(u"count"));
    if (!count.isUndefined()) {
        if (count.isString()) {
            const JsText cs = count.toString();
            if (cs != JsText(u"auto") && cs != JsText(u"screen") && cs != JsText(u"input")) {
                errors.push_back(label + ": unknown count '" + toStd(cs) + "'");
            }
        } else if (!isJsInteger(count) || count.toDouble() < 1) {
            errors.push_back(label + ": \"count\" must be a positive integer, 'auto', 'screen', or 'input'");
        }
    }
    const JsValue countUniform = pass.value(JsText(u"countUniform"));
    if (!countUniform.isUndefined()) {
        if (!isNonEmptyString(countUniform)) {
            errors.push_back(label + ": \"countUniform\" must be a non-empty string");
        } else if (!referencesGlobal(countUniform.toString(), context)) {
            errors.push_back(label + ": countUniform '" + toStd(countUniform.toString())
                             + "' does not reference a declared global");
        }
    }
    const JsValue repeat = pass.value(JsText(u"repeat"));
    if (!repeat.isUndefined()) {
        if (repeat.isString()) {
            if (repeat.toString().isEmpty()) {
                errors.push_back(label + ": \"repeat\" string must name a uniform");
            }
        } else if (!isJsInteger(repeat) || repeat.toDouble() < 1) {
            errors.push_back(label + ": \"repeat\" must be a positive integer or a uniform name string");
        }
    }
    const JsValue blend = pass.value(JsText(u"blend"));
    if (!blend.isUndefined()) {
        bool ok = blend.isBool();
        if (!ok && blend.isArray()) {
            const JsArray arr = blend.toArray();
            ok = arr.size() == 2;
            for (const JsValue& v : arr) {
                if (!isNonEmptyString(v)) { ok = false; break; }
            }
        }
        if (!ok) {
            errors.push_back(label + ": \"blend\" must be a boolean or [src, dst] factor strings");
        }
    }
    const JsValue clear = pass.value(JsText(u"clear"));
    if (!clear.isUndefined() && !clear.isBool()) {
        errors.push_back(label + ": \"clear\" must be a boolean");
    }
    const JsValue samplerTypeMap = pass.value(JsText(u"samplerTypes"));
    if (!samplerTypeMap.isUndefined()) {
        if (!samplerTypeMap.isObject()) {
            errors.push_back(label + ": \"samplerTypes\" must be an object mapping sampler names to sampler types");
        } else {
            const JsObject types = samplerTypeMap.toObject();
            for (auto it = types.constBegin(); it != types.constEnd(); ++it) {
                if (!samplerTypes().contains(it.value().toString()) || !it.value().isString()) {
                    errors.push_back(label + ": samplerTypes '" + toStd(it.key()) +
                                     "' must be one of " + toStd(samplerTypes().join(JsText(u", "))));
                }
            }
        }
    }
    const JsValue workgroups = pass.value(JsText(u"workgroups"));
    if (!workgroups.isUndefined()) {
        bool ok = workgroups.isArray();
        if (ok) {
            const JsArray arr = workgroups.toArray();
            ok = arr.size() >= 1 && arr.size() <= 3;
            for (const JsValue& v : arr) {
                if (!isFiniteNumber(v) && !isNonEmptyString(v)) { ok = false; break; }
            }
        }
        if (!ok) {
            errors.push_back(label + ": \"workgroups\" must be an array of 1-3 numbers or uniform names");
        }
    }
    const JsValue storageBuffers = pass.value(JsText(u"storageBuffers"));
    if (!storageBuffers.isUndefined() && !storageBuffers.isObject()) {
        errors.push_back(label + ": \"storageBuffers\" must be an object");
    }
    const JsValue storageTextures = pass.value(JsText(u"storageTextures"));
    if (!storageTextures.isUndefined() && !storageTextures.isObject()) {
        errors.push_back(label + ": \"storageTextures\" must be an object");
    }
    const JsValue viewport = pass.value(JsText(u"viewport"));
    if (!viewport.isUndefined()) {
        if (!viewport.isObject()) {
            errors.push_back(label + ": \"viewport\" must be an object");
        } else {
            const JsObject viewportObj = viewport.toObject();
            for (const JsText& key : viewportObj.keys()) {
                if (key == JsText(u"width") || key == JsText(u"height")) {
                    validateDimSpec(viewportObj.value(key), errors, label + ".viewport." + toStd(key));
                } else if (key == JsText(u"x") || key == JsText(u"y") ||
                           key == JsText(u"w") || key == JsText(u"h")) {
                    if (!isFiniteNumber(viewportObj.value(key))) {
                        errors.push_back(label + ".viewport." + toStd(key) + " must be a finite number");
                    }
                } else {
                    errors.push_back(label + ".viewport: unknown field '" + toStd(key) + "'");
                }
            }
        }
    }
    const JsValue conditions = pass.value(JsText(u"conditions"));
    if (!conditions.isUndefined()) {
        if (!conditions.isObject()) {
            errors.push_back(label + ": \"conditions\" must be an object");
        } else {
            const JsObject conditionsObj = conditions.toObject();
            for (const JsText& key : conditionsObj.keys()) {
                if (!conditionContainerKeys().contains(key)) {
                    errors.push_back(label + ".conditions: unknown field '" + toStd(key) + "'");
                }
            }
            for (const JsText& listKey : conditionContainerKeys()) {
                const JsValue listV = conditionsObj.value(listKey);
                if (listV.isUndefined()) continue;
                if (!listV.isArray()) {
                    errors.push_back(label + ".conditions." + toStd(listKey) + " must be an array");
                    continue;
                }
                for (const JsValue& conditionV : listV.toArray()) {
                    if (!conditionV.isObject()) {
                        errors.push_back(label + ".conditions." + toStd(listKey)
                                         + ": condition must be an object");
                        continue;
                    }
                    const JsObject condition = conditionV.toObject();
                    for (const JsText& key : condition.keys()) {
                        if (key != JsText(u"uniform") && key != JsText(u"equals")) {
                            errors.push_back(label + ".conditions." + toStd(listKey)
                                             + ": unknown condition field '" + toStd(key) + "'");
                        }
                    }
                    const JsValue uniform = condition.value(JsText(u"uniform"));
                    if (!isNonEmptyString(uniform)) {
                        errors.push_back(label + ".conditions." + toStd(listKey)
                                         + ": \"uniform\" must be a non-empty string");
                    } else if (!referencesGlobal(uniform.toString(), context)) {
                        errors.push_back(label + ".conditions." + toStd(listKey) + ": uniform '"
                                         + toStd(uniform.toString()) + "' does not reference a declared global");
                    }
                    if (condition.value(JsText(u"equals")).isUndefined()) {
                        errors.push_back(label + ".conditions." + toStd(listKey)
                                         + ": condition requires an \"equals\" value");
                    }
                }
            }
        }
    }

    const JsValue uniforms = pass.value(JsText(u"uniforms"));
    if (!uniforms.isUndefined()) {
        if (!uniforms.isObject()) {
            errors.push_back(label + ": \"uniforms\" must be an object");
        } else {
            const JsObject uniformsObj = uniforms.toObject();
            for (auto it = uniformsObj.begin(); it != uniformsObj.end(); ++it) {
                // Numeric literals are preserved; strings name runtime uniforms.
                const JsValue value = it.value();
                if (!isFiniteNumber(value) && !isNonEmptyString(value)) {
                    errors.push_back(label + ": uniforms['" + toStd(it.key())
                                     + "'] must be a finite number or a non-empty string");
                }
            }
        }
    }

    const JsValue defines = pass.value(JsText(u"defines"));
    if (!defines.isUndefined()) {
        if (!defines.isObject()) {
            errors.push_back(label + ": \"defines\" must be an object");
        } else {
            const JsObject definesObj = defines.toObject();
            for (auto it = definesObj.begin(); it != definesObj.end(); ++it) {
                const JsValue value = it.value();
                if (!value.isString() && !isFiniteNumber(value)) {
                    errors.push_back(label + ": defines['" + toStd(it.key())
                                     + "'] must be a string or finite number");
                }
            }
        }
    }

    JsSet<JsText> declaredTextures;
    {
        auto addKeys = [&](const JsValue& value) {
            if (value.isObject()) {
                for (const JsText& key : value.toObject().keys()) declaredTextures.insert(key);
            } else if (value.isArray() || value.isString()) {
                const std::size_t size = value.isArray() ? value.toArray().size() : value.toString().size();
                for (std::size_t i = 0; i < size; ++i) declaredTextures.insert(js::numberToString(i));
            }
        };
        addKeys(source.value(JsText(u"textures")));
        addKeys(source.value(JsText(u"textures3d")));
    }

    const JsValue inputs = pass.value(JsText(u"inputs"));
    if (!inputs.isUndefined()) {
        if (!inputs.isObject()) {
            errors.push_back(label + ": \"inputs\" must be an object");
        } else {
            const JsObject inputsObj = inputs.toObject();
            const JsValue externalTexture = source.value(JsText(u"externalTexture"));
            for (auto it = inputsObj.begin(); it != inputsObj.end(); ++it) {
                const JsValue texRef = it.value();
                if (!isNonEmptyString(texRef)) {
                    errors.push_back(label + ": inputs['" + toStd(it.key())
                                     + "'] must be a non-empty texture reference string");
                    continue;
                }
                const JsText ref = texRef.toString();
                if (pipelineInputs().contains(ref)) continue;
                // /^o[0-7]$/
                if (ref.size() == 2 && ref.at(0) == u'o' &&
                    ref.at(1) >= u'0' && ref.at(1) <= u'7') continue;
                if (ref.startsWith(JsText(u"global_"))) continue;
                if (declaredTextures.contains(ref)) continue;
                if (context.globalKeys.contains(ref)) continue;
                if (externalTexture.isString() && ref == externalTexture.toString()) continue;
                errors.push_back(label + ": inputs['" + toStd(it.key())
                                 + "'] references unsupported texture '" + toStd(ref) + "'");
            }
        }
    }

    const JsValue outputs = pass.value(JsText(u"outputs"));
    if (!outputs.isUndefined()) {
        if (!outputs.isObject()) {
            errors.push_back(label + ": \"outputs\" must be an object");
        } else {
            const JsObject outputsObj = outputs.toObject();
            for (auto it = outputsObj.begin(); it != outputsObj.end(); ++it) {
                const JsValue texRef = it.value();
                if (!isNonEmptyString(texRef)) {
                    errors.push_back(label + ": outputs['" + toStd(it.key())
                                     + "'] must be a non-empty texture reference string");
                    continue;
                }
                const JsText ref = texRef.toString();
                if (pipelineOutputs().contains(ref)) continue;
                if (ref.startsWith(JsText(u"global_"))) continue;
                if (declaredTextures.contains(ref)) continue;
                errors.push_back(label + ": outputs['" + toStd(it.key())
                                 + "'] references unsupported output '" + toStd(ref) + "'");
            }
        }
    }
}

} // namespace

void setEffectValidatorStdEnums(const JsObject& stdEnums) {
    g_stdEnumsOverride = stdEnums;
}

void setEffectValidatorStdEnums(const Value& stdEnums) {
    setEffectValidatorStdEnums(JsObject(stdEnums));
}

std::vector<std::string> validateEffectDefinition(const JsValue& def) {
    Errors errors;

    if (isFalsy(def)) {
        return {"Effect definition is null or undefined"};
    }
    if (def.isArray()) {
        return {"Effect definition must be a plain object or Effect instance, not an array"};
    }
    if (!def.isObject() && !def.isFunction()) {
        return {std::string("Effect definition must be a plain object or Effect instance, not ")
                + jsTypeOf(def)};
    }

    JsObject source = def.toObject();
    bool diagnoseTopLevelUnknowns = true;
    const JsText tag = source.value(JsText(u"$js")).toString();
    if (tag == JsText(u"effectInstance") || tag == JsText(u"effectSubclass")) {
        const JsObject wrapper = source;
        diagnoseTopLevelUnknowns = false;
        JsObject merged;
        for (const JsValue& method : wrapper.value(JsText(u"prototypeMethods")).toArray()) {
            if (method.isString()) merged.insert(method.toString(), Value::function(u""));
        }
        const JsObject members = wrapper.value(tag == JsText(u"effectInstance") ? JsText(u"props")
                                                                          : JsText(u"statics")).toObject();
        for (auto it = members.constBegin(); it != members.constEnd(); ++it) {
            merged.insert(it.key(), it.value());
        }
        source = merged;
    }

    ValidatorContext context;
    const JsValue globalsV = source.value(JsText(u"globals"));
    if (globalsV.isObject()) {
        const JsObject globalsObj = globalsV.toObject();
        for (auto it = globalsObj.begin(); it != globalsObj.end(); ++it) {
            context.globalKeys.insert(it.key());
            if (it.value().isObject()) {
                const JsValue uniform = it.value().toObject().value(JsText(u"uniform"));
                if (isNonEmptyString(uniform)) {
                    context.globalUniformNames.insert(uniform.toString());
                }
            }
        }
    }

    // --- name (existing message preserved) ---
    if (!isNonEmptyString(source.value(JsText(u"name")))) {
        errors.push_back("Missing or invalid \"name\" property");
    }

    // --- simple typed metadata ---
    const JsValue namespaceV = source.value(JsText(u"namespace"));
    if (!namespaceV.isUndefined() && !isNonEmptyString(namespaceV)) {
        errors.push_back("\"namespace\" must be a non-empty string");
    }
    const JsValue funcV = source.value(JsText(u"func"));
    if (!funcV.isUndefined() && !isNonEmptyString(funcV)) {
        errors.push_back("\"func\" must be a non-empty string");
    }
    const JsValue description = source.value(JsText(u"description"));
    if (!description.isUndefined() && !description.isString()) {
        errors.push_back("\"description\" must be a string");
    }
    const JsValue tags = source.value(JsText(u"tags"));
    if (!tags.isUndefined()) {
        if (!tags.isArray()) {
            errors.push_back("\"tags\" must be an array of tag strings");
        } else {
            for (const JsValue& tag : tags.toArray()) {
                if (!isNonEmptyString(tag)) {
                    errors.push_back("\"tags\" must contain non-empty strings");
                } else if (!validTags().contains(tag.toString())) {
                    errors.push_back("Unknown tag '" + toStd(tag.toString()) + "'");
                }
            }
        }
    }
    const JsValue openCategories = source.value(JsText(u"openCategories"));
    if (!openCategories.isUndefined()) {
        bool ok = openCategories.isArray();
        if (ok) {
            for (const JsValue& c : openCategories.toArray()) {
                if (!c.isString()) { ok = false; break; }
            }
        }
        if (!ok) {
            errors.push_back("\"openCategories\" must be an array of strings");
        }
    }
    const JsValue defaultProgram = source.value(JsText(u"defaultProgram"));
    if (!defaultProgram.isUndefined() && !defaultProgram.isString()) {
        errors.push_back("\"defaultProgram\" must be a string");
    }
    const JsValue hidden = source.value(JsText(u"hidden"));
    if (!hidden.isUndefined() && !hidden.isBool()) {
        errors.push_back("\"hidden\" must be a boolean");
    }
    const JsValue deprecatedBy = source.value(JsText(u"deprecatedBy"));
    if (!deprecatedBy.isUndefined() && !isNonEmptyString(deprecatedBy)) {
        errors.push_back("\"deprecatedBy\" must be a non-empty string");
    }
    const JsValue externalTexture = source.value(JsText(u"externalTexture"));
    if (!externalTexture.isUndefined() && !isNonEmptyString(externalTexture)) {
        errors.push_back("\"externalTexture\" must be a non-empty string");
    }
    const JsValue externalMesh = source.value(JsText(u"externalMesh"));
    if (!externalMesh.isUndefined() && !isNonEmptyString(externalMesh)) {
        errors.push_back("\"externalMesh\" must be a non-empty string");
    }
    const JsValue builtinMeshes = source.value(JsText(u"builtinMeshes"));
    if (!builtinMeshes.isUndefined()) {
        if (builtinMeshes.isArray()) {
            // Port-authored array form emitted by tools/convert-definitions.mjs
            // (effects/render/meshLoader.json): [{name, path}].
            const JsArray arr = builtinMeshes.toArray();
            for (int i = 0; i < arr.size(); ++i) {
                const JsValue entry = arr.at(i);
                if (!entry.isObject()) {
                    errors.push_back("builtinMeshes[" + numToStd(i)
                                     + "] must be an object with \"name\" and \"path\" strings");
                    continue;
                }
                const JsObject obj = entry.toObject();
                if (!isNonEmptyString(obj.value(JsText(u"name")))) {
                    errors.push_back("builtinMeshes[" + numToStd(i)
                                     + "] must have a non-empty \"name\" string");
                }
                if (!isNonEmptyString(obj.value(JsText(u"path")))) {
                    errors.push_back("builtinMeshes[" + numToStd(i)
                                     + "] must have a non-empty \"path\" string");
                }
            }
        } else if (!builtinMeshes.isObject()) {
            errors.push_back("\"builtinMeshes\" must be an object");
        } else {
            // Reference map form ({name: "path"}).
            const JsObject meshObj = builtinMeshes.toObject();
            for (auto it = meshObj.begin(); it != meshObj.end(); ++it) {
                if (!isNonEmptyString(it.value())) {
                    errors.push_back("builtinMeshes['" + toStd(it.key())
                                     + "'] must be a non-empty string");
                }
            }
        }
    }
    const JsValue outputTex3d = source.value(JsText(u"outputTex3d"));
    if (!outputTex3d.isUndefined() && !isNonEmptyString(outputTex3d)) {
        errors.push_back("\"outputTex3d\" must be a non-empty string");
    }
    const JsValue outputGeo = source.value(JsText(u"outputGeo"));
    if (!outputGeo.isUndefined() && !isNonEmptyString(outputGeo)) {
        errors.push_back("\"outputGeo\" must be a non-empty string");
    }

    // --- lifecycle hooks must be functions ---
    for (const char* hook : {"onInit", "onUpdate", "onDestroy", "asyncInit"}) {
        const JsValue value = source.value(JsText::fromLatin1(hook));
        if (!value.isUndefined() && !value.isFunction()) {
            errors.push_back(std::string("\"") + hook + "\" must be a function");
        }
    }

    // --- globals ---
    validateGlobals(globalsV, errors, context);

    // --- passes ---
    const JsValue passes = source.value(JsText(u"passes"));
    if (!passes.isArray() || passes.toArray().isEmpty()) {
        errors.push_back("Missing or empty \"passes\" array");
    } else {
        const JsArray passesArr = passes.toArray();
        for (int index = 0; index < passesArr.size(); ++index) {
            const JsValue passV = passesArr.at(index);
            if (passV.isObject()) {
                validatePass(source, passV.toObject(), index, errors, context);
            } else {
                errors.push_back("Pass " + numToStd(index) + ": must be an object");
            }
        }
    }

    // --- textures ---
    validateTextureMap(source.value(JsText(u"textures")), errors, "textures");
    validateTextureMap(source.value(JsText(u"textures3d")), errors, "textures3d");

    // --- shaders ---
    const JsValue shaders = source.value(JsText(u"shaders"));
    if (!shaders.isUndefined()) {
        if (!shaders.isObject()) {
            errors.push_back("\"shaders\" must be an object mapping program names to shader maps");
        } else {
            const JsObject shadersObj = shaders.toObject();
            for (auto it = shadersObj.begin(); it != shadersObj.end(); ++it) {
                if (!it.value().isObject()) {
                    errors.push_back("shaders['" + toStd(it.key()) + "'] must be an object");
                }
            }
        }
    }

    // --- uniform layouts ---
    const JsValue uniformLayout = source.value(JsText(u"uniformLayout"));
    if (!uniformLayout.isUndefined()) {
        validateUniformLayout(uniformLayout, errors, "uniformLayout");
    }
    const JsValue uniformLayouts = source.value(JsText(u"uniformLayouts"));
    if (!uniformLayouts.isUndefined()) {
        if (!uniformLayouts.isObject()) {
            errors.push_back("\"uniformLayouts\" must be an object mapping program names to layouts");
        } else {
            const JsObject layoutsObj = uniformLayouts.toObject();
            for (auto it = layoutsObj.begin(); it != layoutsObj.end(); ++it) {
                validateUniformLayout(it.value(),
                                      errors, "uniformLayouts['" + toStd(it.key()) + "']");
            }
        }
    }

    // --- param aliases ---
    const JsValue paramAliases = source.value(JsText(u"paramAliases"));
    if (!paramAliases.isUndefined()) {
        if (!paramAliases.isObject()) {
            errors.push_back("\"paramAliases\" must be an object mapping aliases to global names");
        } else {
            const JsObject aliasesObj = paramAliases.toObject();
            for (auto it = aliasesObj.begin(); it != aliasesObj.end(); ++it) {
                const JsValue target = it.value();
                if (!isNonEmptyString(target)) {
                    errors.push_back("paramAliases['" + toStd(it.key()) + "'] must be a non-empty string");
                } else if (!context.globalKeys.contains(target.toString())) {
                    errors.push_back("paramAliases['" + toStd(it.key()) + "'] references unknown global '"
                                     + toStd(target.toString()) + "'");
                }
            }
        }
    }

    // --- top-level unknown-field diagnosis (plain definition objects only) ---
    if (diagnoseTopLevelUnknowns) {
        for (const JsText& key : source.keys()) {
            if (!topLevelKeys().contains(key)) {
                errors.push_back("Unknown definition field '" + toStd(key) + "'");
            }
        }
    }

    return errors;
}

std::vector<std::string> validateEffectDefinition(const JsObject& def) {
    return validateEffectDefinition(JsValue(def));
}

std::vector<std::string> validateEffectDefinition(const Value& def) {
    return validateEffectDefinition(JsValue(def));
}

} // namespace nm
