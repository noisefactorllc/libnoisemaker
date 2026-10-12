#include "gpu/wgsl/jsre.h"

#include <limits>
#include <stdexcept>
#include <utility>

// ECMAScript 2024 §22.2 for patterns without the u or v flag: characters are
// UTF-16 code units, \w \d \b are ASCII, \s is WhiteSpace and LineTerminator,
// `.` stops at the four line terminators, and the i flag canonicalizes with
// toUpperCase, keeping a non-ASCII character that would map to ASCII. The
// reference's patterns are ASCII, so folding ASCII letters is that rule.

namespace libnoisemaker::jsre {

namespace {

constexpr size_t kInfinity = std::numeric_limits<size_t>::max();

bool is_word(char16_t c) {
    return (c >= u'a' && c <= u'z') || (c >= u'A' && c <= u'Z') || (c >= u'0' && c <= u'9') || c == u'_';
}

bool is_space(char16_t c) {
    switch (c) {
        case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D: case 0x20: case 0xA0: case 0x1680:
        case 0x2028: case 0x2029: case 0x202F: case 0x205F: case 0x3000: case 0xFEFF:
            return true;
        default:
            return c >= 0x2000 && c <= 0x200A;
    }
}

bool is_line_terminator(char16_t c) { return c == 0x0A || c == 0x0D || c == 0x2028 || c == 0x2029; }

char16_t canonicalize(char16_t c) { return (c >= u'a' && c <= u'z') ? char16_t(c - 32) : c; }

[[noreturn]] void unsupported(const JsString& pattern, const char* what) {
    throw std::invalid_argument(std::string("jsre: unsupported ") + what + " in /" + js_to_utf8(pattern) + "/");
}

}  // namespace

struct Regex::Node {
    enum class Kind { Empty, Char, Any, Class, Start, End, WordBoundary, NotWordBoundary, Group, Sequence, Repeat };
    Kind kind = Kind::Empty;
    char16_t ch = 0;
    // Class: inclusive ranges, plus the predefined classes it names.
    std::vector<std::pair<char16_t, char16_t>> ranges;
    bool word = false, not_word = false, digit = false, not_digit = false, space = false, not_space = false;
    bool negated = false;
    // Group: capture index (0 when non-capturing) and its alternatives.
    size_t capture = 0;
    std::vector<std::unique_ptr<Node>> children;  // Group: alternatives; Sequence: items; Repeat: one atom
    size_t min = 0, max = 0;
    bool greedy = true;
    // The capture groups inside a repeated atom, reset on each iteration.
    size_t first_capture = 0, last_capture = 0;

    bool single_char() const { return kind == Kind::Char || kind == Kind::Any || kind == Kind::Class; }
};

namespace {

using Node = Regex::Node;
using Kind = Node::Kind;

class Parser {
public:
    Parser(const JsString& pattern) : p_(pattern) {}

    std::unique_ptr<Node> parse(size_t& groups) {
        auto node = disjunction();
        if (i_ != p_.size()) unsupported(p_, "')'");
        groups = groups_;
        return node;
    }

private:
    const JsString& p_;
    size_t i_ = 0;
    size_t groups_ = 0;

    bool at(char16_t c) const { return i_ < p_.size() && p_[i_] == c; }

    std::unique_ptr<Node> disjunction(size_t capture = 0) {
        auto group = std::make_unique<Node>();
        group->kind = Kind::Group;
        group->capture = capture;
        group->children.push_back(alternative());
        while (at(u'|')) {
            ++i_;
            group->children.push_back(alternative());
        }
        return group;
    }

    std::unique_ptr<Node> alternative() {
        auto seq = std::make_unique<Node>();
        seq->kind = Kind::Sequence;
        while (i_ < p_.size() && !at(u'|') && !at(u')')) seq->children.push_back(term());
        return seq;
    }

    std::unique_ptr<Node> term() {
        const size_t captures_before = groups_;
        auto atom_node = atom();
        if (atom_node->kind == Kind::Start || atom_node->kind == Kind::End ||
            atom_node->kind == Kind::WordBoundary || atom_node->kind == Kind::NotWordBoundary) {
            return atom_node;
        }
        size_t min = 0, max = 0;
        if (at(u'*')) { min = 0; max = kInfinity; ++i_; }
        else if (at(u'+')) { min = 1; max = kInfinity; ++i_; }
        else if (at(u'?')) { min = 0; max = 1; ++i_; }
        else if (at(u'{') && braced(min, max)) {}
        else return atom_node;
        auto rep = std::make_unique<Node>();
        rep->kind = Kind::Repeat;
        rep->min = min;
        rep->max = max;
        if (at(u'?')) { rep->greedy = false; ++i_; }
        rep->first_capture = captures_before + 1;
        rep->last_capture = groups_;
        rep->children.push_back(std::move(atom_node));
        return rep;
    }

