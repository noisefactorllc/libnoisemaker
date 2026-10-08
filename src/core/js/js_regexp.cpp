#include "js_regexp.h"

#include "js_unicode.h"
#include "text.h"


#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// js_regexp.cpp -- regular-expression literal validation. A C++ port of the
// RegExp validator in acorn 8.16 (acorn/dist/acorn.js, `pp$1.regexp_*`),
// whose results js_syntax.cpp's differential tests compare against V8. The
// acorn code carries this notice:
//
//   MIT License
//
//   Copyright (C) 2012-2022 by various contributors (see AUTHORS)
//
//   Permission is hereby granted, free of charge, to any person obtaining a
//   copy of this software and associated documentation files (the
//   "Software"), to deal in the Software without restriction, including
//   without limitation the rights to use, copy, modify, merge, publish,
//   distribute, sublicense, and/or sell copies of the Software, and to permit
//   persons to whom the Software is furnished to do so, subject to the
//   following conditions:
//
//   The above copyright notice and this permission notice shall be included
//   in all copies or substantial portions of the Software.
//
//   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
//   OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
//   MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN
//   NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
//   DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
//   OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
//   USE OR OTHER DEALINGS IN THE SOFTWARE.

namespace nm::js {

namespace {

struct RegexError {
    JsText message;
};

struct RegexTooDeep {};

// Groups nested deeper than this are not decided (the validator recurses
// once per group).
constexpr int kMaxRegexDepth = 400;

std::unordered_set<JsText> words(const char* list) {
    std::unordered_set<JsText> out;
    for (const JsText& w : JsText::fromLatin1(list).split(u' ', true)) out.insert(w);
    return out;
}

// ECMA-262 tables of Unicode property names and values (acorn's
// unicode-property-data, Script values through Unicode 17). V8 rejects the
// Script value Katakana_Or_Hiragana (Hrkt), which acorn lists, so it is
// omitted; every name and value here and every four-letter Script code was
// checked against V8.
const char* const kBinaryProperties =
    "ASCII ASCII_Hex_Digit AHex Alphabetic Alpha Any Assigned Bidi_Control Bidi_C Bidi_Mirrored Bidi_M "
    "Case_Ignorable CI Cased Changes_When_Casefolded CWCF Changes_When_Casemapped CWCM Changes_When_Lowercased CWL "
    "Changes_When_NFKC_Casefolded CWKCF Changes_When_Titlecased CWT Changes_When_Uppercased CWU Dash "
    "Default_Ignorable_Code_Point DI Deprecated Dep Diacritic Dia Emoji Emoji_Component Emoji_Modifier "
    "Emoji_Modifier_Base Emoji_Presentation Extender Ext Grapheme_Base Gr_Base Grapheme_Extend Gr_Ext Hex_Digit Hex "
    "IDS_Binary_Operator IDSB IDS_Trinary_Operator IDST ID_Continue IDC ID_Start IDS Ideographic Ideo Join_Control "
    "Join_C Logical_Order_Exception LOE Lowercase Lower Math Noncharacter_Code_Point NChar Pattern_Syntax Pat_Syn "
    "Pattern_White_Space Pat_WS Quotation_Mark QMark Radical Regional_Indicator RI Sentence_Terminal STerm "
    "Soft_Dotted SD Terminal_Punctuation Term Unified_Ideograph UIdeo Uppercase Upper Variation_Selector VS "
    "White_Space space XID_Continue XIDC XID_Start XIDS Extended_Pictographic EBase EComp EMod EPres ExtPict";
const char* const kBinaryPropertiesOfStrings =
    "Basic_Emoji Emoji_Keycap_Sequence RGI_Emoji_Modifier_Sequence RGI_Emoji_Flag_Sequence RGI_Emoji_Tag_Sequence "
    "RGI_Emoji_ZWJ_Sequence RGI_Emoji";
const char* const kGeneralCategoryValues =
    "Cased_Letter LC Close_Punctuation Pe Connector_Punctuation Pc Control Cc cntrl Currency_Symbol Sc "
    "Dash_Punctuation Pd Decimal_Number Nd digit Enclosing_Mark Me Final_Punctuation Pf Format Cf "
    "Initial_Punctuation Pi Letter L Letter_Number Nl Line_Separator Zl Lowercase_Letter Ll Mark M Combining_Mark "
    "Math_Symbol Sm Modifier_Letter Lm Modifier_Symbol Sk Nonspacing_Mark Mn Number N Open_Punctuation Ps Other C "
    "Other_Letter Lo Other_Number No Other_Punctuation Po Other_Symbol So Paragraph_Separator Zp Private_Use Co "
    "Punctuation P punct Separator Z Space_Separator Zs Spacing_Mark Mc Surrogate Cs Symbol S Titlecase_Letter Lt "
    "Unassigned Cn Uppercase_Letter Lu";
const char* const kScriptValues =
    "Adlam Adlm Ahom Anatolian_Hieroglyphs Hluw Arabic Arab Armenian Armn Avestan Avst Balinese Bali Bamum Bamu "
    "Bassa_Vah Bass Batak Batk Bengali Beng Bhaiksuki Bhks Bopomofo Bopo Brahmi Brah Braille Brai Buginese Bugi "
    "Buhid Buhd Canadian_Aboriginal Cans Carian Cari Caucasian_Albanian Aghb Chakma Cakm Cham Cham Cherokee Cher "
    "Common Zyyy Coptic Copt Qaac Cuneiform Xsux Cypriot Cprt Cyrillic Cyrl Deseret Dsrt Devanagari Deva Duployan "
    "Dupl Egyptian_Hieroglyphs Egyp Elbasan Elba Ethiopic Ethi Georgian Geor Glagolitic Glag Gothic Goth Grantha "
    "Gran Greek Grek Gujarati Gujr Gurmukhi Guru Han Hani Hangul Hang Hanunoo Hano Hatran Hatr Hebrew Hebr Hiragana "
    "Hira Imperial_Aramaic Armi Inherited Zinh Qaai Inscriptional_Pahlavi Phli Inscriptional_Parthian Prti Javanese "
    "Java Kaithi Kthi Kannada Knda Katakana Kana Kayah_Li Kali Kharoshthi Khar Khmer Khmr Khojki Khoj Khudawadi Sind "
    "Lao Laoo Latin Latn Lepcha Lepc Limbu Limb Linear_A Lina Linear_B Linb Lisu Lisu Lycian Lyci Lydian Lydi "
    "Mahajani Mahj Malayalam Mlym Mandaic Mand Manichaean Mani Marchen Marc Masaram_Gondi Gonm Meetei_Mayek Mtei "
    "Mende_Kikakui Mend Meroitic_Cursive Merc Meroitic_Hieroglyphs Mero Miao Plrd Modi Mongolian Mong Mro Mroo "
    "Multani Mult Myanmar Mymr Nabataean Nbat New_Tai_Lue Talu Newa Newa Nko Nkoo Nushu Nshu Ogham Ogam Ol_Chiki "
    "Olck Old_Hungarian Hung Old_Italic Ital Old_North_Arabian Narb Old_Permic Perm Old_Persian Xpeo "
    "Old_South_Arabian Sarb Old_Turkic Orkh Oriya Orya Osage Osge Osmanya Osma Pahawh_Hmong Hmng Palmyrene Palm "
    "Pau_Cin_Hau Pauc Phags_Pa Phag Phoenician Phnx Psalter_Pahlavi Phlp Rejang Rjng Runic Runr Samaritan Samr "
    "Saurashtra Saur Sharada Shrd Shavian Shaw Siddham Sidd SignWriting Sgnw Sinhala Sinh Sora_Sompeng Sora "
    "Soyombo Soyo Sundanese Sund Syloti_Nagri Sylo Syriac Syrc Tagalog Tglg Tagbanwa Tagb Tai_Le Tale Tai_Tham Lana "
    "Tai_Viet Tavt Takri Takr Tamil Taml Tangut Tang Telugu Telu Thaana Thaa Thai Thai Tibetan Tibt Tifinagh Tfng "
    "Tirhuta Tirh Ugaritic Ugar Vai Vaii Warang_Citi Wara Yi Yiii Zanabazar_Square Zanb "
    "Dogra Dogr Gunjala_Gondi Gong Hanifi_Rohingya Rohg Makasar Maka Medefaidrin Medf Old_Sogdian Sogo Sogdian Sogd "
    "Elymaic Elym Nandinagari Nand Nyiakeng_Puachue_Hmong Hmnp Wancho Wcho "
    "Chorasmian Chrs Diak Dives_Akuru Khitan_Small_Script Kits Yezi Yezidi "
    "Cypro_Minoan Cpmn Old_Uyghur Ougr Tangsa Tnsa Toto Vithkuqi Vith "
    "Berf Beria_Erfe Gara Garay Gukh Gurung_Khema Kawi Kirat_Rai Krai Nag_Mundari Nagm "
    "Ol_Onal Onao Sidetic Sidt Sunu Sunuwar Tai_Yo Tayo Todhri Todr Tolong_Siki Tols Tulu_Tigalari Tutg Unknown Zzzz";

struct PropertyData {
    std::unordered_set<JsText> binary;          // binary properties and lone General_Category values
    std::unordered_set<JsText> binaryOfStrings; // v flag only
    std::unordered_set<JsText> generalCategory;
    std::unordered_set<JsText> script;
};

const PropertyData& propertyData() {
    static const PropertyData data = [] {
        PropertyData d;
        d.binary = words(kBinaryProperties);
        for (const auto& name : words(kGeneralCategoryValues)) d.binary.insert(name);
        d.binaryOfStrings = words(kBinaryPropertiesOfStrings);
        d.generalCategory = words(kGeneralCategoryValues);
        d.script = words(kScriptValues);
        return d;
    }();
    return data;
}

enum CharSet { CharSetNone = 0, CharSetOk = 1, CharSetString = 2 };

// Disjunction structure, to allow a duplicate group name in a separate
// alternative (ES2025 duplicate named groups).
struct Branch {
    const Branch* parent = nullptr;
    const Branch* base = nullptr;

