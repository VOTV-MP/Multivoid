// coop/save_identity_map.h -- Phase 1: the in-memory {objectsData-order -> eid} map for keyless save-loaded
// forms (chipPile + off-prop kerfur). docs/COOP_STABLE_ID_SIDECAR.md S2.1/S3; build plan S1.
//
// THE GOAL: give keyless save-loaded natives (chipPiles, off-prop kerfurs; Key==None for piles, untracked
// for jUuC kerfurs) a STABLE cross-peer identity = the host eid. The host builds this map at save-capture
// (the same blob the client loads), sends it as a save_transfer SIDECAR, and the client binds each loaded
// native to its host eid by SPAWN ORDER (1A PROVED the BeginDeferred thunk catches every keyless load-spawn).
//
// PHASE 1B (THIS FILE, first cut): HOST-side BUILD + LOG only, NO wire. Proves the map builds with the right
// entries (874 = 870 chipPile + 4 kerfurOff, eids == the S8.2 capture-eids) BEFORE any transport/bind. The
// SIDECAR transport (S2) + the client BindLocalNativeToHostEid (mini-design b) come AFTER 1B is verified.
//
// ORDER: the map is built by replaying saveObjects' gather -- GetAllActorsWithInterface(int_save_C), the
// SAME call + order saveObjects uses (bytecode saveObjects_dump.txt [32]) -- so the keyless entries are in
// objectsData order == the client's loadObjects spawn order. (1B skips the per-actor ignoreSave() gate that
// saveObjects applies: the keyless families are never ignoreSave'd, so every one appears regardless; the
// exact ignoreSave-filtered objectsData INDEX is a S3.3 bijection-backstop refinement, not needed by the
// spawn-order PRIMARY, and is added in step 2 if the bijection is ever built. `index` here is the int_save
// gather ordinal -- the keyless RELATIVE order is exact, which is all the spawn-order bind needs.)
//
// Game-thread only (built at save-capture, save_transfer::OnRequest, the same frame as saveObjects).
#pragma once

#include <cstdint>
#include <vector>

namespace coop::save_identity_map {

// The two keyless save-loaded families the map covers. Keyed forms (Aprop_C, keyed kerfurs) are NOT in the
// map -- they bind by their stable cross-peer key (S3.1), not by the eid map.
enum class Family : uint8_t { ChipPile = 0, KerfurOff = 1 };

// One entry per keyless save-loaded native. NO position, NO key: `index` (gather ordinal) is the anchor,
// `eid` (host-minted) is the identity. 9 bytes on the wire (S1) when the sidecar transport lands (step 2).
struct IdEntry {
    uint32_t index;   // int_save gather ordinal (== objectsData order for the keyless subsequence)
    uint32_t eid;     // host ElementId (the S8.2-stable capture-eid)
    uint8_t  family;  // Family
};

using IdMap = std::vector<IdEntry>;

// HOST (game thread, at save-capture): build `outMap` by replaying GetAllActorsWithInterface(int_save_C) and
// emitting an entry for every keyless-family actor (eid resolved via prop_element_tracker; self-seeded if
// unseeded, the same idempotent mint CollectTrackedPileTransforms uses). Logs a per-family summary. Returns
// the entry count. NO wire (Phase 1B). Leak-free (frees the engine-allocated OutActors array via EngineFree).
int BuildHostMap(IdMap& outMap);

}  // namespace coop::save_identity_map
