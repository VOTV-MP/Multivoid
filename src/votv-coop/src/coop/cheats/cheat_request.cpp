// coop/cheats/cheat_request.cpp -- see coop/cheats/cheat_request.h.

#include "coop/cheats/cheat_request.h"

#include "coop/cheats/cheat_limits.h"
#include "coop/cheats/cheat_run.h"
#include "coop/comms/chat_feed.h"
#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/dev/dev_gate.h"
#include "coop/net/intent_bucket.h"
#include "coop/net/net_clock.h"
#include "coop/net/session.h"
#include "coop/permissions/grants_core.h"
#include "coop/permissions/permission_host.h"
#include "coop/player/players_registry.h"
#include "coop/player/roster_ledger.h"
#include "coop/session/local_grants.h"
#include "coop/session/net_pump.h"
#include "coop/text/utf8_codec.h"
#include "l10n/l10n.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/hot_path_guard.h"
#include "ue_wrap/core/log.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace coop::cheats {
namespace {

namespace GT = ue_wrap::game_thread;
using coop::net::CheatOp;
using coop::net::CheatReason;

constexpr size_t kReasons = static_cast<size_t>(CheatReason::Count);

constexpr uint64_t kSayEveryMs = 10000;

std::atomic<coop::net::Session*> g_session{nullptr};
coop::net::Session* LoadSession() { return g_session.load(std::memory_order_acquire); }

std::atomic<uint32_t> g_nextId{0};
void (*g_observer)(const coop::net::CheatResultPayload&) = nullptr;

// CLIENT: the request the status line waits on, and this machine's mirror of the host's bucket (game thread), which
// publishes when its next request is allowed so the menu, on the render thread, can offer none before then.
std::atomic<uint32_t> g_pendingId{0};
coop::net::IntentBucket g_mirror;
std::atomic<uint64_t> g_fullAtMs{0};   // when the mirror is full again

std::mutex g_statusMutex;
Status g_status;   // guarded by g_statusMutex

void SetStatus(const Status& s) {
    std::lock_guard<std::mutex> lock(g_statusMutex);
    g_status = s;
}

// HOST, game thread: per-sender state.
coop::roster_ledger::PerSlotState<coop::net::IntentBucket> g_bucket;
coop::roster_ledger::PerSlotState<uint64_t> g_nextSayMs;
uint32_t g_answered[coop::players::kMaxPeers][kReasons] = {};

const char* OpName(CheatOp op) {
    return op == CheatOp::SpawnItem ? "spawn" : op == CheatOp::Points ? "points" : op == CheatOp::Event ? "event" : "?";
}

const char* ReasonName(CheatReason r) {
    switch (r) {
        case CheatReason::Ok: return "ok";
        case CheatReason::Off: return "refused (the setting is off)";
        case CheatReason::NotReady: return "refused (not ready)";
        case CheatReason::Rate: return "refused (rate)";
        case CheatReason::BadRequest: return "refused (bad request)";
        case CheatReason::UnknownItem: return "refused (unknown item)";
        case CheatReason::UnknownEvent: return "refused (unknown event)";
        case CheatReason::Refused: return "refused (not offered to players)";
        case CheatReason::NoTarget: return "refused (no such player)";
        case CheatReason::Failed: return "failed";
        default: return "?";
    }
}

std::string Describe(const Request& r) {
    char buf[160];
    switch (r.op) {
        case CheatOp::SpawnItem:
            if (r.target == coop::net::kCheatInFront)
                std::snprintf(buf, sizeof(buf), "spawn %d x '%s' in front of the player", r.count, r.name.c_str());
            else
                std::snprintf(buf, sizeof(buf), "spawn %d x '%s' at slot %d", r.count, r.name.c_str(), r.target);
            break;
        case CheatOp::Points: std::snprintf(buf, sizeof(buf), "points %+d", r.amount); break;
        case CheatOp::Event:  std::snprintf(buf, sizeof(buf), "event '%s'", r.name.c_str()); break;
        default: std::snprintf(buf, sizeof(buf), "unknown op %d", static_cast<int>(r.op)); break;
    }
    return buf;
}

// ---- client ---------------------------------------------------------------------------------------------------

bool SendRequest(coop::net::Session& s, const Request& r, uint32_t* idOut) {
    coop::net::CheatRequestPayload p{};
    p.id = g_nextId.fetch_add(1) + 1;
    *idOut = p.id;
    p.amount = r.amount;
    p.op = static_cast<uint8_t>(r.op);
    p.count = static_cast<uint8_t>(r.count < 0 ? 0 : r.count > 255 ? 255 : r.count);
    p.target = static_cast<uint8_t>(r.target);
    const size_t n = r.name.size() < coop::net::kCheatNameMax - 1 ? r.name.size() : coop::net::kCheatNameMax - 1;
    std::memcpy(p.name, r.name.data(), n);
    return s.SendReliableToSlot(0, coop::net::ReliableKind::CheatRequest, &p, sizeof(p));
}

// A client's own notice of a refusal, in the feed: at most once a few seconds per reason, so a held button is one line.
void NoticeToFeed(CheatReason r) {
    static uint64_t sShownAt[kReasons] = {};
    const size_t i = static_cast<size_t>(r);
    if (i >= kReasons || r == CheatReason::Ok) return;
    const uint64_t now = ::GetTickCount64();
    if (sShownAt[i] != 0 && now - sShownAt[i] < 3000) return;
    sShownAt[i] = now;
    Status s;
    s.valid = true;
    s.reason = r;
    coop::chat_feed::Push(StatusLine(s), coop::chat_feed::Keep::Transient);
}

// What a request draws from the bucket, on the host's judge and on a client's mirror alike. The red arm (dev row
// cheat_count_cost_off) charges one token whatever the count, so a request for props costs what a press does.
float CostOf(CheatOp op, int count) {
    static const bool flat = coop::config::ResolveFlag(coop::config_registry::rows::cheat_count_cost_off);
    return flat ? 1.0f : coop::cheats::RequestCost(op, count);
}

// The route: the host and a solo game run it, a client asks the host.
void Route(Request r) {
    GT::Post([r = std::move(r)] {
        coop::net::Session* s = LoadSession();
        if (s && s->running() && s->role() == coop::net::Role::Client) {
            Status st;
            st.valid = true;
            st.op = r.op;
            // The menu offers nothing before this machine's world is announced to the host or while its mirror of the
            // host's bucket is empty (CanAsk). A call that arrives anyway is told no and sends nothing: the host would
            // drop it unanswered.
            const uint64_t now = coop::net::NowMs();
            const bool settled = coop::net_pump::IsWorldSettled();
            if (!settled || !g_mirror.TakeN(coop::cheats::kMirrorBudget, now, CostOf(r.op, r.count))) {
                st.reason = settled ? CheatReason::Rate : CheatReason::NotReady;
                SetStatus(st);
                UE_LOGW("cheats: the request for %s was not sent (%s)", Describe(r).c_str(), ReasonName(st.reason));
                return;
            }
            g_fullAtMs.store(g_mirror.FullAtMs(coop::cheats::kMirrorBudget, now), std::memory_order_release);
            st.pending = true;
            uint32_t id = 0;
            if (SendRequest(*s, r, &id)) {
                g_pendingId.store(id, std::memory_order_release);
                SetStatus(st);
                UE_LOGI("cheats: asked the host for %s", Describe(r).c_str());
            } else {
                st.pending = false;
                st.reason = CheatReason::Failed;
                SetStatus(st);
                UE_LOGW("cheats: the request for %s was not sent", Describe(r).c_str());
            }
            return;
        }
        const Outcome o = Run(r, 0);
        Status st;
        st.valid = true;
        st.op = r.op;
        st.reason = o.reason;
        st.done = o.done;
        SetStatus(st);
        UE_LOGI("cheats: the host's own %s -> %s", Describe(r).c_str(), ReasonName(o.reason));
    });
}

// ---- host -----------------------------------------------------------------------------------------------------

void Answer(coop::net::Session& s, uint8_t slot, const coop::net::CheatRequestPayload& p, const Outcome& o) {
    const size_t i = static_cast<size_t>(o.reason);
    if (slot < coop::players::kMaxPeers && i < kReasons) ++g_answered[slot][i];
    coop::net::CheatResultPayload rp{};
    rp.id = p.id;
    rp.op = p.op;
    rp.reason = static_cast<uint8_t>(o.reason);
    rp.done = static_cast<uint16_t>(o.done < 0 ? 0 : o.done);
    s.SendReliableToSlot(slot, coop::net::ReliableKind::CheatResult, &rp, sizeof(rp));
}

// What the host says in its own chat of a request it ran, so a cheat done in its world is never silent.
void TellHost(const std::string& nick, const Request& r, int done) {
    char line[256];
    switch (r.op) {
        case CheatOp::SpawnItem:
            l10n::Fmt(line, sizeof(line), l10n::T("%1$s spawned %2$d x %3$s"), nick.c_str(), done, r.name.c_str());
            break;
        case CheatOp::Points:
            if (r.amount >= 0)
                l10n::Fmt(line, sizeof(line), l10n::T("%1$s added %2$d points"), nick.c_str(), r.amount);
            else
                l10n::Fmt(line, sizeof(line), l10n::T("%1$s removed %2$d points"), nick.c_str(), -r.amount);
            break;
        case CheatOp::Event:
            l10n::Fmt(line, sizeof(line), l10n::T("%1$s triggered the event %2$s"), nick.c_str(), r.name.c_str());
            break;
        default: return;
    }
    coop::chat_feed::Push(std::string(line), coop::chat_feed::Keep::Transient);
}

// Whether the host lets this sender use cheats: its master setting, then the per-player right. The id is proved the way
// the remover derives it; an id that is not proved holds no right.
bool SenderMayCheat(const coop::net::Session& s, uint8_t slot) {
    static const bool nodeCheckOff = coop::config::ResolveFlag(coop::config_registry::rows::cheat_node_check_off);
    if (!PlayersMayCheat()) return false;
    if (nodeCheckOff) return true;   // the red arm: the right is not asked
    const std::string guid = s.ProvedGuidForSlotWithToken(slot, coop::roster_ledger::Get(slot).bornGeneration);
    if (guid.empty()) return false;
    const coop::permissions::grants::Entry& right =
        coop::permissions::grants::kProjected[static_cast<size_t>(coop::permissions::grants::Projected::Cheats)];
    return coop::permissions::host::Allows(guid, right.node, right.defaultGranted, false);
}

bool PrintableAscii(const char* s, size_t n) {
    for (size_t i = 0; i < n; ++i)
        if (static_cast<unsigned char>(s[i]) < 0x20 || static_cast<unsigned char>(s[i]) > 0x7E) return false;
    return true;
}

}  // namespace