    bool separatedFrom(const Branch* alt) const {
        for (const Branch* self = this; self; self = self->parent) {
            for (const Branch* other = alt; other; other = other->parent) {
                if (self->base == other->base && self != other) return true;
            }
        }
        return false;
    }
};

bool isSyntaxCharacter(long ch) {
    return ch == 0x24 || (ch >= 0x28 && ch <= 0x2B) || ch == 0x2E || ch == 0x3F || (ch >= 0x5B && ch <= 0x5E)
           || (ch >= 0x7B && ch <= 0x7D);
}
bool isDecimalDigit(long ch) { return ch >= 0x30 && ch <= 0x39; }
bool isHexDigit(long ch) {
    return (ch >= 0x30 && ch <= 0x39) || (ch >= 0x41 && ch <= 0x46) || (ch >= 0x61 && ch <= 0x66);
}
int hexToInt(long ch) {
    if (ch >= 0x41 && ch <= 0x46) return 10 + static_cast<int>(ch - 0x41);
    if (ch >= 0x61 && ch <= 0x66) return 10 + static_cast<int>(ch - 0x61);
    return static_cast<int>(ch - 0x30);
}
bool isOctalDigit(long ch) { return ch >= 0x30 && ch <= 0x37; }
bool isControlLetter(long ch) { return (ch >= 0x41 && ch <= 0x5A) || (ch >= 0x61 && ch <= 0x7A); }
bool isCharacterClassEscape(long ch) {
    return ch == 0x64 || ch == 0x44 || ch == 0x73 || ch == 0x53 || ch == 0x77 || ch == 0x57;
}
bool isUnicodePropertyNameCharacter(long ch) { return isControlLetter(ch) || ch == 0x5F; }
bool isUnicodePropertyValueCharacter(long ch) { return isUnicodePropertyNameCharacter(ch) || isDecimalDigit(ch); }
bool isRegularExpressionModifier(long ch) { return ch == 0x69 || ch == 0x6D || ch == 0x73; }
bool isRegExpIdentifierStart(long ch) {
    return ch >= 0 && (isIdStartCodePoint(static_cast<char32_t>(ch)) || ch == 0x24 || ch == 0x5F);
}
bool isRegExpIdentifierPart(long ch) {
    return ch >= 0 && (isIdPartCodePoint(static_cast<char32_t>(ch)) || ch == 0x24 || ch == 0x5F || ch == 0x200C || ch == 0x200D);
}
bool isClassSetReservedDoublePunctuatorCharacter(long ch) {
    return ch == 0x21 || (ch >= 0x23 && ch <= 0x26) || (ch >= 0x2A && ch <= 0x2C) || ch == 0x2E
           || (ch >= 0x3A && ch <= 0x40) || ch == 0x5E || ch == 0x60 || ch == 0x7E;
}
bool isClassSetSyntaxCharacter(long ch) {
    return ch == 0x28 || ch == 0x29 || ch == 0x2D || ch == 0x2F || (ch >= 0x5B && ch <= 0x5D) || (ch >= 0x7B && ch <= 0x7D);
}
bool isClassSetReservedPunctuator(long ch) {
    return ch == 0x21 || ch == 0x23 || ch == 0x25 || ch == 0x26 || ch == 0x2C || ch == 0x2D || (ch >= 0x3A && ch <= 0x3E)
           || ch == 0x40 || ch == 0x60 || ch == 0x7E;
}

JsText codePointToString(long cp) {
    if (cp < 0) return JsText();
    const char32_t c = static_cast<char32_t>(cp);
    return JsText::fromUcs4(&c, 1);
}

class RegexValidator {
public:
    RegexValidator(const JsText& pattern, const JsText& flags) : source_(pattern), flags_(flags) {
        const bool unicodeSets = flags.contains(u'v');
        const bool unicode = flags.contains(u'u');
        if (unicodeSets) {
            switchU_ = switchV_ = switchN_ = true;
        } else {
            switchU_ = unicode;
            switchN_ = unicode;
        }
    }

