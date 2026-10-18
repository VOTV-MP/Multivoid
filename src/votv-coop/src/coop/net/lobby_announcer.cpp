// coop/net/lobby_announcer.cpp -- see coop/net/lobby_announcer.h.

#include "coop/net/lobby_announcer.h"
#include "coop/game_build/game_build.h"

#include "coop/build_trust/build_trust.h"
#include "coop/net/http_client.h"
#include "coop/net/lobby_proof.h"
#include "coop/net/peer_identity.h"  // LocalIdentityString -- the identity joiners dial
#include "coop/net/protocol.h"  // kProtocolVersion -- announced for the browser's join gate
#include "coop/session/shutdown.h"
#include "coop/version.h"  // kGameTarget -- the announced identity pair's game half
#include "ice_config.h"  // co-located: AppliedTurnUser, QueueTurnRenewal
#include "json_util.h"  // internal, co-located in src/coop/net/ (not a public API header)
#include "ue_wrap/core/log.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
#include <utility>

namespace coop::net::lobby {
namespace {

namespace J = coop::net::jsonu;

// The build claim: this binary's own hash, its signing key's id (0 when unsigned) and its
// signature, announced as read, verified or not -- the master records it and verifies nothing.
// The master-list reply of MTA's ASE QueryLight carries the build type and number among the
// server's fields (reference/mtasa-blue/Server/mods/deathmatch/logic/ASE.cpp:429-466, pushed at
// reference/mtasa-blue/Server/mods/deathmatch/utils/CMasterServerAnnouncer.h:96-109);
// this one carries the host's whole signed claim.
struct BuildClaim {
    bool present = false;
    std::string sha;
    unsigned key = 0;
    std::string sig;
};

// A hash that was never computed is no claim: it must not read as an unsigned build.
BuildClaim ReadBuildClaim() {
    BuildClaim c;
    if (coop::build_trust::Self().step < coop::build_trust::SelfStep::NoSigFile) return c;
    c.present = true;
    c.sha = coop::build_trust::SelfShaHex();
    c.key = (coop::build_trust::Self().step == coop::build_trust::SelfStep::SigPresent)
                ? static_cast<unsigned>(coop::build_trust::Self().sig.keyId) : 0u;
    c.sig = coop::build_trust::SelfSigHex();
    return c;
}

// The POST /v1/host body of a request: the first announce and every re-announce build it here, so
// a re-announced lobby carries the same identity, claim and (current) listing as the first.
// `nonce` and `proof` ride along when the create is proved (both empty: the old master's create).
std::string HostBody(const AnnounceRequest& r, bool listed, const BuildClaim& claim,
                     const std::string& nonce, const std::string& proof) {
    // nlohmann builds + escapes the body (name/world are user-supplied -> may contain
    // quotes/backslashes; never string-concat them into JSON). J::Dump uses the
    // replace error handler so invalid-UTF-8 input can't throw out of a worker.
    J::Json b;
    b["name"] = r.name;
    // The Paper-pair identity: the game target plus the build number (proto). There is no "version"
    // field -- the mod-semver axis was retired whole (RULE 2).
    b["game"] = coop::version::kGameTarget;      // join gate tier 1
    b["game_build"] = coop::game_build::Running().id.key;  // the game this host runs, for the browser's mark
    b["proto"] = static_cast<int>(coop::net::kProtocolVersion);  // the build number (tier 2)
    b["world"] = r.world;
    // The identity joiners dial. It is this install's durable PUBLIC KEY rendered `gen:<64 hex>` --
    // the same value the admission challenge proves possession of, so the rendezvous name and the
    // provable name are one thing (RULE 2). It is REQUIRED: the master mints no per-session name to
    // stand in for a host that sends none, and `/v1/host` answers a missing or malformed identity
    // with a named 400. There is no fallback, because a minted name is one the master asserts on
    // our behalf, and the relay registers only names their holder signed for.
    b["identity"] = coop::net::peer_identity::LocalIdentityString();
    b["locked"] = r.locked;
    b["players_max"] = r.playersMax;
    if (r.directPort > 0) {  // DIRECT lobby (see header): master advertises src-ip:port
        b["conn"] = "direct";
        b["direct_port"] = r.directPort;
    }
    if (claim.present) {
        b["build_sha"] = claim.sha;
        b["build_key"] = claim.key;
        b["build_sig"] = claim.sig;
    }
    b["listed"] = listed;
    if (!proof.empty()) {
        b["nonce"] = nonce;
        b["proof"] = proof;
    }
    return J::Dump(b);
}

// What the challenge came to: a nonce to sign, a master that has no such route (an old master),
// or a failure to hand back as the create's own.
enum class Challenge { Nonce, OldMaster, Failed };

// An unknown route on today's master answers exactly this (server/src/bin/master.rs's 404), so a
// proxy's or a captive portal's 404 is not mistaken for an old master.
bool IsOldMasterNotFound(const http::Response& resp) {
    J::Json j;
    if (resp.status != 404 || !J::ParseObject(resp.body, j) || j.size() != 1) return false;
    auto it = j.find("error");
    return it != j.end() && it->is_string() && it->get<std::string>() == "not found";
}

bool Is64LowerHex(const std::string& s) {
    if (s.size() != 64) return false;
    for (char c : s)
        if (!(('0' <= c && c <= '9') || ('a' <= c && c <= 'f'))) return false;
    return true;
}

// POST /v1/host/challenge, whose answer is the nonce one create signs. `failure` is the response to
// return as the create's when this is Failed (unreachable, another status, another 404). A 200
// whose nonce is not 64 hex is a master that does not speak this exchange: the old-master case.
Challenge FetchChallenge(const AnnounceRequest& r, std::string& nonce, http::Response& failure) {
    const http::Response resp = http::Post(r.masterUrl, "/v1/host/challenge", "{}", r.timeoutMs);
    if (resp.ok && IsOldMasterNotFound(resp)) return Challenge::OldMaster;
    if (!resp.ok || resp.status != 200) { failure = resp; return Challenge::Failed; }
    J::Json j;
    if (!J::ParseObject(resp.body, j) || !Is64LowerHex(J::Str(j, "nonce"))) return Challenge::OldMaster;
    nonce = J::Str(j, "nonce");
    return Challenge::Nonce;
}

// The master's own error word of a refusal ("proof-required"), empty when the body has none.
std::string ErrorWord(const http::Response& resp) {
    J::Json j;
    return J::ParseObject(resp.body, j) ? J::Str(j, "error") : std::string();
}

// The word of a refusal that waiting cannot clear: a proof the master will not accept, a build
// below its floor, an identity it blocks. Empty for anything else, which may clear. MTA retries
// only a timeout (CMasterServerAnnouncer.h:154) and gives up on every other answer (:169-180); we
// also retry what the host's own clock or the master's restart can clear (410, 409, 5xx).
std::string PermanentRefusal(const http::Response& resp) {
    if (!resp.ok) return {};
    const std::string why = ErrorWord(resp);
    if ((resp.status == 400 && why == "bad-proof") ||
        (resp.status == 403 && (why == "build-too-old" || why == "blocked")))
        return why;
    return {};
}

// What ends a create early, besides the process shutdown. The heartbeat thread's re-announce is
// ended by the announcer's stop flag. A Host() worker cannot read that flag (its own leading Stop()
// left it set); it is ended by a Stop() that began after its own, which moved the generation.
struct CreateStop {
    const std::atomic<bool>* flag = nullptr;
    const std::atomic<uint64_t>* gen = nullptr;
    uint64_t genAtStart = 0;

