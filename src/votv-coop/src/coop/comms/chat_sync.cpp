// coop/chat_sync.cpp -- see coop/chat_sync.h.

#include "coop/comms/chat_sync.h"

#include "coop/comms/chat_bubbles.h"
#include "coop/comms/chat_feed.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/session/player_handshake.h"

#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"

#include <atomic>
#include <cstring>

namespace coop::chat_sync {
namespace {

namespace GT = ue_wrap::game_thread;

std::atomic<coop::net::Session*> g_session{nullptr};

// 2026-07-04: the feed carries UTF-8 now (ui::fonts loads Cyrillic glyphs), so the
// old printable-ASCII squash ('?' for every Russian char) is gone. Sanitize = strip
// control bytes (keep TAB), pass everything else through -- ImGui renders invalid
// UTF-8 sequences as replacement glyphs, so malformed wire bytes can't corrupt the
// draw. Identity/trust note: the TEXT is the only attacker-controlled surface; the
// nickname prefix comes from the transport slot, never the payload.
std::string SanitizeUtf8(const char* p, size_t n) {
    std::string out;
    out.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        const unsigned char u = static_cast<unsigned char>(p[i]);
        if (u >= 0x20 || u == 0x09) out.push_back(static_cast<char>(u));
    }
    return out;
}

// UTF-8-encode the wide nickname (mirrors chat_feed's encoder for the nick prefix;
// the feed stores UTF-8 whole-line).
std::string NickUtf8(const std::wstring& w) {
    std::string s;
    for (size_t i = 0; i < w.size(); ++i) {
        uint32_t cp = w[i];
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < w.size() &&
            w[i + 1] >= 0xDC00 && w[i + 1] <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (w[i + 1] - 0xDC00);
            ++i;
        }
        if (cp < 0x20) continue;
        if (cp < 0x80) s.push_back(static_cast<char>(cp));
        else if (cp < 0x800) {
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

// Trim leading/trailing whitespace + cap to the payload text size WITHOUT splitting
// a UTF-8 multibyte sequence (a mid-char cut would ship a malformed tail byte).
std::string TrimAndCap(const std::string& in) {
    size_t b = 0, e = in.size();
    while (b < e && (in[b] == ' ' || in[b] == '\t' || in[b] == '\r' || in[b] == '\n')) ++b;
    while (e > b && (in[e-1] == ' ' || in[e-1] == '\t' || in[e-1] == '\r' || in[e-1] == '\n')) --e;
    std::string out = in.substr(b, e - b);
    size_t cap = sizeof(coop::net::ChatMessagePayload{}.text);
    if (out.size() > cap) {
        // Back off past UTF-8 continuation bytes (10xxxxxx) to the char boundary.
        while (cap > 0 && (static_cast<unsigned char>(out[cap]) & 0xC0) == 0x80) --cap;
        out.resize(cap);
    }
    return out;
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
}

bool SessionActive() {
    // Chat exists for the whole COOP SESSION, not just while a peer link is up
    // (user 2026-07-04: the HOST could not open T-chat until the first client
    // joined -- a hosting session with zero clients is Handshaking, connected()
    // false). A RUNNING host session IS a live lobby: typing while alone is
    // legitimate (the line shows locally; joiners simply weren't there for it).
    // A client, by contrast, is only in a session while its link is connected.
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running()) return false;
    return s->role() == coop::net::Role::Host || s->connected();
}

void QueueSend(const std::string& utf8Text) {
    const std::string text = TrimAndCap(utf8Text);
    if (text.empty()) return;
    // Hop to the game thread: SendReliable + chat_feed::Push + LocalNickname
    // are game-thread paths; the ImGui input bar submits on the render thread.
    GT::Post([text] {
        auto* s = g_session.load(std::memory_order_acquire);
        if (!s || !s->running()) return;  // session died between type + send
        // Wire send is best-effort: a host alone in its lobby has nobody to send
        // to yet (connected() false) but the LINE still belongs in its own feed.
        const bool wired = s->connected();
        if (wired) {
            coop::net::ChatMessagePayload p{};
            p.len = static_cast<uint8_t>(text.size());
            std::memcpy(p.text, text.data(), text.size());
            s->SendReliable(coop::net::ReliableKind::ChatMessage, &p, sizeof(p));
        }
        // Local echo (the origin never receives its own send). PushChat carries the
        // nick byte-length so the HUD colors the prefix by slot; our own line uses
        // the local slot (host = 0; client = registry peer id) so the color matches
        // what the other peers see for us (roster.cpp's local-slot shape).
        uint8_t localSlot = coop::players::Registry::Get().LocalPeerId();
        if (s->role() == coop::net::Role::Host || localSlot == coop::players::kPeerIdUnknown)
            localSlot = 0;
        const std::string nick = NickUtf8(coop::player_handshake::LocalNickname());
        const std::string msg = SanitizeUtf8(text.data(), text.size());
        const std::string line = nick + ": " + msg;
        coop::chat_feed::PushChat(line, static_cast<uint8_t>(nick.size() > 255 ? 255 : nick.size()),
                                  localSlot);
        // 12g overhead bubble (message only -- the plate names the speaker). Our own
        // slot never renders locally (no self puppet); stored anyway for symmetry.
        coop::chat_bubbles::OnChatLine(localSlot, msg.c_str());
        UE_LOGI("chat: sent %u byte(s)%s", static_cast<unsigned>(text.size()),
                wired ? "" : " (no peers connected -- local echo only)");
    });
}

void OnReliable(const coop::net::ChatMessagePayload& payload, uint8_t senderPeerSlot) {
    uint8_t n = payload.len;
    if (n == 0) return;
    if (n > sizeof(payload.text)) n = sizeof(payload.text);
    const std::string nick = NickUtf8(coop::player_handshake::NicknameForSlot(senderPeerSlot));
    const std::string msg = SanitizeUtf8(payload.text, n);
    const std::string line = nick + ": " + msg;
    coop::chat_feed::PushChat(line, static_cast<uint8_t>(nick.size() > 255 ? 255 : nick.size()),
                              senderPeerSlot);
    coop::chat_bubbles::OnChatLine(senderPeerSlot, msg.c_str());  // 12g overhead bubble
}

void OnDisconnect() {
    g_session.store(nullptr, std::memory_order_release);
}

}  // namespace coop::chat_sync