    void validate() {
        validateFlags();
        pattern();
        // A pattern with a GroupName is reparsed with the N parameter.
        if (!switchN_ && !groupNames_.empty()) {
            switchN_ = true;
            pattern();
        }
    }

private:
    JsText source_;
    JsText flags_;
    bool switchU_ = false;
    bool switchV_ = false;
    bool switchN_ = false;
    std::ptrdiff_t pos_ = 0;
    double lastIntValue_ = 0;
    JsText lastStringValue_;
    bool lastAssertionIsQuantifiable_ = false;
    double numCapturingParens_ = 0;
    double maxBackReference_ = 0;
    std::unordered_map<JsText, std::vector<const Branch*>> groupNames_;
    std::vector<JsText> backReferenceNames_;
    std::deque<Branch> branches_;
    const Branch* branchID_ = nullptr;
    int depth_ = 0;

    [[noreturn]] void raise(const JsText& message) const {
        throw RegexError{JsText(u"Invalid regular expression: /%1/: %2").arg(source_, message)};
    }

    // The code point at index i when the u (or forced) mode is on, else the
    // code unit; -1 past the end.
    long at(std::ptrdiff_t i, bool forceU = false) const {
        const std::ptrdiff_t l = source_.size();
        if (i < 0 || i >= l) return -1;
        const char16_t c = source_.at(i);
        if (!(forceU || switchU_) || c < 0xD800 || c > 0xDBFF || i + 1 >= l) return c;
        const char16_t next = source_.at(i + 1);
        if (next >= 0xDC00 && next <= 0xDFFF) return 0x10000 + ((static_cast<long>(c) - 0xD800) << 10) + (next - 0xDC00);
        return c;
    }
    std::ptrdiff_t nextIndex(std::ptrdiff_t i, bool forceU = false) const {
        const std::ptrdiff_t l = source_.size();
        if (i < 0 || i >= l) return l;
        const char16_t c = source_.at(i);
        if (!(forceU || switchU_) || c < 0xD800 || c > 0xDBFF || i + 1 >= l) return i + 1;
        const char16_t next = source_.at(i + 1);
        return (next >= 0xDC00 && next <= 0xDFFF) ? i + 2 : i + 1;
    }
    long current(bool forceU = false) const { return at(pos_, forceU); }
    long lookahead(bool forceU = false) const { return at(nextIndex(pos_, forceU), forceU); }
    void advance(bool forceU = false) { pos_ = nextIndex(pos_, forceU); }
    bool eat(long ch, bool forceU = false) {
        if (current(forceU) == ch) {
            advance(forceU);
            return true;
        }
        return false;
    }
    bool eatChars(std::initializer_list<long> chs) {
        std::ptrdiff_t pos = pos_;
        for (const long ch : chs) {
            const long cur = at(pos);
            if (cur == -1 || cur != ch) return false;
            pos = nextIndex(pos);
        }
        pos_ = pos;
        return true;
    }

