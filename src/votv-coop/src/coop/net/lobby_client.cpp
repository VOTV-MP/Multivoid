// coop/net/lobby_client.cpp -- see coop/net/lobby_client.h.

#include "coop/net/lobby_client.h"
#include "coop/game_build/game_build.h"

#include "coop/net/http_client.h"
#include "coop/net/peer_identity.h"  // PublicKeyFromIdentityString -- the shape of a listed key
#include "coop/text/name_filter.h"
#include "coop/text/utf8_codec.h"
#include "json_util.h"  // internal, co-located in src/coop/net/ (not a public API header)
#include "ue_wrap/core/log.h"

#include <algorithm>
#include <thread>
#include <utility>

namespace coop::net::lobby {
namespace {

namespace J = coop::net::jsonu;

// The master clamps each text to this many CHARACTERS (MAX_NAME, MAX_VERSION, MAX_GAME, MAX_WORLD in
// server/src/lobby.rs); a UTF-8 character is at most 4 bytes.
constexpr size_t kNameChars = 63;
constexpr size_t kVersionChars = 23;
constexpr size_t kGameChars = 23;
constexpr size_t kWorldChars = 39;
constexpr size_t kUtf8Max = 4;

// A master-supplied text as the browser may draw it: through the name filter, as a stranger's name.
// Ill-formed UTF-8 (the JSON parser already refuses it, so this is a backstop) draws as nothing.
std::string ShownText(const std::string& raw) {
    std::wstring wide;
    if (!coop::text::FromUtf8Strict(raw.data(), raw.size(), &wide)) return std::string();
    return coop::text::ToUtf8(coop::text::FilterNameChars(wide));
}

// One GET of `masterUrl`'s lobby list, parsed into `out`. True when the rows are a real answer;
// false with `st` saying why not. Blocking: the refresh worker's body.
bool FetchList(const std::string& masterUrl, const std::string& versionFilter,
               std::vector<LobbyRow>& out, std::string& st) {
    // Sanitise the version filter into the request line: keep only an unreserved allowlist, so a
    // caller can never inject a space, a line break or a hash into the request line or a header
    // (defensive; the current caller passes empty).
    std::string vf;
    for (char ch : versionFilter)
        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
            (ch >= '0' && ch <= '9') || ch == '.' || ch == '-' || ch == '_')
            vf += ch;
    std::string path = "/v1/lobbies";
    if (!vf.empty()) path += "?version=" + vf;
    // 2 MiB for this call alone: a full table of 1000 rows is about 1.1 MB, and a list cut short
    // would parse as malformed and read as a broken master.
    constexpr size_t kListBodyCap = 2u << 20;
    const http::Response resp = http::Get(masterUrl, path, 8000, kListBodyCap);

