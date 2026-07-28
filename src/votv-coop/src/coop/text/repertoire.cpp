// coop/text/repertoire.cpp -- see coop/text/repertoire.h.

#include "coop/text/repertoire.h"

#include "ue_wrap/core/log.h"

namespace coop::text {
namespace {

// Both tables are GENERATED (tools/text/build_repertoire.py) from the fonts in
// assets/fonts. Regenerate -- never hand-edit -- when a family or the donor
// changes; the script re-derives the base-coverage gap and FAILS if it moved,
// because that set decides which names render and therefore which names fold.
constexpr CodepointRange kRepertoire[] = {
#include "coop/text/repertoire_ranges.inc"
};
constexpr CodepointRange kIgnorable[] = {
#include "coop/text/ignorable_ranges.inc"
};

constexpr size_t kRepertoireCount = sizeof(kRepertoire) / sizeof(kRepertoire[0]);
constexpr size_t kIgnorableCount  = sizeof(kIgnorable) / sizeof(kIgnorable[0]);

bool Contains(const CodepointRange* t, size_t n, uint32_t cp) {
    size_t lo = 0, hi = n;
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        if (cp < t[mid].begin)      hi = mid;
        else if (cp > t[mid].end)   lo = mid + 1;
        else                        return true;
    }
    return false;
}

}  // namespace

bool InRepertoire(uint32_t cp) { return Contains(kRepertoire, kRepertoireCount, cp); }

bool IsDefaultIgnorable(uint32_t cp) { return Contains(kIgnorable, kIgnorableCount, cp); }

const CodepointRange* RepertoireRanges(size_t* outCount) {
    if (outCount) *outCount = kRepertoireCount;
    return kRepertoire;
}

bool RunRepertoireSelftest() {
    int pass = 0, total = 0;
    auto ok = [&](bool cond, const char* what) {
        ++total;
        if (cond) ++pass;
        else UE_LOGE("repertoire selftest: FAIL -- %s", what);
    };

    // The binary search is only correct on a sorted, disjoint table, and the
    // table is generated -- so assert the shape here rather than trusting the
    // generator. A regenerated table that overlaps would make membership depend
    // on which half the search happened to land in.
    auto wellFormed = [&](const CodepointRange* t, size_t n, const char* what) {
        bool good = n > 0;
        for (size_t i = 0; i < n && good; ++i) {
            if (t[i].begin > t[i].end) good = false;
            if (i && t[i].begin <= t[i - 1].end) good = false;
        }
        ok(good, what);
    };
    wellFormed(kRepertoire, kRepertoireCount, "repertoire table is sorted and disjoint");
    wellFormed(kIgnorable, kIgnorableCount, "ignorable table is sorted and disjoint");

    // The four membership facts the fold rests on. Two IN (the scripts the user
    // asked for, and the emoji the donor exists for), two OUT (the script whose
    // absence is the whole reason the fold changed, and an invisible character).
    ok(InRepertoire(U'A') && InRepertoire(0x043F), "Latin and Cyrillic are in");
    ok(InRepertoire(0x1F600), "an astral emoji is in (donor + WCHAR32)");
    ok(!InRepertoire(0x4E2D), "hanzi is OUT -- it folds to the sentinel");
    ok(InRepertoire(0xFFFD),
       "U+FFFD is baked -- it is BOTH ImGui's fallback glyph and the fold sentinel, "
       "and nothing else in the range list asks for it");
    ok(!InRepertoire(0xFE0F) && !InRepertoire(0xE007F),
       "ignorables are OUT of the bake (they cost index tables and draw nothing)");
    // Private use arrived through the donor's cmap: Twemoji assigns U+E50A to its
    // own logo. A PUA codepoint means whatever ONE font says it means, so it can
    // never be a shared, uniqueness-bearing character in somebody's name.
    ok(!InRepertoire(0xE50A) && !InRepertoire(0xF8FF),
       "private-use is OUT (a vendor logo is not a character)");

    // The denylist row. U+034F is the measured hole: advance 0 in Fixedsys AND
    // Roboto, accepted mid-name, distinct fold key, zero pixels.
    ok(IsDefaultIgnorable(0x034F) && IsDefaultIgnorable(0x200B) &&
       IsDefaultIgnorable(0xFEFF) && IsDefaultIgnorable(0x3164),
       "the ignorable set covers CGJ, ZWSP, BOM and HANGUL FILLER");
    ok(!IsDefaultIgnorable(U' ') && !IsDefaultIgnorable(0x00A0),
       "a space is not ignorable, and neither is NBSP (it carries real advance)");

    // Nothing the atlas bakes may also be denied in a name: if a codepoint were
    // in both tables, a name could contain a character that renders -- or one
    // that does not -- depending on which check ran first.
    {
        bool disjoint = true;
        for (size_t i = 0; i < kRepertoireCount && disjoint; ++i)
            for (uint32_t c = kRepertoire[i].begin; c <= kRepertoire[i].end; ++c)
                if (IsDefaultIgnorable(c)) { disjoint = false; break; }
        ok(disjoint, "the repertoire and the ignorable set do not overlap");
    }

    UE_LOGI("repertoire selftest: %s (%d/%d) -- %zu ranges", pass == total ? "PASS" : "FAIL",
            pass, total, kRepertoireCount);
    return pass == total;
}

}  // namespace coop::text
