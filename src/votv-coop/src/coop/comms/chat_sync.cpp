// coop/comms/chat_sync.cpp -- see coop/comms/chat_sync.h.

#include "coop/comms/chat_sync.h"

#include "coop/text/novelty_ledger.h"
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

// The client's applied range: a contiguous range, not a high-water mark. The join seed delivers
// rows older than anything a client may already hold, and apply-iff-newer-than-the-highest
// would have discarded the entire seed, an empty history with nothing logged. It lives next to
// the apply that reads it and is cleared by the same Reset as the record; a free-floating
// watermark is how a reset gets forgotten.
bool     g_haveRange = false;
uint32_t g_rangeLo = 0;
uint32_t g_rangeHi = 0;

// Set once a gap proves the applied set is not one interval, after which the range can no
// longer describe the truth and dedup is abandoned rather than allowed to lie (see
// OnChatLine). Cleared by the same Reset.
bool     g_rangeBroken = false;

// The speaker binding table on the client. A speaker message always immediately precedes its
// line on the same ordered lane, so this only has to stay valid across two consecutive
// messages. The speaker id is a per-burst index: a live line uses 0, a seed burst numbers its
// distinct speakers. No minting or eviction policy, since a later burst simply overwrites.
constexpr int kMaxSpeakers = 16;
struct Speaker {
    bool        valid = false;
    uint8_t     slot = 0;
    uint32_t    nickArgb = 0;
    std::string nick;
};
Speaker g_speakers[kMaxSpeakers];

// Which slots the host has already seeded. A slot receives live rows only after its seed has
// been sent, which is what makes the client's applied range a single interval that only grows
// upward: otherwise a line authored between a slot's world-ready and its seed reaches it
// first, the seed then delivers rows below everything applied, and a contiguous range cannot
// express the hole. Relying on the pre-world send gate for this would break the moment a
// chat line became pre-world sendable.
bool g_seeded[coop::net::kMaxPeers] = {};

// Trim is the only genuinely local part; the sanitising and the byte cap are coop/text's.
std::string TrimAndCap(const std::string& in) {
    size_t b = 0, e = in.size();
    while (b < e && (in[b] == ' ' || in[b] == '\t' || in[b] == '\r' || in[b] == '\n')) ++b;
    while (e > b && (in[e-1] == ' ' || in[e-1] == '\t' || in[e-1] == '\r' || in[e-1] == '\n')) --e;
    return coop::text::CapUtf8Bytes(in.substr(b, e - b),
                                    sizeof(coop::net::ChatMessagePayload{}.text));
}

// The receive boundary: well-formedness is established where we read, not where we wrote, and
// the boundary decodes strictly and refuses a whole ill-formed field rather than repairing it.
// Chat text is the one attacker-controlled string in the process, and a repaired line is a
// sentence nobody typed; the whole message is dropped and the refusal logged, so a drill can
// see it. Two admission questions, one funnel: well-formedness, and the novelty budget, since
// every codepoint in the repertoire is rasterised on demand from exactly this string, and a
// few hundred bytes of deliberately diverse text force thousands of rasterisations on every
// receiving peer inside one frame; the ledger caps how fast a peer may widen the alphabet,
// with the same refusal shape. At the boundary and not in a draw loop: three surfaces
// rasterise remote text (the feed rows, the overhead bubble, the scoreboard's nicks), so a
// draw-time cap would be a site list that misses two.
bool Admissible(uint8_t authorSlot, const char* p, size_t n) {
    std::wstring decoded;
    if (!coop::text::FromUtf8Strict(p, n, &decoded)) return false;
    return coop::text::AdmitRemoteText(authorSlot, decoded);
}

bool IsHost() {
    auto* s = g_session.load(std::memory_order_acquire);
    return s && s->role() == coop::net::Role::Host;
}

