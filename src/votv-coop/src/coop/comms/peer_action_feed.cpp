// coop/comms/peer_action_feed.cpp -- see coop/comms/peer_action_feed.h.

#include "coop/comms/peer_action_feed.h"

#include "coop/comms/chat_feed.h"
#include "coop/session/player_handshake.h"

#include "harness/config.h"

#include <atomic>
#include <mutex>

namespace coop::peer_action_feed {
namespace {

// Default ON; the persisted value (votv-coop.ini ui.chat.peer_actions) is loaded on
// first access (Enabled/Announce) via g_loadOnce -- no session-install hook needed.
std::atomic<bool> g_enabled{true};
std::once_flag    g_loadOnce;

void EnsureLoaded() {
    std::call_once(g_loadOnce, [] {
        const std::string v = harness::config::ReadIniValue("ui.chat.peer_actions", "1");
        g_enabled.store(v != "0", std::memory_order_relaxed);
    });
}

// UTF-8 encode (mirrors chat_feed's ToUtf8 -- the feed carries UTF-8 so a Cyrillic
// nick passes through; no shared util header exists yet).
std::string ToUtf8(const std::wstring& w) {
    std::string s;
    s.reserve(w.size() * 2);
    for (size_t i = 0; i < w.size(); ++i) {
        uint32_t cp = w[i];
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < w.size() &&
            w[i + 1] >= 0xDC00 && w[i + 1] <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (w[i + 1] - 0xDC00);
            ++i;
        }
        if (cp < 0x20 && cp != 0x09) continue;  // strip control chars
        if (cp < 0x80) {
            s.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            s.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            s.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            s.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            s.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    return s;
}

}  // namespace

void SetEnabled(bool on) {
    EnsureLoaded();
    g_enabled.store(on, std::memory_order_relaxed);
    harness::config::WriteIniValue("ui.chat.peer_actions", on ? "1" : "0");
}

bool Enabled() {
    EnsureLoaded();
    return g_enabled.load(std::memory_order_relaxed);
}

void Announce(uint8_t slot, bool isLocalActor, const std::wstring& action) {
    if (!Enabled()) return;

    const std::wstring nickW =
        isLocalActor ? std::wstring(L"You")
                     : coop::player_handshake::NicknameForSlot(static_cast<int>(slot));
    const std::string nick = ToUtf8(nickW.empty() ? std::wstring(L"Player") : nickW);
    const std::string line = nick + " " + ToUtf8(action);

    // PushChat colors the first nickLen BYTES per `slot` (chat parity); the rest is
    // the action predicate in the default event color.
    coop::chat_feed::PushChat(line,
                              static_cast<uint8_t>(nick.size() > 255 ? 255 : nick.size()),
                              slot);
}

}  // namespace coop::peer_action_feed
