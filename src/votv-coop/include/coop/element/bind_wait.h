// coop/element/bind_wait.h -- the one hold for a message whose prop has no actor here yet.
//
// A bind can be deferred: a save-time pile whose twin has not loaded, a kerfur prop awaiting its local
// twin, a mirror the create path refused. A wire message for that eid (a drive end, a pose) that lands in
// the gap used to be parked by each module in a map of its own, each with its own clock. This module is
// the single owner: the deferring bind MARKS the eid, a receiver HOLDS its message against it, and the
// bind funnel's OnBound moves the holds to a due list that Pump replays in arrival order, from the
// reliable drain. A bind that gives up (or a destroy of the eid, a slot that left, a world change)
// DROPS the holds with a named reason; only the holds taken by HoldReplayOnGiveUp survive a twin's give-up. A message for an eid that was never marked is not held: it is
// counted (NoteNeverBound) and dropped by the caller. Game thread only, no mutex.

#pragma once

#include <cstddef>
#include <cstdint>

namespace coop::element::bind_wait {

// MTA drops a packet that names an element the client does not have (Client/mods/deathmatch/logic/rpc/
// CRPCFunctions.cpp:156-164, the null GetElement returns) and keeps no queue; its elements exist from the
// create packet and the engine object streams in later. Ours holds only for an eid whose bind is DEFERRED,
// because a client adopts its save-loaded twin: the identity exists before any actor does. An eid that was
// never marked is dropped as MTA does. MTA keeps the latest state on the element; the holds of a coalesced
// kind do the same, and drive_sync alone queues (several lines per eid, each judged by its role).
//
// A coalesced hold keeps its first arrival's place: across KINDS of one eid the replay is in first-arrival
// order, within a (kind, sender slot) the latest bytes win; two slots' holds of one kind are both kept.
// Bounds: 64 holds per eid and 1 MiB of held bytes in all, of which a slot other than 0 may use 256 KiB;
// past either the new hold is refused, with one warning per eid (per slot for the share). A replay that
// holds again lands in the next Pump, not the one running.

// A held message's replay: the bytes handed to Hold, the slot that sent them.
using Replay = void (*)(const uint8_t* bytes, size_t n, int senderSlot);

// The eid's bind was deferred (or refused): later messages for it wait. Idempotent. `why` names the path.
void MarkDeferred(uint32_t eid, const char* why);
bool IsDeferred(uint32_t eid);

// True while a hold of `kind` for the eid is stored or due (not yet replayed).
bool Holds(uint32_t eid, uint16_t kind);

// Keep a copy of the message until the eid binds. False when the eid is not deferred (or the red row is
// on): the caller then owns the message. An unmarked eid whose registry row holds a purged actor is marked
// here, "a row awaiting its re-bind": the game's re-created native binds onto it and frees the mark. With the
// drill row bind_wait_mark_unknown, an eid this peer has no registry row for is marked here too (a seed that
// lands before its spawn then waits for the spawn's bind). True when stored, and also when refused by a
// bound (warned, counted in RefusedCount).
// `coalesce`: an earlier hold of the same `kind` and `senderSlot` for the eid is replaced in place, latest wins.
bool Hold(uint32_t eid, uint16_t kind, int senderSlot, Replay fn, const void* bytes, size_t n, bool coalesce);

// Hold, coalescing latest-wins per (kind, senderSlot), for a message that is the receiver's own unbound path
// and carries the host's full state (a convert's pose and form): GiveUpReplayHeld still replays it. A newer
// hold replaces the older in place (CoalescedOf) and the replay flag is kept: a replace never clears it.
// Same returns as Hold.
bool HoldReplayOnGiveUp(uint32_t eid, uint16_t kind, int senderSlot, Replay fn, const void* bytes, size_t n);

// A bind took: the eid is no longer deferred and its holds become due.
void OnBound(uint32_t eid);

// Run every due hold, in arrival order. O(1) when none is due. Called from the reliable drain. A GiveUp of
// an eid, or a slot that left, while the pass runs voids the holds of it the pass has not yet reached.
void Pump();

// The module's own newer state voids its held messages: erase the eid's stored and due holds of `kind`,
// also those a Pump in flight has swapped out and not yet reached. Counted into DroppedOf, never GivenUpCount.
// One kind number carrying several ops must hold with one op only, else the Drop voids the others too.
void Drop(uint32_t eid, uint16_t kind, const char* why);

// The bind will not come: drop the eid's mark and holds. A no-op for an eid neither deferred nor holding.
void GiveUp(uint32_t eid, const char* why);

// The eid's twin gave up, but the eid itself lives: drop its mark, queue the HoldReplayOnGiveUp holds for the
// next Pump (the eid is no longer deferred, so they take the receiver's unbound path) and drop the rest. Not
// for a destroy of the eid: that is GiveUp.
void GiveUpReplayHeld(uint32_t eid, const char* why);

// A slot left: drop the holds it sent.
void OnSlotLeft(int slot);

// Disconnect or world change: drop every hold and mark.
void Clear(const char* why);

// True while any hold is stored or due: the dev selftest runs only once none is.
bool HasHolds();

// A message named an eid that was never deferred, so nothing waits for it: counted and warned.
void NoteNeverBound(uint16_t kind, uint32_t eid);

// For the drills: the totals of this process.
uint32_t DropsNeverBound();
uint32_t DeferredCount();
uint32_t HeldCount();
uint32_t ReplayedCount();
uint32_t GivenUpCount();
uint32_t RefusedCount();   // holds refused at a bound (Hold returned true, the message was not kept)

// Per kind, for a drill's verdict on one module. HeldOf counts distinct holds (a coalesced overwrite counts
// into CoalescedOf); a correct run keeps HeldOf == ReplayedOf + DroppedOf + GivenUpOf. GivenUpOf counts the
// holds of the kind dropped by GiveUp, OnSlotLeft and Clear (those a running Pump skips included). A module's
// own drop at replay time is the module's to count.
uint32_t HeldOf(uint16_t kind);
uint32_t CoalescedOf(uint16_t kind);
uint32_t ReplayedOf(uint16_t kind);
uint32_t DroppedOf(uint16_t kind);
uint32_t GivenUpOf(uint16_t kind);
uint32_t NeverBoundOf(uint16_t kind);

}  // namespace coop::element::bind_wait
