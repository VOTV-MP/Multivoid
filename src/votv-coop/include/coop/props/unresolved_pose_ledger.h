// coop/props/unresolved_pose_ledger.h -- tell a pose/spawn RACE apart from a real identity GAP.
//
// An incoming PropPose whose key AND eid both fail to resolve used to log one WARN per PACKET, and
// a WARN is a synchronous fflush -- ue_wrap/core/log.cpp flushes every non-INFO line -- so the
// receive path paid a disk sync for what is usually a RACE: an unreliable pose overtakes its own
// reliable spawn broadcast by milliseconds, the spawn lands, and the prop works from then on. In a
// measured field session almost every such identity carried one packet, the worst benign one eight.
//
// The condition that IS a defect looks nothing like that: one identity streaming dozens of
// unresolvable poses with eid=0 and no spawn at all -- a peer holding an item the receiver never
// received. Per packet it was invisible, the same line ninety times among a hundred harmless ones.
// So the ledger separates the two by signature, with the thresholds in the empty band between those
// populations. It is NOT a suppression: nothing is filtered, every packet is still counted, and the
// condition that matters announces itself once as sustained rather than hiding in its own
// repetitions. Game thread only.

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
