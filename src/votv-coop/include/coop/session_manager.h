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

// Push the master URL + the host fallback Config from the harness at boot (it
// owns the env/ini readers, principle 7). Call ONCE before any browser action.
// The fallback Config is what HostWithSave uses when the master announce fails
// (RULE 1 decouple -- hosting must not require a reachable master).
void Configure(const std::string& masterUrl, const coop::net::Config& fallbackHostCfg);

// The master server URL ("host:port") as set by Configure (or the env/localhost
// default if Configure was not called -- a direct unit probe). The mod version
// tag (announce + browser display).
std::string MasterUrl();
const char* ModVersion();

// Last host-action status, for the browser/picker to surface (empty until a
// Host action runs). Set by HostWithSave's worker / the harness boot path on
// success, master-unreachable (unlisted), or hard failure. Thread-safe.
std::string HostStatus();
void SetHostStatus(const std::string& status);

// Our OWN announced lobbyId (empty if we are not hosting an announced lobby). The
// browser filters this row out + JoinLobby refuses it, so a host can never connect
// to its own server. Set on announce, cleared on AbortHost. Thread-safe.
std::string OwnLobbyId();

// The local player's display nickname. Seeded from config (env VOTVCOOP_NET_NICK /
// ini net.nick / "Player") at boot, then overwritten by the SERVER BROWSER (the user
// sets their name there). Applied to the wire/nameplate at the next StartCoopSession,
// so the browser value WINS over the config default. Thread-safe.
std::string Nickname();
void SetNickname(const std::string& nick);

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
// Returns true if ACCEPTED (the caller should raise the host-boot cover + close the
// picker); false if rejected (another action already in flight) so the caller leaves
// the picker open and does NOT raise a cover that nothing would drop.
// `directConnection` (Host-Game "Connection" selector, user 2026-06-11): false =
// AUTO (recommended) -- master-brokered P2P/ICE, direct when NAT allows, TURN
// relay as the automatic fallback (TURN is NOT a separate user choice; it lives
// inside AUTO). AUTO games are ALWAYS LISTED at host time: the master is a
// relay game's ONLY rendezvous, so a hidden one would be unjoinable (the in-
// game scoreboard's Hide toggle is the right place to hide once friends are
// in). true = DIRECT -- a LanDirect UDP listen on net.port (the host forwarded
// it); the announce carries conn=direct + the port, the master advertises the
// announce's source ip, and /v1/join hands joiners "ip:port" for a plain UDP
// connect. `hideFromBrowser` (DIRECT only): announce then immediately hide --
// heartbeat lives, friends Direct Connect by IP; ignored for AUTO.
bool HostWithSave(const SaveChoice& choice, const std::string& name, bool locked, int playersMax,
                  bool directConnection = false, bool hideFromBrowser = false);

// Join a master lobby by its opaque lobbyId (POST /v1/join on a worker) -> build a P2P
// client Config + queue a session start. Non-blocking. `displayName` is the lobby's name,
// shown on the loading screen ("Connecting to <name>"). Raises the browser-only loading
// state (join_progress::BeginConnect) and, on a master/HTTP failure, drops it again
// (join_progress::Fail). Returns true if the action was accepted (the browser should
// Close); false if it was rejected (another action already in flight) so the browser
// stays open. (regression A/B/C, 2026-06-06.)
// `hostProto` (v59) = the row's announced kProtocolVersion: a non-zero mismatch is
// rejected HERE with an "update your mod" HostStatus message (the user-chosen
// "show normally, reject on Join" browser policy); 0 = unknown (pre-field host),
// the wire-level protocol-mismatch close stays the backstop.
bool JoinLobby(const std::string& lobbyId, const std::string& displayName, int hostProto = 0);

// Direct-IP connect (rung 0 / LanDirect; works with the master down). "host" or
// "host:port". Builds a LanDirect client Config + queues a session start. Raises the
// browser-only loading state on a good address. Returns true if accepted (the browser
// should Close); false on a bad address or a busy action (browser stays open).
bool ConnectDirect(const std::string& hostPort);

// v59 launch toast: kick ONE async GET /v1/latest per process (Configure calls it
// at boot; safe to call again -- latched). The verdict line is readable via
// LatestVersionLine: empty until the check completes successfully; the overlay
// polls it and toasts once. Unreachable master / pre-v59 master = stays empty
// (never nag an offline player).
void CheckLatestVersionAsync();
std::string LatestVersionLine(bool* outdated);

// Host hide-toggle passthrough (POST /v1/visibility). Session stays live. (design 5.6)
// Also mirrors the state for ListedState() (the scoreboard's Hide checkbox).
void SetListed(bool listed);

// The lobby's current listed state as last set by HostWithSave/SetListed (UI
// mirror only -- the master owns the truth). True when not hosting a lobby.
bool ListedState();

// The UDP port a DIRECT host will listen on (env/ini net.port or the default).
// The picker's port check probes THIS port. Thread-safe.
uint16_t HostListenPort();

// v56 env-host plane (user 2026-06-10): announce the CURRENT, already-started
// env-configured host session to the master as a HIDDEN lobby -- the heartbeat
// keeps it alive and creds stay fresh, but the public browser never lists it
// (.bat/test lobbies must not pollute the list; joiners direct-connect by IP).
// Best-effort on a worker: master down -> logged, hosting continues (RULE 1).
// NOTE: the lobby is briefly listed between the announce and the async
// visibility flip (~1 round-trip); an announce-time hidden flag is a master-API
// addition for later.
void AnnounceEnvHostHidden(const std::string& name, const std::string& world);

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
    bool              listed = true;  // false = master unreachable, hosting unlisted
};
bool TakePendingHostWithSave(PendingHost& out);

}  // namespace coop::session_manager
