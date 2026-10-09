// l10n/printf_check.cpp -- see l10n/printf_check.h.

#include "printf_check.h"

namespace l10n::printf_check {
namespace {

bool IsDigit(char c) { return c >= '0' && c <= '9'; }
bool IsIntType(char t) {
    return t == 'd' || t == 'i' || t == 'u' || t == 'x' || t == 'X' || t == 'o';
}
bool IsFloatType(char t) {
    return t == 'f' || t == 'F' || t == 'e' || t == 'E' || t == 'g' || t == 'G';
}
bool IsType(char t) { return IsIntType(t) || IsFloatType(t) || t == 's'; }
// `d` and `i` read the same argument the same way; every other type is its own.
char TypeClass(char t) { return t == 'i' ? 'd' : t; }

}  // namespace

bool Parse(std::string_view s, Parsed* out, const char** why) {
    *out = Parsed{};
    auto fail = [&](const char* w) { if (why) *why = w; return false; };
    if (s.size() > 0xFFFF) return fail("a format longer than 64 KiB");
    int numbered = -1;   // unknown until the first conversion: 1 numbered, 0 unnumbered
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '%') continue;
        const size_t begin = i++;
        if (i >= s.size()) return fail("a '%' at the end of the format");
        if (s[i] == '%') continue;
        Conv c;
        c.begin = static_cast<uint16_t>(begin);
        // An argument number is digits followed by '$'; anything else is the width or a flag.
        size_t j = i;
        unsigned num = 0;
        while (j < s.size() && IsDigit(s[j]) && j - i < 3) num = num * 10 + static_cast<unsigned>(s[j++] - '0');
        const bool hasNum = j > i && j < s.size() && s[j] == '$';
        if (hasNum) {
            if (num == 0 || num > kMaxConvs) return fail("an argument number outside 1..16");
            if (numbered == 0) return fail("numbered and unnumbered conversions mixed");
            numbered = 1;
            c.arg = static_cast<uint8_t>(num);
            i = j + 1;
        } else {
            if (numbered == 1) return fail("numbered and unnumbered conversions mixed");
            numbered = 0;
            c.arg = static_cast<uint8_t>(out->count + 1);
        }
        c.specBegin = static_cast<uint16_t>(i);
        bool zero = false, sign = false, alt = false, prec = false, left = false, plus = false, space = false;
        // Each flag at most once, so the longest spec (5 flags, 2 width, '.', 2 precision, 2 length,
        // the type) fits the formatter's buffer whole.
        for (; i < s.size(); ++i) {
            const char g = s[i];
            bool* seen = g == '0' ? &zero : g == '+' ? &plus : g == ' ' ? &space : g == '#' ? &alt
                       : g == '-' ? &left : nullptr;
            if (!seen) break;
            if (*seen) return fail("a repeated flag");
            *seen = true;
        }
        sign = plus || space;
        if (i < s.size() && s[i] == '*') return fail("a '*' width or precision");
        for (int d = 0; i < s.size() && IsDigit(s[i]); ++d, ++i)
            if (d == 2) return fail("a width of more than two digits");
        if (i < s.size() && s[i] == '.') {
            ++i;
            prec = true;
            if (i < s.size() && s[i] == '*') return fail("a '*' width or precision");
            for (int d = 0; i < s.size() && IsDigit(s[i]); ++d, ++i)
                if (d == 2) return fail("a precision of more than two digits");
        }
        if (i < s.size() && s[i] == 'l') {
            ++i;
            c.len = Len::L;
            if (i < s.size() && s[i] == 'l') { ++i; c.len = Len::LL; }
        } else if (i < s.size() && s[i] == 'z') {
            ++i;
            c.len = Len::Z;
        }
        if (i >= s.size()) return fail("a conversion without its type");
        const char t = s[i];
        if (t == 'n') return fail("a '%n' conversion");
        if (!IsType(t)) return fail("a conversion type outside d i u x X o f F e E g G s");
        if (c.len != Len::None && !IsIntType(t)) return fail("a length modifier on a non-integer type");
        // C leaves these pairings undefined; a precision on %s counts bytes and would cut a name.
        if (zero && t == 's') return fail("a '0' flag on %s");
        if (sign && !(t == 'd' || t == 'i' || IsFloatType(t)))
            return fail("a '+' or ' ' flag on an unsigned or string type");
        if (alt && !(t == 'x' || t == 'X' || t == 'o' || IsFloatType(t)))
            return fail("a '#' flag on a type it has no form for");
        if (prec && t == 's') return fail("a precision on %s");
        c.type = t;
        c.end = static_cast<uint16_t>(i + 1);
        if (out->count == kMaxConvs) return fail("more than 16 conversions");
        out->convs[out->count++] = c;
    }
    // Each argument exactly once: unnumbered is so by construction; numbered must cover 1..k.
    uint32_t seen = 0;
    unsigned top = 0;
    for (uint8_t k = 0; k < out->count; ++k) {
        const unsigned a = out->convs[k].arg;
        if (seen & (1u << a)) return fail("an argument read twice");
        seen |= 1u << a;
        if (a > top) top = a;
    }
    if (top != out->count) return fail("a gap in the argument numbers");
    out->args = out->count;
    return true;
}

const Conv* ForArg(const Parsed& p, uint8_t arg) {
    for (uint8_t k = 0; k < p.count; ++k)
        if (p.convs[k].arg == arg) return &p.convs[k];
    return nullptr;
}

bool Matches(const Parsed& msgid, const Parsed& tr, const char** why) {
    if (msgid.args != tr.args) {
        if (why) *why = "a different number of conversions than the msgid";
        return false;
    }
    for (uint8_t a = 1; a <= msgid.args; ++a) {
        const Conv* m = ForArg(msgid, a);
        const Conv* t = ForArg(tr, a);
        if (!m || !t || TypeClass(m->type) != TypeClass(t->type) || m->len != t->len) {
            if (why) *why = "a conversion whose type or length differs from the msgid's";
            return false;
        }
    }
    return true;
}

}  // namespace l10n::printf_check