    void validateFlags() {
        bool u = false;
        bool v = false;
        for (std::ptrdiff_t i = 0; i < static_cast<std::ptrdiff_t>(flags_.size()); ++i) {
            const char16_t flag = flags_.at(i);
            if (!JsText(u"dgimsuyv").contains(flag)) raise(JsText(u"Invalid regular expression flag"));
            if (flags_.indexOf(flag, i + 1) > -1) raise(JsText(u"Duplicate regular expression flag"));
            if (flag == u'u') u = true;
            if (flag == u'v') v = true;
        }
        if (u && v) raise(JsText(u"Invalid regular expression flag"));
    }

    const Branch* newBranch(const Branch* parent, const Branch* base) {
        branches_.push_back(Branch{parent, base});
        Branch& b = branches_.back();
        if (!base) b.base = &b;
        return &b;
    }

    void pattern() {
        pos_ = 0;
        lastIntValue_ = 0;
        lastStringValue_.clear();
        lastAssertionIsQuantifiable_ = false;
        numCapturingParens_ = 0;
        maxBackReference_ = 0;
        groupNames_.clear();
        backReferenceNames_.clear();
        branchID_ = nullptr;

        disjunction();

        if (pos_ != static_cast<std::ptrdiff_t>(source_.size())) {
            if (eat(0x29)) raise(JsText(u"Unmatched ')'"));
            if (eat(0x5D) || eat(0x7D)) raise(JsText(u"Lone quantifier brackets"));
            raise(JsText(u"Unexpected character"));
        }
        if (maxBackReference_ > numCapturingParens_) raise(JsText(u"Invalid escape"));
        for (const JsText& name : backReferenceNames_) {
            if (!groupNames_.contains(name)) raise(JsText(u"Invalid named capture referenced"));
        }
    }

    void disjunction() {
        if (++depth_ > kMaxRegexDepth) throw RegexTooDeep{};
        branchID_ = newBranch(branchID_, nullptr);
        alternative();
        while (eat(0x7C)) {
            branchID_ = newBranch(branchID_->parent, branchID_->base);
            alternative();
        }
        branchID_ = branchID_->parent;
        if (eatQuantifier(true)) raise(JsText(u"Nothing to repeat"));
        if (eat(0x7B)) raise(JsText(u"Lone quantifier brackets"));
        --depth_;
    }

    void alternative() {
        while (pos_ < static_cast<std::ptrdiff_t>(source_.size()) && eatTerm()) {
        }
    }

    bool eatTerm() {
        if (eatAssertion()) {
            if (lastAssertionIsQuantifiable_ && eatQuantifier()) {
                if (switchU_) raise(JsText(u"Invalid quantifier"));
            }
            return true;
        }
        if (switchU_ ? eatAtom() : eatExtendedAtom()) {
            eatQuantifier();
            return true;
        }
        return false;
    }

    bool eatAssertion() {
        const std::ptrdiff_t start = pos_;
        lastAssertionIsQuantifiable_ = false;
        if (eat(0x5E) || eat(0x24)) return true;
        if (eat(0x5C)) {
            if (eat(0x42) || eat(0x62)) return true;
            pos_ = start;
        }
        if (eat(0x28) && eat(0x3F)) {
            const bool lookbehind = eat(0x3C);
            if (eat(0x3D) || eat(0x21)) {
                disjunction();
                if (!eat(0x29)) raise(JsText(u"Unterminated group"));
                lastAssertionIsQuantifiable_ = !lookbehind;
                return true;
            }
        }
        pos_ = start;
        return false;
    }

    bool eatQuantifier(bool noError = false) {
        if (eatQuantifierPrefix(noError)) {
            eat(0x3F);
            return true;
        }
        return false;
    }

    bool eatQuantifierPrefix(bool noError) {
        return eat(0x2A) || eat(0x2B) || eat(0x3F) || eatBracedQuantifier(noError);
    }

    bool eatBracedQuantifier(bool noError) {
        const std::ptrdiff_t start = pos_;
        if (eat(0x7B)) {
            double min = 0;
            double max = -1;
            if (eatDecimalDigits()) {
                min = lastIntValue_;
                if (eat(0x2C) && eatDecimalDigits()) max = lastIntValue_;
                if (eat(0x7D)) {
                    if (max != -1 && max < min && !noError) raise(JsText(u"numbers out of order in {} quantifier"));
                    return true;
                }
            }
            if (switchU_ && !noError) raise(JsText(u"Incomplete quantifier"));
            pos_ = start;
        }
        return false;
    }

    bool eatAtom() {
        return eatPatternCharacters() || eat(0x2E) || eatReverseSolidusAtomEscape() || eatCharacterClass()
               || eatUncapturingGroup() || eatCapturingGroup();
    }