    // {n}, {n,} or {n,m}; anything else is a literal brace in non-Unicode mode,
    // which the reference never writes.
    bool braced(size_t& min, size_t& max) {
        size_t j = i_ + 1;
        auto number = [&](size_t& out) {
            const size_t start = j;
            out = 0;
            while (j < p_.size() && p_[j] >= u'0' && p_[j] <= u'9') out = out * 10 + (p_[j++] - u'0');
            return j > start;
        };
        if (!number(min)) unsupported(p_, "literal '{'");
        max = min;
        if (j < p_.size() && p_[j] == u',') {
            ++j;
            if (!number(max)) max = kInfinity;
        }
        if (j >= p_.size() || p_[j] != u'}') unsupported(p_, "literal '{'");
        if (max < min) unsupported(p_, "repeat bounds out of order");
        i_ = j + 1;
        return true;
    }

    std::unique_ptr<Node> atom() {
        auto node = std::make_unique<Node>();
        const char16_t c = p_[i_++];
        switch (c) {
            case u'^': node->kind = Kind::Start; return node;
            case u'$': node->kind = Kind::End; return node;
            case u'.': node->kind = Kind::Any; return node;
            case u'(': {
                size_t capture = 0;
                if (at(u'?')) {
                    if (i_ + 1 < p_.size() && p_[i_ + 1] == u':') i_ += 2;
                    else unsupported(p_, "group syntax");
                } else {
                    capture = ++groups_;
                }
                auto group = disjunction(capture);
                if (!at(u')')) unsupported(p_, "unterminated group");
                ++i_;
                return group;
            }
            case u'[': return char_class();
            case u'\\': return escape(false);
            case u'*': case u'+': case u'?': case u'{': case u')': case u']': case u'}': case u'|':
                unsupported(p_, "syntax character");
            default:
                node->kind = Kind::Char;
                node->ch = c;
                return node;
        }
    }

    // An escape after the backslash; in a class, \b is a backspace.
    std::unique_ptr<Node> escape(bool in_class) {
        if (i_ >= p_.size()) unsupported(p_, "trailing backslash");
        auto node = std::make_unique<Node>();
        const char16_t e = p_[i_++];
        node->kind = Kind::Class;
        switch (e) {
            case u'w': node->word = true; return node;
            case u'W': node->not_word = true; return node;
            case u'd': node->digit = true; return node;
            case u'D': node->not_digit = true; return node;
            case u's': node->space = true; return node;
            case u'S': node->not_space = true; return node;
            case u'b':
                if (in_class) { node->kind = Kind::Char; node->ch = 0x08; return node; }
                node->kind = Kind::WordBoundary;
                return node;
            case u'B':
                if (in_class) unsupported(p_, "\\B in a class");
                node->kind = Kind::NotWordBoundary;
                return node;
            default: break;
        }
        node->kind = Kind::Char;
        switch (e) {
            case u'n': node->ch = 0x0A; break;
            case u't': node->ch = 0x09; break;
            case u'r': node->ch = 0x0D; break;
            case u'f': node->ch = 0x0C; break;
            case u'v': node->ch = 0x0B; break;
            case u'0':
                if (i_ < p_.size() && p_[i_] >= u'0' && p_[i_] <= u'9') unsupported(p_, "octal escape");
                node->ch = 0;
                break;
            default:
                if ((e >= u'1' && e <= u'9') || e == u'k' || e == u'c' || e == u'x' || e == u'u' || e == u'p' ||
                    e == u'P') {
                    unsupported(p_, "escape");
                }
                // Identity escape: \/ \. \( \[ \{ and the rest.
                node->ch = e;
                break;
        }
        return node;
    }

