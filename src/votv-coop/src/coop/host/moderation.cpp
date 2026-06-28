// coop/moderation.cpp -- see coop/moderation.h.

#include "coop/host/moderation.h"

#include "coop/host/ban_list.h"
#include "coop/dev/teleport_client.h"
#include "coop/net/session.h"
#include "coop/player_handshake.h"
#include "coop/players_registry.h"
#include "ue_wrap/game_thread.h"
#include "ue_wrap/log.h"

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

void KickSlot(int peerSlot) {
    if (!ValidClientSlot(peerSlot)) return;
    GT::Post([peerSlot] {
        auto* s = HostSession("kick");
        if (!s) return;
        if (s->Kick(peerSlot, "kicked by host"))
            UE_LOGI("moderation: kicked slot %d", peerSlot);
        else
            UE_LOGW("moderation: kick slot %d failed (not connected?)", peerSlot);
    });
}

void BanSlot(int peerSlot) {
    if (!ValidClientSlot(peerSlot)) return;
    GT::Post([peerSlot] {
        auto* s = HostSession("ban");
        if (!s) return;
        // Capture the IP + nick BEFORE the kick -- Kick() zeroes the slot, after
        // which GetPeerAddress / NicknameForSlot can no longer resolve it.
        char ip[64] = {};
        const bool haveIp = s->GetPeerAddress(peerSlot, ip, sizeof(ip));
        char nick[24] = {};
        NarrowNick(coop::player_handshake::NicknameForSlot(peerSlot), nick);

        if (haveIp && ip[0]) {
            coop::ban_list::Add(ip, nick);
        } else {
            // No resolvable IP (already disconnected, or GNS has no remote addr):
            // still kick, but we can't persist a ban. Surface it rather than
            // silently doing a kick-shaped no-ban.
            UE_LOGW("moderation: ban slot %d -- no resolvable IP, kicking WITHOUT a persistent ban",
                    peerSlot);
        }
        if (!s->Kick(peerSlot, "banned by host"))
            UE_LOGW("moderation: ban slot %d -- kick failed (already gone?)", peerSlot);
        else
            UE_LOGI("moderation: banned + kicked slot %d (ip=%s)", peerSlot, ip[0] ? ip : "?");
    });
}

void TeleportSlotToMe(int peerSlot) {
    if (!ValidClientSlot(peerSlot)) return;
    // teleport_client self-gates on host + posts to the game thread itself.
    coop::dev::teleport_client::TeleportSlotToHost(peerSlot);
}

}  // namespace coop::moderation
