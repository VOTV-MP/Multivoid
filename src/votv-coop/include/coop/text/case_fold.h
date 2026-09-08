// coop/text/case_fold.h -- simple lowercase, over the alphabet this build draws.
//
// Owned here, beside repertoire.h, because it answers the same kind of question -- something
// about the set of codepoints this build can put on screen -- and is minted by the same
// generator run, so what FOLDS and what RENDERS cannot drift.
//
// That coupling is the whole point. A hand-written table covering ASCII, Latin-1 and Cyrillic is
// correct only for the repertoire of the day it is written, and a widened repertoire breaks it
// with no symptom: of 890 cased codepoints whose lowercase is also drawable, 649 fold to
// THEMSELVES under such a table, so a Greek nickname does not collide with its own lowercase.
// Nothing folds WRONG -- incomplete, never incorrect, which is why there is nothing to grep for.

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
