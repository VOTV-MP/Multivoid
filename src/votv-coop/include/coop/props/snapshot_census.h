// coop/props/snapshot_census.h -- per-class completeness floor for the joiner's claim sweep.
//
// It guards a join where the host fails to express its keyless chipPiles: the claim sweep dooms
// every unclaimed chipPile, and its >50% abort valve is GLOBAL rather than per-class, so wiping
// 100% of one class can still sit under the global threshold.
//
// The signal is positive and exact. The host reports how many live actors of class C it has; the
// sweep destroys unclaimed actors of C only once it has claimed at least that many, and keeps them
// when it claimed fewer, because the missing expressions are then in flight or failed. Being a
// count rather than a percentage, it separates "the host expressed none of them" from "the player
// cleared the world".
//
// The crux is INDEPENDENCE: the census is a raw GUObjectArray walk over every live actor, not the
// prop element registry the snapshot enumeration reads, because the piles it guards were untracked
// and missing from that registry -- a manifest built there would share the failure mode.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace coop::snapshot_census {

// HOST (game thread, once per drain-complete): walk GUObjectArray, count live chipPiles per class,
// serialize the classes that fit `budgetBytes` (ordered by count desc) into `outTail`. Returns the
// number of classes emitted. Tail layout: uint16 classCount, then per class { uint16
// nameLen(wchars), wchar name[nameLen], uint32 liveCount }.
//
// It rides as a tail on the existing SnapshotComplete message rather than a new ReliableKind, so
// the budget is whatever is left of the 228-byte reliable payload; ordering by count keeps the mass
// classes inside it, and an omitted tail is logged rather than dropped silently. Only chipPile
// classes are counted -- IsChipPile is a class-lineage test with no per-object string allocation on
// the walk. Keyed interactables are claimed by their stable key instead and stay backstopped by the
// >50% valve, and the client floor below is already per-class and general.
int BuildHostTail(std::vector<uint8_t>& outTail, int budgetBytes);

// CLIENT: parse a census tail received on SnapshotComplete. Replaces any prior census. Tolerant of
// a truncated tail (logs + keeps what parsed).
void SetFromWire(const uint8_t* tail, size_t len);

// CLIENT: clear the census for a new bracket. Called at BeginClaimTracking.
void Reset();

// CLIENT: the host's live count for `cls`, or -1 if the host sent no census entry for it (the sweep
// then falls back to its existing >50% valve only for that class).
int HostCountForClass(const std::wstring& cls);

// CLIENT: true if any census entry arrived this bracket.
bool HasCensus();

}  // namespace coop::snapshot_census
