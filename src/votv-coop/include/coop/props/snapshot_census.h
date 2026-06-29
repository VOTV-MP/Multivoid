// coop/snapshot_census.h -- Phase 0 per-class completeness floor for the claim sweep.
// (docs/COOP_STABLE_ID_SIDECAR.md S4; the catastrophe guard for docs/piles/10.)
//
// THE BUG IT GUARDS (docs/piles/10, 2026-06-25 11:16): a join where the host failed to
// EXPRESS/claim its keyless chipPiles. The client's claim sweep dooms every UNCLAIMED chipPile
// UNCONDITIONALLY (remote_prop_spawn.cpp:1071) and its >50% abort valve is GLOBAL not per-class
// (953/3083 = 31% < 50% -> no abort even though it wiped 100% of the 870 piles). ALL piles vanished.
//
// THE FIX: a POSITIVE per-class completeness signal. The host tells the client "I have N live of
// class C". The sweep destroys unclaimed actors of class C only when it has CLAIMED at least N of
// them (the snapshot for C is complete -> the rest are genuine deletions). If it claimed fewer than
// the host has, the snapshot for C is INCOMPLETE -> KEEP the unclaimed (the missing expressions are
// in flight or failed; dooming would destroy genuine objects). This is an EXACT signal, not a crude
// percentage: it distinguishes "host expressed 0 of 870 (the bug)" from "the player legitimately
// cleared the world (host genuinely has 0; nothing to keep)".
//
// INDEPENDENCE (the crux): the census is built from a RAW GUObjectArray walk -- every live actor,
// tracked or not -- NOT from the Prop Element registry the snapshot enumeration uses. The 11:16
// root was untracked piles MISSING from the registry, hence never expressed; a raw walk still
// COUNTS them, so the manifest does not share the failure mode it guards.
//
// SCOPE (Phase 0): the census counts CHIPPILE classes -- the keyless MASS class that 11:16 wiped
// (IsChipPile is a cheap class-hierarchy test, no per-object string alloc on the ~237k walk). Keyed
// interactables match by their stable key (claimed reliably; a mass keyed over-destroy is far less
// likely and stays backstopped by the existing >50% valve). The CLIENT floor below is fully general
// (per-class map): Phase 3 (the in-memory index->eid map) extends the census to every class for free.
//
// WIRE: appended as a tail to the existing SnapshotComplete message -- NO new ReliableKind. The
// reliable payload cap is 228 B; the host emits classes ORDERED BY COUNT DESC up to the budget, so
// the mass classes always fit; any omitted small tail is LOGGED (never a silent cap) and the >50%
// valve still backstops it.
//
// Phase 0 of the stable-ID plan: ships FIRST and INDEPENDENT. Does NOT touch identity migration,
// the in-memory map, the spawn-order probe, or the position reconcile layer (all Phase 1+).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace coop::snapshot_census {

// HOST (game thread, once per drain-complete): walk GUObjectArray, count live chipPiles per class,
// serialize the classes that fit `budgetBytes` (ordered by count desc) into `outTail`. Returns the
// number of classes emitted. Tail layout: uint16 classCount, then per class
// { uint16 nameLen(wchars), wchar name[nameLen], uint32 liveCount }.
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
