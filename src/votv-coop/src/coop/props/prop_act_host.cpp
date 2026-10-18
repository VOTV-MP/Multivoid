// coop/props/prop_act_host.cpp -- see coop/props/prop_act_host.h.

#include "coop/props/prop_act_host.h"

#include "coop/creatures/kerfur_entity.h"
#include "coop/element/intent_authority.h"
#include "coop/element/registry.h"
#include "coop/net/intent_queue.h"
#include "coop/net/net_clock.h"
#include "coop/net/protocol.h"
#include "coop/net/refusal_tally.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/roster_ledger.h"
#include "coop/props/prop_hold.h"
#include "coop/props/remote_prop.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/actors/prop_gather.h"
#include "ue_wrap/actors/prop_use.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/hot_path_guard.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>

namespace coop::prop_act_host {
namespace {

namespace R  = ue_wrap::reflection;
namespace EL = coop::element;
using coop::net::PropActFamily;
using coop::net::PropActReason;
using coop::net::ReliableKind;

// Past this many queued requests a sender is refused with QueueFull. Past it for a sender whose world is not ready
// the request is only counted: nothing is sent to a link that cannot take it.
constexpr size_t kCap = 2;
constexpr coop::net::IntentBudget kBudget{4.0f, 2.0f};
// The reach a take or use is allowed beyond the target's bounds and the pose-staleness ball. A lenient bound on the
// sender's last-synced place, not its exact range: the shape of MTA's server cutoff (CGame.cpp:3113).
constexpr float kActReachUU = 200.0f;

struct Queued {
    coop::net::PropActRequestPayload req;
};

constexpr int kWaitCauses = static_cast<int>(WaitCause::kCount);
constexpr int kCommitSteps = 4;

struct Slot {
    coop::net::IntentQueue<Queued> queue;
    uint32_t answered[coop::net::kPropActReasons] = {};
    uint32_t waits[kWaitCauses] = {};
    uint32_t commits[kCommitSteps] = {};
    uint32_t maxDepth = 0;
};

coop::net::Session* g_session = nullptr;
coop::roster_ledger::PerSlotState<Slot> g_slots;
uint32_t g_tally[static_cast<int>(Tally::kCount)] = {};
coop::net::RefusalTally g_refusals{"prop_act"};

bool InRoster(int slot) { return slot >= 1 && slot < coop::net::kMaxPeers; }
bool IsHost() { return g_session && g_session->running() && g_session->role() == coop::net::Role::Host; }
coop::net::RefusalTally::Clock::time_point Now() { return coop::net::RefusalTally::Clock::now(); }

// A class by name, re-resolved when the cached one is no longer alive: a world reload can re-create a class.
void* ResolveClass(ue_wrap::CachedObjRef& ref, const wchar_t* name) {
    if (ref.Alive()) return ref.Raw();
    ref.Set(ue_wrap::object_index::ClassByName(name));
    return ref.Raw();
}

// The use family's classes: the ones whose eat the player is offered and whose destroy this lane carries.
// `foodBody` marks prop_food_C's own row, the one whose `notEdible` the game tests before it eats.
struct UseClass {
    const wchar_t* name;
    bool foodBody;
    ue_wrap::CachedObjRef cls;
};
UseClass g_useClasses[] = {{L"prop_food_C", true, {}}, {L"prop_food_mre_C", false, {}},
                           {L"prop_sleeppills_C", false, {}}};

// Food-body classes that are not offered the use: the plush offers no option (its getActionOptions is empty), and
// the others own their action 4, whose destroy is carried by another request.
constexpr const wchar_t* kUseDenied[] = {L"prop_eriePlush_C", L"prop_food_shrimp_C", L"prop_dingus_C",
                                         L"prop_cookietray_C", L"prop_breadmold_bread_C"};
ue_wrap::CachedObjRef g_useDenied[std::size(kUseDenied)];

// Classes whose act facts could not be read, said once each per session.
std::unordered_set<std::wstring> g_unreadSaid;

void NoteUnread(void* actor) {
    const std::wstring name = R::ToString(R::NameOf(R::ClassOf(actor)));
    if (g_unreadSaid.insert(name).second) UE_LOGW("prop_act: cannot read %ls's act facts -- refusing it", name.c_str());
}

// Reads the facts of the pocket (or hold) verb into the pure traits; `read` says whether they could be read.
void PocketTraits(void* actor, internal::ActTraits& t) {
    ue_wrap::prop_gather::PocketFacts f{};
    t.read = ue_wrap::prop_gather::ReadPocketFacts(actor, f);
    if (!t.read) return;
    t.collectRefused = f.collectRefused;
    t.child = f.child;
    t.frozen = f.frozen;
    t.simulating = f.simulating;
    t.canCollect = f.canCollect;
    t.heavy = f.heavy;
    t.meshRoot = f.meshRoot;
    t.volumeFits = f.volumeFits;
    t.saveClass = f.saveClass;
}

void HoldTraits(void* actor, internal::ActTraits& t) {
    ue_wrap::prop_gather::HoldFacts f{};
    t.read = ue_wrap::prop_gather::ReadHoldFacts(actor, f);
    if (!t.read) return;
    t.pickupRefused = f.pickupRefused;
    t.ignoreSave = f.ignoreSave;
    t.child = f.child;
    t.frozen = f.frozen;
    t.simulating = f.simulating;
    t.canHold = f.canHold;
    t.heavy = f.heavy;
    t.saveClass = f.saveClass;
    t.equipRow = f.equipRow;
}

// The use verb's traits: the class the game offers the eat on, and for food's body whether it is edible. The term that
// refuses the class when it is not offered, none when it is.
std::optional<Tally> UseTraits(void* actor, internal::ActTraits& t) {
    void* const cls = R::ClassOf(actor);
    for (size_t i = 0; i < std::size(kUseDenied); ++i) {
        void* const c = ResolveClass(g_useDenied[i], kUseDenied[i]);
        if (c && ue_wrap::prop::WalksToBase(cls, c)) return Tally::FitDenied;
    }
    const UseClass* row = nullptr;
    for (UseClass& u : g_useClasses) {
        void* const c = ResolveClass(u.cls, u.name);
        if (c && ue_wrap::prop::WalksToBase(cls, c)) {
            row = &u;
            break;
        }
    }
    if (!row) return Tally::FitNoUseRow;
    t.offered = true;
    t.foodBody = row->foodBody;
    t.read = true;
    if (row->foodBody) {
        int32_t uses = 0;
        t.read = ue_wrap::prop_use::ReadFoodUse(actor, uses, t.notEdible);
    }
    return std::nullopt;
}

// Does the act the request names fit `actor`, by the game's own conditions for that verb? The trash lanes own the
// trash actors, so no verb takes them. Nothing is tallied here: `why` gets the first term that refuses, when one is
// named.
bool FitCensus(void* actor, uint8_t family, std::optional<Tally>& why) {
    why.reset();
    if (!internal::KnownFamily(family)) {
        why = Tally::FitBadFamily;
        return false;
    }
    if (ue_wrap::prop::IsTrashActor(actor) || ue_wrap::prop::IsTrashBitsPile(actor)) {
        why = Tally::FitTrash;
        return false;
    }
    internal::ActTraits t{};
    const bool use = family == static_cast<uint8_t>(PropActFamily::Use);
    if (use) {
        why = UseTraits(actor, t);
        if (why) return false;
    } else {
        if (!ue_wrap::prop::IsDescendantOfProp(actor)) {
            why = Tally::FitNotProp;
            return false;
        }
        t.driven = coop::remote_prop::IsActorDrivenByOtherThan(actor, -1);
        if (family == static_cast<uint8_t>(PropActFamily::Pocket)) PocketTraits(actor, t);
        else HoldTraits(actor, t);
    }
    if (!t.read) NoteUnread(actor);
    const bool fits = use ? internal::UseFits(t)
                          : family == static_cast<uint8_t>(PropActFamily::Pocket) ? internal::PocketFits(t)
                                                                                 : internal::HoldFits(t);
    if (fits) return true;
    why = internal::FirstRefusal(family, t);
    return false;
}

// The request's verdict on its act: the first term that refuses is tallied; the reply stays WrongFamily.
bool FamilyFits(void* actor, uint8_t family) {
    std::optional<Tally> why;
    if (FitCensus(actor, family, why)) return true;
    if (why) ++g_tally[static_cast<int>(*why)];
    return false;
}

// The reply goes to the sender alone: MTA broadcasts its entry confirm because every peer animates the entry
// (CGame.cpp:3196-3197), while here nobody else shows anything before the destroy.
bool Answer(uint8_t slot, const coop::net::PropActReplyPayload& reply) {
    const size_t i = reply.granted ? 0 : reply.reason;
    if (i < coop::net::kPropActReasons) ++g_slots[slot].answered[i];
    return g_session->SendReliableToSlot(slot, ReliableKind::PropActReply, &reply, sizeof(reply));
}

// Reads Decide's inputs in Decide's order and stops at the first that decides, so a request that must wait reads only
// what the wait needs.
void ReadInputs(uint8_t slot, const Queued& q, internal::ActInputs& in, void*& actor, float& dist, float& allowed) {
    in.worldReady = g_session->IsSlotWorldReady(slot);
    if (!in.worldReady) return;
    in.holdsReservation = coop::prop_hold::HoldsReservation(slot);
    if (in.holdsReservation) return;
    const auto tok = EL::IntentTarget::ForClientIntent(*g_session, slot, kActReachUU);
    in.hasBody = tok.HasBody();
    if (!in.hasBody) return;
    actor = coop::element::LivePropActor(q.req.eid);
    in.live = actor != nullptr;
    if (!in.live) return;
    in.heldByOther = coop::prop_hold::HeldByOther(actor, slot);
    if (in.heldByOther) return;
    const EL::IntentSubject sub = tok.Authorize(actor);
    in.reach = sub.outcome;
    dist = sub.distUU;
    allowed = sub.reachUU;
    if (in.reach != EL::IntentOutcome::Ok) return;
    in.familyFits = FamilyFits(actor, q.req.family);
}

// One queued request, judged.
coop::net::Served ServeHead(uint8_t slot, const Queued& q) {
    internal::ActInputs in;
    void* actor = nullptr;
    float dist = -1.f, allowed = -1.f;
    ReadInputs(slot, q, in, actor, dist, allowed);

    const internal::ActVerdict v = internal::Decide(in);
    if (v == internal::ActVerdict::Wait) {
        // A request waits where MTA answers or drops at once (a sender not joined, CGame.cpp:3025; one with an action in
        // flight, FAIL_ACTION at :3350): a peer joining mid-activity is never refused for it.
        const WaitCause cause = !in.worldReady ? WaitCause::NotReady
                                : in.holdsReservation ? WaitCause::Reservation : WaitCause::NoBody;
        ++g_slots[slot].waits[static_cast<int>(cause)];
        return coop::net::Served::Wait;
    }
    if (v == internal::ActVerdict::Grant) coop::prop_hold::Reserve(slot, q.req.id, q.req.eid, actor);
    const bool sent = Answer(slot, *internal::ReplyFor(v, q.req.id));
    if (v == internal::ActVerdict::Grant) {
        if (!sent) {
            coop::prop_hold::Release(slot);
            ++g_tally[static_cast<int>(Tally::GrantUnsent)];
            UE_LOGW("prop_act: the grant for slot %u id %u was not sent -- released", static_cast<unsigned>(slot),
                    static_cast<unsigned>(q.req.id));
        }
    } else {
        g_refusals.Note(slot, internal::VerdictName(v), Now());
    }
    UE_LOGI("prop_act: slot %u family %u eid %u -> %s (dist %.0f of %.0f uu)", static_cast<unsigned>(slot),
            static_cast<unsigned>(q.req.family), static_cast<unsigned>(q.req.eid),
            v == internal::ActVerdict::Grant && !sent ? "grant-unsent" : internal::VerdictName(v), dist,
            allowed);
    return coop::net::Served::Done;
}

void LogCommit(uint8_t slot, const coop::net::PropActCommitPayload& p, const char* outcome) {
    UE_LOGI("prop_act: commit slot %u id %u eid %u -> %s", static_cast<unsigned>(slot), static_cast<unsigned>(p.id),
            static_cast<unsigned>(p.eid), outcome);
}

}  // namespace

namespace internal {

ActVerdict Decide(const ActInputs& in) {
    if (!in.worldReady) return ActVerdict::Wait;
    if (in.holdsReservation) return ActVerdict::Wait;
    if (!in.hasBody) return ActVerdict::Wait;
    if (!in.live) return ActVerdict::Gone;
    if (in.heldByOther) return ActVerdict::Held;
    // Authorize answers NoBody for the state HasBody reads, so this is the defence for a body lost between the two.
    if (in.reach == coop::element::IntentOutcome::NoBody) return ActVerdict::Wait;
    if (in.reach != coop::element::IntentOutcome::Ok) return ActVerdict::Reach;
    if (!in.familyFits) return ActVerdict::WrongFamily;
    return ActVerdict::Grant;
}

bool KnownFamily(uint8_t family) {
    return family >= static_cast<uint8_t>(PropActFamily::Pocket) && family <= static_cast<uint8_t>(PropActFamily::Hold);
}

bool PocketFits(const ActTraits& t) {
    return t.read && !t.collectRefused && !t.child && (t.frozen || t.simulating || t.driven) && t.canCollect &&
           !t.heavy && t.meshRoot && t.volumeFits && t.saveClass;
}

bool HoldFits(const ActTraits& t) {
    return t.read && !t.pickupRefused &&
           (t.frozen || ((t.simulating || t.driven) && !t.child && !t.heavy && t.canHold)) && !t.ignoreSave &&
           t.saveClass && t.equipRow;
}

bool UseFits(const ActTraits& t) { return t.read && t.offered && !(t.foodBody && t.notEdible); }

std::optional<Tally> FirstRefusal(uint8_t family, const ActTraits& t) {
    if (!KnownFamily(family)) return Tally::FitBadFamily;
    if (!t.read) return Tally::FitUnread;
    if (family == static_cast<uint8_t>(PropActFamily::Pocket)) {
        if (t.collectRefused) return Tally::FitCollectRefused;
        if (t.child) return Tally::FitChild;
        if (!(t.frozen || t.simulating || t.driven)) return Tally::FitPhysics;
        if (!t.canCollect) return Tally::FitCanCollect;
        if (t.heavy) return Tally::FitHeavy;
        if (!t.meshRoot) return Tally::FitMeshRoot;
        if (!t.volumeFits) return Tally::FitVolume;
        if (!t.saveClass) return Tally::FitSaveClass;
    } else if (family == static_cast<uint8_t>(PropActFamily::Hold)) {
        if (t.pickupRefused) return Tally::FitPickupRefused;
        if (!t.frozen) {
            if (!(t.simulating || t.driven)) return Tally::FitPhysics;
            if (t.child) return Tally::FitChild;
            if (t.heavy) return Tally::FitHeavy;
            if (!t.canHold) return Tally::FitCanHold;
        }
        if (t.ignoreSave) return Tally::FitIgnoreSave;
        if (!t.saveClass) return Tally::FitSaveClass;
        if (!t.equipRow) return Tally::FitEquipRow;
    } else {
        if (!t.offered) return Tally::FitNoUseRow;
        if (t.foodBody && t.notEdible) return Tally::FitNotEdible;
    }
    return std::nullopt;
}

std::optional<coop::net::PropActReplyPayload> ReplyFor(ActVerdict v, uint16_t id) {
    switch (v) {
    case ActVerdict::Grant:       return coop::net::PropActReplyPayload{id, 1, 0};
    case ActVerdict::Gone:        return coop::net::PropActReplyPayload{id, 0, 1};
    case ActVerdict::WrongFamily: return coop::net::PropActReplyPayload{id, 0, 2};
    case ActVerdict::Held:        return coop::net::PropActReplyPayload{id, 0, 3};
    case ActVerdict::Reach:       return coop::net::PropActReplyPayload{id, 0, 4};
    case ActVerdict::Wait:        break;
    }
    return std::nullopt;
}

CommitStep DecideCommit(bool matched, uint8_t committed, bool liveAndSame) {
    if (!matched) return CommitStep::BadCommit;
    if (committed == 0) return CommitStep::Abort;
    if (committed != 1) return CommitStep::BadCommit;
    if (!liveAndSame) return CommitStep::TargetGone;
    return CommitStep::Destroy;
}

AfterStep AfterDestroy(bool destroyed) { return destroyed ? AfterStep::FanOut : AfterStep::DestroyFailed; }

const char* VerdictName(ActVerdict v) {
    switch (v) {
    case ActVerdict::Wait:        return "wait";
    case ActVerdict::Gone:        return "gone";
    case ActVerdict::WrongFamily: return "wrong-family";
    case ActVerdict::Held:        return "held";
    case ActVerdict::Reach:       return "reach";
    case ActVerdict::Grant:       return "grant";
    }
    return "";
}

const char* CommitName(CommitStep s) {
    switch (s) {
    case CommitStep::BadCommit:  return "bad";
    case CommitStep::Abort:      return "abort";
    case CommitStep::TargetGone: return "target-gone";
    case CommitStep::Destroy:    return "destroy";
    }
    return "";
}

}  // namespace internal

void Install(coop::net::Session* session) { g_session = session; }

void Tick() {
    if (!IsHost()) return;
    UE_ASSERT_GAME_THREAD("prop_act_host::Tick");
    for (uint8_t slot = 1; slot < coop::net::kMaxPeers; ++slot) {
        g_slots[slot].queue.Serve(kBudget, coop::net::NowMs,
                                  [&](const Queued& q) { return ServeHead(slot, q); });
    }
    g_refusals.Flush(Now());
}

void OnRequest(const coop::net::PropActRequestPayload& p, uint8_t senderSlot) {
    if (!IsHost()) return;
    UE_ASSERT_GAME_THREAD("prop_act_host::OnRequest");
    if (!InRoster(senderSlot)) return;   // the dispatcher counted it
    Slot& sl = g_slots[senderSlot];
    if (sl.queue.size() >= kCap) {
        if (!g_session->IsSlotWorldReady(senderSlot)) {
            ++g_tally[static_cast<int>(Tally::QueueFullUnready)];
            return;
        }
        Answer(senderSlot, coop::net::PropActReplyPayload{p.id, 0, static_cast<uint8_t>(PropActReason::QueueFull)});
        g_refusals.Note(senderSlot, "queue full", Now());
        return;
    }
    Queued q;
    q.req = p;
    sl.queue.Push(std::move(q), kCap);   // room was checked above
    sl.maxDepth = std::max<uint32_t>(sl.maxDepth, static_cast<uint32_t>(sl.queue.size()));
}

void OnCommit(const coop::net::PropActCommitPayload& p, uint8_t senderSlot) {
    if (!IsHost()) return;
    UE_ASSERT_GAME_THREAD("prop_act_host::OnCommit");
    if (!InRoster(senderSlot)) return;
    void* actor = nullptr;
    const bool matched = coop::prop_hold::Match(senderSlot, p.id, p.eid, &actor);
    const bool liveAndSame = actor && actor == coop::element::LivePropActor(p.eid);
    const internal::CommitStep step = internal::DecideCommit(matched, p.committed, liveAndSame);
    ++g_slots[senderSlot].commits[static_cast<int>(step)];
    if (step == internal::CommitStep::BadCommit) {
        // An unmatched commit owns nothing; a matched one with a bad byte keeps its reservation for the real commit.
        ++g_tally[static_cast<int>(Tally::BadCommit)];
        g_refusals.Note(senderSlot, "bad commit", Now());
        return;
    }
    switch (step) {
    case internal::CommitStep::BadCommit:
        return;
    case internal::CommitStep::Abort:
        coop::prop_hold::Release(senderSlot);
        LogCommit(senderSlot, p, internal::CommitName(step));
        return;
    case internal::CommitStep::TargetGone:
        ++g_tally[static_cast<int>(Tally::TargetGone)];
        coop::prop_hold::Release(senderSlot);
        LogCommit(senderSlot, p, internal::CommitName(step));
        return;
    case internal::CommitStep::Destroy:
        break;
    }
    // The eid alone names the prop (no key), so the key fallback can never pick a sibling.
    ue_wrap::CachedObjRef before;
    before.Set(actor);
    coop::net::PropDestroyPayload dp{};
    dp.elementId = p.eid;
    coop::remote_prop::DestroyJudged(dp, senderSlot);
    const bool destroyed = !before.Is(actor);
    if (internal::AfterDestroy(destroyed) == internal::AfterStep::FanOut) {
        coop::kerfur_entity::ReleaseKerfurForEid(p.eid);
        g_session->SendReliable(ReliableKind::PropDestroy, &dp, sizeof(dp), senderSlot);
    } else {
        ++g_tally[static_cast<int>(Tally::DestroyFailed)];
        UE_LOGE("prop_act: the host could not destroy eid %u for slot %u -- the client holds the item and the prop "
                "stands on every peer", static_cast<unsigned>(p.eid), static_cast<unsigned>(senderSlot));
    }
    coop::prop_hold::Release(senderSlot);
    LogCommit(senderSlot, p, destroyed ? "destroy" : "destroy-failed");
}

// MTA releases a departed ped's vehicle in the element's destructor (CPed.cpp:92-127); the roster's departure is the
// destructor here.
void OnPeerGone(uint8_t slot) {
    if (!IsHost() || !InRoster(slot)) return;
    if (!coop::prop_hold::ReservationCount(slot)) return;
    const uint32_t eid = coop::prop_hold::ReservedEid(slot);
    ++g_tally[static_cast<int>(Tally::HeldAtDisconnect)];
    UE_LOGI("prop_act: slot %u left holding a reservation (eid %u) -- released", static_cast<unsigned>(slot),
            static_cast<unsigned>(eid));
    coop::prop_hold::Release(slot);
}

const char* TallyName(Tally t) {
    static const char* const kNames[] = {
        "target_gone", "bad_commit", "held_at_disconnect", "destroy_failed", "queue_full_unready", "bad_sender",
        "grant_unsent", "fit_unread", "fit_bad_family", "fit_trash", "fit_not_prop", "fit_denied", "fit_no_use_row",
        "fit_collect_refused", "fit_pickup_refused", "fit_ignore_save", "fit_child", "fit_physics",
        "fit_can_collect", "fit_can_hold", "fit_heavy", "fit_mesh_root", "fit_volume", "fit_save_class",
        "fit_equip_row", "fit_not_edible"};
    static_assert(std::size(kNames) == static_cast<size_t>(Tally::kCount), "a name for every tally");
    return t < Tally::kCount ? kNames[static_cast<int>(t)] : "";
}

void OnDisconnect() {
    std::string fits;   // the session's act refusals, said once at its end
    for (int i = static_cast<int>(Tally::FitUnread); i < static_cast<int>(Tally::kCount); ++i) {
        if (!g_tally[i]) continue;
        fits += ' ';
        fits += TallyName(static_cast<Tally>(i));
        fits += '=';
        fits += std::to_string(g_tally[i]);
    }
    if (!fits.empty()) UE_LOGI("prop_act: fit refusals this session:%s", fits.c_str());
    std::fill(std::begin(g_tally), std::end(g_tally), 0u);
    g_unreadSaid.clear();
    g_refusals.Clear();
    for (int slot = 1; slot < coop::net::kMaxPeers; ++slot) coop::prop_hold::Release(slot);
}

void NoteBadSender() { ++g_tally[static_cast<int>(Tally::BadSender)]; }

void NoteShortPayload(uint8_t senderSlot) { g_refusals.Note(senderSlot, "payload too short", Now()); }

uint32_t AnsweredFor(int slot, PropActReason reason) {
    const size_t i = static_cast<size_t>(reason);
    return InRoster(slot) && i < coop::net::kPropActReasons ? g_slots[slot].answered[i] : 0;
}

uint32_t TallyOf(Tally t) { return t < Tally::kCount ? g_tally[static_cast<int>(t)] : 0; }

namespace dev {

bool CensusFits(void* actor, uint8_t family, Tally* first) {
    std::optional<Tally> why;
    const bool fits = FitCensus(actor, family, why);
    if (first) *first = why.value_or(Tally::kCount);
    return fits;
}

}  // namespace dev

uint32_t WaitsOf(int slot, WaitCause c) {
    return InRoster(slot) && c < WaitCause::kCount ? g_slots[slot].waits[static_cast<int>(c)] : 0;
}

uint32_t MaxDepthOf(int slot) { return InRoster(slot) ? g_slots[slot].maxDepth : 0; }

uint32_t AnsweredTotal(int slot) {
    uint32_t total = 0;
    if (!InRoster(slot)) return 0;
    for (const uint32_t n : g_slots[slot].answered) total += n;
    return total;
}

uint32_t CommitsOf(int slot, internal::CommitStep s) {
    const int i = static_cast<int>(s);
    return InRoster(slot) && i >= 0 && i < kCommitSteps ? g_slots[slot].commits[i] : 0;
}

}  // namespace coop::prop_act_host
