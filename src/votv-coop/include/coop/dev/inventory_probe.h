// coop/dev/inventory_probe.h -- DEV-ONLY self-test for the v73 Inc-4 inventory APPLY path
// (ini inventory_probe=1; OFF by default; never ships enabled).
//
// The apply's risky core is ue_wrap::inventory::ApplyToSaveObject -- it hand-builds engine-owned
// TArray<Fstruct_save> (nested value groups, engine-minted FStrings, interned FNames, FindClass'd
// UClasses) via reflection::EngineAlloc. This probe exercises that machinery on the LOCAL player's
// OWN inventory in SINGLE-PLAYER, with no multiplayer wiring, and proves it round-trips without a
// crash or data loss:
//
//   ~5 s after world-up (saveSlot resolvable), ONE shot:
//     1. ReadAll(live saveSlot) -> inv0
//     2. Serialize(inv0) -> blob0; Deserialize(blob0) -> inv1; Serialize(inv1) -> blob1
//     3. ApplyToSaveObject(live saveSlot, inv1)   [overwrites the saveSlot MIRROR arrays;
//        NON-destructive to live gameplay -- obj_11 is untouched until a save/load]
//     4. ReadAll(live saveSlot) -> inv2; Serialize(inv2) -> blob2
//     5. PASS iff blob0 == blob1 == blob2 (the read->wire->apply->read loop is identity).
//
// PASS proves the engine-side write is structurally sound (no fault, no corruption, faithful
// FName/FString/UClass/group reconstruction). It does NOT prove "items are usable after a real
// load" -- that needs the materialize path (loadObjects) which only runs on a fresh world load;
// verify it via the multiplayer join hands-on (a client with inventory_apply=1 rejoining and
// confirming its items are present + usable).
//
// NOT dev_gate-gated (it never sends/mutates cross-peer state); the ini key is the only gate.

#pragma once

namespace coop::dev::inventory_probe {

// One-shot SP self-test (no-op unless ini inventory_probe=1). Game thread.
void Tick();

}  // namespace coop::dev::inventory_probe
