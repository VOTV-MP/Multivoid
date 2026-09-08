// coop/interactables/serverbox_sync.h -- the signal-SERVER simulation as host-authoritative
// shared-world state.
//
// The sim -- mainGamemode.{servers, brokenServers, serverEfficiency_calc/downl} and
// per-serverBox_C.IsBroken, driven by ticker_serverBreaker -- is not replicated, and each peer runs
// its own gamemode and ticker, so a client diverges and authors a false "SERVER X is down". The
// break/fix verbs dispatch as EX_LocalVirtualFunction, invisible to both our function seams, so we
// mirror state instead of intercepting the verb.
//
// Host-authoritative and one-directional, the alarm_sync shape: the host polls at ~1 Hz and
// broadcasts ServerStatePayload on a change; the client raw-writes each IsBroken (resolved by name
// at runtime), dispatches the box's notify-free check(), mirrors brokenServers and serverEfficiency
// so its SAT-console reads the host's numbers, neutralizes its own ticker_serverBreaker, and never
// sends server state. Neutralizing the breaker does not close every local break: serverBox's own
// graph reaches breakServer() from a damage path, and that broadcasts serverBroke and bumps
// brokenServers before the next mirror overwrites the STATE, so a transient false notice on a
// client is still possible. Identity is the save-stable servers[] array index: isBrokenMask bit i is
// servers[i].IsBroken, and a joiner is sent the current state at world-ready.

#pragma once

namespace coop::net {
class Session;
struct ServerStatePayload;
}  // namespace coop::net

namespace coop::serverbox_sync {

// Cache the session. Class + offset + check() resolution is lazy in Tick.
void Install(coop::net::Session* session);

// Per net-pump tick, game thread, ~1 Hz internally throttled. HOST: poll server state -> broadcast on
// change. CLIENT: neutralize the local ticker_serverBreaker (cached, idempotent). No-op until resolved.
void Tick();

// HOST, game thread, at a joiner's ClientWorldReady edge: send the current server state to this slot.
void QueueConnectBroadcastForSlot(int slot);

// Game thread (event_feed drain): the host's authoritative server state. CLIENT applies (drive-real +
// aggregate mirror); a non-host sender is dropped (host-authoritative one-directional).
void OnReliable(const coop::net::ServerStatePayload& payload, int senderPeerSlot);

// Teardown: drop the cached gamemode and poll baseline, re-enable the neutralized
// ticker_serverBreaker, and clear the session. The resolved class, offsets and check() pointer are
// kept: they outlive a session.
void OnDisconnect();

}  // namespace coop::serverbox_sync