    bool eatReverseSolidusAtomEscape() {
        const std::ptrdiff_t start = pos_;
        if (eat(0x5C)) {
            if (eatAtomEscape()) return true;
            pos_ = start;
        }
        return false;
    }

    JsText eatModifiers() {
        JsText modifiers;
        long ch = 0;
        while ((ch = current()) != -1 && isRegularExpressionModifier(ch)) {
            modifiers += codePointToString(ch);
            advance();
        }
        return modifiers;
    }

    bool eatUncapturingGroup() {
        const std::ptrdiff_t start = pos_;
        if (eat(0x28)) {
            if (eat(0x3F)) {
                const JsText addModifiers = eatModifiers();
                const bool hasHyphen = eat(0x2D);
                if (!addModifiers.empty() || hasHyphen) {
                    for (std::ptrdiff_t i = 0; i < static_cast<std::ptrdiff_t>(addModifiers.size()); ++i) {
                        if (addModifiers.indexOf(addModifiers.at(i), i + 1) > -1) {
                            raise(JsText(u"Duplicate regular expression modifiers"));
                        }
                    }
                    if (hasHyphen) {
                        const JsText removeModifiers = eatModifiers();
                        if (addModifiers.empty() && removeModifiers.empty() && current() == 0x3A) {
                            raise(JsText(u"Invalid regular expression modifiers"));
                        }
                        for (std::ptrdiff_t i = 0; i < static_cast<std::ptrdiff_t>(removeModifiers.size()); ++i) {
                            if (removeModifiers.indexOf(removeModifiers.at(i), i + 1) > -1
                                || addModifiers.contains(removeModifiers.at(i))) {
                                raise(JsText(u"Duplicate regular expression modifiers"));
                            }
                        }
                    }
                }
                if (eat(0x3A)) {
                    disjunction();
                    if (eat(0x29)) return true;
                    raise(JsText(u"Unterminated group"));
                }
            }
            pos_ = start;
        }
        return false;
    }

    bool eatCapturingGroup() {
        if (eat(0x28)) {
            groupSpecifier();
            disjunction();
            if (eat(0x29)) {
                numCapturingParens_ += 1;
                return true;
            }
            raise(JsText(u"Unterminated group"));
        }
        return false;
    }

    bool eatExtendedAtom() {
        return eat(0x2E) || eatReverseSolidusAtomEscape() || eatCharacterClass() || eatUncapturingGroup()
               || eatCapturingGroup() || eatInvalidBracedQuantifier() || eatExtendedPatternCharacter();
    }

    bool eatInvalidBracedQuantifier() {
        if (eatBracedQuantifier(true)) raise(JsText(u"Nothing to repeat"));
        return false;
    }

    bool eatSyntaxCharacter() {
        const long ch = current();
        if (isSyntaxCharacter(ch)) {
            lastIntValue_ = static_cast<double>(ch);
            advance();
            return true;
        }
        return false;
    }

    bool eatPatternCharacters() {
        const std::ptrdiff_t start = pos_;
        long ch = 0;
        while ((ch = current()) != -1 && !isSyntaxCharacter(ch)) advance();
        return pos_ != start;
    }

    bool eatExtendedPatternCharacter() {
        const long ch = current();
        if (ch != -1 && ch != 0x24 && !(ch >= 0x28 && ch <= 0x2B) && ch != 0x2E && ch != 0x3F && ch != 0x5B && ch != 0x5E
            && ch != 0x7C) {
            advance();
            return true;
        }
        return false;
    }

    void groupSpecifier() {
        if (eat(0x3F)) {
            if (!eatGroupName()) raise(JsText(u"Invalid group"));
            auto known = groupNames_.find(lastStringValue_);
            if (known != groupNames_.end()) {
                for (const Branch* altID : known->second) {
                    if (!altID->separatedFrom(branchID_)) raise(JsText(u"Duplicate capture group name"));
                }
                known->second.push_back(branchID_);
            } else {
                groupNames_.insert_or_assign(lastStringValue_, std::vector<const Branch*>{branchID_});
            }
        }
    }

    bool eatGroupName() {
        lastStringValue_.clear();
        if (eat(0x3C)) {
            if (eatRegExpIdentifierName() && eat(0x3E)) return true;
            raise(JsText(u"Invalid capture group name"));
        }
        return false;
    }

    bool eatRegExpIdentifierName() {
        lastStringValue_.clear();
        if (eatRegExpIdentifierStart()) {
            lastStringValue_ += codePointToString(static_cast<long>(lastIntValue_));
            while (eatRegExpIdentifierPart()) lastStringValue_ += codePointToString(static_cast<long>(lastIntValue_));
            return true;
        }
        return false;
    }

    bool eatRegExpIdentifierStart() {
        const std::ptrdiff_t start = pos_;
        long ch = current(true);
        advance(true);
        if (ch == 0x5C && eatRegExpUnicodeEscapeSequence(true)) ch = static_cast<long>(lastIntValue_);
        if (isRegExpIdentifierStart(ch)) {
            lastIntValue_ = static_cast<double>(ch);
            return true;
        }
        pos_ = start;
        return false;
    }

    bool eatRegExpIdentifierPart() {
        const std::ptrdiff_t start = pos_;
        long ch = current(true);
        advance(true);
        if (ch == 0x5C && eatRegExpUnicodeEscapeSequence(true)) ch = static_cast<long>(lastIntValue_);
        if (isRegExpIdentifierPart(ch)) {
            lastIntValue_ = static_cast<double>(ch);
            return true;
        }
        pos_ = start;
        return false;
    }

