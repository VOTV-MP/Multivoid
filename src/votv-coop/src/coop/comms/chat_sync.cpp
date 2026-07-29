// coop/chat_sync.cpp -- see coop/chat_sync.h.

#include "coop/comms/chat_sync.h"

#include "coop/text/utf8_codec.h"

#include "coop/comms/chat_bubbles.h"
#include "coop/comms/chat_feed.h"
#include "coop/comms/chat_log.h"
#include "coop/comms/chat_nick_color.h"
#include "coop/config/config.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/session/player_handshake.h"

#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"

#include <atomic>
#include <cstring>
#include <string>

namespace coop::chat_sync {
namespace {

namespace GT = ue_wrap::game_thread;

std::atomic<coop::net::Session*> g_session{nullptr};

// ---- the CLIENT's applied range.
//
// A CONTIGUOUS range, not a high-watermark. The distinction is not academic: the join
// seed delivers rows OLDER than anything a client may already hold, and "apply iff
// lineSeq > highest" would have discarded the ENTIRE seed -- an empty history with no
// error logged anywhere. It lives here, next to the apply that reads it, and is
// cleared by the SAME Reset() as the record itself; a free-floating watermark is
// exactly how a reset gets forgotten.
bool     g_haveRange = false;
uint32_t g_rangeLo = 0;
uint32_t g_rangeHi = 0;

// ---- the speaker binding table (client).
//
// A ChatSpeaker always immediately precedes its ChatLine on the same ordered lane, so
// this only has to stay valid across two consecutive messages. speakerId is a PER-BURST
// index: a live line always uses 0, a seed burst numbers its distinct speakers. There
// is no minting policy and no eviction policy because there is nothing to bound -- a
// later burst simply overwrites.
constexpr int kMaxSpeakers = 16;
struct Speaker {
    bool        valid = false;
    uint8_t     slot = 0;
    uint32_t    nickArgb = 0;
    std::string nick;
};
Speaker g_speakers[kMaxSpeakers];

// ---- which slots the HOST has already seeded.
//
// A slot receives live rows ONLY after its seed has been sent, and this gate is what
// makes the client's applied range a single interval that can only grow upward. Without
// it the two streams interleave: a line authored between a slot's world-ready and its
// seed reaches it first, so the seed then delivers rows BELOW everything applied, and a
// contiguous range cannot express the hole that leaves. Relying on the send path's
// pre-world gate to do this implicitly would work today and break the moment somebody
// adds ChatLine to IsPreWorldSendableKind -- which the design very nearly did.
bool g_seeded[coop::net::kMaxPeers] = {};

// RULE 2, 2026-07-29: the two hand-rolled copies that lived here are GONE. This file
// already included coop/text/utf8_codec.h -- the ONE owner of text encoding -- while
// carrying a byte-identical re-implementation of its SanitizeUtf8 and a second
// re-implementation of its CapUtf8Bytes.
//
// Trim is the only part that was genuinely local, so it is the only part left.
std::string TrimAndCap(const std::string& in) {
    size_t b = 0, e = in.size();
    while (b < e && (in[b] == ' ' || in[b] == '\t' || in[b] == '\r' || in[b] == '\n')) ++b;
    while (e > b && (in[e-1] == ' ' || in[e-1] == '\t' || in[e-1] == '\r' || in[e-1] == '\n')) --e;
    return coop::text::CapUtf8Bytes(in.substr(b, e - b),
                                    sizeof(coop::net::ChatMessagePayload{}.text));
}

// THE RECEIVE BOUNDARY. utf8_codec.h states the contract in its own header --
// "well-formedness is established where we READ, not where we wrote ... the receive
// boundary decodes STRICTLY and rejects a whole ill-formed field rather than repairing
// it" -- and chat was the one surface that never honoured it until 2026-07-29.
//
// Chat text is the ONLY attacker-controlled string in the process. Refuse, do not
// repair: a repaired chat line is a sentence nobody typed. The whole message is
// dropped and the refusal is logged, so a drill can see it and a real defect in
// someone's sender is attributable rather than silent.
bool WellFormed(const char* p, size_t n) {
    std::wstring ignored;
    return coop::text::FromUtf8Strict(p, n, &ignored);
}

bool IsHost() {
    auto* s = g_session.load(std::memory_order_acquire);
    return s && s->role() == coop::net::Role::Host;
}

// Render one committed row into this peer's feed. `seeded` rows land retained.
void ApplyRow(uint8_t slot, const std::string& nick, uint32_t custom,
              const std::string& text, uint32_t lineSeq, bool seeded) {
    // The RECEIVER resolves the colour, once, here: the wire carries the speaker's
    // CUSTOM pick (or 0 for none) and the per-slot fallback palette stays render-side
    // where it belongs. Then it is FROZEN onto the line -- user 2026-07-29, "old chat
    // history is essentially a frozen history".
    const uint32_t argb = coop::nick_color::IsCustom(custom)
        ? custom
        : coop::chat_nick_color::kSlotCols[slot % 8u];
    const std::string line = nick + ": " + text;
    coop::chat_feed::PushWireChat(line,
                                  static_cast<uint8_t>(nick.size() > 255 ? 255 : nick.size()),
                                  argb, lineSeq, seeded);
    // The overhead bubble is a LIVE-world effect. A seeded row must never reach it --
    // structurally, not by a flag test at the far end: replaying a joiner's whole
    // history through it would put N bubbles over peers for conversations that
    // happened before that player existed.
    if (!seeded) coop::chat_bubbles::OnChatLine(slot, text.c_str());
    // The lane's only ORDER observable. A drill cannot read a sort key off a
    // screenshot, and "the lines appeared" is not the same claim as "they appeared in
    // the order the lobby said them" -- which is the half a seed interleaving with live
    // traffic breaks. One line per applied row; a seed burst is a one-time ~100.
    UE_LOGI("chat: applied line %u seeded=%d \"%.40s\"", lineSeq, seeded ? 1 : 0,
            line.c_str());
}

void SendSpeaker(coop::net::Session& s, int toSlot, uint16_t speakerId, uint8_t slot,
                 const std::string& nick, uint32_t custom) {
    coop::net::ChatSpeakerPayload sp{};
    sp.speakerId = speakerId;
    sp.slot      = slot;
    sp.nickArgb  = custom;
    const size_t n = nick.size() > sizeof(sp.nick) ? sizeof(sp.nick) : nick.size();
    sp.nickLen = static_cast<uint8_t>(n);
    std::memcpy(sp.nick, nick.data(), n);
    s.SendReliableToSlot(toSlot, coop::net::ReliableKind::ChatSpeaker, &sp, sizeof(sp));
}

void SendLine(coop::net::Session& s, int toSlot, uint32_t lineSeq, uint16_t speakerId,
              const std::string& text, bool seeded) {
    coop::net::ChatLinePayload lp{};
    lp.lineSeq   = lineSeq;
    lp.speakerId = speakerId;
    lp.flags     = seeded ? coop::net::kChatLineFlagSeed : 0u;
    const size_t n = text.size() > sizeof(lp.text) ? sizeof(lp.text) : text.size();
    lp.len = static_cast<uint8_t>(n);
    std::memcpy(lp.text, text.data(), n);
    s.SendReliableToSlot(toSlot, coop::net::ReliableKind::ChatLine, &lp, sizeof(lp));
}

// HOST: commit `text` as spoken by `slot`, broadcast it, and render it locally.
void AuthorAndBroadcast(uint8_t slot, const std::string& text) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running()) return;

