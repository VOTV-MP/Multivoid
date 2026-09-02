// harness/session_runtime.h -- the coop-session LIFECYCLE DRIVER on the
// TimelineThread (extracted verbatim from harness/harness.cpp, 2026-07-19 s27
// cut): owns THE production session object (g_session) and everything
// per-Start/Stop -- world boot (story save / menu-mode save-transfer join /
// host-with-save picker), session bringup (StartCoopSession wiring), and the
// unified 60 Hz play loop (browser-start drain + pump-composite + abort/death
// edges). harness.cpp keeps the PROCESS boot (one-shot installs) + the
// scenario timeline; it reaches the session object via Session() and drives
// the lifecycle through the functions below.
//
// Not engine-wrapper and not coop/network logic: this glues the scenario/UX
// surfaces (join_progress cover, server_browser reopen) onto coop::session --
// harness-side by principle 7 (coop/session modules never touch ui::).

#pragma once

#include <string>

namespace coop::net {
class Session;
struct Config;
}  // namespace coop::net

namespace harness::session_runtime {

// THE production session object (owned here; stable address for the process
// lifetime). Process-boot registrations (shutdown/save_transfer/roster in
// harness::Start) take its address; per-session wiring happens inside
// StartCoopSession.
coop::net::Session& Session();

// Bring up a coop session: reset per-session edge state, wire every sync
// subsystem, (host) back up the save + install the LanDirect ban filter, then
// Start. ONE code path for "start a coop session" (RULE 2) -- called by the
// env-configured boot (play scenario) AND the browser drain in RunPlayLoop.
// TimelineThread only. Returns Start()'s success.
bool StartCoopSession(const coop::net::Config& netCfg);

// Boot STORY gameplay (LoadStorySave re-issue loop / StartFreshGame). Blocks
// the calling TimelineThread until gameplay or the ~120 s cap. See the .cpp
// for the forceFresh / slotOverride / forceGameMode semantics.
bool BootStorySaveBlocking(bool forceFresh = false, const wchar_t* slotOverride = nullptr,
                           int forceGameMode = -1);

// Spawn the static 2nd player the instant the local mainPlayer_C exists
// ([dev] static_2nd_player solo visual aid; the play scenario's non-net arm).
void SpawnSecondPlayerWhenReady();

// The main loop (TimelineThread): browser-start drain + host-boot picker +
// the coalesced 60 Hz pump composite + the client-abort / host-death edges.
// Install the source of the lobby heartbeat's player count (this module owns the
// session object it reads). Call ONCE at boot, ABOVE the scenario branch and so
// before every announce site on every lane -- see the call site's comment for why
// no per-scenario location is correct. Idempotent.
void InstallLobbyPlayerCountSource();

// Blocks until shutdown. idleInGameplay: see the .cpp header comment.
void RunPlayLoop(bool idleInGameplay);

}  // namespace harness::session_runtime
