// coop/net/lobby_announcer.h -- the host side of the master plane.
//
// MTA is the precedent: CMasterServerAnnouncer and CMasterServer announce on host start, then keep the
// lobby alive with a periodic heartbeat. We diverge in four ways: one master rather than a redundant list,
// a 30 s heartbeat -- three to the master's 90 s lobby expiry --, an explicit leave on stop, and a
// re-announce when the master answers that it forgot the session (the announcer owns the lifecycle).
//
//   POST /v1/host        (after /v1/host/challenge) sessionId, an opaque lobbyId, the host token, identities, ICE
//   POST /v1/heartbeat   every 30 s: the lobby kept alive, its player count and their links, the host's
//                        TURN credential reported and its renewal taken from the answer
//   POST /v1/visibility  the "hide from the browser" toggle; POST /v1/leave on stop
//
// Threading: Host() blocks, so call it on a worker. On success it spawns the heartbeat worker thread, and
// Stop() signals and joins it. The credentials are mutex-guarded, since the heartbeat thread reads them
// while SetListed writes listed_ and a re-announce replaces them.

#pragma once

#include "coop/net/lobby_links.h"
#include "coop/net/turn_credential.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

namespace coop::net::lobby {

// What POST /v1/host returns: the host's session creds + identities + ICE block.
// Everything needed to build a P2P host coop::net::Config and to keep the lobby
// alive (sessionId + token). ok=false on any failure. A DIRECT lobby's answer
// carries no signaling or ICE block (the master's h_host never gives one to a
// direct host), so its ok needs only the session, the token and the identity.
struct HostInfo {
    bool ok = false;
    std::string sessionId;
    std::string lobbyId;         // opaque public id (browser key)
    std::string token;           // host bearer (heartbeat/leave/visibility auth)
    std::string hostIdentity;    // our signaling identity (the host listens under it)
    std::string signalingUrl;    // "host:port"
    std::string signalingToken;  // shared signaling bearer
    std::string stun;            // "host:port" or ""
    TurnCredential turn;         // the relay credential minted for this host, or empty
};

// What a lobby was announced with. Kept by the announcer so it can announce the same game again
// when the master forgets it; timeoutMs bounds every POST of the request, the re-announce's too.
struct AnnounceRequest {
    std::string masterUrl;
    std::string name;
    std::string world;
    bool locked = false;
    int playersMax = 0;
    int timeoutMs = 0;
    int directPort = 0;   // positive: a DIRECT lobby, announced with this listen port
};

// The outcome of one re-announce POST (heartbeat thread). `listed` is the value the POSTed body
// carried.
struct ReannounceResult {
    bool ok = false;        // the master answered 200 with a session, a token and a lobby id
    bool stopped = false;   // Stop() or shutdown was seen before the first POST: nothing was POSTed.
                            // Seen between the create's POSTs, the create is abandoned and reads reached=false
    bool reached = false;   // the master answered at all
    int status = 0;
    bool listed = false;
    std::string refused;    // the master's word of a refusal no retry can clear ("bad-proof",
                            // "build-too-old", "blocked"); empty for every other outcome
    std::string lobbyId;    // the new lobby's id when ok
};

// The announcer owns the lobby's lifecycle: it keeps the request it announced with, and when the
// master answers a heartbeat that it no longer knows the session (a restart, a reap), the heartbeat
// thread announces the same game again, the master minting a new session and lobby id.
class LobbyAnnouncer {
public:
    ~LobbyAnnouncer();

    // Blocking POST /v1/host (after its challenge, see lobby_proof.h). CALL ON A WORKER THREAD. On
    // success it stashes the credentials and STARTS the 30 s heartbeat thread that keeps the lobby
    // alive; a prior lobby is Stop()ped first. Returns the HostInfo, with ok=false on failure, in
    // which case nothing was started.
    //
    // A Stop() that begins while this runs (the session ended under the worker) ends the create at
    // its next POST; a lobby it had already made is left again and no heartbeat is started.
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

    // The heartbeat publishes the host's players counted by the link it measures on each from this
    // callback (the host wires it to the session's links); null publishes none. Atomic for the reason
    // the player count's is.
    void SetLinksFn(LobbyLinks (*fn)()) { linksFn_.store(fn, std::memory_order_release); }

    // Hide / show the lobby in the public browser (POST /v1/visibility, async). The
    // session stays live -- this only flips `listed`. (design 5.6)
    void SetListed(bool listed);

    // POST /v1/leave + stop/join the heartbeat thread. Idempotent. Blocks briefly
    // (joins the worker) -- call off the game thread.
    void Stop();

    bool active() const { return active_.load(); }

    // The lobby this announcer holds; empty while it holds none, from the start of a Stop() and
    // through an announce's POST. Any thread.
    std::string OwnLobbyId() const;

private:
    void HeartbeatLoop();
    // Heartbeat thread: POSTs the kept request again.
    ReannounceResult Reannounce();
    void StopHeartbeatLocked();   // caller holds threadMu_; signals + joins hbThread_

    std::mutex threadMu_;         // serializes hbThread_ start/stop (a concurrent Host
                                  // must never move-assign over a joinable thread)
    mutable std::mutex mu_;       // guards the creds snapshot, listed_, request_
    AnnounceRequest request_;     // what the live lobby was announced with
    std::string sessionId_;
    std::string token_;
    std::string lobbyId_;
    // The last two TURN credentials the master handed this lobby's host: while its session holds one of
    // them, the session is this lobby's, and a beat reports it and takes a renewal.
    std::string turnCur_;
    std::string turnPrev_;
    bool listed_ = true;

    std::atomic<bool> active_{false};
    std::atomic<bool> stop_{false};
    std::atomic<uint64_t> gen_{0};   // bumped first by every Stop(): a Host() that sees it move beyond its own Stop() was cancelled
    std::thread hbThread_;
    std::atomic<int (*)()> playerCountFn_{nullptr};
    std::atomic<LobbyLinks (*)()> linksFn_{nullptr};
};

}  // namespace coop::net::lobby
