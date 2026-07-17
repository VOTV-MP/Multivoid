// coop/net/lobby_client.cpp -- see coop/net/lobby_client.h.

#include "coop/net/lobby_client.h"

#include "coop/net/http_client.h"
#include "json_util.h"  // internal, co-located in src/coop/net/ (not a public API header)
#include "ue_wrap/core/log.h"

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
                    // Length-cap + range-clamp every server-supplied field (audit L4/L5):
                    // a hostile/MITM master must not feed oversized strings or out-of-range
                    // numbers into the browser UI. Caps mirror the master's own clamps.
                    r.lobbyId    = J::StrN(e, "lobbyId", 64);
                    r.name       = J::StrN(e, "name", 64);
                    r.version    = J::StrN(e, "version", 24);
                    r.world      = J::StrN(e, "world", 40);
                    r.playersCur = J::IntClamped(e, "players_cur", 0, 64);
                    r.playersMax = J::IntClamped(e, "players_max", 0, 64);
                    r.ageSec     = J::IntClamped(e, "age", 0, 2000000000);
                    r.proto      = J::IntClamped(e, "proto", 0, 65535);  // v59 join gate (0 = pre-field host)
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
    // Length-cap identities/URLs/tokens (audit L5) -- a hostile/MITM master must not
    // feed oversized strings into GNS config. turnUser/turnPass go through CredField
    // (audit L6): a comma/whitespace there would inject an extra element into GNS's
    // parallel comma-separated TURN user/pass/uri lists and desync them.
    info.sessionId      = J::StrN(j, "sessionId", 64);
    info.peerIdentity   = J::StrN(j, "peerIdentity", 64);
    info.hostIdentity   = J::StrN(j, "hostIdentity", 64);
    info.signalingUrl   = J::StrN(j, "signalingUrl", 128);
    info.signalingToken = J::StrN(j, "signalingToken", 128);
    info.stun           = J::StrN(j, "stun", 128);
    auto turn = j.find("turn");
    if (turn != j.end() && turn->is_object()) {
        info.turnUri  = J::FirstTurnUri(*turn);      // comma/ws-rejected inside
        info.turnUser = J::CredField(*turn, "user", 256);
        info.turnPass = J::CredField(*turn, "pass", 256);
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
