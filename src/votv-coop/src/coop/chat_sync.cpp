// coop/chat_sync.cpp -- see coop/chat_sync.h.

#include "coop/chat_sync.h"

#include "coop/chat_feed.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player_handshake.h"

#include "ue_wrap/game_thread.h"
#include "ue_wrap/log.h"

#include <atomic>
#include <cstring>

namespace coop::chat_sync {
namespace {

namespace GT = ue_wrap::game_thread;

std::atomic<coop::net::Session*> g_session{nullptr};

// Sanitize one wire/UI byte for the ASCII chat_feed store: printable ASCII
// passes, anything else (control bytes, UTF-8 multibyte units) becomes '?'.
// Identity/trust note: the TEXT is the only attacker-controlled surface; the
// nickname prefix comes from the transport slot, never the payload.
wchar_t SanitizeChar(char c) {
    const unsigned char u = static_cast<unsigned char>(c);
    return (u >= 0x20 && u < 0x7F) ? static_cast<wchar_t>(u) : L'?';
}

// Trim leading/trailing whitespace + cap to the payload text size. Returns
// empty when nothing displayable remains (caller drops the line).
std::string TrimAndCap(const std::string& in) {
    size_t b = 0, e = in.size();
    while (b < e && (in[b] == ' ' || in[b] == '\t' || in[b] == '\r' || in[b] == '\n')) ++b;
    while (e > b && (in[e-1] == ' ' || in[e-1] == '\t' || in[e-1] == '\r' || in[e-1] == '\n')) --e;
    std::string out = in.substr(b, e - b);
    if (out.size() > sizeof(coop::net::ChatMessagePayload{}.text))
        out.resize(sizeof(coop::net::ChatMessagePayload{}.text));
    return out;
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
}

bool SessionActive() {
    auto* s = g_session.load(std::memory_order_acquire);
    return s && s->connected();
}

void QueueSend(const std::string& utf8Text) {
    const std::string text = TrimAndCap(utf8Text);
    if (text.empty()) return;
    // Hop to the game thread: SendReliable + chat_feed::Push + LocalNickname
    // are game-thread paths; the ImGui input bar submits on the render thread.
    GT::Post([text] {
        auto* s = g_session.load(std::memory_order_acquire);
        if (!s || !s->connected()) return;  // session died between type + send
        coop::net::ChatMessagePayload p{};
        p.len = static_cast<uint8_t>(text.size());
        std::memcpy(p.text, text.data(), text.size());
        s->SendReliable(coop::net::ReliableKind::ChatMessage, &p, sizeof(p));
        // Local echo (the origin never receives its own send).
        std::wstring line = coop::player_handshake::LocalNickname() + L": ";
        for (char c : text) line.push_back(SanitizeChar(c));
        coop::chat_feed::Push(line);
        UE_LOGI("chat: sent %u byte(s)", static_cast<unsigned>(text.size()));
    });
}

void OnReliable(const coop::net::ChatMessagePayload& payload, uint8_t senderPeerSlot) {
    uint8_t n = payload.len;
    if (n == 0) return;
    if (n > sizeof(payload.text)) n = sizeof(payload.text);
    std::wstring line = coop::player_handshake::NicknameForSlot(senderPeerSlot) + L": ";
    for (uint8_t i = 0; i < n; ++i) line.push_back(SanitizeChar(payload.text[i]));
    coop::chat_feed::Push(line);
}

void OnDisconnect() {
    g_session.store(nullptr, std::memory_order_release);
}

}  // namespace coop::chat_sync
