// coop/net/lobby_announcer.h -- the host side of the master plane.
//
// MTA is the precedent: CMasterServerAnnouncer and CMasterServer announce on host start, then keep
// the lobby alive with a periodic heartbeat. We diverge in three ways: one master rather than a
// redundant list, a 30 s heartbeat -- under the master's 120 s TURN-credential lifetime and its 300
// s lobby expiry -- and an explicit leave on stop.
//
//   POST /v1/host        sessionId, an opaque lobbyId, the host token, identities and ICE
//   POST /v1/heartbeat   every 30 s: keep the lobby alive, refresh the current player count
//   POST /v1/visibility  the "hide from the browser" toggle
//   POST /v1/leave       on stop
//
// Threading: Host() blocks, so call it on a worker. On success it spawns the heartbeat worker
// thread, and Stop() signals and joins it. The credentials are mutex-guarded, since the heartbeat
// thread reads them while SetListed writes listed_.

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

    // Blocking POST /v1/host. CALL ON A WORKER THREAD. On success it stashes the credentials and
    // STARTS the 30 s heartbeat thread that keeps the lobby alive; a prior lobby is Stop()ped
    // first. Returns the HostInfo, with ok=false on failure, in which case nothing was started.
    //
    // A POSITIVE `directPort` announces a DIRECT lobby: the master records conn="direct" with this
    // LISTEN port and advertises the announce's source ip, so /v1/join returns {conn, addr} for a
    // plain UDP connect instead of ICE credentials. Zero is the normal P2P lobby.
    //
    // The announce body carries the mod identity itself -- the version pair, game from
    // coop::version::kGameTarget and proto from kProtocolVersion -- so there is a single authority
    // and no caller-passed version string, which is what once let a stale duplicate ride.
    HostInfo Host(const std::string& masterUrl, const std::string& name,
                  const std::string& world,
                  bool locked, int playersMax, int timeoutMs, int directPort = 0);

    // The heartbeat publishes a live player count from this callback (host wires it to
    // the session's connected-peer count + 1). null -> publishes playersMax's host (1).
    // ATOMIC because the heartbeat worker may ALREADY be running when this is
    // installed: the env-host lane announces (spawning HeartbeatLoop) before the
    // harness reaches its install point, so the std::thread constructor's
    // happens-before edge does not cover this write. Benign in practice on x86-64
    // and the first beat is 30 s out, but an unsynchronised raw function pointer
    // read from another thread is UB, and this module's own callers decline
    // cheaper races than that one.
    void SetPlayerCountFn(int (*fn)()) { playerCountFn_.store(fn, std::memory_order_release); }

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
    std::atomic<int (*)()> playerCountFn_{nullptr};
};

}  // namespace coop::net::lobby
