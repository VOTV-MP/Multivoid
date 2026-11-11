// coop/items/inventory_wire.h -- serialize the per-player profile <-> a byte blob.
//
// Turns coop::player_profile::Profile (the player's own save record, the vitals and the pose) into
// a self-contained, version-prefixed little-endian byte blob, and back. The record is the game's
// own stream and rides whole (deflated); the vitals' food names ride as strings.
//
// The blob is what coop/blob_chunks carries in chunks (client->host), what persists to
// <mod data dir>/coop_players/<hostSlot>/<guid>.json, and what the join apply reads back.

#pragma once

#include "coop/items/player_profile.h"

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace coop::inventory_wire {

// Current blob format version (the first byte). Bump on any change of layout OR meaning. Version 5 is a u32 length and
// then the DEFLATE of what version 4 held after its version byte: the player's own save record, then the vitals (behind a
// present byte) and the pose. A full inventory's record is the game's tagged-property stream, one ~2.4 KB record per
// item, which a profile blob of version 4 carried raw (13 crowbars already passed the 56100-byte transport ceiling); the
// deflate takes it to ~50 bytes per item. Version 4 is still READ (a profile file of an earlier build); Serialize writes
// it only when deflate fails. The versions before 4 held the save object's item arrays of a game build whose saves this
// one does not load, and are not parsed.
inline constexpr uint8_t kVersion      = 5;
inline constexpr uint8_t kVersionPlain = 4;
// What a body may claim to hold once inflated: the largest byte array the game's own record reader accepts.
inline constexpr uint32_t kMaxBodyBytes = 64u * 1024u * 1024u;

// Serialize `p` into a fresh blob (always succeeds; bounded by the record's size).
std::vector<uint8_t> Serialize(const coop::player_profile::Profile& p);

// Parse `blob` back into `out` (cleared first). False on a truncated / malformed / unknown-version
// blob (a corrupt or hostile blob must never over-read or over-allocate).
bool Deserialize(const std::vector<uint8_t>& blob, coop::player_profile::Profile& out);

// ---- a blob past the transport ceiling: parts ----
//
// One transport blob is at most blob_chunks::MaxBlobBytes (255 chunks). A deflated profile past that (a bag of thousands
// of the smallest items) travels as PARTS, each a transport blob of its own that begins with kPartMarker (a byte no
// profile blob begins with), a u16 group, the part's index and the count, then the bytes of the profile blob it is a
// slice of. SplitParts cuts; PartJoiner puts the parts of one group back together per sender, and passes a blob that is
// not a part through unchanged, so every receiver feeds every completed transport blob through it.
inline constexpr uint8_t kPartMarker = 0xF5;
inline constexpr size_t  kPartHeader = 5;   // marker, u16 group, index, count
inline constexpr size_t  kMaxParts   = 24;

// `blob` cut into parts of at most `maxPart` bytes each (header included), or none at all when that takes more than
// kMaxParts. A blob that fits `maxPart` is returned as it is, as the one element.
std::vector<std::vector<uint8_t>> SplitParts(const std::vector<uint8_t>& blob, size_t maxPart, uint16_t group);

class PartJoiner {
public:
    // Feed one completed transport blob from `slot`. True when `out` is a whole profile blob: `blob` itself when it is
    // not a part, else the join of the group's parts when this one was the last. A part out of order, of another group
    // or malformed drops the group in progress and is refused (a group restarts at its first part).
    bool Feed(uint8_t slot, std::vector<uint8_t>& blob, std::vector<uint8_t>& out);
    void Clear() { slots_.clear(); }
    void ClearSlot(uint8_t slot);

private:
    struct Group { uint16_t id = 0; uint8_t count = 0, got = 0; std::vector<uint8_t> bytes; };
    std::vector<std::pair<uint8_t, Group>> slots_;
};

// The un-gated selftest (coop/items/inventory_wire_selftest.cpp), once per process: the envelope round-trips, a version 4
// blob still reads, a hostile length is refused, and parts join byte for byte.
bool RunSelftest();

}  // namespace coop::inventory_wire
