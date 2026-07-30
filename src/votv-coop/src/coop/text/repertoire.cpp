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
constexpr CodepointRange kExclude[] = {
#include "coop/text/exclude_ranges.inc"
};

constexpr size_t kRepertoireCount = sizeof(kRepertoire) / sizeof(kRepertoire[0]);
constexpr size_t kIgnorableCount  = sizeof(kIgnorable) / sizeof(kIgnorable[0]);
constexpr size_t kExcludeCount    = sizeof(kExclude) / sizeof(kExclude[0]);

// The one property nothing downstream can detect: ImGui walks the exclude list
// as a zero-terminated ImWchar array, so a table beginning at U+0000 excludes
// NOTHING and every codepoint any face carries bakes -- fold != bake, silently,
// with both of ImGui's own asserts passing before NDEBUG strips them. The
// generator hard-fails on it; this catches a hand-edit of a generated file.
static_assert(kExclude[0].begin != 0,
              "exclude_ranges.inc must not begin at U+0000 -- ImGui reads it as a "
              "zero-terminated list and the whole table would become a no-op");

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

bool InExcludeSet(uint32_t cp) { return Contains(kExclude, kExcludeCount, cp); }

const CodepointRange* RepertoireRanges(size_t* outCount) {
    if (outCount) *outCount = kRepertoireCount;
    return kRepertoire;
}

const CodepointRange* ExcludeRanges(size_t* outCount) {
    if (outCount) *outCount = kExcludeCount;
    return kExclude;
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
    wellFormed(kExclude, kExcludeCount, "exclude table is sorted and disjoint");

    // The four membership facts the fold rests on. Two IN (the scripts the user
    // asked for, and the emoji the donor exists for), two OUT (the script whose
    // absence is the whole reason the fold changed, and an invisible character).
    ok(InRepertoire(U'A') && InRepertoire(0x043F), "Latin and Cyrillic are in");
    // Latin Extended-A/B + Greek (2026-07-29). Free: the embedded faces already
    // carried these, the bake just never asked. U+0141 L-stroke, U+015E S-cedilla,
    // U+0393 capital gamma -- Michal / Gunes / Giorgos spelled properly.
    ok(InRepertoire(0x0141) && InRepertoire(0x015E) && InRepertoire(0x0393),
       "Latin Extended-A/B and Greek are in");
    // U+03A2 sits inside the Greek block and is PERMANENTLY UNASSIGNED in Unicode,
    // so no font can carry it and the generator's gate lists it as an expected gap.
    // It must therefore be OUT: asking the atlas for a codepoint nothing can draw is
    // how a hole becomes a box on someone's screen.
    ok(!InRepertoire(0x03A2),
       "an unassigned Greek codepoint stays OUT (the block has holes; fonts do not)");
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

    // THE WIDENING, asserted as itself (2026-07-30, the 1.92 flip). These are not
    // new bytes in the DLL -- every one is in a face we already ship and the old
    // inclusion list simply never asked for it. They are the deliverable the user
    // can point at, so a regression that quietly narrows the repertoire back has
    // to fail here rather than show up as boxes in somebody's chat.
    ok(InRepertoire(0x2014) && InRepertoire(0x2019) && InRepertoire(0x201C) &&
       InRepertoire(0x2026) && InRepertoire(0x20BD),
       "em dash, curly quotes, ellipsis and the ruble sign draw (they used to be boxes)");
    ok(InRepertoire(0x05D0) && InRepertoire(0x0E01) && InRepertoire(0x0627),
       "Hebrew, Thai and Arabic draw -- the shipped faces always carried them");

    // THE EXCLUDE SET IS NOT `!InRepertoire`, and conflating the two would make
    // the pack-failure detector lie in both directions. Three states exist:
    //   in repertoire        -- a face has it and we bake it
    //   in the exclude set   -- a face has it and we REFUSE to bake it
    //   in neither           -- no face has it at all
    ok(InExcludeSet(0x0301) && !InRepertoire(0x0301),
       "a combining mark is excluded (commit 1 keeps today's behaviour; NFC admits "
       "it in commit 2)");
    ok(InExcludeSet(0xFE0F) && InExcludeSet(0x200B) && InExcludeSet(0xE007F),
       "the ignorables and the TAG block are excluded (they cost index tables and "
       "draw nothing)");
    ok(!InExcludeSet(U' ') && InRepertoire(U' '),
       "the space is NOT excluded -- it is Zs, and the generator carves it out by hand");
    ok(!InExcludeSet(0x4E00) && !InRepertoire(0x4E00),
       "hanzi is in NEITHER table: nothing forbade it, no face has it");
    ok(InRepertoire(0x055B) && !InExcludeSet(0x055B),
       "the adjudicated zero-advance residue (Armenian punctuation) is admitted");

    // The invariant the whole two-table construction exists for. Anything in both
    // would be folded as itself while the atlas refused to bake it -- a name that
    // is unique in the arbiter and a row of boxes on screen, which is precisely
    // the arc D2 defect arriving through the mechanism meant to prevent it.
    {
        bool disjoint = true;
        for (size_t i = 0; i < kRepertoireCount && disjoint; ++i)
            for (uint32_t c = kRepertoire[i].begin; c <= kRepertoire[i].end; ++c)
                if (InExcludeSet(c)) { disjoint = false; break; }
        ok(disjoint, "the fold set and the exclude set do not overlap (fold == bake)");
    }

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

    UE_LOGI("repertoire selftest: %s (%d/%d) -- %zu fold ranges, %zu exclude ranges",
            pass == total ? "PASS" : "FAIL", pass, total, kRepertoireCount, kExcludeCount);
    return pass == total;
}

}  // namespace coop::text