void Install(coop::net::Session* session) { g_session.store(session, std::memory_order_release); }

bool PlayersMayCheat() { return coop::config::ResolveFlag(coop::config_registry::rows::net_allow_player_cheats); }

ClientRights ReadClientRights() {
    ClientRights r;
    r.hostAllows = PlayersMayCheat();
    r.mine = coop::session::local_grants::Has(coop::permissions::grants::Projected::Cheats);
    return r;
}

void SpawnItem(const std::string& name, int count, int target) {
    Request r;
    r.op = CheatOp::SpawnItem;
    r.name = name;
    r.count = count;
    r.target = target;
    Route(std::move(r));
}

void AddPoints(int amount) {
    Request r;
    r.op = CheatOp::Points;
    r.amount = amount;
    Route(std::move(r));
}

void FireEvent(const std::string& name) {
    Request r;
    r.op = CheatOp::Event;
    r.name = name;
    Route(std::move(r));
}

Status LastStatus() {
    std::lock_guard<std::mutex> lock(g_statusMutex);
    return g_status;
}

std::string StatusLine(const Status& s) {
    if (!s.valid) return std::string();
    if (s.pending) return l10n::T("Asked the host...");
    char buf[128];
    switch (s.reason) {
        case CheatReason::Ok:
            if (s.op == CheatOp::SpawnItem) {
                l10n::Fmt(buf, sizeof(buf), l10n::T("Spawned %d."), s.done);
                return buf;
            }
            // An event's fire is posted to the game: "Started" is what the host knows, not that it finished.
            return s.op == CheatOp::Event ? l10n::T("Started.") : l10n::T("Done.");
        case CheatReason::Off: return l10n::T("The host has not allowed cheats.");
        case CheatReason::NotReady: return l10n::T("Not ready yet -- try again in a moment.");
        case CheatReason::Rate: return l10n::T("Too fast -- wait a moment.");
        case CheatReason::BadRequest: return l10n::T("The host refused that request.");
        case CheatReason::UnknownItem: return l10n::T("The host's game cannot spawn that item.");
        case CheatReason::UnknownEvent: return l10n::T("The host's game has no such event.");
        case CheatReason::Refused: return l10n::T("The host does not offer that to players.");
        case CheatReason::NoTarget: return l10n::T("That player is not here.");
        case CheatReason::Failed: return l10n::T("The game refused it.");
        default: return std::string();
    }
}