    bool Now() const {
        return coop::shutdown::IsShuttingDown() || (flag != nullptr && flag->load()) ||
               (gen != nullptr && gen->load() != genAtStart);
    }
};

// One create, proved. The first announce and every re-announce come through here, so both sign the
// same way. A create is a challenge then a POST that carries the signed nonce; the signature is
// made fresh on every try, since a nonce is single-use. A refused nonce (410) or a missing proof
// (409) is retried ONCE with a new challenge, so a create takes at most four POSTs (two
// challenges, two creates); a bad proof (400) is a bug or a misconfigured master and is not
// retried. A master with no challenge route is an old master and gets the create it understands,
// unproved: a P2P host's signaling and TURN come only from that answer, so an unlisted host would
// fall back to a direct listen.
//
// `stop` is read before every POST after the first, so a slow master cannot hold a stopping caller
// for the sum of four requests. An abandoned create returns a response that is not ok.
http::Response CreateLobby(const AnnounceRequest& r, bool listed, const BuildClaim& claim,
                           const CreateStop& stop) {
    auto stopped = [&stop] {
        const bool now = stop.Now();
        if (now) UE_LOGI("lobby: host announce -- stopped before the next request");
        return now;
    };
    // MTA sends one request per announce and retries only a timeout (CMasterServerAnnouncer.h:154);
    // here a create is a challenge and a POST, and a spent or missing nonce is retried once.
    http::Response resp;
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (attempt > 0 && stopped()) return http::Response{};
        std::string nonce;
        http::Response failure;
        const Challenge ch = FetchChallenge(r, nonce, failure);
        if (ch == Challenge::Failed) return failure;
        if (stopped()) return http::Response{};
        if (ch == Challenge::OldMaster) {
            // RETIREMENT: this branch goes when every master in the compiled-in list answers the
            // challenge route (the lobby-proof design, "Retirement"); the build that drops it says so
            // in its release notes.
            static std::atomic<bool> said{false};
            if (!said.exchange(true))
                UE_LOGW("lobby: this master predates the lobby proof -- announcing unproved");
            return http::Post(r.masterUrl, "/v1/host", HostBody(r, listed, claim, {}, {}), r.timeoutMs);
        }
        // The name signed is the host part of the URL that was dialled, taken by the same code that
        // dials it, so a proof made for a master that was not asked verifies nowhere else.
        const std::string proof = coop::net::lobby_proof::ProofHex(
            coop::net::peer_identity::LocalIdentityString(), nonce, http::HostName(r.masterUrl));
        if (proof.empty()) {
            UE_LOGW("lobby: host announce -- no proof could be made");
            return http::Response{};
        }
        resp = http::Post(r.masterUrl, "/v1/host", HostBody(r, listed, claim, nonce, proof),
                          r.timeoutMs);
        if (!resp.ok) return resp;
        const std::string why = ErrorWord(resp);
        if (resp.status == 400 && why == "bad-proof") {
            UE_LOGW("lobby: host announce -- the master refused the proof (bad-proof)");
            return resp;
        }
        if (resp.status == 403) {
            if (why == "build-too-old")
                UE_LOGW("lobby: host announce -- the master refuses this build -- update Multivoid");
            else if (why == "blocked")
                UE_LOGW("lobby: host announce -- the master has blocked this install's identity");
            else
                UE_LOGW("lobby: host announce -- the master refused it (403)");
            return resp;
        }
        const bool retryable = (resp.status == 410 && why == "nonce-expired") ||
                               (resp.status == 409 && why == "proof-required");
        if (!retryable) return resp;
        if (attempt == 1 && resp.status == 409)
            UE_LOGW("lobby: host announce -- the master requires a signed announce and refused this one");
    }
    return resp;
}

// POST /v1/leave for a session: the retraction of a lobby this process announced.
void SendLeave(const std::string& url, const std::string& sessionId, const std::string& token) {
    J::Json b;
    b["sessionId"] = sessionId;
    b["token"] = token;
    const http::Response resp = http::Post(url, "/v1/leave", J::Dump(b), 5000);
    UE_LOGI("lobby: left lobby -> %s (%d)", resp.ok ? "ok" : "unreachable", resp.status);
}

}  // namespace

