// coop/net/lobby_announcer.cpp -- see coop/net/lobby_announcer.h.

#include "coop/net/lobby_announcer.h"

#include "coop/net/http_client.h"
#include "coop/net/protocol.h"  // kProtocolVersion -- announced for the v59 browser join gate
#include "coop/shutdown.h"
#include "json_util.h"  // internal, co-located in src/coop/net/ (not a public API header)
#include "ue_wrap/log.h"

#include <chrono>
#include <thread>
#include <utility>

namespace coop::net::lobby {
namespace {

namespace J = coop::net::jsonu;

}  // namespace

LobbyAnnouncer::~LobbyAnnouncer() { Stop(); }

HostInfo LobbyAnnouncer::Host(const std::string& masterUrl, const std::string& name,
                              const std::string& version, const std::string& world,
                              bool locked, int playersMax, int timeoutMs, int directPort) {
    HostInfo info;
    Stop();  // retire any prior lobby first (sends /leave, joins) -- on a worker, safe

    // nlohmann builds + escapes the body (name/world are user-supplied -> may contain
    // quotes/backslashes; never string-concat them into JSON). J::Dump uses the
    // replace error handler so invalid-UTF-8 input can't throw out of this worker.
    J::Json b;
    b["name"] = name;
    b["version"] = version;
    b["proto"] = static_cast<int>(coop::net::kProtocolVersion);  // v59: the REAL compat key
    b["world"] = world;
    b["locked"] = locked;
    b["players_max"] = playersMax;
    if (directPort > 0) {  // DIRECT lobby (see header): master advertises src-ip:port
        b["conn"] = "direct";
        b["direct_port"] = directPort;
    }
    const http::Response resp = http::Post(masterUrl, "/v1/host", J::Dump(b), timeoutMs);
    // No raw master URL (console-visible line; the official server shows as "DEFAULT").
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
    auto turn = j.find("turn");
    if (turn != j.end() && turn->is_object()) {
        info.turnUri  = J::FirstTurnUri(*turn);
        info.turnUser = J::Str(*turn, "user");
        info.turnPass = J::Str(*turn, "pass");
    }
    info.ok = !info.sessionId.empty() && !info.token.empty() &&
              !info.hostIdentity.empty() && !info.signalingUrl.empty();
    if (!info.ok) { UE_LOGW("lobby: host announce -- response missing session/token/identity"); return info; }

    // Stash the creds + (re)start the heartbeat under threadMu_ so a concurrent Host
    // can never move-assign over a joinable hbThread_ (which would std::terminate).
    {
        std::lock_guard<std::mutex> tl(threadMu_);
        StopHeartbeatLocked();  // join anything that snuck in since the top Stop()
        {
            std::lock_guard<std::mutex> lk(mu_);
            masterUrl_ = masterUrl;
            sessionId_ = info.sessionId;
            token_     = info.token;
            lobbyId_   = info.lobbyId;
            listed_    = true;
        }
        stop_.store(false);
        active_.store(true);
        hbThread_ = std::thread(&LobbyAnnouncer::HeartbeatLoop, this);
    }
    UE_LOGI("lobby: announced '%s' lobbyId=%s -- heartbeating", name.c_str(), info.lobbyId.c_str());
    return info;
}

void LobbyAnnouncer::HeartbeatLoop() {
    // The master stamped last_seen at /v1/host, so the first beat is one interval out.
    // 30s < the master's TURN-cred TTL/2 (60s) and << the 300s lobby expiry (design 7/8).
    // Sleep in 0.5s slices so Stop()/shutdown is responsive; bail on global shutdown so
    // we never issue HTTP into a tearing-down process.
    while (!stop_.load() && !coop::shutdown::IsShuttingDown()) {
        for (int i = 0; i < 60 && !stop_.load() && !coop::shutdown::IsShuttingDown(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        if (stop_.load() || coop::shutdown::IsShuttingDown()) break;

        std::string url, sid, tok;
        bool listed;
        {
            std::lock_guard<std::mutex> lk(mu_);
            url = masterUrl_; sid = sessionId_; tok = token_; listed = listed_;
        }
        const int pc = playerCountFn_ ? playerCountFn_() : 1;

        J::Json b;
        b["sessionId"] = sid;
        b["token"] = tok;
        b["players_cur"] = pc;
        b["listed"] = listed;
        const http::Response resp = http::Post(url, "/v1/heartbeat", J::Dump(b), 8000);
        if (!resp.ok || resp.status != 200) {
            UE_LOGW("lobby: heartbeat -> %s (status=%d) -- lobby may expire if this persists",
                    resp.ok ? "non-200" : "unreachable", resp.status);
        }
    }
}

void LobbyAnnouncer::SetListed(bool listed) {
    std::string url, pub, tok;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!active_.load() || lobbyId_.empty()) return;
        listed_ = listed;
        url = masterUrl_; pub = lobbyId_; tok = token_;
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
    bool was;
    {
        std::lock_guard<std::mutex> tl(threadMu_);
        was = active_.exchange(false);
        StopHeartbeatLocked();
    }
    if (!was) return;  // nothing was announced -> no /leave to send

    std::string url, sid, tok;
    {
        std::lock_guard<std::mutex> lk(mu_);
        url = masterUrl_; sid = sessionId_; tok = token_;
        sessionId_.clear(); token_.clear(); lobbyId_.clear();
    }
    if (!url.empty() && !sid.empty()) {
        J::Json b;
        b["sessionId"] = sid;
        b["token"] = tok;
        const http::Response resp = http::Post(url, "/v1/leave", J::Dump(b), 5000);
        UE_LOGI("lobby: left lobby -> %s (%d)", resp.ok ? "ok" : "unreachable", resp.status);
    }
}

}  // namespace coop::net::lobby
