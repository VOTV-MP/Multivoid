// coop/net/lobby_announcer.h -- the host side of the master plane.
//
// MTA precedent: CMasterServerAnnouncer / CMasterServer (announce on host start +
// periodic reminder/heartbeat, last-seen kept alive) -- reference/mtasa-blue/Server/
// mods/deathmatch/utils/CMasterServerAnnouncer.h. We diverge: one master (not a
// redundant list yet), a 30s heartbeat (< the master's 120s TURN-cred TTL / 300s
// lobby expiry, per design 7/8), and an explicit leave on stop.
//
//   POST /v1/host       -> sessionId + opaque lobbyId + host token + identities + ICE
//   POST /v1/heartbeat   (every 30s) -> keep the lobby alive + refresh players_cur/listed
//   POST /v1/visibility  -> the "hide from browser" toggle (design 5.6)
//   POST /v1/leave        on stop
//
// Threading: Host() blocks (call on a worker). On success it spawns the heartbeat
// worker thread; Stop() signals + joins it. The creds are mutex-guarded (the
// heartbeat thread reads them; SetListed writes listed_).

#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

namespace coop::net::lobby {

// What POST /v1/host returns: the host's session creds + identities + ICE block.
// Everything needed to build a P2P host coop::net::Config and to keep the lobby
// alive (sessionId + token). ok=false on any failure.
struct HostInfo {
    bool ok = false;
    std::string sessionId;
    std::string lobbyId;         // opaque public id (browser key)
    std::string token;           // host bearer (heartbeat/leave/visibility auth)
    std::string hostIdentity;    // our signaling identity (the host listens under it)
    std::string signalingUrl;    // "host:port"
    std::string signalingToken;  // shared signaling bearer
    std::string stun;            // "host:port" or ""
    std::string turnUri;         // "turn:host:port" or ""
    std::string turnUser;
    std::string turnPass;
};

class LobbyAnnouncer {
public:
    ~LobbyAnnouncer();

    // Blocking POST /v1/host. CALL ON A WORKER THREAD. On success stashes the creds
    // and STARTS the 30s heartbeat thread (keeping the lobby alive). A prior lobby is
    // Stop()'d first. Returns the HostInfo (ok=false on failure -> nothing started).
    HostInfo Host(const std::string& masterUrl, const std::string& name,
                  const std::string& version, const std::string& world,
                  bool locked, int playersMax, int timeoutMs);

    // The heartbeat publishes a live player count from this callback (host wires it to
    // the session's connected-peer count + 1). null -> publishes playersMax's host (1).
    void SetPlayerCountFn(int (*fn)()) { playerCountFn_ = fn; }

    // Hide / show the lobby in the public browser (POST /v1/visibility, async). The
    // session stays live -- this only flips `listed`. (design 5.6)
    void SetListed(bool listed);

    // POST /v1/leave + stop/join the heartbeat thread. Idempotent. Blocks briefly
    // (joins the worker) -- call off the game thread.
    void Stop();

    bool active() const { return active_.load(); }

private:
    void HeartbeatLoop();
    void StopHeartbeatLocked();   // caller holds threadMu_; signals + joins hbThread_

    std::mutex threadMu_;         // serializes hbThread_ start/stop (a concurrent Host
                                  // must never move-assign over a joinable thread)
    mutable std::mutex mu_;       // guards the creds snapshot + listed_
    std::string masterUrl_;
    std::string sessionId_;
    std::string token_;
    std::string lobbyId_;
    bool listed_ = true;

    std::atomic<bool> active_{false};
    std::atomic<bool> stop_{false};
    std::thread hbThread_;
    int (*playerCountFn_)() = nullptr;
};

}  // namespace coop::net::lobby
