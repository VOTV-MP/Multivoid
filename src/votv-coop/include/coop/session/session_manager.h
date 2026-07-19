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
// default if Configure was not called -- a direct unit probe).
std::string MasterUrl();

// The mod's version identity (2026-07-19, the Paper-Minecraft PAIR -- user
// decision; no separate mod semver, the old hand-kept semver axis was RULE-2
// rot nobody bumped): GameTarget() = the VOTV cook this build targets
// ("0.9.0-n", generated coop/version.h); the BUILD NUMBER = kProtocolVersion
// (moves exactly when compatibility moves; every release bumps it).
// DisplayVersion() = the user-facing composite "votv-coop 0.9.0-n b122".
const char* GameTarget();
std::string DisplayVersion();

// Last host-action status, for the browser/picker to surface (empty until a
// Host action runs). Set by HostWithSave's worker / the harness boot path on
// success, master-unreachable (unlisted), or hard failure. Thread-safe.
std::string HostStatus();
void SetHostStatus(const std::string& status);

// Our OWN announced lobbyId (empty if we are not hosting an announced lobby). The
// browser filters this row out + JoinLobby refuses it, so a host can never connect
// to its own server. Set on announce, cleared on EndHostedLobby. Thread-safe.
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
// `hostProto`/`hostGame` = the row's announced identity pair (v59 proto = the
// build number; v122 game target). The EQUALITY gate (game -> build, Minecraft
// shape 2026-07-19) rejects HERE with the connect-failed POPUP
// (join_progress::RefuseJoin) per the "show normally, reject on Join" policy;
// empty/0 = unknown (old host), that tier is skipped -- the Join wire gate +
// wire-level close stay the backstop.
bool JoinLobby(const std::string& lobbyId, const std::string& displayName, int hostProto = 0,
               const std::string& hostGame = {});

// Direct-IP connect (rung 0 / LanDirect; works with the master down). "host" or
// "host:port". Builds a LanDirect client Config + queues a session start. Raises the
// browser-only loading state on a good address. Returns true if accepted (the browser
// should Close); false on a bad address or a busy action (browser stays open).
bool ConnectDirect(const std::string& hostPort);

// Kick an async GET /v1/latest. Configure calls it at boot; coop::multiplayer_menu
// calls it again on EACH main-menu entrance so the native version label refreshes.
// Self-debounced (no DoS): at most one worker in flight, and a min-interval floor
// between fetch starts coalesces a burst of entrances into one fetch. The verdict
// line is readable via LatestVersionLine: empty until a check completes successfully;
// unreachable / pre-v59 master keeps the last known line (never nag an offline player).
void RefreshLatestVersion();
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

// Retire the announced lobby: POST /v1/leave + stop the heartbeat thread, and clear the
// host-side lobby state (pending host-with-save, own-lobby guard, listed mirror). The
// lobby's lifetime IS the host session's lifetime -- callers are its two end-of-life
// edges: (a) an announced-but-never-started host whose world load/create failed
// (HostWithSave announces BEFORE the load -- audit HIGH-1: no phantom lobby), and
// (b) the harness's host-session running->stopped edge (death / quit-to-menu). Without
// (b) the heartbeat thread keeps a DEAD lobby listed on the master forever (2026-07-04
// user repro: host died to the killerwisp, everyone fled to the menu, the server stayed
// in the browser). Safe no-op if nothing was announced. Blocking (/leave HTTP + thread
// join) -- call from a worker/timeline thread, never the game thread.
void EndHostedLobby();

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
