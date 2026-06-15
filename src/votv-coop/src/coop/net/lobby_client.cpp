// coop/net/lobby_client.cpp -- see coop/net/lobby_client.h.

#include "coop/net/lobby_client.h"

#include "coop/net/http_client.h"
#include "json_util.h"  // internal, co-located in src/coop/net/ (not a public API header)
#include "ue_wrap/log.h"

#include <thread>
#include <utility>

namespace coop::net::lobby {
namespace {

namespace J = coop::net::jsonu;

}  // namespace

void LobbyClient::RefreshAsync(const std::string& masterUrl, const std::string& versionFilter) {
    bool expected = false;
    if (!inFlight_.compare_exchange_strong(expected, true)) return;  // coalesce
    {
        std::lock_guard<std::mutex> lk(mu_);
        status_ = "Refreshing...";
    }
    // Detached worker. LobbyClient is a process-lifetime singleton (session_manager
    // owns it as a never-destroyed static), so `this` outlives the thread.
    std::thread([this, masterUrl, versionFilter] {
      // try/catch around the whole body (audit F4): a std::bad_alloc from the
      // row parse must not escape a detached thread (std::terminate) NOR leave
      // inFlight_ latched true -- which would permanently coalesce every future
      // RefreshAsync into a no-op, freezing the browser at "Refreshing...".
      // Mirrors the try/catch every other action worker already has.
      try {
        // Sanitize the version filter into the request line: keep only an unreserved
        // allowlist so a caller can never inject a space/CRLF/'#' into the HTTP
        // request line or a header (defensive -- the current caller passes empty).
        std::string vf;
        for (char ch : versionFilter)
            if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                (ch >= '0' && ch <= '9') || ch == '.' || ch == '-' || ch == '_')
                vf += ch;
        std::string path = "/v1/lobbies";
        if (!vf.empty()) path += "?version=" + vf;
        const http::Response resp = http::Get(masterUrl, path, 8000);

        std::vector<LobbyRow> parsed;
        std::string st;
        if (!resp.ok) {
            st = "master unreachable (" + masterUrl + ")";
        } else if (resp.status != 200) {
            st = "master error " + std::to_string(resp.status);
        } else {
            J::Json j;
            J::Json::const_iterator lit;
            if (J::ParseObject(resp.body, j) &&
                (lit = j.find("lobbies")) != j.cend() && lit->is_array()) {
                for (const auto& e : *lit) {
                    if (!e.is_object()) continue;
                    LobbyRow r;
                    r.lobbyId    = J::Str(e, "lobbyId");
                    r.name       = J::Str(e, "name");
                    r.version    = J::Str(e, "version");
                    r.world      = J::Str(e, "world");
                    r.playersCur = J::Int(e, "players_cur");
                    r.playersMax = J::Int(e, "players_max");
                    r.ageSec     = J::Int(e, "age");
                    r.proto      = J::Int(e, "proto");  // v59 join gate (0 = pre-field host)
                    r.locked     = J::Bool(e, "locked");
                    r.direct     = (J::Str(e, "conn") == "direct");  // 2026-06-11 direct lobbies
                    if (!r.lobbyId.empty()) parsed.push_back(std::move(r));
                }
                st = std::to_string(parsed.size()) +
                     (parsed.size() == 1 ? " server" : " servers");
            } else {
                st = "master sent a malformed list";
            }
        }

        {
            std::lock_guard<std::mutex> lk(mu_);
            rows_ = std::move(parsed);
            ++generation_;
            status_ = st;  // keep st for the log line below
        }
        // No raw master URL in this line: it mirrors into the user-visible
        // console (the official server shows as "DEFAULT" elsewhere; the
        // status string already carries the row count / error).
        UE_LOGI("lobby: refresh done [%s]", st.c_str());
      } catch (...) {
        std::lock_guard<std::mutex> lk(mu_);
        status_ = "refresh error";
      }
      inFlight_.store(false);  // ALWAYS clear (the latch that gates the next refresh)
    }).detach();
}

uint64_t LobbyClient::CopyRows(std::vector<LobbyRow>& out) const {
    std::lock_guard<std::mutex> lk(mu_);
    out = rows_;
    return generation_;
}

std::string LobbyClient::Status() const {
    std::lock_guard<std::mutex> lk(mu_);
    return status_;
}

LatestInfo LobbyClient::FetchLatest(const std::string& masterUrl, int timeoutMs) {
    LatestInfo info;
    const http::Response resp = http::Get(masterUrl, "/v1/latest", timeoutMs);
    if (!resp.ok) { UE_LOGI("lobby: /v1/latest -- master unreachable (no toast)"); return info; }
    if (resp.status != 200) {  // a pre-v59 master 404s -- silent, not an error
        UE_LOGI("lobby: /v1/latest -- master returned %d (no toast)", resp.status);
        return info;
    }
    J::Json j;
    if (!J::ParseObject(resp.body, j)) { UE_LOGW("lobby: /v1/latest -- malformed response"); return info; }
    info.proto = J::Int(j, "proto");
    info.mod   = J::Str(j, "mod");
    info.url   = J::Str(j, "url");
    info.ok    = info.proto > 0;
    return info;
}

JoinInfo LobbyClient::Join(const std::string& masterUrl, const std::string& lobbyId,
                           int timeoutMs) {
    JoinInfo info;
    J::Json b;
    b["lobbyId"] = lobbyId;  // nlohmann escapes -> safe even if a caller passes oddities
    const http::Response resp = http::Post(masterUrl, "/v1/join", J::Dump(b), timeoutMs);
    if (!resp.ok) {
        UE_LOGW("lobby: join '%s' -- master unreachable", lobbyId.c_str());
        return info;
    }
    if (resp.status != 200) {
        UE_LOGW("lobby: join '%s' -- master returned %d", lobbyId.c_str(), resp.status);
        return info;
    }
    J::Json j;
    if (!J::ParseObject(resp.body, j)) {
        UE_LOGW("lobby: join '%s' -- malformed response", lobbyId.c_str());
        return info;
    }
    // Direct lobby (2026-06-11): the whole join capability is the host's addr.
    if (J::Str(j, "conn") == "direct") {
        info.direct = true;
        info.addr = J::Str(j, "addr");
        info.ok = !info.addr.empty();
        if (!info.ok) UE_LOGW("lobby: join '%s' -- direct response missing addr", lobbyId.c_str());
        return info;
    }
    info.sessionId      = J::Str(j, "sessionId");
    info.peerIdentity   = J::Str(j, "peerIdentity");
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
    // The peer identity + the host identity to dial are the minimum to start P2P.
    info.ok = !info.peerIdentity.empty() && !info.hostIdentity.empty() &&
              !info.signalingUrl.empty();
    if (!info.ok) {
        UE_LOGW("lobby: join '%s' -- response missing identities/signaling", lobbyId.c_str());
    }
    return info;
}

}  // namespace coop::net::lobby
