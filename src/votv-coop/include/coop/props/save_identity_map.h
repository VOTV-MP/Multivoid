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
#include <string>
#include <vector>

namespace coop::save_identity_map {

// The two save-loaded families the map covers. The map is the TRANSPORT for both (only it sees every save
// object regardless of runtime tracking -- a save-loaded off-kerfur is UNTRACKED/not-key-indexed at join, so
// the keyed mirror path S3.1 cannot resolve it; see COOP_STABLE_ID_SIDECAR.md S3.6). But the PAIRING RULE
// inside the map splits keyed-vs-keyless (sidecar v3, 2026-06-29): a kerfurOff carries a PORTABLE save key
// (`p0KP` byte-identical on both peers) -> the client pairs it native<->eid BY KEY (cross-peer-stable). A
// chipPile is genuinely keyless -> the client pairs it by per-family ORDINAL cursor (load order). Pairing the
// keyed family by load-order cursor was the 2026-06-29 15:55 retire regression root
// ([[lesson-eid-not-cross-peer-stable-loadorder-bind]]): the cursor floats under async-load/GC churn, so the
// same physical kerfur bound different eids across peers. Aprop_C keyed forms still bind off-map via S3.1.
enum class Family : uint8_t { ChipPile = 0, KerfurOff = 1 };

// One entry per save-loaded native. chipPile entries pair by per-family ordinal (the array index == the
// client's k-th keyless spawn); kerfurOff entries pair by `key` (the portable save key). `index` is the
// objectsData index (logging + the chip ordinal); `eid` is the cross-peer identity.
struct IdEntry {
    uint32_t index;   // objectsData array index (== client loadObjects spawn order for the keyless subsequence)
    uint32_t eid;     // host ElementId (the S8.2-stable capture-eid)
    uint8_t  family;  // Family
    // Save-time world position of this native (sidecar v2, 2026-06-27). The host already reads it from the save
    // array to do the host-local location->eid join; carrying it lets the CLIENT re-bind a sparse engine-GC
    // -churned save native (which re-creates at its save position, UNBOUND) by an authoritative position match at
    // quiescence -- the order/count/timing-independent fix for the purge-timing ghost (no client-side eid->pos
    // source exists; see coop-purge-timing-reconcile-race-DESIGN-2026-06-27.md 2.7/2.8).
    // IMMUTABLE after receive (2026-07-03, docs/piles/12 eid=4435): this is where the GAME re-creates the
    // native on every purge/churn (loadObjects replays the save arrays) -- it must always name that spot. The
    // earlier PropSnapPos handling RETRACKED it to the host's current pos, which (a) made the re-bind search a
    // position no re-create can ever spawn at (the eid went permanently unbindable -> the client-local dup +
    // the pinned 4 Hz drain) and (b) broke the DUP-RETIRE arm (bound-vs-savePos read as 0 cm). The host's
    // current pos lives in the SEPARATE overlay below.
    float    savePosX, savePosY, savePosZ;
    // The portable save key (sidecar v3, 2026-06-29). EMPTY for a chipPile (genuinely keyless -> ordinal pair).
    // Non-empty for a kerfurOff: the host reads it from the save array (+0x40) -- the SAME blob both peers load,
    // so it is cross-peer-stable -- and the client pairs its loading off-kerfur to the entry whose key matches
    // `GetInteractableKeyString(native)`, making the bound eid cross-peer-stable (the retire-regression fix).
    std::wstring key;
    // RUNTIME OVERLAY -- NOT part of the wire sidecar (SerializeSidecar/DeserializeSidecar never touch it;
    // trailing members so the positional aggregate init sites stay valid, default-initialized). The host's
    // authoritative CURRENT position for this entry, learned from PropSnapPos (b3). The re-bind searches it
    // FIRST (a churned mirror's surviving actor lives here -- the take-3 resurrect protection), then falls
    // back to the immutable savePos (a purge re-create spawns there); a savePos re-bind with a far hostPos
    // then arms the position correction that snaps the actor to the host's truth.
    float    hostPosX = 0.f, hostPosY = 0.f, hostPosZ = 0.f;
    bool     hasHostPos = false;
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
// x64 Windows): ['V','C','I','D'] magic | u32 version | u32 count | count x { u32 index, u32 eid, u8 family,
// f32 savePosX, f32 savePosY, f32 savePosZ, u16 keyLen, keyLen x u8 key (ASCII, byte-narrowed) }. v3
// (2026-06-29) made entries VARIABLE-length by appending the portable save key (empty for chipPile, the
// off-kerfur key for kerfurOff) -- a version mismatch fails the parse gracefully (DeserializeSidecar returns
// false -> the bind stays in cursor-only mode; both peers must run the same mod version, gated by the handshake).
inline constexpr uint8_t  kSidecarMagic[4]        = {'V', 'C', 'I', 'D'};
inline constexpr uint32_t kSidecarVersion         = 3u;
inline constexpr size_t   kSidecarHeaderBytes     = 12u;  // magic(4) + version(4) + count(4)
inline constexpr size_t   kSidecarFixedEntryBytes = 23u;  // index(4)+eid(4)+family(1)+savePos 3xf32(12)+keyLen(2)

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
