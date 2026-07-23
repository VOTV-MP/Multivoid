// coop/dev/director/dup_verifier.h -- the container-race NO-DUP verifier (director Phase-2).
//
// The ONLY instrument that can READ a staged concurrent container-take race: does the raced item
// X exist EXACTLY ONCE across the whole save after the race (winner has it, loser's take refused),
// or TWICE (a dup -- R11b's reasoned-but-unproven refusal-dup residual)?
//
// Instrument discipline (it is a tool, so the tool rules apply -- feedback_probe_must_count_not_confirm):
//  (a) it COUNTS, does not confirm: it walks the WHOLE global saveSlot.GObjStack (every propInventory
//      index) + the player stores (inventoryData/equipment/hold) and prints EVERY matching row -- it does
//      NOT ask a container "do you still have X?" by key/eid (that would query the very subsystem whose
//      correctness it measures). It matches X by CONTENT SIGNATURE (class + key + a hash of the value
//      groups), read straight off the engine save, independent of container_contents_sync.
//  (b) it demands a POSITIVE CONTROL before any race verdict is trusted: a solo run where a dup is
//      IMPOSSIBLE (one bot takes X once) must count EXACTLY 1. Without it, count==1 on a race cannot
//      distinguish "no dup" from "instrument blind" -- the exact R11b trap (a probe that counted on empty
//      containers and nearly reported a false green). The control also proves X is UNIQUE in the world
//      (a fungible X would count >1 with no dup) and that the walk sees BOTH the source (container) and
//      the destination (player) store an item moves between.
//
// DEV-ONLY (RULE 3). Game thread. Self-contained: no dependency on the container-sync lane.

#pragma once

#include "ue_wrap/actors/save_record.h"

#include <cstdint>
#include <string>

namespace coop::director {

// The content signature of a saved item -- class + key + a hash over the value groups. Two records are
// "the same X" iff their signatures are equal. Deliberately NOT the container eid / GObjStack index
// (that is the subsystem under test); this is derived only from the item's own serialized content.
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

// Capture X's signature from a live container's propInventory GObjStack slice at slot `slotIdx`.
// Reads the raw slice itself (not via the sync lane). Invalid ItemSig if unresolvable / out of range.
ItemSig CaptureContainerSlotSig(void* containerActor, int32_t slotIdx);

// Count instances of `x` across the WHOLE global GObjStack (all propInventory indices) + the player
// stores. When `print`, logs EVERY matching row with its location + a scan summary (rows scanned /
// slices / matches). Returns the total match count (-1 if the saveSlot is unresolvable). Game thread.
int CountItemInstances(const ItemSig& x, bool print);

}  // namespace coop::director