    bool eatAtomEscape() {
        if (eatBackReference() || eatCharacterClassEscape() != CharSetNone || eatCharacterEscape()
            || (switchN_ && eatKGroupName())) {
            return true;
        }
        if (switchU_) {
            if (current() == 0x63) raise(JsText(u"Invalid unicode escape"));
            raise(JsText(u"Invalid escape"));
        }
        return false;
    }

    bool eatBackReference() {
        const std::ptrdiff_t start = pos_;
        if (eatDecimalEscape()) {
            const double n = lastIntValue_;
            if (switchU_) {
                if (n > maxBackReference_) maxBackReference_ = n;
                return true;
            }
            if (n <= numCapturingParens_) return true;
            pos_ = start;
        }
        return false;
    }

    bool eatKGroupName() {
        if (eat(0x6B)) {
            if (eatGroupName()) {
                backReferenceNames_.push_back(lastStringValue_);
                return true;
            }
            raise(JsText(u"Invalid named reference"));
        }
        return false;
    }

    bool eatCharacterEscape() {
        return eatControlEscape() || eatCControlLetter() || eatZero() || eatHexEscapeSequence()
               || eatRegExpUnicodeEscapeSequence(false) || (!switchU_ && eatLegacyOctalEscapeSequence())
               || eatIdentityEscape();
    }

    bool eatCControlLetter() {
        const std::ptrdiff_t start = pos_;
        if (eat(0x63)) {
            if (eatControlLetter()) return true;
            pos_ = start;
        }
        return false;
    }

    bool eatZero() {
        if (current() == 0x30 && !isDecimalDigit(lookahead())) {
            lastIntValue_ = 0;
            advance();
            return true;
        }
        return false;
    }

    bool eatControlEscape() {
        const long ch = current();
        long value = -1;
        if (ch == 0x74) value = 0x09;
        else if (ch == 0x6E) value = 0x0A;
        else if (ch == 0x76) value = 0x0B;
        else if (ch == 0x66) value = 0x0C;
        else if (ch == 0x72) value = 0x0D;
        if (value < 0) return false;
        lastIntValue_ = static_cast<double>(value);
        advance();
        return true;
    }

    bool eatControlLetter() {
        const long ch = current();
        if (isControlLetter(ch)) {
            lastIntValue_ = static_cast<double>(ch % 0x20);
            advance();
            return true;
        }
        return false;
    }

    bool eatRegExpUnicodeEscapeSequence(bool forceU) {
        const std::ptrdiff_t start = pos_;
        const bool switchU = forceU || switchU_;
        if (eat(0x75)) {
            if (eatFixedHexDigits(4)) {
                const double lead = lastIntValue_;
                if (switchU && lead >= 0xD800 && lead <= 0xDBFF) {
                    const std::ptrdiff_t leadSurrogateEnd = pos_;
                    if (eat(0x5C) && eat(0x75) && eatFixedHexDigits(4)) {
                        const double trail = lastIntValue_;
                        if (trail >= 0xDC00 && trail <= 0xDFFF) {
                            lastIntValue_ = (lead - 0xD800) * 0x400 + (trail - 0xDC00) + 0x10000;
                            return true;
                        }
                    }
                    pos_ = leadSurrogateEnd;
                    lastIntValue_ = lead;
                }
                return true;
            }
            if (switchU && eat(0x7B) && eatHexDigits() && eat(0x7D) && lastIntValue_ >= 0 && lastIntValue_ <= 0x10FFFF) {
                return true;
            }
            if (switchU) raise(JsText(u"Invalid unicode escape"));
            pos_ = start;
        }
        return false;
    }

    bool eatIdentityEscape() {
        if (switchU_) {
            if (eatSyntaxCharacter()) return true;
            if (eat(0x2F)) {
                lastIntValue_ = 0x2F;
                return true;
            }
            return false;
        }
        const long ch = current();
        if (ch != 0x63 && (!switchN_ || ch != 0x6B)) {
            lastIntValue_ = static_cast<double>(ch);
            advance();
            return true;
        }
        return false;
    }

    bool eatDecimalEscape() {
        lastIntValue_ = 0;
        long ch = current();
        if (ch >= 0x31 && ch <= 0x39) {
            do {
                lastIntValue_ = 10 * lastIntValue_ + static_cast<double>(ch - 0x30);
                advance();
            } while ((ch = current()) >= 0x30 && ch <= 0x39);
            return true;
        }
        return false;
    }

    int eatCharacterClassEscape() {
        const long ch = current();
        if (isCharacterClassEscape(ch)) {
            lastIntValue_ = -1;
            advance();
            return CharSetOk;
        }
        bool negate = false;
        if (switchU_ && ((negate = (ch == 0x50)) || ch == 0x70)) {
            lastIntValue_ = -1;
            advance();
            int result = CharSetNone;
            if (eat(0x7B) && (result = eatUnicodePropertyValueExpression()) != CharSetNone && eat(0x7D)) {
                if (negate && result == CharSetString) raise(JsText(u"Invalid property name"));
                return result;
            }
            raise(JsText(u"Invalid property name"));
        }
        return CharSetNone;
    }

