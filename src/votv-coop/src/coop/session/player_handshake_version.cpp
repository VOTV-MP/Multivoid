// coop/session/player_handshake_version.cpp -- the v122 wire version gate.
//
// Third TU of the player_handshake module (see player_handshake_detail.h).
// Owns the Paper-pair equality validation that runs at the TOP of the Join
// handler: extract the peer's game target from the Join payload (pure
// pre-pass, zero identity minting), byte-equality check against our own
// coop::version::kGameTarget, and the refuse action on mismatch (host:
// Kick-with-reason + deduped feed line; client: join_progress::Fail popup).
// The identity's other half -- the build number -- IS the packet header's
// protocol version: anything not byte-equal was cut upstream by ParseHeader,
// so the gate only ever runs between same-build peers.

#include "player_handshake_detail.h"

#include "coop/comms/chat_feed.h"
#include "coop/net/session.h"
#include "coop/session/join_progress.h"
#include "coop/version.h"
#include "ue_wrap/core/log.h"

#include <windows.h>

#include <cstdio>
#include <string>

namespace coop::player_handshake {
namespace {

// Pure pre-pass over the Join payload chain (eid / nick / skin / flags / color /
// game): extracts the trailing game-target field + the nick (for the host feed
// line) WITHOUT any side effects. Returns false on a malformed chain (any length
// prefix overruns the payload) -- fail-closed: a malformed Join is refused, not
// tolerated (both peers are >=v122 by the header prologue, so the full chain is
// always present when well-formed).
//
// THIS IS THE SECOND WALKER OF THAT CHAIN, and the two must agree field for
// field. `HandleJoinMessage` in player_handshake.cpp is the other. Measured
// 2026-08-29 by the smoke, the hard way: v144 deleted the guid field from the
// Join and this walker still consumed TWO length-prefixed fields where one
// remained, so it read the flags byte as a length, ran off the end and refused
// every Join with "version field missing" -- on BOTH peers, with the pose stream
// then never starting. A field added or removed here must be changed in both, and
// the failure is loud but names the wrong field, which is what cost the time.
bool ExtractJoinVersionFields(const uint8_t* payload, size_t len,
                              std::string* outGame, std::wstring* outNick) {
    size_t off = 4;  // [u32 senderElementId] (caller already checked len >= 4)
    // [u8 nicklen][nick]
    if (off + 1 > len) return false;
    {
        const size_t n = payload[off];
        if (off + 1 + n > len) return false;
        if (n > 0 && outNick)
            *outNick = FromUtf8(payload + off + 1, static_cast<int>(n));
        off += 1 + n;
    }
    // [u8 skinlen][skin] -- v144 deleted the [u8 guidlen][guid] field that used
    // to precede it (the host derives the guid from the proved key instead).
    {
        if (off + 1 > len) return false;
        const size_t n = payload[off];
        if (off + 1 + n > len) return false;
        off += 1 + n;
    }
    // [u8 flags] + [u8 has][r][g][b]
    if (off + 1 + 4 > len) return false;
    off += 1 + 4;
    // [u8 gamelen][game]
    if (off + 1 > len) return false;
    {
        const size_t n = payload[off];
        if (off + 1 + n > len || n > 23) return false;
        outGame->assign(reinterpret_cast<const char*>(payload + off + 1), n);
    }
    return true;
}

// The wire-gate verdict (server/client-phrased -- the reason string travels in
// the GNS close and is read on EITHER end). `peerIsClient` = the validated peer
// is a client joining us-the-host; false = we-the-client validate the host's
// Join. Empty = compatible.
std::string WireVersionVerdict(const std::string& peerGame, bool peerIsClient) {
    const char* ourGame = coop::version::kGameTarget;
    if (peerGame != ourGame) {
        const std::string& srvGame = peerIsClient ? ourGame : peerGame;
        const std::string& cliGame = peerIsClient ? peerGame : ourGame;
        return "Game version mismatch: server plays VOTV " + srvGame +
               ", client has VOTV " + cliGame + ".";
    }
    return {};
}

// Host feed-line dedup (a reconnect-looping refused client would otherwise spam
// the chat feed once per connection). One line per (nick + reason) per window;
// the WARN log stays undeduped -- it is the diagnostic.
uint64_t g_lastRefuseFeedMs = 0;
std::string g_lastRefuseFeedKey;
constexpr uint64_t kRefuseFeedDedupMs = 30000;

void PushRefuseFeedLineDeduped(const std::wstring& nick, const std::string& reason) {
    const std::string key = std::string(nick.begin(), nick.end()) + "|" + reason;
    const uint64_t now = ::GetTickCount64();
    if (key == g_lastRefuseFeedKey && now - g_lastRefuseFeedMs < kRefuseFeedDedupMs) return;
    g_lastRefuseFeedKey = key;
    g_lastRefuseFeedMs = now;
    const std::wstring wreason(reason.begin(), reason.end());  // ASCII by construction
    // History: a refused join is an EVENT in this lobby, not a passing notice.
    coop::chat_feed::Push(nick + L" was turned away: " + wreason,
                          coop::chat_feed::Keep::History);
}

}  // namespace

bool ValidateJoinVersionOrRefuse(coop::net::Session& session, int senderSlot,
                                 const uint8_t* payload, size_t payloadLen) {
    std::string peerGame;
    std::wstring refuseNick = L"A player";
    std::string verdict;
    if (!ExtractJoinVersionFields(payload, payloadLen, &peerGame, &refuseNick)) {
        verdict = "malformed join (version field missing)";
    } else {
        verdict = WireVersionVerdict(peerGame,
                                     session.role() == net::Role::Host);
    }
    if (verdict.empty()) return false;
    refuseNick = SanitizeNickname(refuseNick);
    if (session.role() == net::Role::Host) {
        UE_LOGW("player_handshake: Join REFUSED (slot=%d nick='%ls' game='%s'): %s",
                senderSlot, refuseNick.c_str(), peerGame.c_str(), verdict.c_str());
        PushRefuseFeedLineDeduped(refuseNick, verdict);
        char reason[128];
        std::snprintf(reason, sizeof(reason), "%s", verdict.c_str());
        session.Kick(senderSlot, reason);  // close-with-reason -> their popup
    } else {
        // We-the-client refused the HOST's Join: surface our own popup (a
        // browser/direct join is Active -> Fail pops the dialog + aborts;
        // env boot: log-only) and let the host's SYMMETRIC gate close the
        // wire (mismatch is byte-symmetric, both ends are >=v122).
        UE_LOGW("player_handshake: host's Join REFUSED (game='%s'): %s",
                peerGame.c_str(), verdict.c_str());
        coop::join_progress::Fail(verdict);
    }
    return true;
}

}  // namespace coop::player_handshake