    // A failed fetch does not mean there are no servers: publishing the empty parse on every path
    // once emptied the browser on one unanswered request, and the player watched every server
    // disappear. The master is not the world, it is our view of it, and losing the view is not the
    // view being empty. MTA does not delete on a failed query either; its rows stay and go
    // half-alpha, the treatment a stale entry gets here. So the return decides whether the parse
    // replaces the list. No address in the status: it reaches the screen, and the browser names
    // the list by its label.
    if (!resp.ok) { st = "master unreachable"; return false; }
    if (resp.status != 200) { st = "master error " + std::to_string(resp.status); return false; }
    J::Json j;
    J::Json::const_iterator lit;
    if (!J::ParseObject(resp.body, j) || (lit = j.find("lobbies")) == j.cend() ||
        !lit->is_array()) {
        st = "master sent a malformed list";
        return false;
    }
    for (const auto& e : *lit) {
        if (!e.is_object()) continue;
        LobbyRow r;
        // Length-cap and range-clamp every server-supplied field: a hostile or on-path master must
        // not feed oversized strings or out-of-range numbers into the browser. A text is capped at the
        // master's own clamp, which counts characters (server/src/lobby.rs MAX_NAME..MAX_GAME), times
        // the 4 bytes a character takes at most, so a long non-ASCII name is not shortened to a
        // quarter; the cut still falls on a codepoint boundary (jsonu::StrN).
        r.lobbyId    = J::StrN(e, "lobbyId", 64);
        // Every text the browser draws goes through the name filter after the length cap: the
        // master's bytes are a stranger's, and a name that holds a control, a bidi override or a
        // zero-width character can hide or reorder the text around it.
        r.name       = ShownText(J::StrN(e, "name", kNameChars * kUtf8Max));
        if (r.name.empty()) r.name = "(unnamed)";
        r.version    = ShownText(J::StrN(e, "version", kVersionChars * kUtf8Max));
        r.game       = ShownText(J::StrN(e, "game", kGameChars * kUtf8Max));  // the game target; empty means a host older than the field
        // The game build the host runs, dropped unless it has the reader's shape (a master's bytes are a peer's).
        r.gameBuild  = J::StrN(e, "game_build", 33);
        if (!coop::game_build::KeyShapeOk(r.gameBuild)) r.gameBuild.clear();
        r.world      = ShownText(J::StrN(e, "world", kWorldChars * kUtf8Max));
        // The key the host advertises, kept only if it is the shape a key is; whether the host
        // PROVED it is the master's word (`proved`), a bool the browser sorts by.
        r.identity   = J::StrN(e, "identity", 68);
        peer_identity::PubKey keyBytes{};
        if (!peer_identity::PublicKeyFromIdentityString(r.identity, keyBytes)) r.identity.clear();
        r.proved     = J::Bool(e, "proved");
        r.playersCur = J::IntClamped(e, "players_cur", 0, 64);
        r.playersMax = J::IntClamped(e, "players_max", 0, 64);
        r.ageSec     = J::IntClamped(e, "age", 0, 2000000000);
        r.proto      = J::IntClamped(e, "proto", 0, 65535);  // the join gate; 0 means a host older than the field
        r.locked     = J::Bool(e, "locked");
        r.direct     = (J::Str(e, "conn") == "direct");  // direct lobbies
        // The link word is one of three or none; anything else a master sends is none.
        const std::string link = J::StrN(e, "link", 8);
        if (link == "relay" || link == "direct" || link == "lan") r.link = link;
        auto lk = e.find("links");
        if (lk != e.end() && lk->is_object()) {
            r.links.relayed = J::IntClamped(*lk, "relayed", 0, 64);
            r.links.direct  = J::IntClamped(*lk, "direct", 0, 64);
            r.links.lan     = J::IntClamped(*lk, "lan", 0, 64);
        }
        if (!r.lobbyId.empty()) out.push_back(std::move(r));
    }
    // A total order, imposed here, because the list arrives with none: the master stores lobbies
    // in a hash map and emits its values, so two fetches of the same lobbies can arrive in
    // different orders, and both browsers render by position, so the rows would permute under the
    // reader every refresh. Here, not in the browser: this is the one place the list is produced,
    // and both surfaces (the native browser and the overlay fallback) read the rows, so sorting in
    // one would leave the other with the defect; it also costs once per fetch on this worker
    // rather than once per sync on the game thread. The key is proof, then the name, then the lobby
    // id, and may not be anything else that moves: sorting by player count reorders the list every
    // time somebody joins or leaves a server the player is not even looking at, the same defect
    // wearing a reason. Proof is stable (a host's row keeps its mark between fetches) and puts the
    // hosts that hold their key first, so the row cap below cuts unproved rows before proved ones.
    // The id breaks ties. Byte-wise on UTF-8, which is codepoint order, and deliberately not
    // case-folded: a correct fold needs the repertoire table (coop/text/case_fold.h), and the
    // ASCII-only lowering that suggests itself here is the hand-rolled casing rule that header
    // exists to have retired.
    std::sort(out.begin(), out.end(), [](const LobbyRow& a, const LobbyRow& b) {
        // MTA keeps the master's order with a tie-break position (CServerList.cpp:719-727); this list
        // arrives with none, so proof, name and id are the position.
        if (a.proved != b.proved) return a.proved;
        if (a.name != b.name) return a.name < b.name;
        return a.lobbyId < b.lobbyId;
    });
    // Bounded, and the bound belongs here rather than in a renderer. Every field is length-capped
    // but the row count never was, and the row copy is a deep copy of six strings per row taken
    // under the mutex, which the overlay browser performs every frame and then renders unbounded,
    // so a master that is hostile, on-path or merely large buys a multi-megabyte copy per frame on
    // the render thread; the native browser's row cap bounds its display loop and never bounded
    // this. Truncating after the sort is what makes it defensible: the kept subset is
    // deterministic rather than whichever ones the hash map yielded first.
    constexpr size_t kMaxLobbies = 512;
    if (out.size() > kMaxLobbies) {
        UE_LOGW("lobby: the master listed %zu lobbies -- keeping the first %zu after sorting and "
                "dropping the rest", out.size(), kMaxLobbies);
        out.resize(kMaxLobbies);
    }
    st = std::to_string(out.size()) + (out.size() == 1 ? " server" : " servers");
    return true;
}

void EraseUrl(std::vector<std::string>& v, const std::string& url) {
    for (auto it = v.begin(); it != v.end(); ++it)
        if (*it == url) { v.erase(it); return; }
}

}  // namespace

