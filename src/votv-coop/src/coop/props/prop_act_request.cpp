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

#include <algorithm>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace coop::prop_act_request {
namespace {

namespace E  = ue_wrap::engine;
namespace PU = ue_wrap::prop_use;
using coop::net::ReliableKind;

// A reply that has not come in this long closes the record: an honest wait ends in milliseconds, so the bound covers
// only a transport that refused the reply. MTA's client has no timer: it refuses a new entry until the server's word
// arrives (CClientPed.cpp:6558, cleared at CPacketHandler.cpp:1796).
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

// A request whose wait ran out, kept until the host answers it, so a grant that still arrives is answered with an abort
// and the host's reservation is always ended.
struct Expired {
    uint16_t id = 0;
    uint32_t eid = 0;
};
std::vector<Expired> g_expired;

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

// A commit the transport refused stays at the head and is sent again, so the host's reservation is always ended. The
// order of the queue is the order the host must see (the Bulk lane pairs by it).
struct QueuedCommit {
    coop::net::PropActCommitPayload payload;
    bool refusedLogged = false;
};
std::deque<QueuedCommit> g_commits;

void DrainCommits() {
    if (!g_session) return;
    while (!g_commits.empty()) {
        QueuedCommit& head = g_commits.front();
        if (!g_session->SendReliableToSlot(0, ReliableKind::PropActCommit, &head.payload, sizeof(head.payload))) {
            if (!head.refusedLogged) {
                head.refusedLogged = true;
                UE_LOGW("prop_act: commit id %u refused by the transport -- kept, sent again",
                        static_cast<unsigned>(head.payload.id));
            }
            return;
        }
        g_commits.pop_front();
    }
}

void QueueCommit(const coop::net::PropActCommitPayload& c) {
    g_commits.push_back(QueuedCommit{c, false});
    DrainCommits();
}

void SendCommit(uint8_t committed) {
    QueueCommit(coop::net::PropActCommitPayload{g_rec.id, committed, 0, g_rec.eid});
    UE_LOGI("prop_act: commit id %u eid %u -> %s", static_cast<unsigned>(g_rec.id), static_cast<unsigned>(g_rec.eid),
            committed ? "committed" : "aborted");
}

// The record's verb, as the player's press runs it, on the target. False when the verb could not run at all (a
// function that does not resolve, a player or target that is not live), as distinct from a verb that ran and bailed.
bool RunVerb(void* target, void* local) {
    switch (g_rec.row) {
    case kPocket: {
        bool taken = false;
        return E::CallPutObjectInventory2(local, target, g_rec.noNotify, taken);
    }
    case kHold: {
        bool collected = false;
        return E::CallMainPlayerHoldObject(local, target, collected);
    }
    case kFood:
    case kMre:
    case kPills:
        return PU::CallActionOptionIndex(target, local, g_rec.hitComponent.Get(), g_rec.hitLocation, g_rec.action,
                                         g_rec.lookAt.Get());
    case kSoap: {
        // The CURRENT hand view: the sponge the player holds now, not the one it held when it pressed.
        E::MainPlayerGrabState g{};
        if (!E::ReadMainPlayerGrabState(local, g)) return false;
        return PU::CallPlayerUsedOn(target, local, g_rec.hitComponent.Get(), g_rec.hitLocation, g_rec.lookAt.Get(),
                                    g.holdingActor, g_rec.holdPropName);
    }
    default:
        return false;
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
    bool ran = false;
    {
        Latched scope(target);
        ran = RunVerb(target, local);
        destroyed = g_latch.destroyed;
    }
    // The destroy decides, not the verb's return: a verb that destroyed its target and then failed still commits, since
    // this copy is gone and its key parked.
    if (destroyed) {
        SendCommit(1);
        coop::remote_prop::UnregisterPropMirror(g_rec.eid);
    } else {
        if (!ran) UE_LOGW("prop_act: the replay of row %u could not run -- aborted", static_cast<unsigned>(g_rec.row));
        SendCommit(0);
    }
    Close();
}

void NoteStale(const coop::net::PropActReplyPayload& p) {
    g_lastStale = StaleReply{p.id, p.granted, p.reason, g_lastStale.count + 1};
    const auto it = std::find_if(g_expired.begin(), g_expired.end(), [&](const Expired& e) { return e.id == p.id; });
    if (it == g_expired.end()) return;
    // The host answered this request after the wait ran out. A grant has reserved the prop, so it is ended with an
    // abort (MTA's client has no late reply to answer: its entry waits for the server's word); a refusal reserved nothing.
    if (p.granted == 1 && p.reason == 0) {
        QueueCommit(coop::net::PropActCommitPayload{it->id, 0, 0, it->eid});
        ++g_lateGrantsAborted;
    }
    g_expired.erase(it);
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
    if (!g_rec.open && g_commits.empty()) return;
    UE_ASSERT_GAME_THREAD("prop_act_request::Tick");
    DrainCommits();
    if (!g_rec.open) return;
    const uint64_t waited = coop::net::NowMs() - g_rec.sentMs;
    if (waited < kReplyWaitMs) return;
    UE_LOGI("prop_act: request id %u eid %u expired after %llu ms", static_cast<unsigned>(g_rec.id),
            static_cast<unsigned>(g_rec.eid), static_cast<unsigned long long>(waited));
    g_expired.push_back(Expired{g_rec.id, g_rec.eid});
    ShowNotAsked();
    Close();
}

void OnReply(const coop::net::PropActReplyPayload& p) {
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
    g_commits.clear();
    g_latch = Latch{};
    g_expired.clear();
    g_lastStale = StaleReply{};
    g_badReasons = 0;
    g_lateGrantsAborted = 0;
    for (size_t i = 0; i < kLineCount; ++i) g_shown[i] = false;
}

}  // namespace coop::prop_act_request