LobbyAnnouncer::~LobbyAnnouncer() { Stop(); }

HostInfo LobbyAnnouncer::Host(const std::string& masterUrl, const std::string& name,
                              const std::string& world,
                              bool locked, int playersMax, int timeoutMs, int directPort) {
    HostInfo info;
    // The generation is read BEFORE our own Stop(), so a Stop() that finishes while that one is
    // still joining and leaving is counted (see `gen` below).
    const uint64_t entryGen = gen_.load();
    Stop();  // retire any prior lobby first (sends /leave, joins) -- on a worker, safe

    const AnnounceRequest req{masterUrl, name, world, locked, playersMax, timeoutMs, directPort};
    const BuildClaim claim = ReadBuildClaim();
    if (claim.present)
        UE_LOGI("lobby: announcing the build claim key=%u sha=%.8s", claim.key, claim.sha.c_str());
    // Stop() above left stop_ set (it ends the old heartbeat), so the flag cannot be read here: a
    // Stop() that begins from now on is told by the generation it moves, and the process shutdown
    // ends this create too. Our own Stop() adds exactly one to the generation, so entry + 1 is the
    // value of a session nobody has stopped since; any other value (a second Stop() before, during
    // or after ours, including one that completed inside the lead-in) is a bump we did not make and
    // cancels the create.
    const uint64_t gen = entryGen + 1;
    const http::Response resp = CreateLobby(req, true, claim, CreateStop{nullptr, &gen_, gen});
    // No master address in these lines: they reach the console, and which master was asked is the
    // caller's to name (master_slots::DisplayName).
    if (!resp.ok) { UE_LOGW("lobby: host announce -- master unreachable"); return info; }
    if (resp.status != 200) { UE_LOGW("lobby: host announce -- master returned %d", resp.status); return info; }

    J::Json j;
    if (!J::ParseObject(resp.body, j)) { UE_LOGW("lobby: host announce -- malformed response"); return info; }
    info.sessionId      = J::Str(j, "sessionId");
    info.lobbyId        = J::Str(j, "lobbyId");
    info.token          = J::Str(j, "token");
    info.hostIdentity   = J::Str(j, "hostIdentity");
    info.signalingUrl   = J::Str(j, "signalingUrl");
    info.signalingToken = J::Str(j, "signalingToken");
    info.stun           = J::Str(j, "stun");
    info.turn           = J::ParseTurnCredential(j);
    // A DIRECT host is answered without the signaling block (the master's h_host: a direct host
    // never uses signaling), so only a P2P announce needs signalingUrl.
    info.ok = !info.sessionId.empty() && !info.token.empty() &&
              !info.hostIdentity.empty() && (directPort > 0 || !info.signalingUrl.empty());
    if (!info.ok) {
        UE_LOGW("lobby: host announce -- response missing session/token/identity%s",
                directPort > 0 ? "" : "/signaling");
        return info;
    }

    // Stash the creds + (re)start the heartbeat under threadMu_ so a concurrent Host
    // can never move-assign over a joinable hbThread_ (which would std::terminate).
    {
        std::lock_guard<std::mutex> tl(threadMu_);
        // A Stop() bumps the generation before it takes threadMu_, so an unchanged generation here
        // means any later Stop() runs after the heartbeat below exists, and its /leave retracts this
        // lobby; a changed one means the session this lobby was made for has ended.
        if (gen_.load() == gen) {
            StopHeartbeatLocked();  // join anything that snuck in since the top Stop()
            {
                std::lock_guard<std::mutex> lk(mu_);
                request_   = req;
                sessionId_ = info.sessionId;
                token_     = info.token;
                lobbyId_   = info.lobbyId;
                turnCur_   = info.turn.user;
                turnPrev_.clear();
                listed_    = true;
            }
            stop_.store(false);
            active_.store(true);
            hbThread_ = std::thread(&LobbyAnnouncer::HeartbeatLoop, this);
            UE_LOGI("lobby: announced '%s' lobbyId=%s -- heartbeating", name.c_str(), info.lobbyId.c_str());
            return info;
        }
    }
    // The POST runs outside threadMu_, like Stop()'s.
    UE_LOGI("lobby: host announce -- stopped while it was made; leaving lobbyId=%s", info.lobbyId.c_str());
    SendLeave(masterUrl, info.sessionId, info.token);
    return HostInfo{};
}

