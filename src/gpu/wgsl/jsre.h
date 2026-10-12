#pragma once

// JavaScript regular expressions for the reference backend's WGSL analysis.
//
// The reference WebGPU backend reads shaders with JavaScript regular
// expressions, and their matches decide bindings, entry points and uniform
// packing. This is a backtracking matcher with JavaScript's non-Unicode
// semantics over UTF-16 code units, for the subset those patterns use:
// literals and escapes, classes, `.`, `\s \S \w \W \d \D \b \B`, `^` and `$`,
// capturing and non-capturing groups, alternation, and greedy and lazy
// quantifiers, with the flags g and i. Anything else (lookaround,
// backreferences, named groups, other flags) is rejected when the pattern
// compiles, so a new reference pattern beyond the subset fails loudly. The
// i flag folds ASCII letters only, so a non-ASCII pattern with it is refused.
// Backtracking keeps its stack on the heap, so input length never bounds
// recursion depth.

#include "core/value/js_string.h"

#include <cstddef>
#include <memory>
#include <optional>
#include <vector>

namespace libnoisemaker::jsre {

struct Match {
    // Group 0 is the whole match. A group that did not participate is nullopt.
    std::vector<std::optional<std::pair<size_t, size_t>>> groups;
    size_t index() const { return groups[0]->first; }
    size_t end() const { return groups[0]->second; }
    bool has(size_t n) const { return n < groups.size() && groups[n].has_value(); }
    JsString str(const JsString& input, size_t n) const;
};

class Regex {
public:
    Regex(const JsString& pattern, const JsString& flags = {});
    ~Regex();
    Regex(Regex&&) noexcept;
    Regex& operator=(Regex&&) noexcept;

    bool global() const;
    // RegExp.prototype.exec from `start` (the lastIndex of a global regex).
    std::optional<Match> exec(const JsString& input, size_t start = 0) const;
    // RegExp.prototype.test, from index 0 (a non-global regex ignores lastIndex).
    bool test(const JsString& input) const { return exec(input).has_value(); }

    struct Node;
    struct Program;

private:
    std::unique_ptr<Node> root_;
    std::unique_ptr<Program> program_;
    size_t groups_ = 0;
    bool global_ = false;
    bool ignore_case_ = false;
};

// String.prototype.match with a global regex: every match, advancing one code
// unit past an empty one.
std::vector<Match> match_all(const Regex& regex, const JsString& input);
// String.prototype.replace with a global regex and a replacement that
// contains no $ pattern.
JsString replace_all(const Regex& regex, const JsString& input, const JsString& replacement);

}  // namespace libnoisemaker::jsre
