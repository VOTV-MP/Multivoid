// coop/props/save_identity_map.h -- the {ordinal -> host eid} map for the keyless
// save-loaded forms: chipPiles and off-prop kerfurs.
//
// The goal is a stable cross-peer identity for natives that have no key -- a pile's Key is
// None and an off-prop kerfur is untracked. The host builds the map at save-capture out of
// the same blob the client will load, sends it as a save-transfer SIDECAR (docs/join.md),
// and the client binds each loaded native to its host eid.
//
// ORDER: the build walks the two save arrays in the order the client's own load replays
// them -- objectsData (off-kerfurs and keyed world objects), then primitivesData (the
// chipPiles, which ignoreSave keeps out of objectsData) -- concatenating their keyless chip
// and kerfur entries. That sequence IS the client's keyless spawn order, and an entry's
// position in it is the cross-peer ordinal. Each eid comes from a host-LOCAL class-and-
// location join against the live actors captured in the same instant; that location is the
// join's key, never an identity. M entries at one location pair to the M eids by rank.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace coop::save_identity_map {

// The two save-loaded families the map covers. The map is the TRANSPORT for both, being the
// only thing that sees every save object regardless of runtime tracking: a save-loaded
// off-kerfur is untracked at join, so the keyed mirror path cannot resolve it. The PAIRING
// RULE inside the map still splits keyed from keyless. A kerfurOff carries a PORTABLE save
// key, byte-identical on both peers, so the client pairs it native-to-eid BY KEY. A chipPile
// is genuinely keyless and pairs by a per-family ORDINAL cursor. Pairing the keyed family by
// that cursor is what broke retirement once: the cursor floats under async-load and GC
// churn, so the same physical kerfur bound a different eid on each peer. Keyed Aprop_C forms
// bind off-map, by key.
enum class Family : uint8_t { ChipPile = 0, KerfurOff = 1 };

// One entry per save-loaded native. chipPile entries pair by per-family ordinal (the array
// index == the client's k-th keyless spawn); kerfurOff entries pair by `key`, the portable
// save key.
struct IdEntry {
    uint32_t index;   // the keyless ordinal: this entry's position in the concatenated
                      // walk == the client's k-th keyless load-spawn
    uint32_t eid;     // host ElementId -- the cross-peer identity
    uint8_t  family;  // Family
    // Save-time world position. The host already reads it from the save array for the host-local
    // location-to-eid join, and carrying it lets the CLIENT re-bind a save native the engine's
    // GC churned away -- it re-creates at its save position, unbound -- by an authoritative
    // position match at quiescence. The client has no other eid-to-position source.
    //
    // IMMUTABLE after receive: this is where the GAME re-creates the native on every purge or
    // churn, since loadObjects replays the save arrays, so it must always name that spot.
    // Retracking it to the host's current position made the re-bind search a place no re-create
    // can spawn at (the eid went permanently unbindable) and read the dup-retire arm as 0 cm.
    // The host's current position lives in the separate overlay below.
    float    savePosX, savePosY, savePosZ;
    // The portable save key. EMPTY for a chipPile, which is genuinely keyless and pairs by
    // ordinal. Non-empty for a kerfurOff: the host reads it out of the save array -- the SAME
    // blob both peers load, so it is cross-peer-stable -- and the client pairs its loading
    // off-kerfur to the entry whose key matches GetInteractableKeyString(native).
    std::wstring key;
    // RUNTIME OVERLAY -- NOT part of the wire sidecar (SerializeSidecar and DeserializeSidecar
    // never touch it; trailing members, so the positional aggregate initialisers stay valid).
    // The host's authoritative CURRENT position for this entry, learned from PropSnapPos. The
    // re-bind searches it FIRST, where a churned mirror's surviving actor lives, then falls back
    // to the immutable savePos, where a purge re-create spawns; a savePos re-bind with a far
    // hostPos then arms the correction that snaps the actor to the host's truth.
    float    hostPosX = 0.f, hostPosY = 0.f, hostPosZ = 0.f;
    bool     hasHostPos = false;
};

using IdMap = std::vector<IdEntry>;

// HOST (game thread, at save-capture): build `outMap` by walking objectsData and then
// primitivesData in index order, keeping the keyless chip and kerfur entries, and joining
// each to its host eid by a host-local class-and-location match against the live actors
// (CollectTracked{Pile,Kerfur}Transforms). Logs a per-family summary with the keyed-excluded,
// unmatched and ambiguous-location counts. Returns the entry count. Reads engine-owned memory
// only. Aborts (returns 0) if the save-struct stride sanity gate fails.
int BuildHostMap(IdMap& outMap);

// ---- The sidecar wire framing -----------------------------------------------------------
// The map travels PREPENDED to the save-transfer blob stream (one stream, one checksum) so it
// can never desync from the blob it indexes. Self-describing layout, little-endian as the
// whole protocol is: ['V','C','I','D'] magic | u32 version | u32 count | count x { u32 index,
// u32 eid, u8 family, f32 savePosX, f32 savePosY, f32 savePosZ, u16 keyLen, keyLen x u8 key
// (ASCII, byte-narrowed) }. Entries are variable-length because of the trailing key, empty
// for a chipPile. A version mismatch fails the parse gracefully -- DeserializeSidecar returns
// false and the bind stays in cursor-only mode -- and cannot happen between peers anyway,
// since the handshake requires the same build on both ends.
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

// CLIENT: log a received map (summary plus the first and last five entries) in the SAME shape
// as the host's BuildHostMap log, so the two can be diffed line for line. Logging only; the
// bind lives in save_identity_bind.
void LogReceivedMap(const IdMap& map);

}  // namespace coop::save_identity_map