std::string LobbyAnnouncer::OwnLobbyId() const {
    std::lock_guard<std::mutex> lk(mu_);
    return active_.load() ? lobbyId_ : std::string();
}

ReannounceResult LobbyAnnouncer::Reannounce() {
    ReannounceResult out;
    if (stop_.load() || coop::shutdown::IsShuttingDown()) { out.stopped = true; return out; }  // before any POST
    AnnounceRequest req;
    bool listed;
    {
        std::lock_guard<std::mutex> lk(mu_);
        req = request_;
        listed = listed_;
    }
    out.listed = listed;
    // Outside mu_: a POST never runs under it. The claim is read again, so the re-announced lobby
    // carries the same one.
    const http::Response resp = CreateLobby(req, listed, ReadBuildClaim(), CreateStop{&stop_, nullptr, 0});
    out.reached = resp.ok;
    out.status = resp.status;
    out.refused = PermanentRefusal(resp);
    if (!resp.ok || resp.status != 200) return out;
    J::Json j;
    if (!J::ParseObject(resp.body, j)) return out;
    std::string sessionId = J::Str(j, "sessionId");
    std::string lobbyId = J::Str(j, "lobbyId");
    std::string token = J::Str(j, "token");
    if (sessionId.empty() || token.empty() || lobbyId.empty()) return out;
    // The TURN, signaling and STUN fields of the answer are not read: the host's session keeps its
    // credential, and the master's next beat answer re-sends the renewal (QueueTurnRenewal accepts
    // it because the session still holds the old one). The swap happens even when stop_ was set during
    // the POST: Stop() joins this thread before it copies the session fields, so its /leave
    // retracts this new lobby.
    {
        std::lock_guard<std::mutex> lk(mu_);
        sessionId_ = std::move(sessionId);
        token_ = std::move(token);
        lobbyId_ = lobbyId;
    }
    out.ok = true;
    out.lobbyId = std::move(lobbyId);
    return out;
}

