// coop/text/case_fold.h -- simple lowercase, over the alphabet this build draws.
//
// Owned here, beside repertoire.h, because it answers the same kind of question
// -- something about the set of codepoints this build can put on screen -- and
// it is minted by the same generator run, so what FOLDS and what RENDERS cannot
// drift.
//
// IT REPLACES A HAND TABLE THAT WENT STALE IN SILENCE (RULE 2). The old
// nickname_arbiter::FoldCase covered ASCII, Latin-1 and Cyrillic, and its comment
// called those "exactly the cased scripts the repertoire draws". True when
// written; falsified by the Latin-Ext/Greek widening and by the lazy-atlas flip's
// +4,741 codepoints, neither of which touched the function. Measured 2026-07-30:
// of 890 cased codepoints whose lowercase is ALSO drawable, 649 (73%) folded to
// THEMSELVES -- so `Ωμέγα` and `ωμέγα` did not collide, and neither did any pair
// in Greek, Armenian, Georgian, Coptic or most of Latin Extended. None folded
// WRONG; incomplete, never incorrect, which is why it was invisible.

#pragma once

#include <cstdint>

namespace coop::text {

// Simple 1:1 lowercase, restricted to pairs where BOTH codepoints are drawable
// (folding onto a codepoint the atlas cannot bake would map a visible character
// onto one that renders as the sentinel). Returns `cp` unchanged when it is
// uncased or its lowercase is not in the repertoire.
//
// Deliberately a compiled table and not LCMapStringW: this feeds an AUTHORITY
// decision, and an authority whose answer comes from the OS is one that can
// differ between two machines running the same build.
uint32_t CaseFold(uint32_t cp);

// Rows for the boot selftest. POSITIVE on purpose: a generated table that
// arrives EMPTY folds nothing, and "nothing collided" is what a healthy lobby
// looks like too -- there is no negative symptom to grep for.
bool RunCaseFoldSelftest();

}  // namespace coop::text
