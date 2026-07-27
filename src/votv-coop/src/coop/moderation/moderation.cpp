// coop/moderation/moderation.cpp -- see coop/moderation/moderation.h.

#include "coop/moderation/moderation.h"

#include "coop/moderation/ban_list.h"
#include "coop/moderation/seen_players.h"
#include "coop/session/teleport_client.h"
#include "coop/net/session.h"
#include "coop/player/roster_ledger.h"
#include "coop/player/players_registry.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"

#include <windows.h>

#include <atomic>
#include <string>

namespace coop::moderation {

namespace GT = ue_wrap::game_thread;

namespace {

std::atomic<coop::net::Session*> g_session{nullptr};

// Resolve the Session iff we are the host. Returns nullptr (and logs) otherwise,
// so every action is a no-op off-host.
coop::net::Session* HostSession(const char* action) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Host) {
        UE_LOGI("moderation: %s ignored -- host-only (local role is not Host)", action);
        return nullptr;
    }
    return s;
}

bool ValidClientSlot(int slot) {
    return slot >= 1 && slot < static_cast<int>(coop::players::kMaxPeers);
}

// Narrow a (game-thread) nickname wstring into a small UTF-8 buffer for the ban
// record. Best-effort -- a banlist nick is informational only.
void NarrowNick(const std::wstring& w, char out[24]) {
    out[0] = '\0';
    if (w.empty()) return;
    int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                                  out, 23, nullptr, nullptr);
    if (n < 0) n = 0;
    out[n] = '\0';
}

}  // namespace

void SetSession(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
}

void KickPlayer(const PlayerToken& token) {
    if (!token.valid()) return;
    GT::Post([token] {
        auto* s = HostSession("kick");
        if (!s) return;
        if (s->KickWithToken(token.slot, token.generation, "kicked by host"))
            UE_LOGI("moderation: kicked #%u (slot %d)",
                    static_cast<unsigned>(token.playerNo), token.slot);
        else
            UE_LOGW("moderation: kick of #%u (slot %d) did nothing -- they are already "
                    "gone, or someone else now holds that slot",
                    static_cast<unsigned>(token.playerNo), token.slot);
    });
}

void BanPlayer(const PlayerToken& token, const char* reason) {
    if (!token.valid()) return;
    GT::Post([token, reason = std::string(reason ? reason : "")] {
        auto* s = HostSession("ban");
        if (!s) return;
        // Capture the IP + nick BEFORE the kick -- Kick() zeroes the slot, after
        // which the address can no longer be resolved. BOTH reads are token-gated:
        // reading the successor's address and writing it to the permanent banlist
        // is exactly the failure this path exists to prevent.
        char ip[64] = {};
        const bool haveIp = s->GetPeerAddressWithToken(token.slot, token.generation,
                                                       ip, sizeof(ip));
        char nick[24] = {};
        NarrowNick(coop::roster_ledger::Get(token.slot).nick, nick);

        // ABORT before writing anything if the captured player is gone. A ban is
        // permanent and IP-keyed; applying it to whoever inherited the seat would
        // be both wrong and effectively irreversible for them.
        if (coop::roster_ledger::Get(token.slot).playerNo != token.playerNo) {
            UE_LOGW("moderation: ban of #%u ABORTED -- slot %d now holds #%u",
                    static_cast<unsigned>(token.playerNo), token.slot,
                    static_cast<unsigned>(coop::roster_ledger::Get(token.slot).playerNo));
            return;
        }

        if (haveIp && ip[0]) {
            coop::ban_list::Add(ip, nick, reason.c_str());
        } else {
            // No resolvable IP (already disconnected, or GNS has no remote addr):
            // still kick, but we can't persist a ban. Surface it rather than
            // silently doing a kick-shaped no-ban.
            UE_LOGW("moderation: ban #%u (slot %d) -- no resolvable IP, kicking WITHOUT "
                    "a persistent ban", static_cast<unsigned>(token.playerNo), token.slot);
        }
        if (!s->KickWithToken(token.slot, token.generation, "banned by host"))
            UE_LOGW("moderation: ban #%u -- kick did nothing (already gone?)",
                    static_cast<unsigned>(token.playerNo));
        else
            UE_LOGI("moderation: banned + kicked #%u (slot %d, ip=%s)",
                    static_cast<unsigned>(token.playerNo), token.slot, ip[0] ? ip : "?");
    });
}

void BanOffline(const char* guid, const char* reason) {
    if (!guid || !guid[0]) return;
    GT::Post([guid = std::string(guid), reason = std::string(reason ? reason : "")] {
        auto* s = HostSession("offline ban");
        if (!s) return;
        coop::seen_players::Entry e;
        if (!coop::seen_players::FindByGuid(guid.c_str(), e)) {
            UE_LOGW("moderation: offline ban -- unknown GUID %s", guid.c_str());
            return;
        }
        if (!e.ip[0]) {
            // A record without an IP has nothing the accept filter can enforce
            // against. Surface it rather than writing a no-op ban row.
            UE_LOGW("moderation: offline ban '%s' -- record has no stored IP, cannot ban",
                    e.nick);
            return;
        }
        coop::ban_list::Add(e.ip, e.nick, reason.c_str());
        UE_LOGI("moderation: offline-banned '%s' (ip=%s)", e.nick, e.ip);
    });
}

void Unban(const char* ip) {
    if (!ip || !ip[0]) return;
    if (!coop::ban_list::Remove(ip))
        UE_LOGW("moderation: unban %s -- was not banned", ip);
}

void TeleportPlayerToMe(const PlayerToken& token) {
    if (!token.valid()) return;
    // teleport_client self-gates on host + posts to the game thread itself. The
    // token is re-checked there, on the game thread, where the ledger is legal to
    // read -- a slot whose occupant changed teleports nobody.
    GT::Post([token] {
        if (coop::roster_ledger::Get(token.slot).playerNo != token.playerNo) {
            UE_LOGW("moderation: teleport of #%u skipped -- slot %d changed hands",
                    static_cast<unsigned>(token.playerNo), token.slot);
            return;
        }
        coop::teleport_client::TeleportSlotToHost(token.slot);
    });
}

}  // namespace coop::moderation