void LobbyAnnouncer::HeartbeatLoop() {
    // The master stamped last_seen at /v1/host, so the first beat is one interval out.
    // 30 s: three beats to the master's 90 s lobby expiry, so one lost beat does not delist.
    // Sleep in 0.5s slices so Stop()/shutdown is responsive; bail on global shutdown so
    // we never issue HTTP into a tearing-down process.
    bool forgotten = false;  // the master answered a beat that it holds no such session
    int retryS = 0;          // seconds the next re-announce waits after a failure; 0: at once
    int waitS = 30;          // seconds before the next beat or re-announce
    while (!stop_.load() && !coop::shutdown::IsShuttingDown()) {
        for (int i = 0; i < waitS * 2 && !stop_.load() && !coop::shutdown::IsShuttingDown(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        if (stop_.load() || coop::shutdown::IsShuttingDown()) break;

        if (forgotten) {
            const ReannounceResult r = Reannounce();
            if (r.stopped) break;
            if (!r.refused.empty()) {
                // The one line for this hosting; the game goes on, unlisted. A host that wants the
                // listing back hosts again.
                UE_LOGW("lobby: the master refused announcing again (%s) and waiting cannot change "
                        "that -- no more tries for this hosting, the game stays unlisted",
                        r.refused.c_str());
                break;
            }
            retryS = (retryS == 0) ? 30 : std::min(retryS * 2, 300);
            if (r.ok) {
                forgotten = false;
                UE_LOGI("lobby: announced again lobbyId=%s listed=%d", r.lobbyId.c_str(),
                        r.listed ? 1 : 0);
                // The next beat goes at once: it carries `listed`, so a master that ignored it on
                // /v1/host hides the lobby within that beat, and it confirms the new lobby.
                waitS = 0;
            } else {
                const std::string why = !r.reached ? "unreachable"
                                        : r.status != 200 ? "status=" + std::to_string(r.status)
                                                          : "a malformed answer";
                UE_LOGW("lobby: announcing again failed (%s) -- next try in %d s", why.c_str(), retryS);
                waitS = retryS;
            }
            continue;
        }

        // The credential the host's session holds is reported while it is one of this lobby's, so the
        // master can renew it or re-send the renewal the host missed; a session holding another is not
        // this lobby's (a late beat of a retired lobby) and asks for nothing.
        const std::string held = AppliedTurnUser();
        std::string url, sid, tok, heldLobby;
        bool listed, ours;
        {
            std::lock_guard<std::mutex> lk(mu_);
            url = request_.masterUrl; sid = sessionId_; tok = token_; heldLobby = lobbyId_;
            listed = listed_;
            ours = !held.empty() && (held == turnCur_ || held == turnPrev_);
        }
        int (*const countFn)() = playerCountFn_.load(std::memory_order_acquire);
        const int pc = countFn ? countFn() : 1;

        J::Json b;
        b["sessionId"] = sid;
        b["token"] = tok;
        b["players_cur"] = pc;
        b["listed"] = listed;
        if (LobbyLinks (*const linksFn)() = linksFn_.load(std::memory_order_acquire)) {
            const LobbyLinks l = linksFn();
            b["links"] = J::Json{{"relayed", l.relayed}, {"direct", l.direct}, {"lan", l.lan}};
        }
        if (ours) b["turn_user"] = held;
        const http::Response resp = http::Post(url, "/v1/heartbeat", J::Dump(b), 8000);
        // The master's own answer to a session it does not hold (h_heartbeat, server/src/lobby.rs)
        // is "unknown session or bad token"; the match is on "unknown session", and the local fake
        // master (fake_master.py) answers the same words. A 403 from anything else, a proxy, is a
        // plain failure and mints no duplicate lobby. MTA re-announces on a timer: after a timeout
        // it retries 5 times, 5 minutes apart, and otherwise gives up to a 24 h reminder (:48-49,
        // :154-181 and :232 of reference/mtasa-blue/Server/mods/deathmatch/utils/
        // CMasterServerAnnouncer.h), and its push re-sends the same entry every 10 minutes (:50,
        // :96-109), an upsert on address. A proved create is an upsert on the host's key (it
        // replaces that key's row and keeps its lobbyId, server/src/lobby/host.rs); a lobby the
        // master forgot has no row to replace, so the re-announce mints a new one. We retry on the
        // 30/60/120/240/300 s schedule until the master answers, because a lobby the master forgot
        // is missing from the browser.
        if (resp.ok && resp.status == 403 &&
            resp.body.find("unknown session") != std::string::npos) {
            UE_LOGW("lobby: the master no longer knows lobbyId=%s (403) -- announcing again",
                    heldLobby.c_str());
            forgotten = true;
            waitS = retryS;
            continue;
        }
        if (!resp.ok || resp.status != 200) {
            UE_LOGW("lobby: heartbeat -> %s (status=%d) -- lobby may expire if this persists",
                    resp.ok ? "non-200" : "unreachable", resp.status);
            waitS = 30;
            continue;
        }
        retryS = 0;
        waitS = 30;
        // A renewal in the answer goes to the net thread, which writes it where no accept runs.
        if (!ours || stop_.load()) continue;
        J::Json j;
        if (!J::ParseObject(resp.body, j)) continue;
        const TurnCredential fresh = J::ParseTurnCredential(j);
        if (fresh.user.empty()) continue;
        // Only a queued renewal becomes one of this lobby's: rotating on a refused one would leave the
        // session's own credential in neither slot, and the host would never report it again.
        if (!QueueTurnRenewal(held, fresh)) continue;
        std::lock_guard<std::mutex> lk(mu_);
        if (fresh.user != turnCur_) {
            turnPrev_ = turnCur_;
            turnCur_ = fresh.user;
        }
    }
}

void LobbyAnnouncer::SetListed(bool listed) {
    std::string url, pub, tok;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!active_.load() || lobbyId_.empty()) return;
        listed_ = listed;
        url = request_.masterUrl; pub = lobbyId_; tok = token_;
    }
    // Fire-and-forget on a detached worker (don't block the UI thread on a round-trip).
    J::Json b;
    b["lobbyId"] = pub;
    b["token"] = tok;
    b["listed"] = listed;
    const std::string body = J::Dump(b);
    std::thread([url, body, listed] {
        if (coop::shutdown::IsShuttingDown()) return;
        const http::Response resp = http::Post(url, "/v1/visibility", body, 8000);
        UE_LOGI("lobby: visibility listed=%d -> %s (%d)", listed ? 1 : 0,
                resp.ok ? "ok" : "unreachable", resp.status);
    }).detach();
}