void OnRequest(const coop::net::CheatRequestPayload& p, uint8_t senderSlot) {
    UE_ASSERT_GAME_THREAD("cheat_request::OnRequest");
    coop::net::Session* s = LoadSession();
    if (!s || s->role() != coop::net::Role::Host) {
        UE_LOGW("cheats: a CheatRequest reached a peer that is not hosting -- dropped");
        return;
    }
    if (senderSlot < 1 || senderSlot >= coop::players::kMaxPeers) {
        UE_LOGW("cheats: a CheatRequest from sender slot %u -- dropped", static_cast<unsigned>(senderSlot));
        return;
    }
    // A sender whose world is not announced is dropped without work and without an answer: CheatResult is not a kind
    // the host sends before a slot is world-ready, so the answer could not reach it, and a client's menu offers no
    // request before its world is up (CanAsk). The receive edge bounds what a client may send at all.
    if (!s->IsSlotWorldReady(senderSlot)) {
        UE_LOGI("cheats: slot %u sent a request before its world was announced -- dropped",
                static_cast<unsigned>(senderSlot));
        return;
    }
    // What a request costs the sender, in the order the host learns what it is: every request, a refused one too, draws
    // ONE token before anything else is read, so a flood of requests the host refuses is bounded by the bucket like a
    // flood it accepts. Then the shape (a name that is text and ends inside its field, an op the host knows, a count the
    // wire allows), then the sender's rights, then the rest of the cost (a request for props, one token per prop; the
    // first is already paid), then the cheat. A request over the bucket at either draw is dropped, answered Rate and
    // logged once per ten seconds per sender; every other answer rides the tokens the bucket allows.
    const uint64_t now = coop::net::NowMs();
    const float cost = CostOf(static_cast<CheatOp>(p.op), p.count);
    const auto overBudget = [&] {
        if (now < g_nextSayMs[senderSlot]) return;
        g_nextSayMs[senderSlot] = now + kSayEveryMs;
        UE_LOGI("cheats: slot %u is over its request budget; requests dropped", static_cast<unsigned>(senderSlot));
        Outcome rate;
        rate.reason = CheatReason::Rate;
        Answer(*s, senderSlot, p, rate);   // told once per window, then dropped without an answer
    };
    if (!g_bucket[senderSlot].TakeN(coop::cheats::kRequestBudget, now, 1.0f)) {
        overBudget();
        return;
    }

    Request r;
    Outcome o;
    const size_t nameLen = strnlen(p.name, sizeof(p.name));
    const bool shapeOk = nameLen < sizeof(p.name) && PrintableAscii(p.name, nameLen) &&
                         p.op >= static_cast<uint8_t>(CheatOp::SpawnItem) && p.op <= static_cast<uint8_t>(CheatOp::Event) &&
                         (p.op != static_cast<uint8_t>(CheatOp::SpawnItem) ||
                          (p.count >= 1 && p.count <= coop::net::kCheatMaxCount));
    const auto nick = [&] { return coop::text::ToUtf8(coop::roster_ledger::DisplayName(senderSlot)); };
    if (!shapeOk) {
        o.reason = CheatReason::BadRequest;
        UE_LOGI("cheats: slot %u (%s) sent a malformed request (op %u, count %u) -> %s",
                static_cast<unsigned>(senderSlot), nick().c_str(), static_cast<unsigned>(p.op),
                static_cast<unsigned>(p.count), ReasonName(o.reason));
    } else {
        r.op = static_cast<CheatOp>(p.op);
        r.name.assign(p.name, nameLen);
        r.count = p.count;
        r.target = p.target;
        r.amount = p.amount;
        if (!SenderMayCheat(*s, senderSlot)) {
            o.reason = CheatReason::Off;
        } else if (cost > 1.0f && !g_bucket[senderSlot].TakeN(coop::cheats::kRequestBudget, now, cost - 1.0f)) {
            overBudget();
            return;
        } else {
            o = Run(r, senderSlot);
        }
        UE_LOGI("cheats: slot %u (%s) asked for %s -> %s", static_cast<unsigned>(senderSlot), nick().c_str(),
                Describe(r).c_str(), ReasonName(o.reason));
    }
    Answer(*s, senderSlot, p, o);
    if (o.reason == CheatReason::Ok) TellHost(nick(), r, o.done);
}

