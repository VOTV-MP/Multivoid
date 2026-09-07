// coop/props/trash_pile_sync.h -- the trashBitsPile_C collect-counter mirror (the "uses 6 of
// 7" dispenser piles). Counters ride TrashPileState; a depletion rides the ordinary keyed
// PropDestroy, both live and in the connect replay.
//
// Gameplay and network layer (principle 7): the wire protocol, the per-tick counter poll, the
// receiver's minimum-wins apply, the key-to-actor index, the depletion death-watch, the
// deferred-apply retry and the connect snapshot. Its engine reads go through ue_wrap::prop
// and ue_wrap::engine, with reflection for the liveness and class tests the index needs.
//
// The model is SYMMETRIC -- the grime_sync shape with an int PAIR and destroy semantics. Each
// use dispenses one item and decrements amountA or amountB, and which side is per-peer RNG, so
// receivers take ABSOLUTE values and never re-simulate. All three writers (the press, the
// vacuum, the broom) run inside the Blueprint, which is why this channel polls the resulting
// STATE rather than observing an input. Keys come from the save actor's own Key, so both peers
// start a transferred save identical and only live decrements ride the wire.

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

// Receiver entry: a TrashPileState packet arrived (event_feed has size- and range-checked it).
// Resolves the pile by key and applies the per-component MIN(local, wire) for a live collect
// (adopt 0), or takes both as sent for the host's connect snapshot (adopt 1), which also trues
// up the per-peer BeginPlay re-rolls of piles with no save row. Defers if the instance has not
// streamed in. Called from event_feed's reliable drain.
void OnReliable(const coop::net::TrashPileStatePayload& payload, uint8_t senderPeerSlot);

// A wire PropDestroy for `key` is about to destroy the local pile: drop it from the index and
// the baselines FIRST, so the death-watch does not re-broadcast the death. Called from
// event_feed's PropDestroy case.
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