    // The nick is resolved ONCE, HERE, and then travels with the row forever. Resolving
    // at render time on each peer answers a different question -- who is in that slot
    // NOW -- and slots recycle, so a resident and a joiner would end up holding
    // permanently different names for the same message.
    const uint8_t localSlot = 0;  // the host is always slot 0
    const std::wstring nickW = (slot == localSlot)
        ? coop::player_handshake::LocalNickname()
        : coop::player_handshake::NicknameForSlot(slot);
    const std::string nick = coop::text::ToUtf8(nickW);
    const uint32_t custom = coop::nick_color::PackedForSlot(slot);

    const uint32_t lineSeq = coop::chat_log::Append(slot, nick, custom, text);

    for (int to = 1; to < static_cast<int>(coop::net::kMaxPeers); ++to) {
        if (!s->IsSlotReady(to) || !g_seeded[to]) continue;
        SendSpeaker(*s, to, 0, slot, nick, custom);
        SendLine(*s, to, lineSeq, 0, text, /*seeded=*/false);
    }
    ApplyRow(slot, nick, custom, text, lineSeq, /*seeded=*/false);
    UE_LOGI("chat: committed line %u from slot %u (%zu byte(s))",
            lineSeq, static_cast<unsigned>(slot), text.size());
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
    // Hop to the game thread: SendReliable, the record and chat_feed are all
    // game-thread paths; the ImGui input bar submits on the render thread.
    GT::Post([text] {
        auto* s = g_session.load(std::memory_order_acquire);
        if (!s || !s->running()) return;  // session died between type + send
        if (s->role() == coop::net::Role::Host) {
            // The host IS the authority. It commits its own line immediately -- a host
            // alone in its lobby has nobody to send to, and the line still belongs in
            // the record so the next joiner is seeded with it.
            AuthorAndBroadcast(0, text);
            return;
        }
        // A CLIENT sends an INTENT and waits for the host's authored row. There is no
        // local echo: the row it will receive is the one with a position in the order,
        // and drawing a second copy now would mean reconciling two of them later.
        coop::net::ChatMessagePayload p{};
        p.len = static_cast<uint8_t>(text.size());
        std::memcpy(p.text, text.data(), text.size());
        // DEV INJECTION -- the must-FAIL control for the receive boundary in
        // OnReliable. A validator that has only ever been shown PASSING passes by
        // construction ([[lesson-an-instrument-blind-to-the-phenomenon-always-passes]]);
        // the codec selftest proves FromUtf8Strict REFUSES, but nothing proved chat
        // CALLS it. It APPENDS a lone continuation byte rather than overwriting one:
        // overwriting is not reliably corrupting, since the last byte of a Cyrillic
        // message is the tail of a 2-byte sequence and D0 80 is a perfectly legal
        // U+0400. A trailing 0x80 is ill-formed unconditionally (utf8_codec.cpp:176).
        if (p.len < sizeof(p.text) &&
            coop::config::ReadEnv("VOTVCOOP_CHAT_CORRUPT_WIRE") == "1") {
            p.text[p.len++] = static_cast<char>(0x80);
            UE_LOGW("chat: [dev] appended a lone continuation byte to the intent "
                    "(len %u)", static_cast<unsigned>(p.len));
        }
        if (s->SendReliable(coop::net::ReliableKind::ChatMessage, &p, sizeof(p)))
            UE_LOGI("chat: sent intent, %u byte(s) -- awaiting the host's authored row",
                    static_cast<unsigned>(text.size()));
        else
            UE_LOGW("chat: intent send FAILED (%u byte(s)) -- the line is lost",
                    static_cast<unsigned>(text.size()));
    });
}