void OnResult(const coop::net::CheatResultPayload& p) {
    UE_ASSERT_GAME_THREAD("cheat_request::OnResult");
    if (p.reason >= kReasons) return;
    Status st;
    st.valid = true;
    st.op = static_cast<CheatOp>(p.op);
    st.reason = static_cast<CheatReason>(p.reason);
    st.done = p.done;
    // The status line belongs to the request last sent: a late answer to an earlier one neither replaces it nor ends
    // its "asked the host". Every answer is still logged, noticed and shown to the observer.
    if (p.id == g_pendingId.load(std::memory_order_acquire)) SetStatus(st);
    UE_LOGI("cheats: the host answered %s: %s (request %u)", OpName(st.op), ReasonName(st.reason),
            static_cast<unsigned>(p.id));
    NoticeToFeed(st.reason);
    if (g_observer) g_observer(p);
}

bool CanAsk(int count) {
    if (coop::dev_gate::Allowed()) return true;   // the host, or a solo game: the cheat runs at once
    if (!coop::net_pump::IsWorldSettled()) return false;
    // The mirror holds the burst less what is still owed: it is full at g_fullAtMs and refills linearly until then.
    const coop::net::IntentBudget& b = coop::cheats::kMirrorBudget;
    const uint64_t now = coop::net::NowMs();
    const uint64_t fullAt = g_fullAtMs.load(std::memory_order_acquire);
    const float owed = fullAt > now ? static_cast<float>(fullAt - now) * b.perSecond / 1000.0f : 0.0f;
    return b.burst - owed >= CostOf(CheatOp::SpawnItem, count);
}

