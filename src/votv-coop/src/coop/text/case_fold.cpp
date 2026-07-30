// coop/text/case_fold.cpp -- see coop/text/case_fold.h.

#include "coop/text/case_fold.h"

#include "ue_wrap/core/log.h"

#include <algorithm>
#include <cstddef>

namespace coop::text {
namespace {

struct Run { uint32_t begin, end; int32_t delta; };

// GENERATED from a frozen Unicode 14.0.0 table intersected with the repertoire.
// Sorted and disjoint -- the binary search below is only correct on that shape,
// and the generator emits it in codepoint order.
constexpr Run kRuns[] = {
#include "coop/text/case_fold.inc"
};

constexpr size_t kRunCount = sizeof(kRuns) / sizeof(kRuns[0]);

// An `#include` of a MISSING file is a compile error; an include of a file whose
// CONTENT was lost is not. A table that arrives empty folds nothing, and nothing
// downstream can tell that apart from an alphabet with no cased letters in it.
static_assert(kRunCount == 575, "case_fold.inc lost rows -- regenerate");

}  // namespace

uint32_t CaseFold(uint32_t cp) {
    const Run* end = kRuns + kRunCount;
    const Run* it = std::upper_bound(
        kRuns, end, cp, [](uint32_t k, const Run& r) { return k < r.begin; });
    if (it == kRuns) return cp;
    --it;
    if (cp > it->end) return cp;
    return static_cast<uint32_t>(static_cast<int64_t>(cp) + it->delta);
}

bool RunCaseFoldSelftest() {
    int pass = 0, total = 0;
    auto ok = [&](bool cond, const char* what) {
        ++total;
        if (cond) ++pass;
        else UE_LOGE("case-fold selftest: FAIL -- %s", what);
    };

    // The table is sorted and disjoint, which the binary search assumes rather
    // than checks. Asserted over the whole table, not sampled.
    bool ordered = true;
    for (size_t i = 1; i < kRunCount; ++i)
        if (kRuns[i].begin <= kRuns[i - 1].end) { ordered = false; break; }
    ok(ordered, "the run table is sorted and disjoint");

    ok(CaseFold(L'A') == L'a', "ASCII folds");
    ok(CaseFold(0x00C0) == 0x00E0, "Latin-1 folds");
    ok(CaseFold(0x0410) == 0x0430, "Cyrillic folds");

    // The four the hand table silently missed. Each is a whole script's worth of
    // players whose names did not deduplicate case-insensitively.
    ok(CaseFold(0x0391) == 0x03B1, "GREEK folds (hand table missed all 146)");
    ok(CaseFold(0x0531) == 0x0561, "ARMENIAN folds (hand table missed 38)");
    ok(CaseFold(0x10A0) == 0x2D00, "GEORGIAN folds (hand table missed 38)");
    ok(CaseFold(0x0100) == 0x0101, "LATIN EXTENDED-A folds (hand table missed 374)");

    ok(CaseFold(L'a') == L'a', "an already-lowercase codepoint is unchanged");
    ok(CaseFold(0x4E2D) == 0x4E2D, "an uncased codepoint is unchanged");
    ok(CaseFold(0x0020) == 0x0020, "a space is unchanged");

    UE_LOGI("case-fold selftest: %s (%d/%d) -- %zu runs",
            pass == total ? "PASS" : "FAIL", pass, total, kRunCount);
    return pass == total;
}

}  // namespace coop::text
