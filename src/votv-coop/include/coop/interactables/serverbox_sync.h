// coop/interactables/serverbox_sync.h -- the signal-SERVER simulation as host-authoritative shared-world state.
//
// PROBLEM (RE 2026-07-09, docs/notifications/ + research/findings/votv-notifications-suppress-mirror-DESIGN-2026-07-09.md):
// VOTV's signal-server sim -- mainGamemode.{servers, brokenServers, serverEfficiency_calc/downl} +
// per-serverBox_C.IsBroken, broken/fixed by the host-only ticker_serverBreaker -- is NOT UE-replicated.
// Each coop peer runs its OWN gamemode + ticker, so a client self-computes DIVERGED server state and
// self-authors a FALSE "SERVER X is down" (email + console line + the serverDown alarm). The break/fix
// VERBS (breakServer/fix/break_type) are ALL EX_LocalVirtualFunction -> invisible to BOTH our
// ProcessEvent detour AND the UFunction::Func patch (M1 measured), so we cannot intercept the verb.
//
// FIX (Inc-1, host-authoritative one-directional, the alarm_sync shape):
//   - HOST polls its server state each ~1 Hz and broadcasts ServerStatePayload on any change.
//   - CLIENT DRIVE-REALs it: raw-write each serverBox.IsBroken (offset from the CXX header dump) then
//     dispatch the box's own check() via reflected CallFunction -- check() re-skins purely from IsBroken
//     (M2 measured: reads IsBroken -> SetMaterial/SetActive; it is notify-free, unlike the verbs which
//     entangle the delegate + minigame + counter + the Server Alert email). Mirrors brokenServers +
//     serverEfficiency onto the client gamemode too (so the SAT-console sv.*/tw.* queries read TRUE).
//   - CLIENT neutralizes its OWN ticker_serverBreaker (disable actor tick) -- the primary autonomous
//     false-break source (the census's path #1); the world-load/event/minigame residual break paths are
//     OVERWRITTEN by the next host mirror (state converges), and their transient false NOTICE is Inc-2's
//     job (forward the host break EDGE + suppress the client edge). Client never SENDS server state.
// Identity: the save-stable servers[] ARRAY INDEX (both peers load the same host save in order) ->
// isBrokenMask bit i == servers[i].IsBroken. Late join: host sends the current state at world-ready.
//
// [[feedback-folder-per-domain-concept-rule]] [[lesson-script-fn-invisible-to-func-patch]]
// [[lesson-tracking-gates-on-hosting-not-connected]]

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

// Teardown: drop cached class/offsets/instances + baseline, clear the session.
void OnDisconnect();

}  // namespace coop::serverbox_sync