void LobbyAnnouncer::StopHeartbeatLocked() {
    // Caller holds threadMu_. Signals + joins the heartbeat thread. Does NOT send
    // /leave (Stop()'s job). Safe when no thread is running (joinable() is false).
    stop_.store(true);
    if (hbThread_.joinable()) hbThread_.join();
}

void LobbyAnnouncer::Stop() {
    // First, before anything can block: a Host() in flight on another thread reads this to know the
    // session it announces for has ended (its own leading Stop() adds one, which it counts for;
    // any further bump cancels it).
    gen_.fetch_add(1);
    bool was;
    std::string url, sid, tok;
    {
        std::lock_guard<std::mutex> tl(threadMu_);
        was = active_.exchange(false);
        StopHeartbeatLocked();
        // Copy + clear the creds INSIDE threadMu_: Host() writes them under threadMu_
        // too, so a concurrent Host() re-host is fully serialized against this Stop() --
        // its fresh sessionId can never be consumed by THIS call's /leave below. The heartbeat
        // thread's Reannounce() has already ended at the join above.
        std::lock_guard<std::mutex> lk(mu_);
        url = request_.masterUrl; sid = sessionId_; tok = token_;
        sessionId_.clear(); token_.clear(); lobbyId_.clear(); turnCur_.clear(); turnPrev_.clear();
    }
    if (!was) return;  // nothing was announced -> no /leave to send
    if (!url.empty() && !sid.empty()) SendLeave(url, sid, tok);
}

}  // namespace coop::net::lobby