void LobbyClient::RefreshAsync(const std::string& masterUrl, const std::string& versionFilter) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        // One master's list at a time. A different master than the one shown switches the view at
        // once: the rows belong to the old master (a lobby id means nothing on another one), and so
        // does the failure count behind the cannot-reach alarm, so both restart. The repaint
        // generation moves, since the rows on screen were replaced by nothing; the data generation
        // does not, since nothing was fetched -- HasData says the new list has not answered yet.
        if (masterUrl != viewUrl_) {
            viewUrl_ = masterUrl;
            rows_.clear();
            consecutiveFailures_ = 0;
            hasData_ = false;
            ++generation_;
            status_ = "Refreshing...";
        }
        // Coalesced per master: a fetch already out to THIS master answers this request too. One
        // still out to a master the player has left does not hold this one up; its answer is
        // dropped when it lands. MTA refreshes per source for the same reason
        // (CServerBrowser.cpp OnTabChanged -> EnsureRefreshFor).
        for (const std::string& u : inFlight_)
            if (u == masterUrl) return;
        inFlight_.push_back(masterUrl);
        status_ = "Refreshing...";
    }
    // A detached worker per master asked. The client is a process-lifetime singleton (the session
    // manager owns it as a never-destroyed static), so `this` outlives the thread.
    try {
        std::thread([this, masterUrl, versionFilter] { Fetch(masterUrl, versionFilter); }).detach();
    } catch (...) {
        // No thread: the in-flight mark must not outlive the attempt, or every later refresh of this
        // master would coalesce into one that never runs.
        std::lock_guard<std::mutex> lk(mu_);
        EraseUrl(inFlight_, masterUrl);
        if (masterUrl == viewUrl_) {
            status_ = "refresh error";
            ++generation_;
        }
    }
}

