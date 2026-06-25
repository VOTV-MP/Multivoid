// coop/save_identity_map.h -- Phase 1: the in-memory {objectsData-order -> eid} map for keyless save-loaded
// forms (chipPile + off-prop kerfur). docs/COOP_STABLE_ID_SIDECAR.md S2.1/S3; build plan S1.
//
// THE GOAL: give keyless save-loaded natives (chipPiles, off-prop kerfurs; Key==None for piles, untracked
// for jUuC kerfurs) a STABLE cross-peer identity = the host eid. The host builds this map at save-capture
// (the same blob the client loads), sends it as a save_transfer SIDECAR, and the client binds each loaded
// native to its host eid by SPAWN ORDER (1A PROVED the BeginDeferred thunk catches every keyless load-spawn).
//
// ORDER (Phase A, parse-objectsData -- the assumption-free fix after the 2b smoke): the map is built by
// reading the LIVE saveSlot->objectsData array (saveSlot+0x300) that saveObjects just rebuilt and the client's
// loadObjects replays IN INDEX ORDER. The objectsData index IS the stable cross-peer ordinal. (The earlier 1B
// cut re-gathered GetAllActorsWithInterface live at join time and used the gather ordinal -- the 2b smoke
// FALSIFIED that: the live GUObjectArray order (chip-first) != the captured objectsData order (kerfur-first),
// so the bind's family tripwire fired at k=0. Reading objectsData directly removes the order ASSUMPTION --
// don't assume the order, read it.) Filter: keyless (key_64 == None) chip/kerfur class -- exactly the client's
// fresh-spawn set (keyed entries adopt-by-key, no BeginDeferred). Each entry's host eid is resolved by a
// host-LOCAL exact class+location join against the live actors' eids (CollectTracked{Pile,Kerfur}Transforms,
// same capture instant); the location is the host-internal lookup key, NOT a cross-peer identity (that stays
// the eid). Same-class-same-location M>1 -> deterministic rank-pairing bijection (benign; see the .cpp).
//
// Game-thread only (built at save-capture, save_transfer::OnRequest, the same frame as saveObjects).
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace coop::save_identity_map {

// The two keyless save-loaded families the map covers. Keyed forms (Aprop_C, keyed kerfurs) are NOT in the
// map -- they bind by their stable cross-peer key (S3.1), not by the eid map.
enum class Family : uint8_t { ChipPile = 0, KerfurOff = 1 };

// One entry per keyless save-loaded native, EMITTED IN objectsData INDEX ORDER (== the client's loadObjects
// keyless fresh-spawn order). The bind correlates by SEQUENCE position (the k-th entry <-> the client's k-th
// keyless spawn); `index` is the objectsData index (logging + a future bijection backstop), `eid` the identity.
struct IdEntry {
    uint32_t index;   // objectsData array index (== client loadObjects spawn order for the keyless subsequence)
    uint32_t eid;     // host ElementId (the S8.2-stable capture-eid)
    uint8_t  family;  // Family
};

using IdMap = std::vector<IdEntry>;

// HOST (game thread, at save-capture): build `outMap` by reading the live saveSlot->objectsData array in index
// order, filtering keyless chip/kerfur entries, and joining each to its host eid by a host-local class+location
// match against the live actors (CollectTracked{Pile,Kerfur}Transforms). Logs a per-family summary + counts of
// keyed-excluded / unmatched / ambiguous-location entries. Returns the entry count. Reads engine-owned memory
// only (no allocation to free). Aborts (returns 0) if the struct_save stride sanity gate fails.
int BuildHostMap(IdMap& outMap);

// ---- Phase 2 sidecar wire framing (transport) ----------------------------------------------------------
// The map travels PREPENDED to the save-transfer blob stream (one stream, one CRC) so it can never desync
// from the blob it indexes. Self-describing layout (little-endian; the whole protocol is same-endian raw on
// x64 Windows): ['V','C','I','D'] magic | u32 version | u32 count | count x { u32 index, u32 eid, u8 family }.
inline constexpr uint8_t  kSidecarMagic[4]    = {'V', 'C', 'I', 'D'};
inline constexpr uint32_t kSidecarVersion     = 1u;
inline constexpr size_t   kSidecarHeaderBytes = 12u;  // magic(4) + version(4) + count(4)
inline constexpr size_t   kSidecarEntryBytes  = 9u;   // index(4) + eid(4) + family(1)

// HOST: serialize `map` into `out` (cleared first) as the framed sidecar -- always writes the 12-byte header,
// even for an empty map. `out.size()` == the value the host stamps into SaveTransferBeginPayload.sidecarBytes.
void SerializeSidecar(const IdMap& map, std::vector<uint8_t>& out);

// CLIENT: parse a framed sidecar from the first `len` bytes of `data`. On success fills `outMap`, sets
// `consumed` to the total sidecar byte length (header + entries), returns true. Returns false (outMap cleared,
// consumed=0) on a bad magic / unknown version / truncation -- the caller treats that as an unreadable map
// (but still strips sidecarBytes from the stream; the .sav blob follows regardless).
bool DeserializeSidecar(const uint8_t* data, size_t len, IdMap& outMap, size_t& consumed);

// CLIENT (Phase 2a dev checkpoint): log a received map (summary + first/last 5 entries) in the SAME shape as
// the host's BuildHostMap log, so the two can be eyeball-diffed line-for-line. NO bind (that is Phase 2b).
void LogReceivedMap(const IdMap& map);

}  // namespace coop::save_identity_map