    int eatUnicodePropertyValueExpression() {
        const std::ptrdiff_t start = pos_;
        if (eatUnicodePropertyName() && eat(0x3D)) {
            const JsText name = lastStringValue_;
            if (eatUnicodePropertyValue()) {
                const JsText value = lastStringValue_;
                validateUnicodePropertyNameAndValue(name, value);
                return CharSetOk;
            }
        }
        pos_ = start;
        if (eatUnicodePropertyValue()) return validateUnicodePropertyNameOrValue(lastStringValue_);
        return CharSetNone;
    }

    void validateUnicodePropertyNameAndValue(const JsText& name, const JsText& value) {
        const PropertyData& d = propertyData();
        const std::unordered_set<JsText>* values = nullptr;
        if (name == JsText(u"General_Category") || name == JsText(u"gc")) values = &d.generalCategory;
        else if (name == JsText(u"Script") || name == JsText(u"sc") || name == JsText(u"Script_Extensions")
                 || name == JsText(u"scx")) values = &d.script;
        if (!values) raise(JsText(u"Invalid property name"));
        if (!values->contains(value)) raise(JsText(u"Invalid property value"));
    }

    int validateUnicodePropertyNameOrValue(const JsText& nameOrValue) {
        const PropertyData& d = propertyData();
        if (d.binary.contains(nameOrValue)) return CharSetOk;
        if (switchV_ && d.binaryOfStrings.contains(nameOrValue)) return CharSetString;
        raise(JsText(u"Invalid property name"));
    }

    bool eatUnicodePropertyName() {
        long ch = 0;
        lastStringValue_.clear();
        while (isUnicodePropertyNameCharacter(ch = current())) {
            lastStringValue_ += codePointToString(ch);
            advance();
        }
        return !lastStringValue_.empty();
    }

    bool eatUnicodePropertyValue() {
        long ch = 0;
        lastStringValue_.clear();
        while (isUnicodePropertyValueCharacter(ch = current())) {
            lastStringValue_ += codePointToString(ch);
            advance();
        }
        return !lastStringValue_.empty();
    }

    bool eatCharacterClass() {
        if (eat(0x5B)) {
            const bool negate = eat(0x5E);
            const int result = classContents();
            if (!eat(0x5D)) raise(JsText(u"Unterminated character class"));
            if (negate && result == CharSetString) raise(JsText(u"Negated character class may contain strings"));
            return true;
        }
        return false;
    }

    int classContents() {
        if (current() == 0x5D) return CharSetOk;
        if (switchV_) return classSetExpression();
        nonEmptyClassRanges();
        return CharSetOk;
    }

    void nonEmptyClassRanges() {
        while (eatClassAtom()) {
            const double left = lastIntValue_;
            if (eat(0x2D) && eatClassAtom()) {
                const double right = lastIntValue_;
                if (switchU_ && (left == -1 || right == -1)) raise(JsText(u"Invalid character class"));
                if (left != -1 && right != -1 && left > right) raise(JsText(u"Range out of order in character class"));
            }
        }
    }

    bool eatClassAtom() {
        const std::ptrdiff_t start = pos_;
        if (eat(0x5C)) {
            if (eatClassEscape()) return true;
            if (switchU_) {
                const long ch = current();
                if (ch == 0x63 || isOctalDigit(ch)) raise(JsText(u"Invalid class escape"));
                raise(JsText(u"Invalid escape"));
            }
            pos_ = start;
        }
        const long ch = current();
        if (ch != 0x5D) {
            lastIntValue_ = static_cast<double>(ch);
            advance();
            return true;
        }
        return false;
    }

    bool eatClassEscape() {
        const std::ptrdiff_t start = pos_;
        if (eat(0x62)) {
            lastIntValue_ = 0x08;
            return true;
        }
        if (switchU_ && eat(0x2D)) {
            lastIntValue_ = 0x2D;
            return true;
        }
        if (!switchU_ && eat(0x63)) {
            if (eatClassControlLetter()) return true;
            pos_ = start;
        }
        return eatCharacterClassEscape() != CharSetNone || eatCharacterEscape();
    }

    int classSetExpression() {
        int result = CharSetOk;
        int subResult = CharSetNone;
        if (eatClassSetRange()) {
        } else if ((subResult = eatClassSetOperand()) != CharSetNone) {
            if (subResult == CharSetString) result = CharSetString;
            const std::ptrdiff_t start = pos_;
            while (eatChars({0x26, 0x26})) {
                if (current() != 0x26 && (subResult = eatClassSetOperand()) != CharSetNone) {
                    if (subResult != CharSetString) result = CharSetOk;
                    continue;
                }
                raise(JsText(u"Invalid character in character class"));
            }
            if (start != pos_) return result;
            while (eatChars({0x2D, 0x2D})) {
                if (eatClassSetOperand() != CharSetNone) continue;
                raise(JsText(u"Invalid character in character class"));
            }
            if (start != pos_) return result;
        } else {
            raise(JsText(u"Invalid character in character class"));
        }
        for (;;) {
            if (eatClassSetRange()) continue;
            subResult = eatClassSetOperand();
            if (subResult == CharSetNone) return result;
            if (subResult == CharSetString) result = CharSetString;
        }
    }

    bool eatClassSetRange() {
        const std::ptrdiff_t start = pos_;
        if (eatClassSetCharacter()) {
            const double left = lastIntValue_;
            if (eat(0x2D) && eatClassSetCharacter()) {
                const double right = lastIntValue_;
                if (left != -1 && right != -1 && left > right) raise(JsText(u"Range out of order in character class"));
                return true;
            }
            pos_ = start;
        }
        return false;
    }

