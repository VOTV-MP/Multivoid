// coop/props/unresolved_pose_ledger.h -- tell a pose/spawn RACE apart from a real identity GAP.
//
// WHY THIS EXISTS (2026-09-04, from a player's field log -- doctaaaaa, Discord)
// ---------------------------------------------------------------------------
// An incoming PropPose whose key AND eid both fail to resolve used to log one WARN per
// PACKET. In a 58-minute field session that produced 205 WARNs, and a WARN is a SYNCHRONOUS
// fflush (`ue_wrap/core/log.cpp` flushes every non-INFO line), so the receive path paid 205
// disk syncs -- for a condition that is USUALLY NOT A DEFECT.
//
// It is usually not a defect because that log proves the common case is a RACE. `[V]` for key
// `_95qO5xwv10Qw_Rx-8rElg` the line ORDER is: the unresolved warn (log line 47367), THEN
// `OnSpawn` (47368), THEN the mirror bind (47372) -- the unreliable pose overtook its own
// reliable spawn broadcast by milliseconds, after which the prop worked normally. `[V]` 69 of
// the 83 unresolved keys in that session carried exactly ONE such packet; the worst benign
// case carried 8.
//
// THE REAL DEFECT LOOKS COMPLETELY DIFFERENT AND WAS INVISIBLE INSIDE THAT NOISE. `[V]` key
// `lSg9QVjeXGHDezEk96eOow` streamed 91 unresolvable poses over ~1.5 s with `eid=0` and NO
// `OnSpawn` for the entire session -- a peer holding an item the receiver never received. The
// same log line, ninety-one times, buried among 114 harmless ones. That is the reported
// "the host does not see the items that fall out", from the other side.
//
// So this ledger separates the two by their MEASURED signature rather than by guesswork. The
// thresholds sit in the empty band the field data leaves between the two populations: worst
// benign 8 packets inside one second, the real one 91 packets over 1.5 s.
//
// IT IS NOT A SUPPRESSION. Nothing is filtered, every packet is still counted, and the
// condition that matters becomes MORE visible than it was -- it now announces itself as
// "sustained" instead of hiding inside its own repetitions.
//
// THREAD: game thread only. The prop receive path runs from `event_feed` on the game thread.

#pragma once

#include <cstdint>
#include <string>

namespace coop::unresolved_pose_ledger {

// Packets and elapsed time a single identity must go unresolved before it stops being a race
// and starts being a gap. BOTH must be crossed. Public so a test/drill can assert the band.
constexpr uint32_t kSustainedCount = 16;
constexpr uint64_t kSustainedMs    = 500;

// Record one unresolved pose for (slot, key, eid).
// Returns true EXACTLY ONCE per identity -- on the packet that crosses both thresholds --
// so the caller emits the sustained WARN a single time and never repeats it.
bool Note(int slot, const std::wstring& keyW, uint32_t eid, uint64_t nowMs);

// A pose for this identity resolved: the race healed, or the peer finally sent the spawn.
// Drops the row so a later, unrelated miss starts its own count. Logs a one-line recovery
// note only if the identity had previously been reported as sustained.
void Clear(int slot, const std::wstring& keyW, uint32_t eid);

// A peer left: forget every identity attributed to its slot. Rows are keyed by slot, so a
// recycled slot must not inherit the previous occupant's counts -- that would let a fresh
// peer's first miss arrive pre-charged and report "sustained" on packet one.
// Returns how many rows were dropped (0 is the normal, quiet case).
size_t ResetSlot(int slot);

}  // namespace coop::unresolved_pose_ledger
