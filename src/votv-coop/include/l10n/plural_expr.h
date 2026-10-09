// l10n/plural_expr.h -- a catalogue's plural rule: the `Plural-Forms` header compiled once, at load.
//
// The header's C expression over `n` picks which of a translation's forms a count takes, and a
// language's rule is whatever its translators wrote there, so it is EVALUATED, as libintl evaluates
// it, rather than matched against a table of known strings -- tinygettext's table
// (vendor/tinygettext/plural_forms.cpp:50-85) misses the rule of 12 of MTA's own 43 locales. The
// grammar is GNU gettext's (plural.y): `n`, decimal integers, `( )`, `!`, `* / %`, `+ -`,
// `< > <= >=`, `== !=`, `&&`, `||`, `?:`, with C's precedence and associativity, over unsigned 64-bit
// values. The text is untrusted translator input: it is bounded in length and nesting, and compiled
// into a node array that Select walks, so no text is parsed per lookup.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace l10n::plural {

inline constexpr size_t   kMaxExprChars = 256;   // longer is refused with the header
inline constexpr unsigned kMaxNesting   = 32;    // each `(`, `!` and `?:` branch is one level
inline constexpr unsigned kMaxForms     = 8;     // nplurals above this is refused

class Rule {
public:
    // Compiles a `Plural-Forms` header value ("nplurals=3; plural=(n%10==1 ? 0 : ...);"). False with
    // the reason in `why` when it does not parse; the rule is then empty.
    bool Compile(std::string_view headerValue, std::string* why);

    // The form `n` takes: always below Count(). A division or modulo by zero in this evaluation,
    // or a result at or above Count(), is form 0, as libintl resets an out-of-range index.
    unsigned Select(uint64_t n) const;

    unsigned Count() const { return nplurals_; }
    bool Valid() const { return nplurals_ != 0; }

private:
    struct Node {
        uint8_t  op = 0;
        int32_t  a = -1, b = -1, c = -1;   // operand node indices
        uint64_t value = 0;                // a literal's value
    };
    friend class Parser;
    std::vector<Node> nodes_;
    int32_t  root_ = -1;
    unsigned nplurals_ = 0;
};

}  // namespace l10n::plural
