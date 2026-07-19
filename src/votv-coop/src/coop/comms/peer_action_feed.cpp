// coop/comms/peer_action_feed.cpp -- see coop/comms/peer_action_feed.h.

#include "coop/comms/peer_action_feed.h"

#include "coop/comms/chat_feed.h"
#include "coop/player/players_registry.h"
#include "coop/session/player_handshake.h"

#include "coop/config/config.h"

#include <atomic>
#include <mutex>

namespace coop::peer_action_feed {
namespace {

// Default ON; the persisted value (multivoid.ini ui.chat.peer_actions) is loaded on
// first access (Enabled/Announce) via g_loadOnce -- no session-install hook needed.
std::atomic<bool> g_enabled{true};
std::once_flag    g_loadOnce;

void EnsureLoaded() {
    std::call_once(g_loadOnce, [] {
        const std::string v = coop::config::ReadIniValue("ui.chat.peer_actions", "1");
        g_enabled.store(v != "0", std::memory_order_relaxed);
    });
}

// (ToUtf8 shared from chat_feed.h -- the local copy retired 2026-07-10, RULE 2.)

}  // namespace

void SetEnabled(bool on) {
    EnsureLoaded();
    g_enabled.store(on, std::memory_order_relaxed);
    coop::config::WriteIniValue("ui.chat.peer_actions", on ? "1" : "0");
}

bool Enabled() {
    EnsureLoaded();
    return g_enabled.load(std::memory_order_relaxed);
}

void AnnounceDirect(uint8_t slot, const std::wstring& action) {
    // The subject is ALWAYS a nickname (Minecraft feed principle, user
    // 2026-07-18): the local actor's own line renders its own nick, same as
    // every other peer sees it. Roster's resolution pattern (roster.cpp).
    const bool isLocal = (slot == coop::players::Registry::Get().LocalPeerId());
    const std::wstring& nickW =
        isLocal ? coop::player_handshake::LocalNickname()
                : coop::player_handshake::NicknameForSlot(static_cast<int>(slot));
    const std::string nick = coop::chat_feed::ToUtf8(nickW.empty() ? std::wstring(L"Player") : nickW);
    const std::string line = nick + " " + coop::chat_feed::ToUtf8(action);

    // PushAction colors the first nickLen BYTES per `slot` (chat parity); the rest is
    // the action predicate in the ACTION color (yellow -- user 2026-07-11), so a
    // world-state action reads apart from typed chat. Out-of-range slot (a local
    // actor before slot assignment) clamps to 0 for the color lookup only.
    const uint8_t colorSlot = slot < coop::players::kMaxPeers ? slot : 0;
    coop::chat_feed::PushAction(line,
                                static_cast<uint8_t>(nick.size() > 255 ? 255 : nick.size()),
                                colorSlot);
}

void Announce(uint8_t slot, const std::wstring& action) {
    if (!Enabled()) return;
    AnnounceDirect(slot, action);
}

}  // namespace coop::peer_action_feed