void OnReliable(const coop::net::ChatMessagePayload& payload, uint8_t senderPeerSlot) {
    if (!IsHost()) {
        // Nothing sends ChatMessage to a client any more -- it left the relay whitelist
        // with v133. Reaching here means a peer is speaking a protocol we retired.
        UE_LOGW("chat: a ChatMessage arrived on a CLIENT from slot %u -- chat is "
                "host-authored since v133; dropping",
                static_cast<unsigned>(senderPeerSlot));
        return;
    }
    uint8_t n = payload.len;
    if (n == 0) return;
    if (n > sizeof(payload.text)) n = sizeof(payload.text);
    // Decode BEFORE anything renders it or enters it into the lobby's permanent
    // record. A field that is not well-formed UTF-8 is refused whole -- see
    // WellFormed() above for why repairing is not an option. This gate matters MORE
    // under host authoring than it did under the relay: an ill-formed line committed
    // here would be re-emitted to every future joiner, for the life of the lobby.
    if (!WellFormed(payload.text, n)) {
        UE_LOGW("chat: refused an ill-formed message from slot %u (%u byte(s)) -- "
                "not well-formed UTF-8", static_cast<unsigned>(senderPeerSlot),
                static_cast<unsigned>(n));
        return;
    }
    AuthorAndBroadcast(senderPeerSlot,
                       coop::text::SanitizeUtf8(payload.text, n));
}

void OnChatSpeaker(const coop::net::ChatSpeakerPayload& payload) {
    if (payload.speakerId >= kMaxSpeakers) {
        UE_LOGW("chat: ChatSpeaker id %u out of range -- dropping",
                static_cast<unsigned>(payload.speakerId));
        return;
    }
    uint8_t n = payload.nickLen;
    if (n > sizeof(payload.nick)) n = sizeof(payload.nick);
    if (!WellFormed(payload.nick, n)) {
        UE_LOGW("chat: refused an ill-formed speaker nick (%u byte(s))",
                static_cast<unsigned>(n));
        return;
    }
    Speaker& sp = g_speakers[payload.speakerId];
    sp.valid    = true;
    sp.slot     = payload.slot;
    sp.nickArgb = payload.nickArgb;
    sp.nick     = coop::text::SanitizeUtf8(payload.nick, n);
}

