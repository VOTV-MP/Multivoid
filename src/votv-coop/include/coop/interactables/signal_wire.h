// coop/interactables/signal_wire.h -- one Fstruct_signalDataDynamic row on the wire, shared by
// every lane that ships one: SavedSignalAppend and CompData, the DrivePayload and RackState drive
// lanes, and the meadow append/order pair. The blob rides BlobChunkPayload chunks. The photo rides
// only in a row read with it (signal_dynamic::ReadStruct's withImage), as SavedSignalAppend's are,
// and only within blob_chunks::MaxBlobBytes(); past it the row crosses without it, logged.
//
// Layout, version byte 1, little-endian:
//   u8 ver; u8 flags(bit0 hasData, bit1 isCopy, bit3 image); u8 frequency; u8 quality;
//   u8 objectType; u8 nameChars; u8 idChars; u8 objectChars; u8 signalChars; u8 pad[3];
//   i32 level; i32 polarity; f32 size; f32 decoded; f32 downloadedAtQuality; f32 locX;
//   f32 locY; i64 date; then name, id, object and signal as UTF-16LE; with bit3, u32 bytes + PNG.
//
// A row's cross-peer identity, which SavedSignalDelete keys on, is the FNV-1a 64 of the blob up to the
// strings' end with the image bit zeroed and its two FName leaves (object, signal) lower-cased: a row
// shares it with its photo or without, and whatever case each peer's engine renders those names in.

#pragma once

#include "ue_wrap/desk/signal_dynamic.h"

#include <cstdint>
#include <vector>

namespace coop::signal_wire {

inline constexpr size_t kNameCap = 96, kIdCap = 48, kObjectCap = 64, kSignalCap = 64;

// Serialize (caps logged + applied by the caller's truncation policy: the
// serializer truncates silently at the caps -- callers WARN once on cap hits
// where a truncated FName would be load-bearing).
std::vector<uint8_t> Serialize(const ue_wrap::signal_dynamic::Row& r);

// Deserialize; false on malformed/over-cap input. A photo that is not a plausible PNG
// (its signature, and an IHDR of at most 2048 x 2048: the laptop decodes it whole,
// ui_laptop.cpp:3836) is left out of the row, logged, and the row kept.
bool Deserialize(const std::vector<uint8_t>& b, ue_wrap::signal_dynamic::Row& out);

// The content identity: the FNV-1a 64 of the blob up to its strings' end, the image bit zeroed, the FName leaves
// lower-cased.
uint64_t ContentHash(const std::vector<uint8_t>& blob);

}  // namespace coop::signal_wire