void ChargeMirrorForDrill(float tokens) {
    const uint64_t now = coop::net::NowMs();
    g_mirror.TakeN(coop::cheats::kMirrorBudget, now, tokens);
    g_fullAtMs.store(g_mirror.FullAtMs(coop::cheats::kMirrorBudget, now), std::memory_order_release);
}

void SetResultObserver(void (*fn)(const coop::net::CheatResultPayload& p)) { g_observer = fn; }

uint32_t AnsweredFor(int slot, CheatReason reason) {
    const size_t i = static_cast<size_t>(reason);
    return slot >= 1 && slot < coop::players::kMaxPeers && i < kReasons ? g_answered[slot][i] : 0;
}

// HOST: when the bucket of the sender in `slot` is full again, in net::NowMs's clock (now when it is). The drill waits on it
// before an arm that needs the whole bucket. Game thread.
uint64_t RequestBucketFullAtMs(int slot) {
    if (slot < 1 || slot >= coop::players::kMaxPeers) return 0;
    return g_bucket[slot].FullAtMs(coop::cheats::kRequestBudget, coop::net::NowMs());
}

void OnDisconnect() {
    SetStatus(Status{});
    g_observer = nullptr;
    g_pendingId.store(0, std::memory_order_release);
    g_mirror.Reset();
    g_fullAtMs.store(0, std::memory_order_release);
}

}  // namespace coop::cheats
