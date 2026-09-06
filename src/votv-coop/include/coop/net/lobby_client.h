// coop/net/lobby_client.h -- the server-browser feed and join (the master's client side).
// The MTA precedent is its server list (fetch, parse and cache the master list) and its row
// model; we diverge from its per-server UDP query (ping is measured after connect, not
// pre-listed). Talks to the master server over the HTTP client: a GET of the lobby list
// (async, coalesced) and a POST to join (the credentials and identities to dial the host).
// Threading: the refresh spawns a detached worker (the round-trip must not stall a frame);
// the parsed rows land in a mutex-guarded snapshot the render thread reads via CopyRows.
// Join is blocking; the session manager calls it on a worker.

#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace coop::net::lobby {

// One browser row, parsed from a lobby-list element. `lobbyId` is the opaque master handle
// (the join key).
struct LobbyRow {
    std::string lobbyId;
    std::string name;
    std::string version;    // the host's announced version label
    std::string game;       // the host's game target, e.g. "0.9.0n" (empty: a host that predates the field)
    std::string world;
    int  playersCur = 0;
    int  playersMax = 0;
    int  ageSec = 0;        // seconds since the host's last heartbeat
    int  proto = 0;         // the host's protocol version (0: a host that predates the field; the wire-level mismatch close stays the backstop)
    // An admission gate stands behind this: a joiner proves knowledge of the lobby password
    // inside the peer-admission exchange (coop/net/lobby_password.h for the construction,
    // peer_admission.cpp for the check), and a wrong or missing proof is refused before a seat
    // is spent. So this field is the browser's display of a real property. A host may still
    // announce whatever it likes: the master echoes this bool and does not verify it. The gate
    // is at the host's own admission, not at the listing.
    bool locked = false;
    bool direct = false;    // a port-forwarded UDP host (a browser badge; join returns ip:port, not relay credentials)
};

// Everything a joining client needs to dial the host, returned by the join POST. ok=false on
// any failure. Two shapes: direct=false is the peer-to-peer credential block below;
// direct=true is just `addr` for a plain direct UDP connect (the host forwarded its port;
// nothing to relay).
struct JoinInfo {
    bool ok = false;
    bool direct = false;
    std::string addr;            // direct only: "ip:port" to ConnectDirect to
    std::string sessionId;
    // No peer identity: a joiner registers on the signalling server under its own durable
    // identity, so the master's per-session mint for the joining side has no reader. The master
    // still emits the field for older clients; we do not read it.
    std::string hostIdentity;    // the host's identity we dial (`gen:<64 hex>`)
    std::string signalingUrl;    // "host:port"
    std::string signalingToken;  // the shared signaling bearer
    std::string stun;            // "host:port" or ""
    std::string turnUri;         // "turn:host:port" (first uri, transport stripped) or ""
    std::string turnUser;
    std::string turnPass;
};

// The master's latest-released-mod record. ok=false when the master is unreachable or has no
// record; the caller stays silent then (never nag an offline player). The master also
// serves a release-page url and we deliberately do not parse it: the version label prints
// no address (a one-line label has no room for one), and the one place the mod names a
// download, the version-mismatch line on a refused join, uses the compiled releases url, so
// a field with no reader is retired. Re-add the parse only with a surface that renders it.
struct LatestInfo {
    bool ok = false;
    int proto = 0;     // latest released kProtocolVersion
    std::string mod;   // human tag, e.g. "0.9.0-n"
};

class LobbyClient {
public:
    // Kick off an async lobby-list GET against `masterUrl` ("host:port"), optionally filtered to
    // `versionFilter` (empty: all). Non-blocking; a refresh already in flight is coalesced
    // (returns immediately). Results are readable via CopyRows.
    void RefreshAsync(const std::string& masterUrl, const std::string& versionFilter);

    // Blocking GET of the latest record (the launch check). Call on a worker thread.
    static LatestInfo FetchLatest(const std::string& masterUrl, int timeoutMs);

    // Render or game thread: copy the latest fetched rows into `out`. Returns the fetch
    // generation (increments on each completed refresh) so the caller can tell new data from a
    // re-read.
    uint64_t CopyRows(std::vector<LobbyRow>& out) const;

    // The same generation without copying the rows. A screen that repaints only on new data has
    // to ask this every tick, and CopyRows is a full vector copy of up to 64 rows of strings;
    // the question "is there anything new" must not cost the answer.
    uint64_t Generation() const;

    // How many times the rows themselves changed, which is not the same question as Generation,
    // and conflating them shipped a real defect. Generation means an attempt completed, repaint:
    // it moves on a failure too, because the status line and the ages on screen did change. A
    // consumer that also used it to answer when the rows were last fetched re-stamped its age
    // clock every time the master failed to answer, so the rows never aged, the stale dim never
    // fired, and the pane printed "updated just now" under "cannot reach the server list". This
    // one moves only when the rows were replaced. Age and freshness key on this; repainting
    // keys on the other.
    uint64_t DataGeneration() const;

    // A short human status for the browser footer ("Refreshing...", "4 servers", "master
    // unreachable").
    std::string Status() const;

    // How many fetches in a row have failed; 0 the moment one succeeds. The browser's
    // cannot-reach alarm keys on this and never on elapsed time since the last success, because
    // those are different claims: a player who alt-tabbed for a minute has an old list and a
    // working master, and telling them the master is down would be a claim the UI invented from
    // a clock. Two consecutive failures is a master that answered neither of the last two
    // attempts.
    int ConsecutiveFailures() const;

    // Blocking join POST. Call on a worker thread (it round-trips). Returns the join info
    // (ok=false on any failure).
    static JoinInfo Join(const std::string& masterUrl, const std::string& lobbyId,
                         int timeoutMs);

private:
    mutable std::mutex mu_;
    std::vector<LobbyRow> rows_;
    uint64_t generation_ = 0;      // completed attempts -- "repaint"
    uint64_t dataGeneration_ = 0;  // successful attempts -- "the rows changed"
    std::string status_ = "Not refreshed yet.";
    int consecutiveFailures_ = 0;
    std::atomic<bool> inFlight_{false};
};

}  // namespace coop::net::lobby
