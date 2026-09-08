// coop/interactables/serverbox_sync.h -- the signal-SERVER sim, host-authoritative.
//
// The sim -- mainGamemode.{servers, brokenServers, serverEfficiency_calc/downl} and
// per-serverBox_C IsBroken, driven by ticker_serverBreaker -- is not replicated, and each peer
// runs its own gamemode and ticker, so a client diverges and authors a false "SERVER X is down".
// The break and fix verbs dispatch as EX_LocalVirtualFunction, invisible to our function seams,
// so we mirror the state rather than intercept the verb, the alarm_sync shape.
//
// On a change the host broadcasts ServerStatePayload, polled at about 1 Hz. The client
// raw-writes each IsBroken, dispatches the notify-free check(), mirrors the aggregates its SAT
// console reads, neutralizes its own ticker_serverBreaker, and never sends server state. That
// does not close every local break: serverBox's own graph reaches breakServer() from a damage
// path and bumps brokenServers before the next mirror overwrites the STATE, so a transient false
// notice is possible. Identity is the save-stable servers[] index; a joiner gets the state at
// world-ready.

#pragma once

namespace coop::net {
class Session;
struct ServerStatePayload;
}  // namespace coop::net

namespace coop::serverbox_sync {

// Cache the session. Class + offset + check() resolution is lazy in Tick.
void Install(coop::net::Session* session);

// Per net-pump tick, game thread, throttled internally to about 1 Hz. HOST: poll the server
// state and broadcast on a change. CLIENT: neutralize the local ticker_serverBreaker (cached,
// idempotent). A no-op until resolved.
void Tick();

// HOST, game thread, at a joiner's ClientWorldReady edge: send the current server state to this
// slot.
void QueueConnectBroadcastForSlot(int slot);

// Game thread (event_feed drain): the host's authoritative server state. The CLIENT applies it
// (drive-real plus the aggregate mirror); a non-host sender is dropped, since this lane is
// host-authoritative and one-directional.
void OnReliable(const coop::net::ServerStatePayload& payload, int senderPeerSlot);

// Teardown: drop the cached gamemode and poll baseline, re-enable the neutralized
// ticker_serverBreaker, and clear the session. The resolved class, offsets and check() pointer
// are kept -- they outlive a session.
void OnDisconnect();

}  // namespace coop::serverbox_sync
