// coop/dev/director/dup_verifier.h -- the container-race no-dup verifier: after a staged
// concurrent take, does the contested item X exist exactly ONCE across the whole save (the
// winner has it, the loser's take was refused) or TWICE?
//
// It COUNTS rather than confirms -- it walks the whole global saveSlot.GObjStack (every
// propInventory index) plus the player stores and prints every matching row, instead of asking a
// container "do you still have X?", which would query the very subsystem whose correctness it
// measures. X is matched by CONTENT SIGNATURE, read off the engine save independently of
// container_contents_sync.
//
// A verdict counts only behind a positive control: a solo run where a dup is impossible must
// count exactly 1, or count == 1 on a race cannot separate "no dup" from "instrument blind". The
// control also proves X is unique in the world and that the walk sees both stores.
//
// Dev only (RULE 3). Game thread. No dependency on the container-sync lane.

#pragma once

#include "ue_wrap/actors/save_record.h"

#include <cstdint>
#include <string>

namespace coop::director {

// The content signature of a saved item -- class, key and a hash over the value groups. Two
// records are "the same X" iff their signatures are equal. Deliberately NOT the container eid or
// the GObjStack index, which are the subsystem under test.
struct ItemSig {
    std::wstring className;
    std::wstring key;
    uint64_t     contentHash = 0;
    bool         valid       = false;
    bool operator==(const ItemSig& o) const {
        return valid && o.valid && className == o.className && key == o.key && contentHash == o.contentHash;
    }
};

// The signature of a Fstruct_save POD record.
ItemSig SigOf(const ue_wrap::save_record::SaveRecord& rec);

// Capture X's signature from a live container's propInventory GObjStack slice at slot `slotIdx`,
// reading the raw slice rather than the sync lane. Invalid ItemSig if unresolvable or out of
// range.
ItemSig CaptureContainerSlotSig(void* containerActor, int32_t slotIdx);

// Count instances of `x` across the WHOLE global GObjStack (all propInventory indices) plus the
// player stores. When `print`, logs every matching row with its location and a scan summary.
// Returns the total match count, or -1 if the saveSlot is unresolvable. Game thread.
int CountItemInstances(const ItemSig& x, bool print);

}  // namespace coop::director
