// l10n/printf_check.h -- the printf conversions a message may carry, read once by the catalogue's
// load (a translation against its msgid) and by l10n::Fmt (a format before it is written).
//
// A translation is untrusted text that reaches a formatter, so its conversions are a closed set:
// `%[n$][flags][width][.prec][length]type`, flags from `-+ #0` where C defines them for the type,
// width and precision of at most two decimal digits (no precision on `s`, which counts bytes and would
// cut a name), length `l` `ll` `z` with an integer type only, types `d i u x X o f F e E g G s`, and
// `%%`. `*` (an argument no call site passes), `%n` (a write through a pointer), `%c` (a NUL or half a
// UTF-8 sequence), wide `%ls` and every other length are refused. Numbering is all-unnumbered (the
// msgid's order) or all-numbered covering 1..k each exactly once: the CRT's positional printf
// terminates the process on a mixed format, a gap or `%n` (measured on the UCRT), and l10n::Fmt does
// not call it, but a format either formatter would choke on never loads.
#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace l10n::printf_check {

inline constexpr size_t kMaxConvs = 16;

enum class Len : uint8_t { None, L, LL, Z };

struct Conv {
    uint16_t begin = 0, end = 0;   // the conversion's bytes in the format, `%` to its type
    uint16_t specBegin = 0;        // the first byte after `%` and any `n$`: flags onward
    uint8_t  arg = 0;              // 1-based argument number
    char     type = 0;             // d i u x X o f F e E g G s
    Len      len = Len::None;
};

struct Parsed {
    std::array<Conv, kMaxConvs> convs{};   // in format order; `%%` is not one
    uint8_t count = 0;                     // conversions in the format
    uint8_t args = 0;                      // arguments they read (== count: each exactly once)
};

// Reads `fmt` with the grammar above. False with the reason in `why`; nothing allocates.
bool Parse(std::string_view fmt, Parsed* out, const char** why);

// A translation's conversions against its msgid's: the same argument count, and for each
// argument number the same type and length (`d` and `i` are one type); flags, width and precision
// may differ. False with the reason.
bool Matches(const Parsed& msgid, const Parsed& translation, const char** why);

// The conversion that reads argument `arg`, or null.
const Conv* ForArg(const Parsed& p, uint8_t arg);

}  // namespace l10n::printf_check
