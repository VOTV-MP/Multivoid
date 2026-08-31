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
    std::string version;    // host's mod semver (v122; legacy hosts announced a game-style tag)
    std::string game;       // host's VOTV game target, e.g. "0.9.0-n" (v122; "" = pre-field host)
    std::string world;
    int  playersCur = 0;
    int  playersMax = 0;
    int  ageSec = 0;        // seconds since the host's last heartbeat
    int  proto = 0;         // host's kProtocolVersion (v59 gate: JoinLobby rejects a
                            // mismatch with an "update your mod" message; 0 = the host
                            // predates the field -- not verifiable, the wire-level
                            // protocol-mismatch close stays the backstop). 2026-06-11.
    // THERE IS AN ADMISSION GATE BEHIND THIS NOW (security A2, built 2026-08-31, proto
    // 149). A joiner proves knowledge of the lobby password inside the peer-admission
    // exchange -- `coop/net/lobby_password.h` for the construction, `peer_admission.cpp`
    // for where it is checked -- and a wrong or missing proof is refused before a seat is
    // spent. So this field is the browser's DISPLAY of a real property, not a promise
    // nothing keeps.
    //
    // THIS COMMENT HAS NOW BEEN WRONG IN BOTH DIRECTIONS, which is why it is worth the
    // space. It first asserted a "game-layer join-secret" that existed nowhere; `6f0c2bf8`
    // deleted the MASTER's copy of that same false assertion and missed this one (a
    // deletion owes a CENSUS, not a fix at the site that happened to be read). It was then
    // corrected to "a badge and nothing else", which was true for six weeks and became
    // false the day the gate shipped -- the stale-open direction. A status sentence in a
    // header is a CLAIM about code that moves.
    //
    // WHAT IS STILL TRUE AND IS NOT A GATE: `direct` below, and the fact that a HOST may
    // announce whatever it likes -- the master echoes this bool and does not verify it.
    // The gate is at the host's own admission, not at the listing.
    bool locked = false;
    bool direct = false;    // conn=="direct": a port-forwarded UDP host (browser
                            // badge; join returns ip:port, not ICE creds). 2026-06-11.
};

// Everything a joining client needs to dial the host, returned by POST /v1/join.
// ok=false on any failure. Two shapes (2026-06-11): direct=false -> the P2P/ICE
// cred block below; direct=true -> just `addr` ("ip:port") for a plain LanDirect
// UDP connect (the host forwarded its port; nothing to relay).
struct JoinInfo {
    bool ok = false;
    bool direct = false;
    std::string addr;            // direct only: "ip:port" to ConnectDirect to
    std::string sessionId;
    // No `peerIdentity` (RULE 2, 2026-08-29): a joiner registers on the signaling
    // server under its OWN durable identity, so the master's per-session mint for
    // the joining side has no reader left. The master still emits the field for
    // already-released clients; we simply stop believing it.
    std::string hostIdentity;    // the host's identity we dial (`gen:<64 hex>`)
    std::string signalingUrl;    // "host:port"
    std::string signalingToken;  // the shared signaling bearer
    std::string stun;            // "host:port" or ""
    std::string turnUri;         // "turn:host:port" (first uri, transport stripped) or ""
    std::string turnUser;
    std::string turnPass;
};

// The master's "latest released mod" record (GET /v1/latest, v59 launch toast).
// ok=false when the master is unreachable / pre-v59 (404) -- the caller stays
// silent in that case (never nag an offline player).
// The /v1/latest verdict. The master ALSO serves a `url` (the release page) and we
// deliberately do not parse it: since 2026-08-31 the version label prints no address
// (user: the one-line label had no room for ~37 characters nobody can click), so a
// field with no reader is retired rather than kept warm (RULE 2). The one place the
// mod still names a download -- the version-mismatch line on a refused join -- uses
// the compiled `net::kReleasesUrl`, not a server-supplied string, so nothing here
// regressed. Re-add the parse only together with a surface that renders it.
struct LatestInfo {
    bool ok = false;
    int proto = 0;     // latest released kProtocolVersion
    std::string mod;   // human tag, e.g. "0.9.0-n"
};

class LobbyClient {
public:
    // Kick off an async GET /v1/lobbies against `masterUrl` ("host:port"), optionally
    // filtered to `versionFilter` (empty = all). Non-blocking; a refresh already in
    // flight is coalesced (returns immediately). Results readable via CopyRows.
    void RefreshAsync(const std::string& masterUrl, const std::string& versionFilter);

    // Blocking GET /v1/latest (the v59 launch toast). CALL ON A WORKER THREAD.
    static LatestInfo FetchLatest(const std::string& masterUrl, int timeoutMs);

    // Render/game thread: copy the latest fetched rows into `out`. Returns the fetch
    // generation (increments on each completed refresh) so the caller can tell new
    // data from a re-read.
    uint64_t CopyRows(std::vector<LobbyRow>& out) const;

    // The same generation WITHOUT copying the rows. A screen that repaints only on new
    // data has to ask this every tick, and CopyRows is a full vector copy of up to 64 rows
    // of strings -- the question "is there anything new" must not cost the answer.
    uint64_t Generation() const;

    // HOW MANY TIMES THE ROWS THEMSELVES CHANGED -- which is NOT the same question as
    // `Generation()`, and conflating them shipped a real defect.
    //
    // `Generation()` means "an attempt COMPLETED, repaint": it moves on a failure too,
    // because the status line and the ages on screen did change. A consumer that also uses
    // it to answer "when were these rows last FETCHED" therefore re-stamps its age clock
    // every time the master fails to answer -- so the rows never age, the stale-dim never
    // fires, and the pane prints "updated just now" directly under "Cannot reach the server
    // list". Both halves were individually reasonable and the pair was wrong (post-ship
    // audit, 2026-08-31).
    //
    // This one moves ONLY when `rows_` was replaced. Age and freshness key on this;
    // repainting keys on the other.
    uint64_t DataGeneration() const;

    // A short human status for the browser footer ("Refreshing...", "4 servers",
    // "master unreachable").
    std::string Status() const;

    // HOW MANY FETCHES IN A ROW HAVE FAILED. 0 the moment one succeeds.
    //
    // The browser's "cannot reach the master" alarm keys on THIS and never on elapsed time
    // since the last success, because those are different claims: a player who alt-tabbed
    // for a minute has an old list and a working master, and telling them the master is
    // down would be a lie the UI invented from a clock. Two consecutive failures is a
    // master that answered neither of the last two 5 s attempts.
    int ConsecutiveFailures() const;

    // Blocking POST /v1/join. CALL ON A WORKER THREAD (it round-trips). Returns the
    // JoinInfo (ok=false on any failure).
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
