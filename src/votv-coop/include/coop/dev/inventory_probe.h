// coop/dev/inventory_probe.h -- DEV-ONLY self-test for the inventory APPLY path (ini
// inventory_probe=1; OFF by default; never ships enabled). Not dev_gate-gated: it never sends or
// mutates cross-peer state, so the ini key is the only gate.
//
// The risky core is ue_wrap::inventory::ApplyToSaveObject, which hand-builds engine-owned
// TArray<Fstruct_save> -- engine-minted FStrings, interned FNames, FindClass'd UClasses, nested
// value groups -- through reflection::EngineAlloc. About 5 s after world-up this takes one shot at
// it in single-player: read the live saveSlot, serialize, deserialize, serialize again, apply the
// result back, read and serialize once more. It PASSES iff the three blobs match, and the apply
// touches only the saveSlot MIRROR arrays, so live gameplay is untouched until a save/load.
//
// A pass proves the engine-side write is structurally sound: no fault, no corruption, faithful
// FName, FString, UClass and group reconstruction. It does NOT prove items are usable after a real
// load -- that runs through loadObjects on a fresh world load, confirmed by a client rejoining and
// finding its items present and usable.

#pragma once

namespace coop::dev::inventory_probe {

// One-shot SP self-test (no-op unless ini inventory_probe=1). Game thread.
void Tick();

}  // namespace coop::dev::inventory_probe
