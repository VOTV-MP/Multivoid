// coop/event_feed.h -- coop session events surfaced to the on-screen feed.
//
// Gameplay/network layer (principle 7): decides WHAT lines the player sees and when,
// from coop/session state -- "X joined", "X left the game", connection errors. It
// surfaces them via coop::chat_feed::Push -- the ImGui HUD (ui::hud) draws the feed.
//
// Mirrors MTA (research/findings/mta/mta-chat-joinquit-reliability-2026-05-23.md): joins
// announce a nickname over the RELIABLE channel; the disconnect message is generated
// LOCALLY from the cause (MTA's quit-reason-enum pattern), not sent as a string.
//
// Game thread only (it calls chat_feed + reads the session).

#pragma once

#include <string>

namespace coop {

namespace net { class Session; }

namespace event_feed {

// The local player's display name (sent in our Join). From multivoid.ini "net.nick".
void SetLocalNickname(const std::wstring& nick);

// Reset per-slot caches before session.Start. File-scope state (Join-sent
// flags, last-connected flags, per-slot nicknames) is zero-initialized at
// DLL load but persists across a Session::Stop()/Start() cycle in the
// same process. Without this hook a restart sees stale "true" entries and
// would either (a) suppress legitimate connect-edge replay or (b) emit a
// spurious "X left the game" on the first new-session tick.
// Called by the harness alongside its own g_wasConnected reset.
void OnSessionStart();

// Per-tick pump (game thread): on connect, announce ourselves (reliable Join with our
// nickname); on disconnect, post the peer's departure; drain delivered reliable
// messages (a peer Join -> "<nick> joined" + label the remote player).
// `localPlayer` is the local mainPlayer_C ptr, passed through to remote_prop::OnRelease
// so the `Aprop_C.thrown(Player)` dispatch has a non-null player arg (BP graph may
// null-check). On disconnect it resets the announce flag so a reconnect re-announces.
//
// Puppets are looked up per-slot via Registry::Puppet(slot). Ping
// update fans across all live puppets; per-slot Join nickname tracking
// + per-slot "left the game" message.
void Update(net::Session& session, void* localPlayer);

// Neutralize the per-slot connect edge detectors WITHOUT the full OnSessionStart
// reset. Called from the FleeToMainMenu funnel (net_pump) the instant the LOCAL
// peer begins tearing down its own session (self quit-to-menu, death, host-close
// eject, host RAM-balloon quit). Rationale: when WE leave, session.Stop() flips
// every peer slot connected->disconnected, and the next Update() would read those
// as per-slot "<X> left the game" departures and Push them into chat_feed -- AFTER
// FleeToMainMenu already cleared it -- so the stale line rides its 11 s TTL into the
// MAIN MENU overlay (user 2026-07-15: "Host left the game" leaking into the menu on
// the client's OWN quit; the message is also semantically wrong -- the host did not
// leave, the client did). Clearing the edge bits here means the aggregate teardown
// produces no spurious peer-left toast. The HOST-stays path (its last client leaving)
// does NOT flee, so its legitimate "<client> left the game" toast is untouched.
// OnSessionStart re-primes the edges for the next session (no restore needed).
void SuppressPeerLeaveEdges();

}  // namespace event_feed
}  // namespace coop