    std::unique_ptr<Node> char_class() {
        auto node = std::make_unique<Node>();
        node->kind = Kind::Class;
        if (at(u'^')) { node->negated = true; ++i_; }
        // In JavaScript `[]` matches nothing and `[^]` anything: `]` always closes.
        while (i_ < p_.size() && !at(u']')) {
            auto low = class_atom();
            if (at(u'-') && i_ + 1 < p_.size() && p_[i_ + 1] != u']' && low.has_value()) {
                ++i_;
                auto high = class_atom();
                if (!high.has_value() || *high < *low) unsupported(p_, "class range");
                node->ranges.emplace_back(*low, *high);
            } else if (low.has_value()) {
                node->ranges.emplace_back(*low, *low);
            } else {
                merge_class(*node);
            }
        }
        if (!at(u']')) unsupported(p_, "unterminated class");
        ++i_;
        return node;
    }

    // A single code unit, or nullopt after recording a class escape in pending_.
    std::optional<char16_t> class_atom() {
        const char16_t c = p_[i_++];
        if (c != u'\\') return c;
        auto escaped = escape(true);
        if (escaped->kind == Kind::Char) return escaped->ch;
        pending_ = std::move(escaped);
        return std::nullopt;
    }

    void merge_class(Node& into) {
        into.word |= pending_->word;
        into.not_word |= pending_->not_word;
        into.digit |= pending_->digit;
        into.not_digit |= pending_->not_digit;
        into.space |= pending_->space;
        into.not_space |= pending_->not_space;
        pending_.reset();
    }

    std::unique_ptr<Node> pending_;
};

bool class_has(const Node& node, char16_t c, bool ignore_case) {
    auto in_ranges = [&](char16_t x) {
        for (const auto& [low, high] : node.ranges) {
            if (x >= low && x <= high) return true;
        }
        return false;
    };
    bool hit = in_ranges(c) ||
               (node.word && is_word(c)) || (node.not_word && !is_word(c)) ||
               (node.digit && c >= u'0' && c <= u'9') || (node.not_digit && !(c >= u'0' && c <= u'9')) ||
               (node.space && is_space(c)) || (node.not_space && !is_space(c));
    if (!hit && ignore_case && !node.ranges.empty()) {
        // A class matches c when some member canonicalizes as c does.
        const char16_t cc = canonicalize(c);
        for (const auto& [low, high] : node.ranges) {
            for (char16_t letter = u'a'; letter <= u'z'; ++letter) {
                const char16_t upper = char16_t(letter - 32);
                if (upper != cc) continue;
                if ((letter >= low && letter <= high) || (upper >= low && upper <= high)) hit = true;
            }
        }
    }
    return node.negated ? !hit : hit;
}

bool char_matches(const Node& node, const JsString& input, bool ignore_case, size_t pos) {
    if (pos >= input.size()) return false;
    const char16_t c = input[pos];
    switch (node.kind) {
        case Kind::Char: return ignore_case ? canonicalize(c) == canonicalize(node.ch) : c == node.ch;
        case Kind::Any: return !is_line_terminator(c);
        case Kind::Class: return class_has(node, c, ignore_case);
        default: return false;
    }
}

bool word_at(const JsString& input, size_t pos) { return pos < input.size() && is_word(input[pos]); }

}  // namespace

// The pattern compiles to instructions for a backtracking machine whose
// stack lives on the heap. Split tries its first target and keeps the second
// for backtracking, so alternatives are explored in the depth-first order of
// the specification's matchers (ECMAScript §22.2.2): alternatives left to
// right, greedy repeats longest first, lazy ones shortest first. A repeated
// group clears its captures on each iteration and, past its minimum, fails an
// iteration that consumed nothing (RepeatMatcher).
struct Regex::Program {
    enum class Op { Atom, RepeatAtom, Begin, End, WordBoundary, NotWordBoundary, Split, Jump, Save, Clear, Mark, Progress, Match };
    struct Instruction {
        Op op;
        const Node* node = nullptr;  // Atom, RepeatAtom
        size_t a = 0, b = 0;         // Split: targets; Jump: target; Save: slot; Clear: first and last group; Mark, Progress: register
        size_t min = 0, max = 0;     // RepeatAtom
        bool greedy = true;          // RepeatAtom
    };
    std::vector<Instruction> code;
    size_t registers = 0;

    size_t emit(Instruction instruction) {
        code.push_back(instruction);
        return code.size() - 1;
    }