void OnChatLine(const coop::net::ChatLinePayload& payload) {
    if (payload.speakerId >= kMaxSpeakers || !g_speakers[payload.speakerId].valid) {
        UE_LOGW("chat: ChatLine %u names speaker %u, which has no binding -- dropping",
                payload.lineSeq, static_cast<unsigned>(payload.speakerId));
        return;
    }
    if (payload.lineSeq == 0) {
        UE_LOGW("chat: ChatLine with lineSeq 0 -- dropping (0 means 'no line')");
        return;
    }
    uint8_t n = payload.len;
    if (n == 0) return;
    if (n > sizeof(payload.text)) n = sizeof(payload.text);
    if (!WellFormed(payload.text, n)) {
        UE_LOGW("chat: refused an ill-formed authored line %u (%u byte(s))",
                payload.lineSeq, static_cast<unsigned>(n));
        return;
    }

    const uint32_t seq = payload.lineSeq;
    if (g_haveRange && seq >= g_rangeLo && seq <= g_rangeHi) return;  // already applied
    if (g_haveRange && seq != g_rangeHi + 1 && seq != g_rangeLo - 1) {
        // A GAP. This cannot happen while ChatLine is absent from
        // IsPreWorldSendableKind: everything said before this peer's world-ready is
        // covered by the seed, and everything after arrives strictly in sequence, so
        // the applied set is one interval that only ever grows. If this ever logs, that
        // premise changed and the dedup needs a real interval SET rather than a range
        // -- extending across the gap here would silently swallow the rows inside it.
        UE_LOGW("chat: applied-range GAP -- line %u against [%u,%u]. The pre-world gate "
                "must have changed; the dedup range can no longer express this",
                seq, g_rangeLo, g_rangeHi);
    }

    const Speaker& sp = g_speakers[payload.speakerId];
    const bool seeded = (payload.flags & coop::net::kChatLineFlagSeed) != 0;
    ApplyRow(sp.slot, sp.nick, sp.nickArgb,
             coop::text::SanitizeUtf8(payload.text, n), seq, seeded);

    if (!g_haveRange) {
        g_haveRange = true;
        g_rangeLo = g_rangeHi = seq;
    } else {
        if (seq < g_rangeLo) g_rangeLo = seq;
        if (seq > g_rangeHi) g_rangeHi = seq;
    }
}

void QueueConnectBroadcastForSlot(int slot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running() || s->role() != coop::net::Role::Host) return;
    if (slot < 1 || slot >= static_cast<int>(coop::net::kMaxPeers)) return;
    // Set BEFORE the empty-record early return: an empty lobby still has to start
    // receiving live rows, and a gate that only opens when there was history to send is
    // a gate that stays shut for the first conversation.
    g_seeded[slot] = true;
    // DEV INJECTION -- the must-FAIL control for the join seed. With this set the slot
    // is opened for live traffic but the history is never sent, which is precisely the
    // "joiner sees an empty history and nothing logs an error" failure the contiguous
    // applied range was introduced to prevent. Drill D-W must go RED.
    if (coop::config::ReadEnv("VOTVCOOP_CHAT_SEED_SUPPRESS") == "1") {
        UE_LOGW("chat: [dev] connect-seed SUPPRESSED for slot %d (%d line(s) withheld)",
                slot, coop::chat_log::Count());
        return;
    }
    if (coop::chat_log::Count() <= 0) {
        UE_LOGI("chat: connect-seed -- no history yet; slot %d is now live", slot);
        return;
    }

    // Dedupe the speaker bindings WITHIN this burst: the same handful of people said
    // most of it, and re-sending an 88-byte binding per line would triple the seed for
    // nothing. Across bursts nothing is remembered -- a global "last binding I sent
    // you" is exactly what strands a joiner who never saw the earlier one.
    struct Binding { uint8_t slot; uint32_t argb; std::string nick; };
    Binding bound[kMaxSpeakers];
    int nBound = 0;
    int sent = 0;

    coop::chat_log::ForEach([&](const coop::chat_log::Row& r) {
        int id = -1;
        for (int i = 0; i < nBound; ++i) {
            if (bound[i].slot == r.slot && bound[i].argb == r.nickArgb &&
                bound[i].nick == r.nick) { id = i; break; }
        }
        if (id < 0) {
            if (nBound >= kMaxSpeakers) {
                // More distinct speakers than the burst can index. Re-bind slot 0 --
                // correctness over compactness: the row still renders with the right
                // name, it just costs another binding.
                nBound = 0;
            }
            id = nBound++;
            bound[id] = {r.slot, r.nickArgb, r.nick};
            SendSpeaker(*s, slot, static_cast<uint16_t>(id), r.slot, r.nick, r.nickArgb);
        }
        SendLine(*s, slot, r.lineSeq, static_cast<uint16_t>(id), r.text, /*seeded=*/true);
        ++sent;
    });
    UE_LOGI("chat: connect-seed -- sent %d history line(s) and %d speaker binding(s) "
            "to slot %d", sent, nBound, slot);
}

void OnSlotDisconnected(int slot) {
    // A slot that turns over must be re-seeded before it hears anything live -- the
    // NEXT occupant's applied range starts empty, and live rows arriving before its
    // seed would put the seed underneath them.
    if (slot >= 0 && slot < static_cast<int>(coop::net::kMaxPeers)) g_seeded[slot] = false;
}

void Reset() {
    coop::chat_log::Reset();
    for (bool& b : g_seeded) b = false;
    g_haveRange = false;
    g_rangeLo = g_rangeHi = 0;
    for (Speaker& sp : g_speakers) { sp.valid = false; sp.nick.clear(); }
}

void OnDisconnect() {
    g_session.store(nullptr, std::memory_order_release);
}

}  // namespace coop::chat_sync