void LobbyClient::Fetch(const std::string& url, const std::string& versionFilter) {
    std::string done;   // the status published, logged once the lock is released
    // The whole body: an allocation failing anywhere must not escape a detached thread (a
    // terminate), nor leave this master's in-flight mark set, which would coalesce every later
    // refresh of it into one that never runs. A thrown attempt is a completed, failed one.
    try {
        std::vector<LobbyRow> parsed;
        std::string st;
        bool ok = false;
        try {
            ok = FetchList(url, versionFilter, parsed, st);
        } catch (...) {
            parsed.clear();
            st = "refresh error";
            ok = false;
        }
        std::lock_guard<std::mutex> lk(mu_);
        EraseUrl(inFlight_, url);
        // The player moved on to another list while this fetch waited: its answer is for a list no
        // longer shown.
        if (url != viewUrl_) return;
        if (ok) {
            rows_ = std::move(parsed);
            consecutiveFailures_ = 0;
            hasData_ = true;
            // Only here: the rows-are-new signal the age clock keys on; see DataGeneration.
            ++dataGeneration_;
        } else if (consecutiveFailures_ < 1000000) {
            ++consecutiveFailures_;
        }
        // The generation moves on every completed attempt, success or not: it is the
        // something-happened, repaint signal the browser polls, and after a failure there is
        // something to repaint, the status line changed and the rows on screen got older. Bumping it
        // only on success would freeze the screen's clock at the last good fetch. It is not the age
        // clock; that distinction is the data generation above. Answering both questions with one
        // counter meant a failed fetch re-stamped last-fetched, which pinned every row's age and
        // made the stale dimming unreachable.
        ++generation_;
        status_ = st;
        done = std::move(st);
    } catch (...) {
        std::lock_guard<std::mutex> lk(mu_);
        EraseUrl(inFlight_, url);
        return;
    }
    if (!done.empty()) UE_LOGI("lobby: refresh done [%s]", done.c_str());
}

uint64_t LobbyClient::CopyRows(std::vector<LobbyRow>& out, std::string* master) const {
    std::lock_guard<std::mutex> lk(mu_);
    out = rows_;
    if (master) *master = viewUrl_;
    return generation_;
}

uint64_t LobbyClient::Generation() const {
    std::lock_guard<std::mutex> lk(mu_);
    return generation_;
}

std::string LobbyClient::Status(std::string* viewUrl) const {
    std::lock_guard<std::mutex> lk(mu_);
    if (viewUrl) *viewUrl = viewUrl_;
    return status_;
}

uint64_t LobbyClient::DataGeneration() const {
    std::lock_guard<std::mutex> lk(mu_);
    return dataGeneration_;
}

bool LobbyClient::HasData() const {
    std::lock_guard<std::mutex> lk(mu_);
    return hasData_;
}

int LobbyClient::ConsecutiveFailures() const {
    std::lock_guard<std::mutex> lk(mu_);
    return consecutiveFailures_;
}

LatestInfo LobbyClient::FetchLatest(const std::string& masterUrl, int timeoutMs) {
    LatestInfo info;
    const http::Response resp = http::Get(masterUrl, "/v1/latest", timeoutMs);
    if (!resp.ok) { UE_LOGI("lobby: /v1/latest -- master unreachable (no toast)"); return info; }
    if (resp.status != 200) {  // an older master answers 404; silent, not an error
        UE_LOGI("lobby: /v1/latest -- master returned %d (no toast)", resp.status);
        return info;
    }
    J::Json j;
    if (!J::ParseObject(resp.body, j)) { UE_LOGW("lobby: /v1/latest -- malformed response"); return info; }
    info.proto = J::Int(j, "proto");
    info.mod   = J::StrN(j, "mod", 24);
    // `url` is served by the master and deliberately not parsed; see LatestInfo's header. Ignoring
    // an extra response field is forward-compatible, so it needs no wire-format change.
    // `games`: the newest release per game version, at most 16 and each field capped (a master's bytes are a peer's).
    const auto git = j.find("games");
    if (git != j.cend() && git->is_array()) {
        info.hasGames = true;
        for (const auto& e : *git) {
            if (!e.is_object() || info.games.size() >= 16) continue;
            LatestGame g;
            g.game  = J::StrN(e, "game", 24);
            g.proto = J::IntClamped(e, "proto", 0, 65535);
            g.mod   = J::StrN(e, "mod", 24);
            if (!g.game.empty() && g.proto > 0) info.games.push_back(g);
        }
    }
    info.ok    = info.proto > 0 || !info.games.empty();
    return info;
}

