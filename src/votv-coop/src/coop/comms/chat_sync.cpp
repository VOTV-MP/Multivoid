// coop/chat_sync.cpp -- see coop/chat_sync.h.

#include "coop/comms/chat_sync.h"

#include "coop/text/utf8_codec.h"

#include "coop/comms/chat_bubbles.h"
#include "coop/comms/chat_feed.h"
#include "coop/comms/chat_nick_color.h"
#include "coop/config/config.h"
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

// RULE 2, 2026-07-29: the two hand-rolled copies that lived here are GONE. This
// file already included coop/text/utf8_codec.h -- the ONE owner of text encoding
// -- while carrying a byte-identical re-implementation of its SanitizeUtf8 and a
// second re-implementation of its CapUtf8Bytes. Two implementations of one
// concept, compiled together, in the file that includes the owner.
//
// Trim is the only part that was genuinely local, so it is the only part left.
// The cap now goes through the owner, which is also where the "never split a
// multi-byte sequence" reasoning is documented.
std::string TrimAndCap(const std::string& in) {
    size_t b = 0, e = in.size();
    while (b < e && (in[b] == ' ' || in[b] == '\t' || in[b] == '\r' || in[b] == '\n')) ++b;
    while (e > b && (in[e-1] == ' ' || in[e-1] == '\t' || in[e-1] == '\r' || in[e-1] == '\n')) --e;
    return coop::text::CapUtf8Bytes(in.substr(b, e - b),
                                    sizeof(coop::net::ChatMessagePayload{}.text));
}

// THE RECEIVE BOUNDARY. utf8_codec.h states the contract in its own header --
// "well-formedness is established where we READ, not where we wrote ... the
// receive boundary decodes STRICTLY and rejects a whole ill-formed field rather
// than repairing it" -- and chat was the one surface that never honoured it.
//
// Chat text is the ONLY attacker-controlled string in the process (the nick
// prefix comes from the transport slot, never the payload). It was reaching TWO
// render surfaces -- the feed and the overhead bubbles -- as raw wire bytes that
// had been control-stripped and nothing else. "ImGui draws invalid sequences as
// replacement glyphs" was true and was not the point: it made ill-formed input
// merely ugly instead of refused, which is exactly the failure FromUtf8Strict was
// introduced to prevent for names (utf8_codec.h:54-57).
//
// Refuse, do not repair: a repaired chat line is a sentence nobody typed. The
// whole message is dropped and the refusal is logged, so a drill can see it and
// a real defect in someone's sender is attributable rather than silent.
bool WellFormed(const char* p, size_t n) {
    std::wstring ignored;
    return coop::text::FromUtf8Strict(p, n, &ignored);
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
            // DEV INJECTION -- the must-FAIL control for the receive boundary
            // below. A validator that has only ever been shown PASSING passes by
            // construction ([[lesson-an-instrument-blind-to-the-phenomenon-always-passes]]);
            // the codec selftest proves FromUtf8Strict REFUSES, but nothing proved
            // chat CALLS it. This corrupts the WIRE copy only -- the local echo
            // below is untouched -- so one drill produces a differential: the
            // sender's own feed shows the line, every receiver logs a refusal.
            // It APPENDS a lone continuation byte rather than overwriting one.
            // Overwriting is not reliably corrupting: the last byte of "привет
            // всем" is the tail of a 2-byte sequence, and D0 80 is a perfectly
            // legal U+0400 -- the injection would have proven nothing on exactly
            // the messages this drill sends. A trailing 0x80 is ill-formed
            // unconditionally, whatever precedes it (utf8_codec.cpp:176).
            if (p.len < sizeof(p.text) &&
                coop::config::ReadEnv("VOTVCOOP_CHAT_CORRUPT_WIRE") == "1") {
                p.text[p.len++] = static_cast<char>(0x80);
                UE_LOGW("chat: [dev] appended a lone continuation byte to the WIRE "
                        "copy (len %u); the local echo is untouched",
                        static_cast<unsigned>(p.len));
            }
            s->SendReliable(coop::net::ReliableKind::ChatMessage, &p, sizeof(p));
        }
        // Local echo (the origin never receives its own send). PushChat carries the
        // nick byte-length plus the RESOLVED nick colour, frozen into the line here
        // (2026-07-29): our own line uses the local slot (host = 0; client = registry
        // peer id) so the colour matches what the other peers see for us (roster.cpp's
        // local-slot shape).
        uint8_t localSlot = coop::players::Registry::Get().LocalPeerId();
        if (s->role() == coop::net::Role::Host || localSlot == coop::players::kPeerIdUnknown)
            localSlot = 0;
        const std::string nick = coop::text::ToUtf8(coop::player_handshake::LocalNickname());
        const std::string msg = coop::text::SanitizeUtf8(text.data(), text.size());
        const std::string line = nick + ": " + msg;
        coop::chat_feed::PushChat(line, static_cast<uint8_t>(nick.size() > 255 ? 255 : nick.size()),
                                  coop::chat_nick_color::ForSlot(localSlot));
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
    // Decode BEFORE anything renders it. A field that is not well-formed UTF-8 is
    // refused whole -- see WellFormed() above for why repairing is not an option.
    if (!WellFormed(payload.text, n)) {
        UE_LOGW("chat: refused an ill-formed message from slot %u (%u byte(s)) -- "
                "not well-formed UTF-8", static_cast<unsigned>(senderPeerSlot),
                static_cast<unsigned>(n));
        return;
    }
    const std::string nick = coop::text::ToUtf8(coop::player_handshake::NicknameForSlot(senderPeerSlot));
    const std::string msg = coop::text::SanitizeUtf8(payload.text, n);
    const std::string line = nick + ": " + msg;
    coop::chat_feed::PushChat(line, static_cast<uint8_t>(nick.size() > 255 ? 255 : nick.size()),
                              coop::chat_nick_color::ForSlot(senderPeerSlot));
    coop::chat_bubbles::OnChatLine(senderPeerSlot, msg.c_str());  // 12g overhead bubble
}

void OnDisconnect() {
    g_session.store(nullptr, std::memory_order_release);
}

}  // namespace coop::chat_sync
