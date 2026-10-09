// l10n/fmt.cpp -- l10n::Fmt and FmtSpan (declared in l10n/l10n.h): our own positional formatter.
//
// MTA formats a translated string with SString::Printf, which has no argument numbers; a translation
// here may reorder its arguments, and the chat feed must know where the actor's name landed to colour
// it. The CRT's positional `_vsprintf_p` would reorder, but it terminates the process on a mixed
// format, a numbering gap or `%n` (measured on the UCRT) and reports no spans. So the format is read
// with printf_check's grammar, the arguments are taken from the va_list in their number order by the
// types the format names, and each conversion is written by snprintf from its own unnumbered spec.

#include "l10n/l10n.h"
#include "printf_check.h"
#include "quiet.h"

#include "ue_wrap/core/log.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace l10n {
namespace {

namespace pc = printf_check;

union Slot {
    long long          i;
    unsigned long long u;
    double             d;
    const char*        s;
};

// The bytes kept end on a whole UTF-8 sequence: a cut inside one would leave an invalid tail.
size_t CutUtf8(const char* p, size_t n) {
    size_t k = n;
    while (k > 0 && (static_cast<unsigned char>(p[k - 1]) & 0xC0) == 0x80) --k;   // continuation bytes
    if (k == 0) return 0;
    const unsigned char lead = static_cast<unsigned char>(p[k - 1]);
    const size_t need = lead < 0x80 ? 1 : (lead >> 5) == 0x6 ? 2 : (lead >> 4) == 0xE ? 3 : 4;
    return (n - (k - 1) >= need) ? n : k - 1;
}

void Read(const pc::Conv& c, va_list& ap, Slot& s) {
    switch (c.type) {
    case 'd': case 'i':
        switch (c.len) {
        case pc::Len::None: s.i = va_arg(ap, int); break;
        case pc::Len::L:    s.i = va_arg(ap, long); break;
        case pc::Len::LL:   s.i = va_arg(ap, long long); break;
        case pc::Len::Z:    s.i = static_cast<long long>(va_arg(ap, ptrdiff_t)); break;
        }
        break;
    case 'u': case 'x': case 'X': case 'o':
        switch (c.len) {
        case pc::Len::None: s.u = va_arg(ap, unsigned int); break;
        case pc::Len::L:    s.u = va_arg(ap, unsigned long); break;
        case pc::Len::LL:   s.u = va_arg(ap, unsigned long long); break;
        case pc::Len::Z:    s.u = static_cast<unsigned long long>(va_arg(ap, size_t)); break;
        }
        break;
    case 's': s.s = va_arg(ap, const char*); break;
    default:  s.d = va_arg(ap, double); break;   // f F e E g G
    }
}

// One conversion, from its own spec without the argument number, into out[0..room).
int WriteOne(const char* fmt, const pc::Conv& c, const Slot& s, char* out, size_t room) {
    char spec[16];
    size_t n = 0;
    spec[n++] = '%';
    for (size_t k = c.specBegin; k < c.end && n + 1 < sizeof(spec); ++k) spec[n++] = fmt[k];
    spec[n] = '\0';
    switch (c.type) {
    case 'd': case 'i':
        switch (c.len) {
        case pc::Len::None: return std::snprintf(out, room, spec, static_cast<int>(s.i));
        case pc::Len::L:    return std::snprintf(out, room, spec, static_cast<long>(s.i));
        case pc::Len::LL:   return std::snprintf(out, room, spec, s.i);
        case pc::Len::Z:    return std::snprintf(out, room, spec, static_cast<ptrdiff_t>(s.i));
        }
        return 0;
    case 'u': case 'x': case 'X': case 'o':
        switch (c.len) {
        case pc::Len::None: return std::snprintf(out, room, spec, static_cast<unsigned int>(s.u));
        case pc::Len::L:    return std::snprintf(out, room, spec, static_cast<unsigned long>(s.u));
        case pc::Len::LL:   return std::snprintf(out, room, spec, s.u);
        case pc::Len::Z:    return std::snprintf(out, room, spec, static_cast<size_t>(s.u));
        }
        return 0;
    case 's': return std::snprintf(out, room, spec, s.s ? s.s : "(null)");
    default:  return std::snprintf(out, room, spec, s.d);
    }
}

int Format(char* buf, size_t size, Span* arg1, const char* fmt, va_list ap) {
    if (arg1) *arg1 = Span{};
    if (!buf || size == 0) return -1;
    buf[0] = '\0';
    if (!fmt) return -1;
    pc::Parsed p;
    const char* why = nullptr;
    const size_t flen = std::strlen(fmt);
    if (!pc::Parse(std::string_view(fmt, flen), &p, &why)) {
        // Translations are checked at load and our English by the template gate, so this is a
        // defect in a literal: said once, never per frame.
        static std::atomic<bool> s_said{false};
        if (!detail::g_selftestQuiet.load(std::memory_order_relaxed) && !s_said.exchange(true))
            UE_LOGE("l10n: a format the conversion grammar refuses (%s): '%.80s'", why ? why : "?", fmt);
        return -1;
    }
    Slot slots[pc::kMaxConvs + 1] = {};
    for (uint8_t a = 1; a <= p.args; ++a) Read(*pc::ForArg(p, a), ap, slots[a]);

    size_t pos = 0;
    const size_t cap = size - 1;   // the terminator's byte
    bool cut = false;
    auto put = [&](const char* src, size_t n) {
        if (cut) return;
        if (n > cap - pos) { n = cap - pos; cut = true; }
        std::memcpy(buf + pos, src, n);
        pos += n;
    };
    size_t at = 0;
    uint8_t k = 0;
    while (at < flen && !cut) {
        if (fmt[at] == '%' && at + 1 < flen && fmt[at + 1] == '%') { put("%", 1); at += 2; continue; }
        if (k < p.count && at == p.convs[k].begin) {
            const pc::Conv& c = p.convs[k++];
            const size_t start = pos;
            const int w = WriteOne(fmt, c, slots[c.arg], buf + pos, cap - pos + 1);
            if (w < 0) { cut = true; break; }
            if (static_cast<size_t>(w) > cap - pos) { pos = cap; cut = true; }
            else pos += static_cast<size_t>(w);
            if (c.arg == 1 && arg1) {
                arg1->begin = static_cast<int>(start);
                arg1->len = static_cast<int>(pos - start);
            }
            at = c.end;
            continue;
        }
        // A literal run up to the next '%'.
        size_t next = at + 1;
        while (next < flen && fmt[next] != '%') ++next;
        put(fmt + at, next - at);
        at = next;
    }
    if (cut) pos = CutUtf8(buf, pos);
    buf[pos] = '\0';
    if (arg1 && arg1->begin >= 0) {
        if (static_cast<size_t>(arg1->begin + arg1->len) > pos) arg1->len = static_cast<int>(pos) - arg1->begin;
        if (arg1->len <= 0) *arg1 = Span{};   // none of argument 1 was kept
    }
    return static_cast<int>(pos);
}

}  // namespace

int Fmt(char* buf, size_t size, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    const int n = Format(buf, size, nullptr, fmt, ap);
    va_end(ap);
    return n;
}

int FmtSpan(char* buf, size_t size, Span* arg1, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    const int n = Format(buf, size, arg1, fmt, ap);
    va_end(ap);
    return n;
}

}  // namespace l10n