// Render one committed row into this peer's feed; seeded rows land retained.
void ApplyRow(uint8_t slot, const std::string& nick, uint32_t custom,
              const std::string& text, uint32_t lineSeq, bool seeded) {
    // The receiver resolves the colour, once, here: the wire carries the speaker's custom pick (or
    // 0 for none) and the per-slot fallback palette stays render-side. Then it is frozen onto the
    // line, since old history is a frozen history.
    const uint32_t argb = coop::nick_color::IsCustom(custom)
        ? custom
        : coop::chat_nick_color::kSlotCols[slot % 8u];
    const std::string line = nick + ": " + text;
    coop::chat_feed::PushWireChat(line,
                                  static_cast<uint8_t>(nick.size() > 255 ? 255 : nick.size()),
                                  argb, lineSeq, seeded);
    // The overhead bubble is a live-world effect. A seeded row must never reach it, structurally
    // rather than by a flag at the far end: replaying a joiner's whole history through it would
    // put bubbles over peers for conversations that happened before that player existed.
    if (!seeded) coop::chat_bubbles::OnChatLine(slot, text.c_str());
    // The lane's only order observable: a drill cannot read a sort key off a screenshot, and "the
    // lines appeared" is not "they appeared in the order the lobby said them", which is the half a
    // seed interleaving with live traffic breaks. One line per applied row.
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

// Host: commit `text` as spoken by `slot`, broadcast it, and render it locally.
void AuthorAndBroadcast(uint8_t slot, const std::string& text) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running()) return;

    // The nick is resolved once, here, and travels with the row forever: resolving at render time
    // on each peer answers who is in that slot now, and slots recycle, so a resident and a joiner
    // would hold different names for the same message.
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
    // Chat exists for the whole coop session, not just while a peer link is up: a hosting session
    // with zero clients is not connected, yet a running host session is a live lobby, and typing
    // while alone is legitimate (the line shows locally; joiners were not there for it). A client
    // is in a session only while its link is connected.
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running()) return false;
    return s->role() == coop::net::Role::Host || s->connected();
}

