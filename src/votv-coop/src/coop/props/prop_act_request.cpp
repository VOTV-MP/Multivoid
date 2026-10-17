// coop/props/prop_act_request.cpp -- see coop/props/prop_act_request.h.

#include "coop/props/prop_act_request.h"

#include "coop/comms/chat_feed.h"
#include "coop/net/net_clock.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/props/remote_prop.h"

#include "l10n/l10n.h"

#include "ue_wrap/actors/prop_use.h"
#include "ue_wrap/core/hot_path_guard.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/engine/engine.h"

#include <cstdint>
#include <string>

namespace coop::prop_act_request {
namespace {

namespace E  = ue_wrap::engine;
namespace PU = ue_wrap::prop_use;
using coop::net::ReliableKind;

// A reply that has not come in this long closes the record: an honest wait ends in milliseconds, so the bound covers
// only a transport that refused the reply.
constexpr uint64_t kReplyWaitMs = 10000;

coop::net::Session* g_session = nullptr;
Pending g_rec;

// The replay's apply latch, for the length of one synchronous verb call: the destroy seam asks it whether a destroy is
// the replay's. `target` is a raw pointer compared by `==`: at the seam's POST the actor already carries the engine's
// dying flags, so a CachedObjRef would never match it.
struct Latch {
    void* target = nullptr;
    bool destroyed = false;
};
Latch g_latch;

struct Latched {
    explicit Latched(void* target) { g_latch = Latch{target, false}; }
    ~Latched() { g_latch = Latch{}; }
    Latched(const Latched&) = delete;
    Latched& operator=(const Latched&) = delete;
};

// A request whose wait ran out, remembered so a grant that still arrives is answered with an abort.
struct Expired {
    uint16_t id = 0;   // 0: empty (ids skip 0)
    uint32_t eid = 0;
};
constexpr size_t kExpiredRing = 4;
Expired g_expired[kExpiredRing];
size_t g_expiredNext = 0;

// The last reply that matched no record, and how many there have been.
struct StaleReply {
    uint16_t id = 0;
    uint8_t granted = 0, reason = 0;
    uint32_t count = 0;
};
StaleReply g_lastStale;
uint32_t g_badReasons = 0;
uint32_t g_lateGrantsAborted = 0;

// ---- the lines ------------------------------------------------------------------------------------------------

// A line is shown at most once per kind per window (D1's shape, remover_request.cpp).
enum Line : size_t { kLineGone, kLineWrongFamily, kLineHeld, kLineReach, kLineQueueFull, kLineBadReason,
                     kLineNotAsked, kLineCount };
constexpr uint64_t kShowEveryMs = 5000;
uint64_t g_shownAt[kLineCount] = {};
bool g_shown[kLineCount] = {};

void Show(Line which, const std::string& line) {
    const uint64_t now = coop::net::NowMs();
    if (g_shown[which] && now - g_shownAt[which] < kShowEveryMs) return;
    g_shown[which] = true;
    g_shownAt[which] = now;
    coop::chat_feed::Push(line, coop::chat_feed::Keep::Transient);
}

void ShowRefusal(uint8_t reason) {
    switch (static_cast<coop::net::PropActReason>(reason)) {
    case coop::net::PropActReason::Gone:
        Show(kLineGone, std::string(l10n::T("That item is gone.")));
        return;
    case coop::net::PropActReason::WrongFamily:
        Show(kLineWrongFamily, std::string(l10n::T("You can't do that with this item.")));
        return;
    case coop::net::PropActReason::Held:
        Show(kLineHeld, std::string(l10n::T("Someone else has that item.")));
        return;
    case coop::net::PropActReason::Reach:
        Show(kLineReach, std::string(l10n::T("That item is too far away.")));
        return;
    case coop::net::PropActReason::QueueFull:
        Show(kLineQueueFull, std::string(l10n::T("Too many actions at once, try again.")));
        return;
    default:
        return;
    }
}

void ShowBadReason() { Show(kLineBadReason, std::string(l10n::T("That can't be done right now."))); }
void ShowNotAsked() { Show(kLineNotAsked, std::string(l10n::T("The host could not be asked; try again."))); }

// ---- the record -----------------------------------------------------------------------------------------------

void Close() { g_rec = Pending{}; }

void SendCommit(uint8_t committed) {
    const coop::net::PropActCommitPayload c{g_rec.id, committed, 0, g_rec.eid};
    g_session->SendReliableToSlot(0, ReliableKind::PropActCommit, &c, sizeof(c));
    UE_LOGI("prop_act: commit id %u eid %u -> %s", static_cast<unsigned>(g_rec.id), static_cast<unsigned>(g_rec.eid),
            committed ? "committed" : "aborted");
}

// The record's verb, as the player's press runs it, on the target.
void RunVerb(void* target, void* local) {
    switch (g_rec.row) {
    case kPocket: {
        bool taken = false;
        E::CallPutObjectInventory2(local, target, g_rec.noNotify, taken);
        return;
    }
    case kHold: {
        bool collected = false;
        E::CallMainPlayerHoldObject(local, target, collected);
        return;
    }
    case kFood:
    case kMre:
    case kPills:
        PU::CallActionOptionIndex(target, local, g_rec.hitComponent.Get(), g_rec.hitLocation, g_rec.action,
                                  g_rec.lookAt.Get());
        return;
    case kSoap: {
        // The CURRENT hand view: the sponge the player holds now, not the one it held when it pressed.
        E::MainPlayerGrabState g{};
        E::ReadMainPlayerGrabState(local, g);
        PU::CallPlayerUsedOn(target, local, g_rec.hitComponent.Get(), g_rec.hitLocation, g_rec.lookAt.Get(),
                             g.holdingActor, g_rec.holdPropName);
        return;
    }
    default:
        return;
    }
}

// The granted verb, replayed under the latch. The commit says whether the target died inside the call: the verb can
// bail (no space, full, too heavy, too cold) and leave the prop where it was.
void Replay() {
    void* const target = g_rec.target.Get();
    if (!target) {
        SendCommit(0);
        Close();
        return;
    }
    void* const local = coop::players::Registry::Get().Local();
    bool destroyed = false;
    {
        Latched scope(target);
        RunVerb(target, local);
        destroyed = g_latch.destroyed;
    }
    if (destroyed) {
        SendCommit(1);
        coop::remote_prop::UnregisterPropMirror(g_rec.eid);
    } else {
        SendCommit(0);
    }
    Close();
}

void NoteStale(const coop::net::PropActReplyPayload& p) {
    g_lastStale = StaleReply{p.id, p.granted, p.reason, g_lastStale.count + 1};
    if (p.granted != 1 || p.reason != 0) return;
    for (Expired& e : g_expired) {
        if (e.id == 0 || e.id != p.id) continue;
        const coop::net::PropActCommitPayload late{e.id, 0, 0, e.eid};
        g_session->SendReliableToSlot(0, ReliableKind::PropActCommit, &late, sizeof(late));
        e = Expired{};
        ++g_lateGrantsAborted;
        return;
    }
}

}  // namespace

namespace internal {

ReplyAction DecideReply(bool pending, uint16_t pendingId, const coop::net::PropActReplyPayload& p) {
    if (!pending || p.id != pendingId) return ReplyAction::Stale;
    if (p.granted == 1 && p.reason == 0) return ReplyAction::Granted;
    if (p.granted == 0 && p.reason >= 1 && p.reason <= 5) return ReplyAction::Refused;
    return ReplyAction::BadReason;
}

const char* ReplyActionName(ReplyAction a) {
    switch (a) {
    case ReplyAction::Stale:     return "stale";
    case ReplyAction::Refused:   return "refused";
    case ReplyAction::BadReason: return "bad-reason";
    case ReplyAction::Granted:   return "granted";
    }
    return "";
}

}  // namespace internal

void Install(coop::net::Session* session) { g_session = session; }

void Tick() {
    if (!g_rec.open) return;
    UE_ASSERT_GAME_THREAD("prop_act_request::Tick");
    const uint64_t waited = coop::net::NowMs() - g_rec.sentMs;
    if (waited < kReplyWaitMs) return;
    UE_LOGI("prop_act: request id %u eid %u expired after %llu ms", static_cast<unsigned>(g_rec.id),
            static_cast<unsigned>(g_rec.eid), static_cast<unsigned long long>(waited));
    g_expired[g_expiredNext] = Expired{g_rec.id, g_rec.eid};
    g_expiredNext = (g_expiredNext + 1) % kExpiredRing;
    ShowNotAsked();
    Close();
}

void OnReply(const coop::net::PropActReplyPayload& p) {
    if (!g_session) return;
    UE_ASSERT_GAME_THREAD("prop_act_request::OnReply");
    const internal::ReplyAction action = internal::DecideReply(g_rec.open, g_rec.id, p);
    const uint64_t after = g_rec.open ? coop::net::NowMs() - g_rec.sentMs : 0;
    UE_LOGI("prop_act: reply id %u -> %s after %llu ms", static_cast<unsigned>(p.id), internal::ReplyActionName(action),
            static_cast<unsigned long long>(after));
    switch (action) {
    case internal::ReplyAction::Stale:
        NoteStale(p);
        return;
    case internal::ReplyAction::BadReason:
        ++g_badReasons;
        ShowBadReason();
        Close();
        return;
    case internal::ReplyAction::Refused:
        ShowRefusal(p.reason);
        Close();
        return;
    case internal::ReplyAction::Granted:
        Replay();
        return;
    }
}

bool NoteSeamDestroy(void* actor) {
    if (!g_latch.target || actor != g_latch.target) return false;
    g_latch.destroyed = true;
    return true;
}

void OnDisconnect() {
    Close();
    g_latch = Latch{};
    for (Expired& e : g_expired) e = Expired{};
    g_expiredNext = 0;
    g_lastStale = StaleReply{};
    g_badReasons = 0;
    g_lateGrantsAborted = 0;
    for (size_t i = 0; i < kLineCount; ++i) g_shown[i] = false;
}

}  // namespace coop::prop_act_request