    void compile(const Node& node) {
        switch (node.kind) {
            case Kind::Empty: return;
            case Kind::Char:
            case Kind::Any:
            case Kind::Class: emit({Op::Atom, &node}); return;
            case Kind::Start: emit({Op::Begin}); return;
            case Kind::End: emit({Op::End}); return;
            case Kind::WordBoundary: emit({Op::WordBoundary}); return;
            case Kind::NotWordBoundary: emit({Op::NotWordBoundary}); return;
            case Kind::Sequence:
                for (const auto& child : node.children) compile(*child);
                return;
            case Kind::Group: {
                if (node.capture) emit({Op::Save, nullptr, 2 * node.capture});
                std::vector<size_t> exits;
                for (size_t i = 0; i < node.children.size(); ++i) {
                    size_t split = 0;
                    const bool last = i + 1 == node.children.size();
                    if (!last) split = emit({Op::Split});
                    if (!last) code[split].a = code.size();
                    compile(*node.children[i]);
                    if (!last) {
                        exits.push_back(emit({Op::Jump}));
                        code[split].b = code.size();
                    }
                }
                for (size_t jump : exits) code[jump].a = code.size();
                if (node.capture) emit({Op::Save, nullptr, 2 * node.capture + 1});
                return;
            }
            case Kind::Repeat: compile_repeat(node); return;
        }
    }

