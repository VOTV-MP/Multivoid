// coop/net/lobby_client.h -- the server-browser feed + join (master client side).
//
// MTA precedent: CServerList (fetch + parse + cache the master list) +
// CServerListItem (the row model) -- reference/mtasa-blue/Client/core/ServerBrowser/
// CServerList.h. We diverge from MTA's ASE-UDP per-server query (ping is measured
// post-connect via GNS, not pre-listed) and render in ImGui, not CEGUI.
//
// Talks to tools/coop_master_server.py over http_client:
//   GET  /v1/lobbies?version=  -> the row list (async, coalesced)
//   POST /v1/join {lobbyId}     -> JoinInfo (the creds + identities to dial the host)
//
// Threading: RefreshAsync spawns a detached worker (the HTTP round-trip must not
// stall a frame); the parsed rows land in a mutex-guarded snapshot the render thread
// reads via CopyRows. Join is blocking -- session_manager calls it on a worker.

#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace coop::net::lobby {

// One browser row, parsed from a /v1/lobbies element. Maps onto ui::server_browser's
// ServerRow. `lobbyId` is the opaque master handle (the /v1/join key).
struct LobbyRow {
    std::string lobbyId;
    std::string name;
    std::string version;
    std::string world;
    int  playersCur = 0;
    int  playersMax = 0;
    int  ageSec = 0;        // seconds since the host's last heartbeat
    bool locked = false;    // passworded (UI hint; gate is the game-layer join-secret)
};

// Everything a joining client needs to build a P2P coop::net::Config and dial the
// host, returned by POST /v1/join. ok=false on any failure.
struct JoinInfo {
    bool ok = false;
    std::string sessionId;
    std::string peerIdentity;    // our freshly-minted signaling identity
    std::string hostIdentity;    // the host's identity we dial
    std::string signalingUrl;    // "host:port"
    std::string signalingToken;  // the shared signaling bearer
    std::string stun;            // "host:port" or ""
    std::string turnUri;         // "turn:host:port" (first uri, transport stripped) or ""
    std::string turnUser;
    std::string turnPass;
};

class LobbyClient {
public:
    // Kick off an async GET /v1/lobbies against `masterUrl` ("host:port"), optionally
    // filtered to `versionFilter` (empty = all). Non-blocking; a refresh already in
    // flight is coalesced (returns immediately). Results readable via CopyRows.
    void RefreshAsync(const std::string& masterUrl, const std::string& versionFilter);

    // Render/game thread: copy the latest fetched rows into `out`. Returns the fetch
    // generation (increments on each completed refresh) so the caller can tell new
    // data from a re-read.
    uint64_t CopyRows(std::vector<LobbyRow>& out) const;

    // A short human status for the browser footer ("Refreshing...", "4 servers",
    // "master unreachable").
    std::string Status() const;

    // Blocking POST /v1/join. CALL ON A WORKER THREAD (it round-trips). Returns the
    // JoinInfo (ok=false on any failure).
    static JoinInfo Join(const std::string& masterUrl, const std::string& lobbyId,
                         int timeoutMs);

private:
    mutable std::mutex mu_;
    std::vector<LobbyRow> rows_;
    uint64_t generation_ = 0;
    std::string status_ = "Not refreshed yet.";
    std::atomic<bool> inFlight_{false};
};

}  // namespace coop::net::lobby