void QueueSend(const std::string& utf8Text) {
    const std::string text = TrimAndCap(utf8Text);
    if (text.empty()) return;
    // Hop to the game thread: the send, the record and the feed are all game-thread paths, and
    // the input bar submits on the render thread.
    GT::Post([text] {
        auto* s = g_session.load(std::memory_order_acquire);
        if (!s || !s->running()) return;  // session died between type + send
        if (s->role() == coop::net::Role::Host) {
            // The host is the authority and commits its own line immediately: a host alone in its
            // lobby has nobody to send to, and the line still belongs in the record so the next
            // joiner is seeded with it.
            AuthorAndBroadcast(0, text);
            return;
        }
        // A client sends an intent and waits for the host's authored row. No local echo: the row it
        // will receive is the one with a position in the order, and a second copy now would need
        // reconciling later.
        coop::net::ChatMessagePayload p{};
        p.len = static_cast<uint8_t>(text.size());
        std::memcpy(p.text, text.data(), text.size());
        // The dev injection, the must-fail control for the receive boundary: a validator only ever
        // shown passing passes by construction, and the codec selftest proves the strict decode
        // refuses, but nothing else proves chat calls it. A lone continuation byte is appended
        // rather than overwriting one, since overwriting is not reliably corrupting (the last byte
        // of a Cyrillic message is the tail of a two-byte sequence); a trailing continuation byte
        // is ill-formed unconditionally.
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
        // Nothing sends a chat message to a client; reaching here means a peer speaks a retired
        // protocol.
        UE_LOGW("chat: a ChatMessage arrived on a CLIENT from slot %u -- chat is "
                "host-authored; dropping",
                static_cast<unsigned>(senderPeerSlot));
        return;
    }
    uint8_t n = payload.len;
    if (n == 0) return;
    if (n > sizeof(payload.text)) n = sizeof(payload.text);
    // Decoded before anything renders it or enters it into the lobby's permanent record: an
    // ill-formed field is refused whole (see Admissible). The gate matters more under host
    // authoring than under the relay, since an ill-formed line committed here would be re-emitted
    // to every future joiner for the life of the lobby.
    if (!Admissible(senderPeerSlot, payload.text, n)) {
        UE_LOGW("chat: refused a message from slot %u (%u byte(s)) -- ill-formed UTF-8 "
                "or past the novelty budget", static_cast<unsigned>(senderPeerSlot),
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
    if (!Admissible(payload.slot, payload.nick, n)) {
        UE_LOGW("chat: refused a speaker nick (%u byte(s)) -- ill-formed or past budget",
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
    if (!Admissible(g_speakers[payload.speakerId].slot, payload.text, n)) {
        UE_LOGW("chat: refused authored line %u (%u byte(s)) -- ill-formed or past budget",
                payload.lineSeq, static_cast<unsigned>(n));
        return;
    }

    const uint32_t seq = payload.lineSeq;
    if (g_haveRange && !g_rangeBroken && seq >= g_rangeLo && seq <= g_rangeHi)
        return;  // already applied
    if (g_haveRange && !g_rangeBroken && seq != g_rangeHi + 1 && seq != g_rangeLo - 1) {
        // A gap. The premise is that this cannot happen: the chat line stays off the pre-world
        // sendable set, the seed is gated and idempotent, and chat sequence numbers in the record
        // are consecutive (only chat appends mint one, and eviction is from the front). This is the
        // behaviour when the premise breaks, and the choice is deliberate: a range cannot express a
        // hole, so dedup stops pretending and everything is applied. Duplicates are visible and
        // recoverable; swallowed rows are neither. Widening the range across the gap would reject
        // every row inside it as already applied, and a joiner would silently lose its history.
        g_rangeBroken = true;
        UE_LOGE("chat: applied-range GAP -- line %u against [%u,%u]. A contiguous range "
                "cannot express this, so dedup is now OFF for this session: later rows "
                "may repeat, but none will be silently dropped",
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
    // Set before the empty-record early return: an empty lobby still has to start receiving live
    // rows, and a gate that only opens when there was history to send stays shut for the first
    // conversation.
    g_seeded[slot] = true;
    // The dev injection, the must-fail control for the join seed: the slot is opened for live
    // traffic but the history is never sent, precisely the empty-history-with-no-error failure the
    // contiguous range was introduced to prevent.
    if (coop::config::ReadEnv("VOTVCOOP_CHAT_SEED_SUPPRESS") == "1") {
        UE_LOGW("chat: [dev] connect-seed SUPPRESSED for slot %d (%d line(s) withheld)",
                slot, coop::chat_log::Count());
        return;
    }
    if (coop::chat_log::Count() <= 0) {
        UE_LOGI("chat: connect-seed -- no history yet; slot %d is now live", slot);
        return;
    }

    // The speaker bindings are deduped within this burst: the same handful of people said most of
    // it, and re-sending a binding per line would triple the seed. Across bursts nothing is
    // remembered; a global last-binding-sent is exactly what strands a joiner who never saw the
    // earlier one.
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
                // More distinct speakers than the burst can index: re-bind from 0. Correctness over
                // compactness; the row still renders with the right name at the cost of another
                // binding.
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
    // A slot that turns over must be re-seeded before it hears anything live: the next occupant's
    // applied range starts empty, and live rows arriving before its seed would put the seed
    // underneath them.
    if (slot >= 0 && slot < static_cast<int>(coop::net::kMaxPeers)) g_seeded[slot] = false;
}

void Reset() {
    coop::chat_log::Reset();
    for (bool& b : g_seeded) b = false;
    g_haveRange = false;
    g_rangeLo = g_rangeHi = 0;
    g_rangeBroken = false;
    for (Speaker& sp : g_speakers) { sp.valid = false; sp.nick.clear(); }
}

void OnDisconnect() {
    g_session.store(nullptr, std::memory_order_release);
}

}  // namespace coop::chat_sync
