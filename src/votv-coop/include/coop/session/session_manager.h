// coop/session/session_manager.h -- the bridge between the multiplayer browser and the network
// layer. It owns the master URL, drives the lobby client (discover, join) and the lobby
// announcer (host, heartbeat) on worker threads so the UI never blocks on a round trip, and
// turns a browser action into a ready coop::net::Config. Bringing the session up is the
// harness's job: it polls TakePendingStart and starts the session, which keeps UI, net and
// harness decoupled.

#pragma once

#include "coop/net/lobby_client.h"   // LobbyRow (returned to the browser)
#include "coop/net/session.h"        // coop::net::Config (the produced start request)

#include <cstdint>
#include <string>

#include "coop/session/host_mode.h"
#include <vector>

namespace coop::session_manager {

// Push the master URL and the host fallback Config from the harness at boot, which owns the
// env and ini readers; call once before any browser action. The fallback is what HostWithSave
// uses when the master announce fails: hosting must not require a reachable master.
void Configure(const std::string& masterUrl, const coop::net::Config& fallbackHostCfg);

// The master URL ("host:port") as set by Configure, or the env/localhost default before it.
std::string MasterUrl();

// The mod's version identity is a pair: GameTarget names the VOTV cook this build targets, compiled
// in from the header generated out of coop/version.h.in; the build number is kProtocolVersion,
// which moves exactly when compatibility moves. DisplayVersion is the composite the menu shows.
const char* GameTarget();
std::string DisplayVersion();

// The last host-action status for the browser and the picker to surface, empty until a Host
// action runs: success, unlisted (master unreachable) or failure. Thread-safe.
std::string HostStatus();
void SetHostStatus(const std::string& status);

// Our own announced lobby id, empty when not hosting an announced lobby. The browser filters
// this row out and JoinLobby refuses it, so a host never connects to itself. Set on announce,
// cleared by EndHostedLobby. Thread-safe.
std::string OwnLobbyId();

// The local display nickname, seeded from config at boot and overwritten by the browser,
// applied at the next session start, so the browser value wins over the config default.
// Thread-safe.
std::string Nickname();
void SetNickname(const std::string& nick);

// The password for the next join, from the prompt the player filled in; consumed by whichever
// join lane starts and never written to the ini, since it is a secret lent for somebody else's
// session and the ini is the file people paste into bug reports. Set it right before the Join
// or Connect call; a stale one is harmless, since a host wanting no password ignores it.
// Thread-safe.
void SetJoinPassword(const std::string& password);

// Browser actions: call from the render or game thread; each dispatches its blocking HTTP onto
// a worker.

// Async fetch of the lobby list into the row snapshot, read via CopyRows.
void Refresh();
uint64_t CopyRows(std::vector<coop::net::lobby::LobbyRow>& out);

// The fetch generation alone, for a screen that repaints only when new rows landed; asking
// every tick must not cost a copy of up to 64 rows of strings.
uint64_t RowsGeneration();

// The generation of the rows themselves, which moves only when a fetch succeeded: "how old is
// this data" keys on this, "should I repaint" on RowsGeneration.
uint64_t RowsDataGeneration();
std::string Status();

// How many list fetches in a row failed, 0 once one succeeds; the browser's cannot-reach-the-
// master alarm keys on this, never on the time since a success.
int FetchFailures();

// Host a lobby on the already-loaded world: the announce on a worker, then an immediate session
// start and the heartbeat. Distinct from HostWithSave, which loads a chosen save first; the
// menu's Host button goes through the picker, and this primitive's one caller is the
// VOTVCOOP_TEST_HOST_LOBBY probe.
void HostLobby(const std::string& name, const std::string& world, bool locked, int playersMax);

// A save selection for the Host-Game picker: an existing slot to load, or a New Game to
// create; `mode` is the gamemode ordinal (story is 0) for a new save.
struct SaveChoice {
    bool        newGame = false;
    std::string slot;       // existing slot to load          (newGame=false)
    std::string newName;    // base name for the new save      (newGame=true)
    uint8_t     mode = 0;   // enum_gamemode for the new save  (newGame=true)
    // Who authored `newName`, which decides whether a taken name may be silently disambiguated: a
    // name a person typed is a request for that exact name, and the create primitive refuses to
    // rename it; a name this mod derived (the native lane has no name field) carries no such
    // intent, and refusing it made the second New Game a player ever hosted impossible. On the
    // choice rather than in the consumer, because only the surface knows which it produced.
    bool        nameIsDerived = false;   // true only when no human typed `newName`
};

// Host on a chosen save: the harness loads the world or creates the new save before starting
// the host session. The announce runs on a worker and queues a pending host-with-save the
// harness drains via TakePendingHostWithSave. True when accepted (raise the host-boot cover and
// close the picker); false when another action is in flight, so the caller leaves the picker
// open and raises no cover nothing would drop. `mode` says how the session is reachable, one
// value rather than three booleans: Brokered is master-brokered P2P, direct when NAT allows and
// the TURN relay as the automatic fallback, always listed since the master is a relay game's
// only rendezvous (`listed` is forced true rather than trusted); Direct is our own UDP listen
// on net.port on every interface, reached as-is on the same network or through a forwarded
// port, and when unlisted nothing leaves the machine (IsMasterFree). There is no LAN-only
// choice: an accept filter refusing non-private remotes did the router's job, and the password
// and the admission challenge are the controls on every lane. `password` is the secret this
// session requires, passed rather than re-read from the ini: a round trip through the ini can
// come back different (a read-only file, an env var that outranks it, trimmed whitespace), and
// an empty read-back would silently downgrade the session to open behind a lit padlock.
bool HostWithSave(const SaveChoice& choice, const std::string& name, bool locked,
                  const std::string& password, int playersMax,
                  coop::session::HostMode mode = {});

// Join a master lobby by its opaque id (the join request on a worker), building a client
// Config and queuing a session start. `displayName` is shown on the loading screen. Raises the
// browser-only loading state and drops it again on a master or HTTP failure. True when accepted
// (the browser should close); false when another action is in flight. `hostProto` and
// `hostGame` are the row's announced identity pair: the equality gate rejects here with the
// connect-failed popup, and 0 or empty (an older host) skips that tier, leaving the wire gate
// as the backstop.
bool JoinLobby(const std::string& lobbyId, const std::string& displayName, int hostProto = 0,
               const std::string& hostGame = {});

// Direct connect, which works with the master down: "host" or "host:port". Builds a direct
// client Config and queues a session start; raises the loading state on a good address. True
// when accepted; false on a bad address or a busy action.
bool ConnectDirect(const std::string& hostPort);

// Dial a host by identity over signaling with no master in the loop, the P2P twin of
// ConnectDirect. `hostIdentity` is the host's rendered identity (gen: and 64 hex, the dial line
// in its log); `fallback` supplies the resolved signaling and ICE fields, from the config the
// caller read. False when either is missing or an action is in flight.
bool ConnectP2PDirect(const std::string& hostIdentity, const coop::net::Config& fallback);

// Kick an async fetch of the latest version, triggered from the browser surface. Self-debounced:
// one worker in flight and a minimum interval between fetch starts. LatestVersionLine is empty
// until a check completes; an unreachable master keeps the last known line.
void RefreshLatestVersion();
std::string LatestVersionLine(bool* outdated);

// The host hide toggle, a visibility request; the session stays live. Mirrored for ListedState.
void SetListed(bool listed);

// The lobby's listed state as last set by HostWithSave or SetListed, a UI mirror; the master
// owns the truth. True when not hosting.
bool ListedState();

// Install the source of the lobby's live player count, published on every heartbeat. Called
// once at boot by the harness, which owns the session object, above the scenario branch, so it
// precedes every announce site on every lane. `fn` runs on the announcer's heartbeat worker
// thread (a 30 s cadence), so it reads atomics only and touches no engine state; the stored
// pointer is atomic, because on the env-host lane that worker already exists when some callers
// would install. Without it the announcer's fallback reported one player in every lobby.
void SetPlayerCountSource(int (*fn)());

// Announce the current, already-started env-configured host session to the master as a hidden
// lobby: the heartbeat keeps it alive and the credentials fresh, but the browser never lists it
// (script and test lobbies must not pollute the list; joiners connect by IP). Best-effort on a
// worker: a master that is down is logged and hosting continues. The lobby is briefly listed
// between the announce and the async visibility flip.
void AnnounceEnvHostHidden(const std::string& name, const std::string& world);

// Retire the announced lobby: the leave request, stop the heartbeat thread, and clear the
// host-side lobby state. The lobby's lifetime is the host session's, and the callers are its
// two end-of-life edges: an announced host whose world load or create failed (HostWithSave
// announces before the load), and the harness's host-session stopped edge (death, quit to
// menu), without which the heartbeat keeps a dead lobby listed forever. A safe no-op if nothing
// was announced. Blocking (HTTP plus a thread join): call from a worker, never the game thread.
void EndHostedLobby();

// The harness seam: if a browser action queued a session start, move the Config into `out` and
// return true, clearing the pending flag. The harness polls this from its tick loop, then
// brings up the session, the sync modules and the pump.
bool TakePendingStart(coop::net::Config& out);

// The host-with-save seam: the queued Config and SaveChoice. The harness polls it, loads the
// chosen world or creates the new save first, then starts the session. Distinct from
// TakePendingStart, which starts on the loaded world.
struct PendingHost {
    coop::net::Config cfg;
    SaveChoice        save;
    bool              listed = true;  // false = master unreachable, hosting unlisted
};
bool TakePendingHostWithSave(PendingHost& out);

}  // namespace coop::session_manager
