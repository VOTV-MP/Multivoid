// harness/session_runtime.h -- the coop-session LIFECYCLE DRIVER, on the TimelineThread. It
// owns THE production session object and everything per Start and Stop: session bringup
// (StartCoopSession wiring) and the unified 60 Hz play loop (browser-start drain, pump
// composite, abort and death edges). WHICH world comes up is harness/world_boot.h; the one
// task posted to the game thread per tick is harness/pump.h. harness.cpp keeps the PROCESS
// boot -- the one-shot installs -- and the scenario timeline; it reaches the session object
// through Session() and drives the lifecycle through the functions below. The scenario and UX
// glue (the join-progress cover, the server-browser reopen) is harness-side by principle 7: a
// coop/session module never touches ui::.

#pragma once

#include <string>

namespace coop::net {
class Session;
struct Config;
struct Refusal;
}  // namespace coop::net

namespace harness::session_runtime {

// THE production session object (owned here; stable address for the process
// lifetime). Process-boot registrations (shutdown/save_transfer/roster in
// harness::Start) take its address; per-session wiring happens inside
// StartCoopSession.
coop::net::Session& Session();

// Bring up a coop session: reset per-session edge state, wire every sync
// subsystem, (host) back up the save + load the ban list on every topology, then
// Start. ONE code path for "start a coop session" (RULE 2) -- called by the
// env-configured boot (play scenario) AND the browser drain in RunPlayLoop.
// TimelineThread only. Returns Start()'s success; `why`, when given, gets Start()'s reason on a
// false return from Start (a start skipped for shutdown leaves it as the caller set it).
bool StartCoopSession(const coop::net::Config& netCfg, coop::net::Refusal* why = nullptr);

// End a CLIENT session from a timeline stop site (a join abort, a Cancel, a world that will not
// load) and put the player at the menu: the one owner of the client-role guard the three sites
// carried -- a stale cancel never stops a host session that has since started. Stops on the
// TimelineThread, where the net-thread join belongs, then RunAndWaits net_pump::TearDownSession so the
// coop state is gone before control returns and before any next start, reading the world kind in the
// same game-thread task. Then the player's way out: a world that is not the menu is left for it through
// join_leave's return, the browser opened when the menu stands (the play loop's Returned); at the menu
// the browser opens now; a degraded world reader moves nobody. Returns whether it stopped a running
// client session (the abort drain branches on it). A no-op, returning false, when no client session
// is running: a session a flee already stopped is the flee's, which travels. At process shutdown it
// stops only: no teardown (the game-thread pump is uninstalled), no return, no browser; still true.
// TimelineThread.
bool EndClientSession(const char* why);

// Spawn the static 2nd player the instant the local mainPlayer_C exists
// ([dev] static_2nd_player solo visual aid; the play scenario's non-net arm).
void SpawnSecondPlayerWhenReady();

// The main loop (TimelineThread): browser-start drain + host-boot picker +
// the coalesced 60 Hz pump composite + the client-abort / host-death edges.
// Install the sources of the lobby heartbeat's player count and its players' links (this module
// owns the session object they read). Call ONCE at boot, ABOVE the scenario branch and so before
// every announce site on every lane -- see the call site's comment for why no per-scenario location
// is correct. Idempotent.
void InstallLobbyHeartbeatSources();

// Blocks until shutdown. `bootedIntoGameplay` is a BOOT fact and only a boot fact: this process
// auto-loaded a gameplay world of its own before the loop started, which the env test scenarios
// do and a native launch never does. Nothing that asks whether a world is up now may read it --
// those gates ask ue_wrap::world_identity inside the loop, because a launch that starts at the
// menu reaches gameplay later and the two answers part there.
void RunPlayLoop(bool bootedIntoGameplay);

}  // namespace harness::session_runtime
