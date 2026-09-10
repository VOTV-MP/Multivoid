// coop/items/save_record_wire.h -- serialize ONE Fstruct_save record (the engine-agnostic
// ue_wrap::save_record::SaveRecord POD) <-> bytes, plus the little-endian byte primitives the
// record grammar is built from. ONE implementation for both callers: the player inventory
// (inventory_wire) and a container's TArray<Fstruct_save> contents (container_contents_sync);
// neither keeps a copy.
//
// FNames and UClasses ride as STRINGS, since a pointer is not portable across peers -- the
// applier re-interns the name and FindClass'es the leaf, exact case preserved. An FTransform
// packs as 10 floats (quat, location, scale). A signal row defers to
// coop/interactables/signal_wire, length-prefixed.
//
// HOSTILE-INPUT CONTRACT: every Rd*/De* is bounds-checked and fails cleanly on overrun, and each
// count is gated by Feasible() -- a declared count of n needs >= n bytes remaining -- so every
// reserve/resize is proportional to the ACTUAL blob size and a small tampered blob cannot
// balloon RSS. Pure byte work: no engine access, callable off the game thread.

#pragma once

#include "ue_wrap/actors/save_record.h"

#include <cstdint>
#include <string>
#include <vector>

namespace coop::save_record_wire {

// Sanity caps (a corrupt / hostile blob must never drive a huge allocation or over-read). These
// are generous-but-realistic absolute ceilings; the PRIMARY bound is Feasible() below.
inline constexpr uint32_t kMaxRecords = 16384;
inline constexpr uint32_t kMaxGroups  = 4096;
inline constexpr uint32_t kMaxElems   = 262144;
inline constexpr uint32_t kMaxStrLen  = 262144;

// A declared count of `n` elements is feasible only if at least `n` bytes remain (1-byte floor per
// element). Bounds every count-driven reserve/resize to the real blob size.
bool Feasible(uint32_t n, const std::vector<uint8_t>& b, size_t o);

// ---- append (little-endian) ----
void AppU8(std::vector<uint8_t>& b, uint8_t v);
void AppU32(std::vector<uint8_t>& b, uint32_t v);
void AppI32(std::vector<uint8_t>& b, int32_t v);
void AppF32(std::vector<uint8_t>& b, float v);
void AppWStr(std::vector<uint8_t>& b, const std::wstring& s);  // u32 count + UTF-16LE code units

// ---- read (cursor + bounds; every Rd fails cleanly on overrun) ----
bool RdU8(const std::vector<uint8_t>& b, size_t& o, uint8_t& v);
bool RdU32(const std::vector<uint8_t>& b, size_t& o, uint32_t& v);
bool RdI32(const std::vector<uint8_t>& b, size_t& o, int32_t& v);
bool RdF32(const std::vector<uint8_t>& b, size_t& o, float& v);
bool RdWStr(const std::vector<uint8_t>& b, size_t& o, std::wstring& s);

// ---- the record ----
void SerSave(std::vector<uint8_t>& b, const ue_wrap::save_record::SaveRecord& r);
bool DeSave(const std::vector<uint8_t>& b, size_t& o, ue_wrap::save_record::SaveRecord& r);

}  // namespace coop::save_record_wire
