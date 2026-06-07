// coop/session_manager.h -- the bridge between the MULTIPLAYER browser (UI) and the
// coop network layer (master plane + the session bring-up).
//
// The server browser calls these; this layer owns the master URL config, drives the
// lobby_client (discover/join) + lobby_announcer (host/heartbeat) on worker threads
// (so the UI never blocks on a round-trip), and turns a browser action into a ready
// coop::net::Config. The actual g_session bring-up (wiring the sync modules + the
// net_pump tick loop) is the HARNESS's job -- it polls TakePendingStart() and starts
// the session. That seam keeps UI/net/harness decoupled (principle 7).

#pragma once

#include "coop/net/lobby_client.h"   // LobbyRow (returned to the browser)
#include "coop/net/session.h"        // coop::net::Config (the produced start request)

#include <cstdint>
#include <string>
#include <vector>

namespace coop::session_manager {

// The master server URL ("host:port") -- env VOTVCOOP_MASTER_URL or a localhost
// default. Resolved once. The mod version tag (announce + browser display).
const std::string& MasterUrl();
const char* ModVersion();

// --- browser actions (call from the render/game thread; each dispatches any blocking
//     HTTP onto a worker, so the caller never stalls) ---

// Async GET /v1/lobbies -> the row snapshot (read via CopyRows). Non-blocking.
void Refresh();
uint64_t CopyRows(std::vector<coop::net::lobby::LobbyRow>& out);
std::string Status();

// Host a lobby on the ALREADY-LOADED world (POST /v1/host on a worker -> P2P host Config
// + queue an immediate session start + heartbeat). Non-blocking. This is the
// host-current-world primitive -- DISTINCT from HostWithSave (which loads a CHOSEN save
// first). The menu "Host Game" button now goes through the save picker -> HostWithSave;
// HostLobby is kept as the building block for a future in-game "Open current game to coop"
// and is exercised by the VOTVCOOP_TEST_HOST_LOBBY autonomous probe. NOT dead (RULE 2: the
// replaced thing was the BUTTON wiring, now gone; this primitive is a distinct capability).
void HostLobby(const std::string& name, const std::string& world, bool locked, int playersMax);

// A save selection for the Host-Game picker: an existing slot to load, or a New Game
// to create. `mode` is an enum_gamemode ordinal (story=0) for a new save.
struct SaveChoice {
    bool        newGame = false;
    std::string slot;       // existing slot to load          (newGame=false)
    std::string newName;    // base name for the new save      (newGame=true)
    uint8_t     mode = 0;   // enum_gamemode for the new save  (newGame=true)
};

// Host on a CHOSEN save (the Host-Game save picker). Like HostLobby, but the harness
// LOADS the chosen world (or CREATES the new save) BEFORE starting the host session.
// Announces on a worker -> queues a pending host-with-save the harness drains via
// TakePendingHostWithSave (then loads the world, then StartCoopSession). Non-blocking.
void HostWithSave(const SaveChoice& choice, const std::string& name, bool locked, int playersMax);

// Join a master lobby by its opaque lobbyId (POST /v1/join on a worker) -> build a P2P
// client Config + queue a session start. Non-blocking. `displayName` is the lobby's name,
// shown on the loading screen ("Connecting to <name>"). Raises the browser-only loading
// state (join_progress::BeginConnect) and, on a master/HTTP failure, drops it again
// (join_progress::Fail). Returns true if the action was accepted (the browser should
// Close); false if it was rejected (another action already in flight) so the browser
// stays open. (regression A/B/C, 2026-06-06.)
bool JoinLobby(const std::string& lobbyId, const std::string& displayName);

// Direct-IP connect (rung 0 / LanDirect; works with the master down). "host" or
// "host:port". Builds a LanDirect client Config + queues a session start. Raises the
// browser-only loading state on a good address. Returns true if accepted (the browser
// should Close); false on a bad address or a busy action (browser stays open).
bool ConnectDirect(const std::string& hostPort);

// Host hide-toggle passthrough (POST /v1/visibility). Session stays live. (design 5.6)
void SetListed(bool listed);

// Cancel an announced-but-never-started host: /leave the master + stop the heartbeat
// thread, so no phantom lobby lingers when the harness fails to load/create the chosen
// world (HostWithSave announces BEFORE the world load). Clears any pending host-with-save.
// Safe no-op if nothing was announced. (audit HIGH-1.)
void AbortHost();

// --- the harness seam (Tier 2 consumes this) ---
// If a session start was queued by a browser action, move the Config into `out` and
// return true (clearing the pending flag); else false. The harness polls this from
// its tick loop, then brings up g_session + the sync modules + the pump.
bool TakePendingStart(coop::net::Config& out);

// The harness seam for HOST-WITH-SAVE: the queued {Config, SaveChoice}. The harness
// polls this, LOADS the chosen world (engine::LoadStorySave) or CREATES the new save
// (save_browser::CreateNamedSave) FIRST, then StartCoopSession(cfg). Distinct from
// TakePendingStart, which starts immediately on the already-loaded world.
struct PendingHost {
    coop::net::Config cfg;
    SaveChoice        save;
};
bool TakePendingHostWithSave(PendingHost& out);

}  // namespace coop::session_manager