ThanksFetch LobbyClient::FetchThanks(const std::string& masterUrl, int timeoutMs,
                                     std::string& outText) {
    const http::Response resp = http::Get(masterUrl, "/v1/thanks", timeoutMs);
    // 404 is the ONE answer that means this master has no list, and the only one a caller may
    // act on destructively. Everything else -- no answer, a 503 for a list the master holds but
    // could not serve this moment, a body that does not parse -- is "no word from the master".
    if (resp.ok && resp.status == 404) {
        UE_LOGI("lobby: /v1/thanks -- this master serves no list");
        return ThanksFetch::NoList;
    }
    if (!resp.ok || resp.status != 200) {
        UE_LOGI("lobby: /v1/thanks -- no usable answer from the master (ok=%d status=%d)",
                resp.ok ? 1 : 0, resp.status);
        return ThanksFetch::Unreachable;
    }
    J::Json j;
    if (!J::ParseObject(resp.body, j)) {
        UE_LOGW("lobby: /v1/thanks -- malformed response");
        return ThanksFetch::Unreachable;
    }
    // The cap is the list parser's own file bound plus slack; a longer text is refused there.
    outText = J::StrN(j, "text", 80 * 1024);
    // A 200 with no text is not the master saying it has none -- it never serves an empty list,
    // so this is a malformed answer, and malformed is "no word", never a reason to evict.
    if (outText.empty()) {
        UE_LOGW("lobby: /v1/thanks -- 200 with an empty text; treating it as no answer");
        return ThanksFetch::Unreachable;
    }
    return ThanksFetch::Text;
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
    // A direct lobby: the whole join capability is the host's address.
    if (J::Str(j, "conn") == "direct") {
        info.direct = true;
        info.addr = J::Str(j, "addr");
        // The host identity is parsed here too. It is read again below on the P2P path, and this
        // branch returns before reaching that, so a direct join once arrived with an empty
        // identity, the client had nothing to bind the answering key to, and a locked direct lobby
        // refused every browser join; an early return inherits none of the branch it skips.
        // Optional, not required: `ok` stays keyed on the address alone, since an older master does
        // not send the field and making it mandatory would turn every direct join against a
        // not-yet-updated master into a hard failure, losing open direct lobbies to fix a case that
        // only affects locked ones. Empty means unbound, a true statement about what the joiner
        // knows.
        info.hostIdentity = J::StrN(j, "hostIdentity", 80);
        info.ok = !info.addr.empty();
        if (!info.ok) UE_LOGW("lobby: join '%s' -- direct response missing addr", lobbyId.c_str());
        return info;
    }
    // Length-cap the identities, URLs and tokens: a hostile or on-path master must not feed
    // oversized strings into the transport config. The TURN block is read as the host's announce
    // reads it (json_util.h).
    info.sessionId      = J::StrN(j, "sessionId", 64);
    // 80, not 64: a durable identity renders as a prefix plus 64 hex, 68 chars, and a cap of 64
    // would truncate it into a name that dials nobody, which reads as P2P being broken rather than
    // a string being cut. The peer identity is no longer read at all (see JoinInfo); it was
    // retired server-side only once the last cohort that required it was, since a field dead in
    // the tree can still be load-bearing on the wire.
    info.hostIdentity   = J::StrN(j, "hostIdentity", 80);
    info.signalingUrl   = J::StrN(j, "signalingUrl", 128);
    info.signalingToken = J::StrN(j, "signalingToken", 128);
    info.stun           = J::StrN(j, "stun", 128);
    info.turn           = J::ParseTurnCredential(j);
    // The host identity to dial and a signaling endpoint are the minimum to start P2P.
    info.ok = !info.hostIdentity.empty() && !info.signalingUrl.empty();
    if (!info.ok) {
        UE_LOGW("lobby: join '%s' -- response missing identities/signaling", lobbyId.c_str());
    }
    return info;
}

}  // namespace coop::net::lobby