    int eatClassSetOperand() {
        if (eatClassSetCharacter()) return CharSetOk;
        const int disjunctionResult = eatClassStringDisjunction();
        if (disjunctionResult != CharSetNone) return disjunctionResult;
        return eatNestedClass();
    }

    int eatNestedClass() {
        const std::ptrdiff_t start = pos_;
        if (eat(0x5B)) {
            const bool negate = eat(0x5E);
            const int result = classContents();
            if (eat(0x5D)) {
                if (negate && result == CharSetString) raise(JsText(u"Negated character class may contain strings"));
                return result;
            }
            pos_ = start;
        }
        if (eat(0x5C)) {
            const int result = eatCharacterClassEscape();
            if (result != CharSetNone) return result;
            pos_ = start;
        }
        return CharSetNone;
    }

    int eatClassStringDisjunction() {
        const std::ptrdiff_t start = pos_;
        if (eatChars({0x5C, 0x71})) {
            if (eat(0x7B)) {
                const int result = classStringDisjunctionContents();
                if (eat(0x7D)) return result;
            } else {
                raise(JsText(u"Invalid escape"));
            }
            pos_ = start;
        }
        return CharSetNone;
    }

    int classStringDisjunctionContents() {
        int result = classString();
        while (eat(0x7C)) {
            if (classString() == CharSetString) result = CharSetString;
        }
        return result;
    }

    int classString() {
        int count = 0;
        while (eatClassSetCharacter()) count++;
        return count == 1 ? CharSetOk : CharSetString;
    }

    bool eatClassSetCharacter() {
        const std::ptrdiff_t start = pos_;
        if (eat(0x5C)) {
            if (eatCharacterEscape() || eatClassSetReservedPunctuator()) return true;
            if (eat(0x62)) {
                lastIntValue_ = 0x08;
                return true;
            }
            pos_ = start;
            return false;
        }
        const long ch = current();
        if (ch < 0 || (ch == lookahead() && isClassSetReservedDoublePunctuatorCharacter(ch))) return false;
        if (isClassSetSyntaxCharacter(ch)) return false;
        advance();
        lastIntValue_ = static_cast<double>(ch);
        return true;
    }

    bool eatClassSetReservedPunctuator() {
        const long ch = current();
        if (isClassSetReservedPunctuator(ch)) {
            lastIntValue_ = static_cast<double>(ch);
            advance();
            return true;
        }
        return false;
    }

    bool eatClassControlLetter() {
        const long ch = current();
        if (isDecimalDigit(ch) || ch == 0x5F) {
            lastIntValue_ = static_cast<double>(ch % 0x20);
            advance();
            return true;
        }
        return false;
    }

    bool eatHexEscapeSequence() {
        const std::ptrdiff_t start = pos_;
        if (eat(0x78)) {
            if (eatFixedHexDigits(2)) return true;
            if (switchU_) raise(JsText(u"Invalid escape"));
            pos_ = start;
        }
        return false;
    }

    bool eatDecimalDigits() {
        const std::ptrdiff_t start = pos_;
        long ch = 0;
        lastIntValue_ = 0;
        while (isDecimalDigit(ch = current())) {
            lastIntValue_ = 10 * lastIntValue_ + static_cast<double>(ch - 0x30);
            advance();
        }
        return pos_ != start;
    }

    bool eatHexDigits() {
        const std::ptrdiff_t start = pos_;
        long ch = 0;
        lastIntValue_ = 0;
        while (isHexDigit(ch = current())) {
            lastIntValue_ = 16 * lastIntValue_ + hexToInt(ch);
            advance();
        }
        return pos_ != start;
    }

    bool eatLegacyOctalEscapeSequence() {
        if (eatOctalDigit()) {
            const double n1 = lastIntValue_;
            if (eatOctalDigit()) {
                const double n2 = lastIntValue_;
                if (n1 <= 3 && eatOctalDigit()) {
                    lastIntValue_ = n1 * 64 + n2 * 8 + lastIntValue_;
                } else {
                    lastIntValue_ = n1 * 8 + n2;
                }
            } else {
                lastIntValue_ = n1;
            }
            return true;
        }
        return false;
    }

    bool eatOctalDigit() {
        const long ch = current();
        if (isOctalDigit(ch)) {
            lastIntValue_ = static_cast<double>(ch - 0x30);
            advance();
            return true;
        }
        lastIntValue_ = 0;
        return false;
    }

    bool eatFixedHexDigits(int length) {
        const std::ptrdiff_t start = pos_;
        lastIntValue_ = 0;
        for (int i = 0; i < length; ++i) {
            const long ch = current();
            if (!isHexDigit(ch)) {
                pos_ = start;
                return false;
            }
            lastIntValue_ = 16 * lastIntValue_ + hexToInt(ch);
            advance();
        }
        return true;
    }
};

} // namespace

RegexCheck validateRegExpLiteral(const JsString& pattern, const JsString& flags, JsString* detail) {
    try {
        RegexValidator validator(pattern, flags);
        validator.validate();
    } catch (const RegexError& e) {
        *detail = e.message;
        return RegexCheck::Invalid;
    } catch (const RegexTooDeep&) {
        *detail = JsText(u"regular expression groups nested deeper than %1").arg(kMaxRegexDepth);
        return RegexCheck::Undecidable;
    }
    return RegexCheck::Valid;
}

} // namespace nm::js
