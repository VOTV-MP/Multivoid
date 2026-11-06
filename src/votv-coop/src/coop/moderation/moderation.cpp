// coop/moderation/moderation.cpp -- see coop/moderation/moderation.h.

#include "coop/moderation/moderation.h"

#include "coop/moderation/ban_list.h"
#include "coop/moderation/seen_players.h"
#include "coop/session/teleport_client.h"
#include "coop/net/link_kind.h"
#include "coop/net/session.h"
#include "coop/player/roster_ledger.h"
#include "coop/player/players_registry.h"
#include "coop/text/utf8_codec.h"
#include "ue_wrap/core/hot_path_guard.h"
#include "ue_wrap/core/log.h"

#include <windows.h>

#include <atomic>
#include <string>

namespace coop::moderation {

namespace {

std::atomic<coop::net::Session*> g_session{nullptr};

// The session iff it is running and hosted, else null: the one place the question is answered.
coop::net::Session* HostedSession() {
    auto* s = g_session.load(std::memory_order_acquire);
    if (s == nullptr || !s->running() || s->role() != coop::net::Role::Host) return nullptr;
    return s;
}

// Resolve the hosted session, or null with a log line, so every action is a no-op outside one.
coop::net::Session* HostSession(const char* action) {
    auto* s = HostedSession();
    if (!s) UE_LOGI("moderation: %s ignored -- no running hosted session", action);
    return s;
}

// The text a verb stores and sends: the typed reason, else the constant that stands in.
std::string ReasonOr(const char* reason, const char* fallback) {
    return (reason && reason[0]) ? std::string(reason) : std::string(fallback);
}

std::string LowerAscii(const char* s) {
    std::string out(s ? s : "");
    for (char& c : out)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    return out;
}

}  // namespace

void SetSession(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
}

bool HostedSessionRunning() { return HostedSession() != nullptr; }

std::string EnforceableAddress(const coop::net::Session& s, int slot, uint32_t generation) {
    char a[64] = {};
    if (!s.GetPeerAddressWithToken(slot, generation, a, sizeof(a))) return {};
    if (!coop::ban_list::IsBannableAddress(a)) return {};
    const coop::net::LinkKind kind = s.LinkKindForSlot(slot);
    if (kind != coop::net::LinkKind::Lan && kind != coop::net::LinkKind::Direct) return {};
    // The kind read is not token-gated: the slot must still be the same occupancy after it.
    if (s.peerGenerationForSlot(slot) != generation) return {};
    return a;
}

ModResult KickPlayer(const PlayerToken& token, const char* reason) {
    return KickPlayerWithReason(token, reason, coop::net::EndReason::KickedByHost);
}

ModResult KickPlayerWithReason(const PlayerToken& token, const char* reason, coop::net::EndReason code) {
    UE_ASSERT_GAME_THREAD("moderation::KickPlayer");
    auto* s = HostSession("kick");
    if (!s) return ModResult::NoSession;
    if (!token.valid()) return ModResult::Gone;
    // The ledger's player number, as in the ban and the teleport: a slot whose roster row moved on
    // while the net layer's generation still matches is another player's seat.
    if (coop::roster_ledger::Get(token.slot).playerNo != token.playerNo) {
        UE_LOGW("moderation: kick of #%u skipped -- slot %d changed hands",
                static_cast<unsigned>(token.playerNo), token.slot);
        return ModResult::Gone;
    }
    const std::string why = ReasonOr(reason, kKickDefaultReason);
    if (!s->KickWithToken(token.slot, token.generation, code, why.c_str())) {
        UE_LOGW("moderation: kick of #%u (slot %d) did nothing -- they are already "
                "gone, or someone else now holds that slot",
                static_cast<unsigned>(token.playerNo), token.slot);
        return ModResult::Gone;
    }
    UE_LOGI("moderation: kicked #%u (slot %d)", static_cast<unsigned>(token.playerNo), token.slot);
    return ModResult::Done;
}

ModResult BanPlayer(const PlayerToken& token, const char* reason, bool byAddress) {
    UE_ASSERT_GAME_THREAD("moderation::BanPlayer");
    auto* s = HostSession("ban");
    if (!s) return ModResult::NoSession;
    // The generation FIRST, before the proved id or the ledger is read: a stale token (the seat
    // changed hands) is Gone without touching anything that now describes the successor.
    if (!token.valid() || s->peerGenerationForSlot(token.slot) != token.generation) {
        UE_LOGW("moderation: ban of #%u ABORTED -- slot %d no longer holds that player, or "
                "their identity proof has not landed",
                static_cast<unsigned>(token.playerNo), token.slot);
        return ModResult::Gone;
    }
    // The proved id, token-gated: it is the ban's key, and reading a successor's id and writing it
    // to the permanent list is exactly the failure this path exists to prevent. Read before the
    // kick, which clears the slot.
    const std::string id = s->ProvedGuidForSlotWithToken(token.slot, token.generation);
    if (id.empty()) {
        UE_LOGW("moderation: ban of #%u refused -- slot %d has no proved identity yet",
                static_cast<unsigned>(token.playerNo), token.slot);
        return ModResult::NoId;
    }
    char nick[coop::text::kNickBufBytes] = {};
    coop::text::CopyUtf8ToBuffer(nick, coop::roster_ledger::Get(token.slot).nick);

    // The second abort: the ledger's player number, for a slot whose roster row moved on while
    // the net layer's generation still matches.
    if (coop::roster_ledger::Get(token.slot).playerNo != token.playerNo) {
        UE_LOGW("moderation: ban of #%u ABORTED -- slot %d now holds #%u",
                static_cast<unsigned>(token.playerNo), token.slot,
                static_cast<unsigned>(coop::roster_ledger::Get(token.slot).playerNo));
        return ModResult::Gone;
    }

    const std::string address =
        byAddress ? EnforceableAddress(*s, token.slot, token.generation) : std::string();
    // The typed reason rides the close as its text, so the banned player reads it under the code;
    // the constant stands in when none was typed, and is what the record stores.
    const std::string why = ReasonOr(reason, kBanDefaultReason);
    if (!coop::ban_list::Add(id.c_str(), nick, address.c_str(), why.c_str())) return ModResult::NoId;
    // Accepted window: the banned player's re-proof over a second connection, whose ban check
    // ran on the net thread just before the Add above, is seated for this session. MTA has
    // none: its join packets are queued by the sync thread and handled on the main thread
    // that also runs its bans (CNetBuffer.cpp:1289-1294, :932-952). Ours proves on the net
    // thread and runs the verbs on the game thread.
    if (!s->KickWithToken(token.slot, token.generation, coop::net::EndReason::BannedByHost,
                          why.c_str()))
        UE_LOGW("moderation: ban #%u -- kick did nothing (already gone?)",
                static_cast<unsigned>(token.playerNo));
    else
        UE_LOGI("moderation: banned + kicked #%u (slot %d, id %.8s..., address %s)",
                static_cast<unsigned>(token.playerNo), token.slot, id.c_str(),
                address.empty() ? "not enforced" : ue_wrap::log::Addr(address).c_str());
    return ModResult::Done;
}

ModResult BanOffline(const char* id, const char* reason, bool byAddress) {
    UE_ASSERT_GAME_THREAD("moderation::BanOffline");
    auto* s = HostSession("offline ban");
    if (!s) return ModResult::NoSession;
    const std::string guid = LowerAscii(id);
    coop::seen_players::Entry e;
    const bool known = coop::seen_players::FindByGuid(guid.c_str(), e);
    // The record's last address, only when it can name one machine: an old record's "::" or
    // loopback is dropped. An id with no record is banned by id alone.
    const std::string address =
        (known && byAddress && coop::ban_list::IsBannableAddress(e.ip)) ? std::string(e.ip)
                                                                        : std::string();
    const std::string why = ReasonOr(reason, kBanDefaultReason);
    if (!coop::ban_list::Add(guid.c_str(), known ? e.nick : "", address.c_str(), why.c_str()))
        return ModResult::NoId;
    UE_LOGI("moderation: offline-banned '%s' (id %.8s..., address %s)", known ? e.nick : "",
            guid.c_str(), address.empty() ? "not enforced" : ue_wrap::log::Addr(address).c_str());
    // A matching player still seated goes too. MTA's AddBan also kicks every seated player
    // matching the ban's IP (CStaticFunctionDefinitions.cpp:12157-12170); ours kicks the banned
    // id only, so bystanders at a shared address keep their session and meet the refusal at
    // their next proof (the named cost of an address key, undone by Unban).
    // Accepted window, both conditions at once: a joiner whose ban check ran on the net
    // thread just before the Add above AND whose id is published just after this scan is
    // seated for this session. MTA has none: its join packets are queued by the sync thread
    // and handled on the main thread that also runs its bans (CNetBuffer.cpp:1289-1294,
    // :932-952). Ours proves on the net thread and runs the verbs on the game thread.
    for (int k = 1; k < static_cast<int>(coop::players::kMaxPeers); ++k) {
        const uint32_t gen = s->peerGenerationForSlot(k);
        if (gen != 0 && s->ProvedGuidForSlotWithToken(k, gen) == guid)
            s->KickWithToken(k, gen, coop::net::EndReason::BannedByHost, why.c_str());
    }
    return ModResult::Done;
}

ModResult Unban(const char* playerId) {
    UE_ASSERT_GAME_THREAD("moderation::Unban");
    if (!HostSession("unban")) return ModResult::NoSession;
    return coop::ban_list::Remove(playerId) ? ModResult::Done : ModResult::NotBanned;
}

ModResult TeleportPlayerToMe(const PlayerToken& token) {
    UE_ASSERT_GAME_THREAD("moderation::TeleportPlayerToMe");
    auto* s = HostSession("teleport");
    if (!s) return ModResult::NoSession;
    if (!token.valid() || s->peerGenerationForSlot(token.slot) != token.generation ||
        coop::roster_ledger::Get(token.slot).playerNo != token.playerNo) {
        UE_LOGW("moderation: teleport of #%u skipped -- slot %d changed hands",
                static_cast<unsigned>(token.playerNo), token.slot);
        return ModResult::Gone;
    }
    if (!coop::teleport_client::TeleportSlotToHostWithToken(token.slot, token.generation)) {
        // A seat that changed hands since the check above is Gone; otherwise the host's own pose
        // could not be read, and nothing was sent.
        if (s->peerGenerationForSlot(token.slot) != token.generation) return ModResult::Gone;
        UE_LOGW("moderation: teleport of #%u failed -- the host's pose could not be read",
                static_cast<unsigned>(token.playerNo));
        return ModResult::Failed;
    }
    return ModResult::Done;
}

}  // namespace coop::moderation