    void compile_repeat(const Node& rep) {
        const Node& atom = *rep.children[0];
        if (rep.max == 0) return;
        if (atom.single_char()) {
            Instruction instruction{Op::RepeatAtom, &atom};
            instruction.min = rep.min;
            instruction.max = rep.max;
            instruction.greedy = rep.greedy;
            emit(instruction);
            return;
        }
        auto iteration = [&](bool optional) {
            const size_t reg = registers++;
            if (optional) emit({Op::Mark, nullptr, reg});
            if (rep.first_capture <= rep.last_capture) emit({Op::Clear, nullptr, rep.first_capture, rep.last_capture});
            compile(atom);
            if (optional) emit({Op::Progress, nullptr, reg});
        };
        for (size_t i = 0; i < rep.min; ++i) iteration(false);
        if (rep.max == kInfinity) {
            const size_t loop = emit({Op::Split});
            const size_t body = code.size();
            iteration(true);
            emit({Op::Jump, nullptr, loop});
            const size_t done = code.size();
            code[loop].a = rep.greedy ? body : done;
            code[loop].b = rep.greedy ? done : body;
            return;
        }
        std::vector<size_t> splits;
        for (size_t i = rep.min; i < rep.max; ++i) {
            splits.push_back(emit({Op::Split}));
            code[splits.back()].a = code.size();  // body; the skip target is set below
            iteration(true);
        }
        const size_t done = code.size();
        for (size_t split : splits) {
            const size_t body = code[split].a;
            code[split].a = rep.greedy ? body : done;
            code[split].b = rep.greedy ? done : body;
        }
    }
};

namespace {

struct Thread {
    size_t pc;
    size_t pos;
    std::vector<size_t> slots;      // 2 per group; npos when unset
    std::vector<size_t> registers;  // positions marked at the start of optional iterations
    // A pending RepeatAtom: the next count to try from base.
    bool repeat = false;
    size_t base = 0, count = 0, limit = 0;
};

constexpr size_t kUnset = std::numeric_limits<size_t>::max();

}  // namespace

JsString Match::str(const JsString& input, size_t n) const {
    if (!has(n)) return {};
    return input.substr(groups[n]->first, groups[n]->second - groups[n]->first);
}

Regex::Regex(const JsString& pattern, const JsString& flags) : program_(std::make_unique<Program>()) {
    for (char16_t f : flags) {
        bool& flag = f == u'g' ? global_ : f == u'i' ? ignore_case_ : (unsupported(pattern, "flag"), global_);
        if (flag) unsupported(pattern, "repeated flag");
        flag = true;
    }
    if (ignore_case_) {
        // Case folding here covers ASCII letters, which is exact only for ASCII patterns.
        for (char16_t c : pattern) {
            if (c > 0x7F) unsupported(pattern, "non-ASCII character with the i flag");
        }
    }
    root_ = Parser(pattern).parse(groups_);
    program_->compile(*root_);
    program_->emit({Program::Op::Match});
}
Regex::~Regex() = default;
Regex::Regex(Regex&&) noexcept = default;
Regex& Regex::operator=(Regex&&) noexcept = default;
bool Regex::global() const { return global_; }

std::optional<Match> Regex::exec(const JsString& input, size_t start) const {
    using Op = Program::Op;
    const auto& code = program_->code;
    std::vector<Thread> stack;
    for (size_t first = start; first <= input.size(); ++first) {
        stack.clear();
        stack.push_back({0, first, std::vector<size_t>(2 * (groups_ + 1), kUnset),
                         std::vector<size_t>(program_->registers, kUnset)});
        while (!stack.empty()) {
            Thread t = std::move(stack.back());
            stack.pop_back();
            if (t.repeat) {
                // Resume a RepeatAtom at its next count.
                const auto& in = code[t.pc - 1];
                t.pos = t.base + t.count;
                if (in.greedy ? t.count > in.min : t.count < t.limit) {
                    Thread next = t;
                    next.count = in.greedy ? t.count - 1 : t.count + 1;
                    stack.push_back(std::move(next));
                }
                t.repeat = false;
            }
            bool alive = true;
            while (alive) {
                const auto& in = code[t.pc];
                switch (in.op) {
                    case Op::Atom:
                        if (char_matches(*in.node, input, ignore_case_, t.pos)) { ++t.pos; ++t.pc; }
                        else alive = false;
                        break;
                    case Op::RepeatAtom: {
                        size_t run = 0;
                        while (run < in.max && char_matches(*in.node, input, ignore_case_, t.pos + run)) ++run;
                        if (run < in.min) { alive = false; break; }
                        const size_t first_count = in.greedy ? run : in.min;
                        if (run > in.min) {
                            Thread next = t;
                            next.pc = t.pc + 1;
                            next.repeat = true;
                            next.base = t.pos;
                            next.count = in.greedy ? run - 1 : in.min + 1;
                            next.limit = run;
                            stack.push_back(std::move(next));
                        }
                        t.pos += first_count;
                        ++t.pc;
                        break;
                    }
                    case Op::Begin:
                        if (t.pos == 0) ++t.pc; else alive = false;
                        break;
                    case Op::End:
                        if (t.pos == input.size()) ++t.pc; else alive = false;
                        break;
                    case Op::WordBoundary:
                    case Op::NotWordBoundary: {
                        const bool boundary = (t.pos > 0 && word_at(input, t.pos - 1)) != word_at(input, t.pos);
                        if (boundary == (in.op == Op::WordBoundary)) ++t.pc; else alive = false;
                        break;
                    }
                    case Op::Split: {
                        Thread other = t;
                        other.pc = in.b;
                        stack.push_back(std::move(other));
                        t.pc = in.a;
                        break;
                    }
                    case Op::Jump: t.pc = in.a; break;
                    case Op::Save: t.slots[in.a] = t.pos; ++t.pc; break;
                    case Op::Clear:
                        for (size_t g = in.a; g <= in.b; ++g) t.slots[2 * g] = t.slots[2 * g + 1] = kUnset;
                        ++t.pc;
                        break;
                    case Op::Mark: t.registers[in.a] = t.pos; ++t.pc; break;
                    case Op::Progress:
                        if (t.pos == t.registers[in.a]) alive = false; else ++t.pc;
                        break;
                    case Op::Match: {
                        Match m;
                        m.groups.assign(groups_ + 1, std::nullopt);
                        m.groups[0] = std::make_pair(first, t.pos);
                        for (size_t g = 1; g <= groups_; ++g) {
                            if (t.slots[2 * g] != kUnset && t.slots[2 * g + 1] != kUnset)
                                m.groups[g] = std::make_pair(t.slots[2 * g], t.slots[2 * g + 1]);
                        }
                        return m;
                    }
                }
            }
        }
    }
    return std::nullopt;
}

std::vector<Match> match_all(const Regex& regex, const JsString& input) {
    std::vector<Match> out;
    size_t last = 0;
    while (last <= input.size()) {
        auto m = regex.exec(input, last);
        if (!m) break;
        last = m->end() == m->index() ? m->end() + 1 : m->end();
        out.push_back(std::move(*m));
    }
    return out;
}

JsString replace_all(const Regex& regex, const JsString& input, const JsString& replacement) {
    if (replacement.find(u'$') != JsString::npos) throw std::invalid_argument("jsre: $ in a replacement");
    JsString out;
    size_t copied = 0;
    for (const Match& m : match_all(regex, input)) {
        out.append(input, copied, m.index() - copied);
        out += replacement;
        copied = m.end();
    }
    out.append(input, copied, JsString::npos);
    return out;
}

}  // namespace libnoisemaker::jsre
