// jsre against V8: each row's expected group spans are what V8 returns for
// the same pattern, flags and input (RegExp with the d flag; matchAll for g).

#include "gpu/wgsl/jsre.h"

#include "check.h"

#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using Span = std::optional<std::pair<size_t, size_t>>;
Span G(size_t start, size_t end) { return std::make_pair(start, end); }
const Span None = std::nullopt;

struct Row {
    const char* id;
    libnoisemaker::JsString pattern;
    libnoisemaker::JsString flags;
    libnoisemaker::JsString input;
    std::vector<std::vector<Span>> matches;
};

const std::vector<Row> rows = {
    {"lazy", u"^array\\s*<\\s*(.+?)\\s*,\\s*(\\d+)\\s*>$", u"", u"array< f32 , 3>", {{G(0, 15), G(7, 10), G(13, 14)}}},
    {"greedy", u"^array\\s*<\\s*(.+)\\s*,\\s*(\\d+)\\s*>$", u"", u"array< f32 , 3>", {{G(0, 15), G(7, 11), G(13, 14)}}},
    {"lazy-star", u"a.*?b", u"", u"xaxxbyyb", {{G(1, 5)}}},
    {"greedy-star", u"a.*b", u"", u"xaxxbyyb", {{G(1, 8)}}},
    {"alternation-order", u"(a|ab)(c|bcd)(d*)", u"", u"abcd", {{G(0, 4), G(0, 1), G(1, 4), G(4, 4)}}},
    {"optional-group", u"(?:let\\s+)?(\\w+)\\s*=", u"", u"let  speed = 1", {{G(0, 12), G(5, 10)}}},
    {"word-boundary", u"\\btex\\b", u"g", u"tex texA a.tex tex2 (tex)", {{G(0, 3)}, {G(11, 14)}, {G(21, 24)}}},
    {"not-word-boundary", u"\\Btex", u"", u"mytex tex", {{G(2, 5)}}},
    {"unicode-space", u"a\\sb\\s+c", u"", u"a\u00a0b\u2003\u3000c", {{G(0, 6)}}},
    {"dot-stops", u"a.b", u"", u"a\u2028b a\u00e9b", {{G(4, 7)}}},
    {"ignore-case-class", u"[a-c]+", u"i", u"xxABcaZ", {{G(2, 6)}}},
    {"ignore-case-literal", u"vec3<F32>", u"i", u"VEC3<f32>", {{G(0, 9)}}},
    {"ignore-case-negated", u"[^a-z]+", u"i", u"abcDEF123", {{G(6, 9)}}},
    {"non-ascii-fold", u"S", u"i", u"\u017f s", {{G(2, 3)}}},
    {"repeat-capture-reset", u"(?:(a)|b)+", u"", u"ab", {{G(0, 2), None}}},
    {"nested-repeat", u"(?:@\\w+\\s*\\([^)]*\\)\\s*)*([A-Za-z_]\\w*)\\s*:", u"", u"@builtin(position) @location(0) pos: vec4f", {{G(0, 36), G(32, 35)}}},
    {"anchors", u"^(\\w+)$", u"", u"abc def", {}},
    {"class-escapes", u"[\\w.-]+", u"g", u"a-b.c d_e", {{G(0, 5)}, {G(6, 9)}}},
    {"braces", u"x{2,3}", u"g", u"xxxxxxx", {{G(0, 3)}, {G(3, 6)}}},
    {"empty-class", u"a[]b", u"", u"a]b ab", {}},
    {"negated-empty-class", u"a[^]b", u"", u"a\nb", {{G(0, 3)}}},
    {"identity-escapes", u"\\/\\/[^\\n]*", u"g", u"a // b\nc // d", {{G(2, 6)}, {G(9, 13)}}},
    {"backtrack-into-group", u"(\\w+)\\s*:\\s*([^;]+)", u"", u"name : texture_2d<f32> ;", {{G(0, 23), G(0, 4), G(7, 23)}}},
    {"group-bounded-greedy", u"(ab){1,3}", u"", u"abababab", {{G(0, 6), G(4, 6)}}},
    {"group-bounded-lazy", u"(ab){1,3}?", u"", u"abababab", {{G(0, 2), G(0, 2)}}},
    {"group-bounded-lazy-then", u"(ab){1,3}?b", u"", u"ababab", {}},
    {"group-bounded-clear", u"(?:(a)|b){2,3}", u"", u"aba", {{G(0, 3), G(2, 3)}}},
    {"group-bounded-empty", u"(a?){2,3}x", u"", u"x", {{G(0, 1), G(0, 0)}}},
    {"group-star-empty", u"(a*)*b", u"", u"b", {{G(0, 1), None}}},
    {"group-star-lazy", u"(?:a|b)*?c", u"", u"abac", {{G(0, 4)}}},
    {"group-plus-lazy-alternation", u"(?:ab|a)+?b", u"", u"abab", {{G(0, 4)}}},
    {"group-optional-capture", u"(a)?b", u"", u"b", {{G(0, 1), None}}},
    {"group-star-lazy-empty", u"(?:ab)*?", u"", u"abab", {{G(0, 0)}}},
    {"group-plus-lazy", u"(ab)+?", u"", u"abab", {{G(0, 2), G(0, 2)}}},
    {"group-star-greedy", u"(ab)*", u"", u"abab", {{G(0, 4), G(2, 4)}}},
};

}  // namespace

int main() {
    using libnoisemaker::jsre::Regex;
    for (const Row& row : rows) {
        const Regex regex(row.pattern, row.flags);
        std::vector<std::vector<Span>> got;
        if (regex.global()) {
            for (const auto& m : libnoisemaker::jsre::match_all(regex, row.input)) got.push_back(m.groups);
        } else if (auto m = regex.exec(row.input)) {
            got.push_back(m->groups);
        }
        if (got != row.matches) {
            std::fprintf(stderr, "jsre row %s differs from V8\n", row.id);
            return 1;
        }
    }

    // Syntax beyond the reference's subset is refused, not guessed at.
    for (const char16_t* pattern : {u"a(?=b)", u"(a)\\1", u"(?<n>a)", u"\\p{L}", u"a{x", u"(a){2,1}"}) {
        bool refused = false;
        try { Regex unused(pattern); } catch (const std::invalid_argument&) { refused = true; }
        NM_CHECK(refused);
    }
    for (const char16_t* flags : {u"u", u"gg", u"ii", u"gig"}) {
        bool refused = false;
        try { Regex unused(u"a", flags); } catch (const std::invalid_argument&) { refused = true; }
        NM_CHECK(refused);
    }

    NM_CHECK(libnoisemaker::jsre::replace_all(Regex(u"\\s+", u"g"), u" vec4 < f32 > ", u"") == u"vec4<f32>");
    std::puts("jsre: ok");
    return 0;
}
