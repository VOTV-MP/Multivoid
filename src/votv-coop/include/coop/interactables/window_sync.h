// coop/interactables/window_sync.h -- the base window's dirt scalar (AbaseWindow_C::clean, the
// base's "main huge window"), on ReliableKind::WindowCleanState. Gameplay/network layer (principle
// 7): the wire protocol, the per-tick poll, the apply, the per-window Key->actor index, the
// deferred retry and the connect snapshot; the engine is reached only through ue_wrap::base_window.
//
// The model is symmetric and monotone, MTA's min register. While connected, each peer polls its
// indexed windows' `clean` once per tick -- not a UFunction observer, since cleanSponge is
// blueprint-internal and never reaches our ProcessEvent detour, as with doors and the keypad -- and
// broadcasts on a DECREASE, keyed by the window's Aactor_save_C::Key. A receiver resolves the
// window by Key, deferring and retrying if it has not streamed in yet, and applies MIN(local,
// wire): a live edge can only make a window CLEANER, so two peers wiping at once converge without
// oscillation, and nothing re-raises `clean` the way a door re-closes, so no host authority is
// needed. The host's relay carries a client's wipe to the other clients. On a connect edge the host
// sends each window's current value with adopt=1, applied AS SENT so a joiner takes the host's
// world even where its own save was cleaner -- which is why an adopt counts only from the host.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct KeyedScalarPayload;
}  // namespace coop::net

namespace coop::window_sync {

// Resolve the baseWindow_C class + build the Key->actor index. Idempotent; retried every
// net-pump tick until the BP class is loaded. Stores the session pointer. Game thread.
void Install(coop::net::Session* session);

// Receiver entry: a WindowCleanState packet arrived, its payload already copied and range-checked
// by event_feed. Resolves the window by Key and applies MIN(local, clean) for a live wipe
// (adopt==0) or the value as sent for a connect snapshot (adopt==1), deferring if the instance has
// not streamed in. Called from event_feed's reliable drain loop.
void OnReliable(const coop::net::KeyedScalarPayload& payload, uint8_t senderPeerSlot);

// HOST-only: snapshot the current `clean` of every indexed window to a freshly connected
// client `peerSlot` with adopt=1 (the joiner adopts the host's world). Called from the
// net-pump connect edge. Game thread.
void QueueConnectBroadcastForSlot(int peerSlot);

// Per-tick: poll for live `clean` decreases (broadcast wipes) + retry deferred applies
// (throttled). Call every net-pump tick on the game thread.
void Tick();

// Session teardown: clear the per-session poll baseline + pending applies.
void OnDisconnect();

}  // namespace coop::window_sync
