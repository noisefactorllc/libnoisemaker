#include "gpu/wgsl/wgsl.h"

#include "core/edit/jsv.h"
#include "core/value/js_number.h"
#include "core/value/parse_double.h"
#include "gpu/wgsl/jsre.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <system_error>
#include <map>
#include <memory>
#include <set>
#include <string>

namespace libnoisemaker::wgsl {

namespace {

using jsre::Match;
using jsre::Regex;

bool is_js_space(char16_t c) {
    switch (c) {
        case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D: case 0x20: case 0xA0: case 0x1680:
        case 0x2028: case 0x2029: case 0x202F: case 0x205F: case 0x3000: case 0xFEFF:
            return true;
        default:
            return c >= 0x2000 && c <= 0x200A;
    }
}

// String.prototype.trim
JsString trim(const JsString& s) {
    size_t start = 0, end = s.size();
    while (start < end && is_js_space(s[start])) ++start;
    while (end > start && is_js_space(s[end - 1])) --end;
    return s.substr(start, end - start);
}

// String.prototype.toLowerCase for the ASCII text these call sites compare.
JsString lower(JsString s) {
    for (char16_t& c : s) {
        if (c >= u'A' && c <= u'Z') c = char16_t(c + 32);
    }
    return s;
}

bool contains(const JsString& s, const char16_t* needle) { return s.find(needle) != JsString::npos; }

// parseInt(digits, 10) for a run of \d: Infinity past the largest double.
double parse_digits(const JsString& digits) {
    const std::string text = js_to_utf8(digits);
    double value = 0;
    const auto result = parse_double(text.data(), text.data() + text.size(), value);
    if (result.ec == std::errc::result_out_of_range) return std::numeric_limits<double>::infinity();
    return value;
}

double js_ceil_div_mul(double offset, double align) { return std::ceil(offset / align) * align; }

const std::vector<char16_t> kComponentOrder = {u'x', u'y', u'z', u'w'};

double component_order(const JsString& components) {
    const auto it = std::find(kComponentOrder.begin(), kComponentOrder.end(), components.empty() ? 0 : components[0]);
    return it == kComponentOrder.end() ? std::nan("") : double(it - kComponentOrder.begin());
}

// layout.sort((a, b) => a.slot - b.slot || componentOrder[...] - componentOrder[...]),
// a stable sort as V8's is.
void sort_slots(std::vector<SlotField>& layout) {
    std::stable_sort(layout.begin(), layout.end(), [](const SlotField& a, const SlotField& b) {
        if (a.slot != b.slot) return a.slot < b.slot;
        return component_order(a.components) < component_order(b.components);
    });
}

struct TypeSize {
    double size;
    double align;
};

TypeSize computeWgslTypeSize(const JsString& typeExpr) {
    static const Regex arrayRe(u"^array\\s*<\\s*(.+?)\\s*,\\s*(\\d+)\\s*>$");
    if (auto m = arrayRe.exec(typeExpr)) {
        const TypeSize elem = computeWgslTypeSize(m->str(typeExpr, 1));
        const double count = parse_digits(m->str(typeExpr, 2));
        const double stride = std::max(elem.size, 16.0);
        return {stride * count, std::max(elem.align, 16.0)};
    }
    static const Regex spaces(u"\\s+", u"g");
    const JsString t = jsre::replace_all(spaces, typeExpr, u"");
    static const std::map<JsString, TypeSize> sizes = {
        {u"f32", {4, 4}}, {u"i32", {4, 4}}, {u"u32", {4, 4}}, {u"f16", {2, 2}}, {u"bool", {4, 4}},
        {u"vec2<f32>", {8, 8}}, {u"vec2f", {8, 8}}, {u"vec2<i32>", {8, 8}}, {u"vec2i", {8, 8}},
        {u"vec2<u32>", {8, 8}}, {u"vec2u", {8, 8}},
        {u"vec3<f32>", {12, 16}}, {u"vec3f", {12, 16}}, {u"vec3<i32>", {12, 16}}, {u"vec3i", {12, 16}},
        {u"vec3<u32>", {12, 16}}, {u"vec3u", {12, 16}},
        {u"vec4<f32>", {16, 16}}, {u"vec4f", {16, 16}}, {u"vec4<i32>", {16, 16}}, {u"vec4i", {16, 16}},
        {u"vec4<u32>", {16, 16}}, {u"vec4u", {16, 16}},
        {u"mat3x3<f32>", {48, 16}}, {u"mat3x3f", {48, 16}}, {u"mat4x4<f32>", {64, 16}}, {u"mat4x4f", {64, 16}},
    };
    const auto it = sizes.find(t);
    return it == sizes.end() ? TypeSize{0, 4} : it->second;
}

double computeWgslStructSize(const JsString& body) {
    static const Regex lineComments(u"\\/\\/[^\\n]*", u"g");
    const JsString cleaned = jsre::replace_all(lineComments, body, u"");
    std::vector<JsString> fields;
    int depth = 0;
    size_t start = 0;
    for (size_t i = 0; i < cleaned.size(); ++i) {
        const char16_t c = cleaned[i];
        if (c == u'<') ++depth;
        else if (c == u'>') --depth;
        else if ((c == u',' || c == u';') && depth == 0) {
            const JsString piece = trim(cleaned.substr(start, i - start));
            if (!piece.empty()) fields.push_back(piece);
            start = i + 1;
        }
    }
    const JsString tail = trim(cleaned.substr(start));
    if (!tail.empty()) fields.push_back(tail);

    double offset = 0;
    double maxAlign = 4;
    for (const JsString& field : fields) {
        const size_t colon = field.find(u':');
        if (colon == JsString::npos) continue;
        const TypeSize ts = computeWgslTypeSize(trim(field.substr(colon + 1)));
        if (ts.size == 0) continue;
        offset = js_ceil_div_mul(offset, ts.align);
        offset += ts.size;
        if (ts.align > maxAlign) maxAlign = ts.align;
    }
    return js_ceil_div_mul(offset, maxAlign);
}

struct TypeInfo {
    double size;
    double align;
    const char16_t* baseType;
    double components;
};

TypeInfo getWgslTypeInfo(const JsString& type) {
    static const std::map<JsString, TypeInfo> types = {
        {u"f32", {4, 4, u"float", 1}}, {u"i32", {4, 4, u"int", 1}}, {u"u32", {4, 4, u"uint", 1}},
        {u"vec2f", {8, 8, u"float", 2}}, {u"vec2<f32>", {8, 8, u"float", 2}},
        {u"vec3f", {12, 16, u"float", 3}}, {u"vec3<f32>", {12, 16, u"float", 3}},
        {u"vec4f", {16, 16, u"float", 4}}, {u"vec4<f32>", {16, 16, u"float", 4}},
        {u"vec2i", {8, 8, u"int", 2}}, {u"vec2<i32>", {8, 8, u"int", 2}},
        {u"vec3i", {12, 16, u"int", 3}}, {u"vec3<i32>", {12, 16, u"int", 3}},
        {u"vec4i", {16, 16, u"int", 4}}, {u"vec4<i32>", {16, 16, u"int", 4}},
        {u"vec2u", {8, 8, u"uint", 2}}, {u"vec2<u32>", {8, 8, u"uint", 2}},
        {u"vec3u", {12, 16, u"uint", 3}}, {u"vec3<u32>", {12, 16, u"uint", 3}},
        {u"vec4u", {16, 16, u"uint", 4}}, {u"vec4<u32>", {16, 16, u"uint", 4}},
    };
    const auto it = types.find(lower(type));
    return it == types.end() ? TypeInfo{4, 4, u"float", 1} : it->second;
}

// parseWgslStructByteLayout; nullopt is the reference's null.
std::optional<std::pair<std::vector<ByteField>, double>> parseWgslStructByteLayout(const JsString& source) {
    const Regex structRe(u"struct\\s+(\\w*(?:Params|Uniforms|Config|Settings))\\s*\\{([^}]+)\\}", u"gi");
    const auto structMatch = structRe.exec(source);
    if (!structMatch) return std::nullopt;
    const JsString structBody = structMatch->str(source, 2);
    static const Regex arrayField(u"\\barray\\s*<");
    if (arrayField.test(structBody)) return std::nullopt;
    static const Regex namedComment(u"\\/\\/\\s*\\(\\s*\\w+(?:\\s*,\\s*\\w+)+\\s*\\)");
    if (namedComment.test(structBody)) return std::nullopt;

    const JsString structName = structMatch->str(source, 1);
    const Regex bindingRe(u"var<uniform>\\s+(\\w+)\\s*:\\s*" + structName + u"\\s*;");
    const auto bindingMatch = bindingRe.exec(source);
    if (!bindingMatch) return std::nullopt;
    const JsString uniformVarName = bindingMatch->str(source, 1);

    const Regex fieldRe(u"(\\w+)\\s*:\\s*(f32|i32|u32|vec2f|vec3f|vec4f|vec2<f32>|vec3<f32>|vec4<f32>|vec2i|vec3i|vec4i|"
                        u"vec2<i32>|vec3<i32>|vec4<i32>|vec2u|vec3u|vec4u|vec2<u32>|vec3<u32>|vec4<u32>)",
                        u"gi");
    std::vector<ByteField> layout;
    double offset = 0;
    double maxAlign = 4;
    for (const Match& m : jsre::match_all(fieldRe, structBody)) {
        const JsString fieldName = m.str(structBody, 1);
        const JsString fieldType = lower(m.str(structBody, 2));
        const TypeInfo info = getWgslTypeInfo(fieldType);
        maxAlign = std::max(maxAlign, info.align);
        offset = js_ceil_div_mul(offset, info.align);
        if (fieldName.starts_with(u"_") || lower(fieldName).starts_with(u"pad")) {
            offset += info.size;
            continue;
        }
        layout.push_back({fieldName, offset, info.size, info.baseType, info.components});
        offset += info.size;
    }
    const double structSize = js_ceil_div_mul(offset, maxAlign);
    const Regex usageRe(uniformVarName + u"\\.(\\w+)");
    if (!usageRe.test(source)) return std::nullopt;
    if (layout.empty()) return std::nullopt;
    return std::make_pair(std::move(layout), structSize);
}

std::vector<SlotField> parseNamedStructLayout(const JsString& source) {
    std::vector<SlotField> layout;
    static const char16_t* componentNames[] = {u"x", u"y", u"z", u"w"};
    const Regex structRe(u"struct\\s+(\\w*(?:Params|Uniforms|Config|Settings))\\s*\\{([^}]+)\\}", u"gi");
    for (const Match& structMatch : jsre::match_all(structRe, source)) {
        const JsString structBody = structMatch.str(source, 2);
        double slot = 0;
        const Regex fieldRe(u"(\\w+)\\s*:\\s*(vec[234]<f32>|f32|i32|u32|array<[^>]+>)[^,;\\n]*(?:,|;)?\\s*"
                            u"(?:\\/\\/\\s*\\(([^)]+)\\))?",
                            u"gi");
        for (const Match& m : jsre::match_all(fieldRe, structBody)) {
            const JsString fieldName = m.str(structBody, 1);
            const JsString fieldType = m.str(structBody, 2);
            double numComponents = 4;
            if (fieldType == u"f32" || fieldType == u"i32" || fieldType == u"u32") numComponents = 1;
            else if (fieldType.starts_with(u"vec2")) numComponents = 2;
            else if (fieldType.starts_with(u"vec3")) numComponents = 3;
            else if (fieldType.starts_with(u"vec4")) numComponents = 4;

            if (m.has(3) && !m.str(structBody, 3).empty()) {
                std::vector<JsString> names;
                const JsString comment = m.str(structBody, 3);
                size_t from = 0;
                while (true) {
                    const size_t comma = comment.find(u',', from);
                    names.push_back(trim(comment.substr(from, comma == JsString::npos ? JsString::npos : comma - from)));
                    if (comma == JsString::npos) break;
                    from = comma + 1;
                }
                for (size_t i = 0; i < std::min<size_t>(names.size(), size_t(numComponents)); ++i) {
                    const JsString& name = names[i];
                    if (!name.empty() && name != u"_" && !lower(name).starts_with(u"pad") &&
                        !lower(name).starts_with(u"unused") && !name.starts_with(u"_")) {
                        layout.push_back({name, slot, componentNames[i]});
                    }
                }
            } else if (numComponents == 1) {
                layout.push_back({fieldName, slot, u"x"});
            }
            ++slot;
        }
    }
    return layout;
}

std::vector<SlotField> parseParamsAccessLayout(const JsString& source) {
    std::vector<SlotField> layout;
    const Regex structRe(u"struct\\s+\\w*(?:Params|Uniforms|Config|Settings)\\s*\\{([^}]+)\\}", u"gi");
    const auto structMatch = structRe.exec(source);
    if (!structMatch) return layout;
    const JsString structBody = structMatch->str(source, 1);
    const Regex fieldOrderRe(u"(\\w+)\\s*:\\s*(?:vec[234]<f32>|f32|i32|u32|array<[^>]+>)", u"gi");
    std::map<JsString, double> fieldSlots;
    double slot = 0;
    for (const Match& m : jsre::match_all(fieldOrderRe, structBody)) fieldSlots[m.str(structBody, 1)] = slot++;

    const Regex accessRe(u"(?:let\\s+)?(\\w+)(?:\\s*:\\s*[^=\\n]+)?\\s*=\\s*(?:i32\\s*\\(\\s*)?params\\.(\\w+)\\.([xyzw]+)",
                         u"g");
    for (const Match& m : jsre::match_all(accessRe, source)) {
        const auto it = fieldSlots.find(m.str(source, 2));
        if (it != fieldSlots.end()) layout.push_back({m.str(source, 1), it->second, m.str(source, 3)});
    }
    sort_slots(layout);
    return layout;
}

// A Set of strings that keeps insertion order.
struct NameSet {
    std::vector<JsString> order;
    std::set<JsString> members;
    bool has(const JsString& name) const { return members.contains(name); }
    void add(const JsString& name) {
        if (members.insert(name).second) order.push_back(name);
    }
};

}  // namespace

JsString stripComments(const JsString& source) {
    JsString stripped;
    stripped.reserve(source.size());
    int blockDepth = 0;
    for (size_t i = 0; i < source.size();) {
        const char16_t c = source[i];
        const char16_t next = i + 1 < source.size() ? source[i + 1] : 0;
        if (blockDepth > 0) {
            if (c == u'/' && next == u'*') {
                stripped += u"  ";
                ++blockDepth;
                i += 2;
            } else if (c == u'*' && next == u'/') {
                stripped += u"  ";
                --blockDepth;
                i += 2;
            } else {
                stripped += c == u'\n' ? u'\n' : u' ';
                ++i;
            }
        } else if (c == u'/' && next == u'/') {
            stripped += u"  ";
            i += 2;
            while (i < source.size() && source[i] != u'\n') {
                stripped += u' ';
                ++i;
            }
        } else if (c == u'/' && next == u'*') {
            stripped += u"  ";
            blockDepth = 1;
            i += 2;
        } else {
            stripped += c;
            ++i;
        }
    }
    return stripped;
}

EntryPoints detectEntryPoints(const JsString& source) {
    static const Regex vertexRe(u"@vertex\\s*\\n?\\s*fn\\s+(\\w+)");
    static const Regex fragmentRe(u"@fragment\\s*\\n?\\s*fn\\s+(\\w+)");
    static const Regex computeRe(u"@compute[^f]*fn\\s+(\\w+)");
    EntryPoints result;
    if (auto m = vertexRe.exec(source)) result.vertex = m->str(source, 1);
    if (auto m = fragmentRe.exec(source)) result.fragment = m->str(source, 1);
    if (auto m = computeRe.exec(source)) result.compute = m->str(source, 1);
    return result;
}

std::vector<JsString> computeEntryPoints(const JsString& source) {
    static const Regex computeRe(u"@compute[^f]*fn\\s+(\\w+)", u"g");
    std::vector<JsString> out;
    for (const Match& m : jsre::match_all(computeRe, source)) out.push_back(m.str(source, 1));
    return out;
}

bool hasComputeEntry(const JsString& source) {
    static const Regex re(u"@compute\\s");
    return re.test(source);
}
bool hasFragmentEntry(const JsString& source) {
    static const Regex re(u"@fragment\\s");
    return re.test(source);
}
bool hasVertexEntry(const JsString& source) {
    static const Regex re(u"@vertex\\s");
    return re.test(source);
}

std::vector<Binding> parseShaderBindings(const JsString& source) {
    const JsString clean = stripComments(source);
    static const Regex bindingRe(
        u"@group\\s*\\(\\s*(\\d+)\\s*\\)\\s*@binding\\s*\\(\\s*(\\d+)\\s*\\)\\s*var(?:<([^>]+)>)?\\s+(\\w+)\\s*:\\s*([^;]+)", u"g");
    std::vector<Binding> bindings;
    for (const Match& m : jsre::match_all(bindingRe, clean)) {
        Binding b;
        b.group = parse_digits(m.str(clean, 1));
        b.binding = parse_digits(m.str(clean, 2));
        b.storage = m.str(clean, 3);
        b.name = m.str(clean, 4);
        b.typeDecl = trim(m.str(clean, 5));
        if (contains(b.typeDecl, u"texture_storage_2d") || contains(b.typeDecl, u"texture_storage_3d"))
            b.type = u"storage_texture";
        else if (contains(b.typeDecl, u"texture_2d") || contains(b.typeDecl, u"texture_3d")) b.type = u"texture";
        else if (b.typeDecl == u"sampler") b.type = u"sampler";
        else if (contains(b.storage, u"uniform")) b.type = u"uniform";
        else if (contains(b.storage, u"storage")) b.type = u"storage";
        else b.type = u"unknown";
        bindings.push_back(std::move(b));
    }
    std::vector<Binding> filtered;
    for (const Binding& b : bindings) {
        if (b.type == u"storage_texture" || b.type == u"storage") {
            filtered.push_back(b);
            continue;
        }
        const Regex word(u"\\b" + b.name + u"\\b", u"g");
        if (jsre::match_all(word, clean).size() > 1) filtered.push_back(b);
    }
    std::stable_sort(filtered.begin(), filtered.end(), [](const Binding& a, const Binding& b) {
        if (a.group != b.group) return a.group < b.group;
        return a.binding < b.binding;
    });
    return filtered;
}

OrderedMap<JsString, std::vector<double>> parseEntryPointBindings(const JsString& input,
                                                                  const std::vector<Binding>& bindings) {
    const JsString source = stripComments(input);
    struct Function {
        JsString body;
        NameSet parameters;
    };
    OrderedMap<JsString, Function> functionBodies;
    auto findFunction = [&](const JsString& name) -> Function* {
        for (auto& [key, fn] : functionBodies) {
            if (key == name) return &fn;
        }
        return nullptr;
    };

    static const Regex functionRe(u"\\bfn\\s+(\\w+)\\s*\\(", u"g");
    static const Regex parameterRe(u"(?:^|,)\\s*(?:@\\w+\\s*\\([^)]*\\)\\s*)*([A-Za-z_]\\w*)\\s*:", u"g");
    size_t lastIndex = 0;
    while (auto functionMatch = functionRe.exec(source, lastIndex)) {
        lastIndex = functionMatch->end();
        const size_t parametersStart = lastIndex;
        size_t parametersEnd = parametersStart;
        int parentheses = 1;
        for (; parametersEnd < source.size() && parentheses > 0; ++parametersEnd) {
            if (source[parametersEnd] == u'(') ++parentheses;
            else if (source[parametersEnd] == u')') --parentheses;
        }
        if (parentheses != 0) continue;
        NameSet parameters;
        const JsString parameterSource = source.substr(parametersStart, parametersEnd - 1 - parametersStart);
        for (const Match& p : jsre::match_all(parameterRe, parameterSource)) parameters.add(p.str(parameterSource, 1));
        const size_t openingBrace = source.find(u'{', parametersEnd);
        if (openingBrace == JsString::npos) continue;
        const size_t start = openingBrace + 1;
        int depth = 1;
        size_t end = start;
        for (; end < source.size() && depth > 0; ++end) {
            if (source[end] == u'{') ++depth;
            else if (source[end] == u'}') --depth;
        }
        const JsString name = functionMatch->str(source, 1);
        Function fn{source.substr(start, end - 1 - start), std::move(parameters)};
        if (Function* existing = findFunction(name)) *existing = std::move(fn);
        else functionBodies.emplace_back(name, std::move(fn));
        lastIndex = end;
    }

    // new Map(bindings.map(b => [b.name, b.binding])): a later name wins.
    std::map<JsString, double> bindingByName;
    for (const Binding& b : bindings) bindingByName[b.name] = b.binding;

    static const Regex tokenRe(u"[A-Za-z_]\\w*|\\d+\\w*|[{}().,:;<>]", u"g");
    static const Regex identifierRe(u"^[A-Za-z_]\\w*$");
    struct Analysis {
        std::vector<double> used;
        NameSet calls;
    };
    auto analyzeBody = [&](const Function& fn) {
        std::vector<JsString> tokens;
        for (const Match& m : jsre::match_all(tokenRe, fn.body)) tokens.push_back(m.str(fn.body, 0));
        auto token_at = [&](long index) -> const JsString* {
            return index >= 0 && size_t(index) < tokens.size() ? &tokens[size_t(index)] : nullptr;
        };
        auto token_is = [&](long index, const char16_t* text) {
            const JsString* t = token_at(index);
            return t && *t == text;
        };
        // Scopes are shared, as the reference's Sets are: a pending
        // declaration keeps the scope it was made in even after it closes.
        using Scope = std::shared_ptr<std::set<JsString>>;
        std::vector<Scope> scopes{std::make_shared<std::set<JsString>>(fn.parameters.members)};
        std::vector<double> used;
        std::set<double> usedSet;
        NameSet calls;
        std::set<long> forBodyOpens;
        std::vector<size_t> forBodyDepths;
        std::set<long> declarations;
        std::vector<std::pair<JsString, Scope>> pendingDeclarations;
        for (long i = 0; size_t(i) < tokens.size(); ++i) {
            const JsString& token = tokens[size_t(i)];
            if (token == u";") {
                for (const auto& [name, scope] : pendingDeclarations) scope->insert(name);
                pendingDeclarations.clear();
                continue;
            }
            if (token == u"for" && token_is(i + 1, u"(")) {
                int parentheses = 0;
                long end = i + 1;
                for (; size_t(end) < tokens.size(); ++end) {
                    if (tokens[size_t(end)] == u"(") ++parentheses;
                    else if (tokens[size_t(end)] == u")" && --parentheses == 0) break;
                }
                if (token_is(end + 1, u"{")) {
                    scopes.push_back(std::make_shared<std::set<JsString>>());
                    forBodyOpens.insert(end + 1);
                }
                continue;
            }
            if (token == u"{") {
                scopes.push_back(std::make_shared<std::set<JsString>>());
                if (forBodyOpens.contains(i)) forBodyDepths.push_back(scopes.size());
                continue;
            }
            if (token == u"}") {
                const size_t closingDepth = scopes.size();
                if (closingDepth > 1) scopes.pop_back();
                if (!forBodyDepths.empty() && forBodyDepths.back() == closingDepth) {
                    scopes.pop_back();
                    forBodyDepths.pop_back();
                }
                continue;
            }
            if (token == u"let" || token == u"var" || token == u"const") {
                long nameIndex = i + 1;
                if (token == u"var" && token_is(nameIndex, u"<")) {
                    int brackets = 1;
                    for (++nameIndex; size_t(nameIndex) < tokens.size() && brackets > 0; ++nameIndex) {
                        if (tokens[size_t(nameIndex)] == u"<") ++brackets;
                        else if (tokens[size_t(nameIndex)] == u">") --brackets;
                    }
                }
                const JsString* localName = token_at(nameIndex);
                if (localName && !localName->empty() && identifierRe.test(*localName)) {
                    declarations.insert(nameIndex);
                    pendingDeclarations.emplace_back(*localName, scopes.back());
                }
            }
            if (declarations.contains(i)) continue;
            const auto binding = bindingByName.find(token);
            if (binding != bindingByName.end() && !token_is(i - 1, u".") &&
                std::none_of(scopes.begin(), scopes.end(), [&](const Scope& s) { return s->contains(token); })) {
                if (usedSet.insert(binding->second).second) used.push_back(binding->second);
            }
            if (findFunction(token) && token_is(i + 1, u"(") && !token_is(i - 1, u".")) calls.add(token);
        }
        return Analysis{std::move(used), std::move(calls)};
    };

    OrderedMap<JsString, std::vector<double>> entryPointBindings;
    static const Regex entryRe(u"@(?:compute|vertex|fragment)[^f]*fn\\s+(\\w+)\\s*\\([^)]*\\)[^{]*\\{", u"g");
    size_t entryIndex = 0;
    while (auto m = entryRe.exec(source, entryIndex)) {
        entryIndex = m->end();
        const JsString entryPoint = m->str(source, 1);
        const size_t startIdx = m->end();
        int braceCount = 1;
        size_t endIdx = startIdx;
        for (size_t i = startIdx; i < source.size() && braceCount > 0; ++i) {
            if (source[i] == u'{') ++braceCount;
            else if (source[i] == u'}') --braceCount;
            endIdx = i;
        }
        std::vector<Function> reachable;
        const Function* entryFunction = findFunction(entryPoint);
        reachable.push_back({source.substr(startIdx, endIdx - startIdx), entryFunction ? entryFunction->parameters : NameSet{}});
        NameSet visited;
        visited.add(entryPoint);
        std::vector<double> usedBindings;
        std::set<double> usedSet;
        for (size_t index = 0; index < reachable.size(); ++index) {
            const Analysis analysis = analyzeBody(reachable[index]);
            for (double b : analysis.used) {
                if (usedSet.insert(b).second) usedBindings.push_back(b);
            }
            for (const JsString& name : analysis.calls.order) {
                if (visited.has(name)) continue;
                visited.add(name);
                reachable.push_back(*findFunction(name));
            }
        }
        bool replaced = false;
        for (auto& [key, value] : entryPointBindings) {
            if (key == entryPoint) {
                value = usedBindings;
                replaced = true;
            }
        }
        if (!replaced) entryPointBindings.emplace_back(entryPoint, std::move(usedBindings));
    }
    return entryPointBindings;
}

double parseDeclaredUniformBufferSize(const JsString& source) {
    static const Regex bindingRe(u"var<uniform>\\s+\\w+\\s*:\\s*(\\w+)\\s*;", u"g");
    double largestSize = 0;
    for (const Match& m : jsre::match_all(bindingRe, source)) {
        const Regex structRe(u"struct\\s+" + m.str(source, 1) + u"\\s*\\{([^}]+)\\}", u"g");
        const auto structMatch = structRe.exec(source);
        if (!structMatch) continue;
        const double size = computeWgslStructSize(structMatch->str(source, 1));
        if (size > largestSize) largestSize = size;
    }
    return largestSize;
}

PackedLayout parsePackedUniformLayout(const JsString& source) {
    PackedLayout result;
    if (auto byteLayout = parseWgslStructByteLayout(source)) {
        result.kind = PackedLayout::Kind::Byte;
        result.bytes = std::move(byteLayout->first);
        result.structSize = byteLayout->second;
        return result;
    }
    if (auto named = parseNamedStructLayout(source); !named.empty()) {
        result.kind = PackedLayout::Kind::Slots;
        result.slots = std::move(named);
        return result;
    }
    if (auto params = parseParamsAccessLayout(source); !params.empty()) {
        result.kind = PackedLayout::Kind::Slots;
        result.slots = std::move(params);
        return result;
    }
    if (!contains(source, u"uniforms.data[")) return result;
    static const Regex unpackRe(
        u"(?:let\\s+)?(\\w+)(?:\\s*:\\s*[^\\n=]+)?\\s*=\\s*(?:max\\s*\\([^,]+,\\s*)?(?:i32\\s*\\(\\s*)?uniforms\\.data\\[(\\d+)\\]\\.([xyzw]+)",
        u"g");
    std::vector<SlotField> layout;
    for (const Match& m : jsre::match_all(unpackRe, source)) {
        layout.push_back({m.str(source, 1), parse_digits(m.str(source, 2)), m.str(source, 3)});
    }
    if (layout.empty()) return result;
    sort_slots(layout);
    result.kind = PackedLayout::Kind::Slots;
    result.slots = std::move(layout);
    return result;
}

OrderedMap<JsString, std::optional<JsString>> parseSamplerTextureUses(const JsString& source) {
    const JsString clean = stripComments(source);
    static const Regex samples(u"\\btextureSample\\w*\\s*\\(\\s*(\\w+)\\s*,\\s*(\\w+)\\s*,", u"g");
    OrderedMap<JsString, std::optional<JsString>> names;
    for (const Match& m : jsre::match_all(samples, clean)) {
        const JsString texture = m.str(clean, 1);
        const JsString sampler = m.str(clean, 2);
        auto it = std::find_if(names.begin(), names.end(), [&](const auto& entry) { return entry.first == sampler; });
        if (it == names.end()) names.emplace_back(sampler, texture);
        else if (it->second != texture) it->second = std::nullopt;
    }
    return names;
}

bool hasShaderBindings(const JsString& source) {
    static const Regex re(u"@binding\\s*\\(");
    return re.test(stripComments(source));
}

JsString injectDefines(const JsString& source, const Value& defines) {
    if (!jsv::truthy(defines)) return source;
    const auto entries = jsv::entries(defines);
    if (entries.empty()) return source;
    JsString injected;
    for (const auto& [key, value] : entries) {
        if (value.is_bool()) {
            injected += u"const " + key + u": bool = " + (value.as_bool() ? u"true" : u"false") + u";\n";
        } else if (value.is_number()) {
            const double n = value.as_number();
            const bool integer = std::isfinite(n) && std::trunc(n) == n;
            injected += u"const " + key + (integer ? u": i32 = " : u": f32 = ") + js_number_to_string(n) + u";\n";
        } else {
            injected += u"const " + key + u" = " + jsv::to_string(value) + u";\n";
        }
    }
    return injected + source;
}

Value toValue(const EntryPoints& e) {
    Object out;
    auto opt = [](const std::optional<JsString>& s) { return s ? Value(*s) : Value::null(); };
    out.set(u"vertex", opt(e.vertex));
    out.set(u"fragment", opt(e.fragment));
    out.set(u"compute", opt(e.compute));
    return Value(std::move(out));
}

Value toValue(const std::vector<Binding>& bindings) {
    Array out;
    for (const Binding& b : bindings) {
        Object o;
        o.set(u"group", Value(b.group));
        o.set(u"binding", Value(b.binding));
        o.set(u"type", Value(b.type));
        o.set(u"name", Value(b.name));
        o.set(u"storage", Value(b.storage));
        o.set(u"typeDecl", Value(b.typeDecl));
        out.push_back(Value(std::move(o)));
    }
    return Value(std::move(out));
}

Value toValue(const OrderedMap<JsString, std::vector<double>>& entryPointBindings) {
    Array out;
    for (const auto& [entry, used] : entryPointBindings) {
        Array indices;
        for (double b : used) indices.push_back(Value(b));
        out.push_back(Value(Array{Value(entry), Value(std::move(indices))}));
    }
    return Value(std::move(out));
}

Value toValue(const PackedLayout& layout) {
    if (layout.kind == PackedLayout::Kind::None) return Value::null();
    Array fields;
    if (layout.kind == PackedLayout::Kind::Slots) {
        for (const SlotField& f : layout.slots) {
            Object o;
            o.set(u"name", Value(f.name));
            o.set(u"slot", Value(f.slot));
            o.set(u"components", Value(f.components));
            fields.push_back(Value(std::move(o)));
        }
        return Value(std::move(fields));
    }
    for (const ByteField& f : layout.bytes) {
        Object o;
        o.set(u"name", Value(f.name));
        o.set(u"offset", Value(f.offset));
        o.set(u"size", Value(f.size));
        o.set(u"type", Value(f.type));
        o.set(u"components", Value(f.components));
        fields.push_back(Value(std::move(o)));
    }
    Object out;
    out.set(u"type", Value(u"byte"));
    out.set(u"layout", Value(std::move(fields)));
    out.set(u"structSize", Value(layout.structSize));
    return Value(std::move(out));
}

Value toValue(const OrderedMap<JsString, std::optional<JsString>>& samplerTextures) {
    Array out;
    for (const auto& [sampler, texture] : samplerTextures) {
        out.push_back(Value(Array{Value(sampler), texture ? Value(*texture) : Value::null()}));
    }
    return Value(std::move(out));
}

}  // namespace libnoisemaker::wgsl
