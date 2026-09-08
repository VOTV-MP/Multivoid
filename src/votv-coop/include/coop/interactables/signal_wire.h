// coop/interactables/signal_wire.h -- one Fstruct_signalDataDynamic row
// (ue_wrap::signal_dynamic::Row) on the wire, shared by the SavedSignalAppend and CompData lanes.
// The blob rides BlobChunkPayload chunks; its FNV-1a 64 is the row's cross-peer content identity,
// which SavedSignalDelete keys on. The image PNG is deliberately absent: it belongs to the bulk
// lane, which does not carry it yet.
//
// Layout, version byte 1, little-endian:
//   u8 ver; u8 flags(bit0 hasData, bit1 isCopy, bit2 adopt); u8 frequency; u8 quality;
//   u8 objectType; u8 nameChars; u8 idChars; u8 objectChars; u8 signalChars; u8 pad[3];
//   i32 level; i32 polarity; f32 size; f32 decoded; f32 downloadedAtQuality; f32 locX;
//   f32 locY; i64 date; then name, id, object and signal as UTF-16LE.
//
// `adopt` marks a host connect-snapshot apply, which the receiver trust-gates to slot 0, and it is
// EXCLUDED from the content hash, so an adopt snapshot and a live append of one row share an
// identity.

#pragma once

#include "ue_wrap/desk/signal_dynamic.h"

#include <cstdint>
#include <vector>

namespace coop::signal_wire {

inline constexpr size_t kNameCap = 96, kIdCap = 48, kObjectCap = 64, kSignalCap = 64;

// Serialize (caps logged + applied by the caller's truncation policy: the
// serializer truncates silently at the caps -- callers WARN once on cap hits
// where a truncated FName would be load-bearing).
std::vector<uint8_t> Serialize(const ue_wrap::signal_dynamic::Row& r, bool adopt);

// Deserialize; false on malformed/over-cap input. outAdopt receives the
// adopt flag (already cleared from the row).
bool Deserialize(const std::vector<uint8_t>& b, ue_wrap::signal_dynamic::Row& out,
                 bool& outAdopt);

// The content identity: the blob's FNV-1a 64 with the adopt bit zeroed
// (compute on a serialize with adopt=false, or use this over any blob).
uint64_t ContentHash(const std::vector<uint8_t>& blob);

}  // namespace coop::signal_wire
