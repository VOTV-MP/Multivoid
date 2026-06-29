// coop/trash_pile_sync.h -- trashBitsPile_C collect-counter mirror (the "uses 6/7"
// garbage dispenser piles). Protocol v57, ReliableKind::TrashPileState (46).
//
// Gameplay/network layer (principle 7): owns the wire protocol, the per-tick counter
// POLL, the receiver min-wins apply, the key->actor index, the depletion death-watch,
// the deferred-apply retry, and the connect-snapshot. Talks to the engine ONLY through
// ue_wrap::prop (IsTrashBitsPile / Read+WriteTrashPileAmounts / GetInteractableKeyString)
// + ue_wrap::engine.
//
// Model (SYMMETRIC, the grime_sync shape with an int PAIR + destroy semantics -- the
// deliberate divergence from the two keyed-float channels): each pile dispenses one
// garbage item per use (E-grab / vacuum / broom -- THREE writers, ALL BP-internal, so
// input observers are unsound; the channel doc's "poll the resulting STATE" doctrine
// applies) and decrements amountA or amountB (per-peer RNG picks the side -> receivers
// take ABSOLUTE values, never re-simulate). Each peer polls its indexed piles and
// broadcasts {key, amountA, amountB} on a DECREASE; the receiver applies per-component
// MIN (monotone-down -> concurrent collects converge) or VERBATIM for the host adopt=1
// connect-snapshot (which also trues up the per-peer BeginPlay RNG re-rolls of piles
// without save rows). Keys are Aactor_save_C::Key@0x0230 -- save-persisted, so the v56
// save-transfer join starts both peers identical; only live decrements ride the wire.
//
// DEPLETION: the final dispense K2_DestroyActor's the pile BP-INTERNALLY (ProcessInternal
// -- invisible to our destroy observer AND to the next poll). Caught by the proximity-
// gated death-watch inside the poll (the grime super-sponge shape): an indexed pile that
// vanishes NEAR the local camera (<= 8 m; all three writers happen at the player) outside
// a transition window was collected-to-empty -> broadcast the EXISTING keyed PropDestroy;
// far/transition deaths are stream-outs -> dropped. NotifyWireDestroy unwatches a key
// when the destroy arrived FROM the wire (echo guard).
//
// The DISPENSED ITEM needs no new wire: pickupObjectDirect hands it to the standard grab
// machinery -> the held-edge broadcast (EnsureHeldItemBroadcast) + pose stream mirror it.
// (Broom-scattered items that land un-grabbed mirror on first grab -- pre-existing
// pipeline behavior.) Event-cluster piles with non-deterministic keys never resolve
// cross-peer (deferred apply expires harmlessly); the keysHash log quantifies them.
//
// RE: the trashBitsPile bytecode pass, findings doc HANDS-ON ROUND 2 (4), 2026-06-10.

#pragma once

#include <cstdint>
#include <string>

namespace coop::net {
class Session;
struct TrashPileStatePayload;
}  // namespace coop::net

namespace coop::trash_pile_sync {

// Resolve the class + build the key->actor index. Idempotent; retried every net-pump
// tick until the BP class is loaded. Stores the session pointer. Game thread.
void Install(coop::net::Session* session);

// Receiver entry: a TrashPileState packet arrived (already size/range-checked by
// event_feed). Resolves the pile by key and applies per-component MIN(local, wire) for
// a live collect (adopt==0) or VERBATIM for a host adopt==1 connect-snapshot, deferring
// if the instance has not streamed in. Called from event_feed's reliable drain.
void OnReliable(const coop::net::TrashPileStatePayload& payload, uint8_t senderPeerSlot);

// A wire PropDestroy for `key` is about to destroy the local pile: drop it from the
// index/baselines FIRST so the death-watch does not re-broadcast the death (echo guard;
// the keyed twin of v52's NotifyPileConsumed). Called from event_feed's PropDestroy case.
void NotifyWireDestroy(const std::wstring& key);

// HOST-only: snapshot every indexed pile's counters to a freshly world-ready client
// `peerSlot` with adopt=1, + replay keyed PropDestroy for piles depleted this session
// (covers the joiner's mid-load window; pre-join depletions reconcile via the claim
// sweep). Called from the net-pump connect replay. Game thread.
void QueueConnectBroadcastForSlot(int peerSlot);

// Per-tick: poll for counter decreases (broadcast collects) + the depletion death-watch
// + retry deferred applies (throttled). `inTransition` suppresses the death-watch during
// flee/join windows (stream-outs masquerade as deaths). Net-pump tick, game thread.
void Tick(bool inTransition);

// Session teardown: clear the index, baselines, pending applies + depleted-key set.
void OnDisconnect();

}  // namespace coop::trash_pile_sync
