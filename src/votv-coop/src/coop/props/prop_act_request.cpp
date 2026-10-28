// coop/props/prop_act_request.cpp -- see coop/props/prop_act_request.h.

#include "coop/props/prop_act_request.h"

#include "coop/comms/chat_feed.h"
#include "coop/element/element.h"
#include "coop/element/registry.h"
#include "coop/net/net_clock.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/props/prop_hold.h"
#include "coop/props/remote_prop.h"
#include "coop/session/net_pump.h"

#include "l10n/l10n.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/actors/prop_use.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/hot_path_guard.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/hit_result.h"
#include "ue_wrap/facts/function_row.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

namespace coop::prop_act_request {
namespace {

namespace E  = ue_wrap::engine;
namespace PU = ue_wrap::prop_use;
namespace R  = ue_wrap::reflection;
namespace EL = coop::element;
namespace sg = ue_wrap::script_gate;
using coop::net::PropActFamily;
using coop::net::ReliableKind;

// A reply that has not come in this long closes the record: an honest wait ends in milliseconds, so the bound covers
// only a transport that refused the reply. MTA's client has no timer: it refuses a new entry until the server's word
// arrives (CClientPed.cpp:6558, cleared at CPacketHandler.cpp:1796).
constexpr uint64_t kReplyWaitMs = 10000;

coop::net::Session* g_session = nullptr;
Pending g_rec;
uint16_t g_nextId = 0;   // the last request id; 0 is never an id

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
dev::LastReply g_lastStale;
uint32_t g_lateGrantsAborted = 0;

// The drill hooks (dev:: below): inert until a drill calls one.
ue_wrap::CachedObjRef g_destroyInReplay;   // destroyed inside the next replay, after its verb
bool g_holdCommits = false;                // a commit is queued, not sent, until ReleaseHeldCommits
bool g_holdReplay = false;                 // a grant parks its replay
uint32_t g_refused[coop::net::kPropActReasons] = {};   // replies taken as refused, per reason
uint32_t g_hostCancels = 0;                // the host's own calls cancelled for a reservation

// ---- the lines ------------------------------------------------------------------------------------------------

// A line is shown at most once per kind per window (D1's shape, remover_request.cpp).
enum Line : size_t { kLineGone, kLineWrongFamily, kLineHeld, kLineReach, kLineQueueFull, kLineBadReason,
                     kLineNotAsked, kLineNotReady, kLineBusy, kLineCount };
constexpr uint64_t kShowEveryMs = 5000;
uint64_t g_shownAt[kLineCount] = {};
bool g_shown[kLineCount] = {};

void Show(Line which, const char* line) {
    const uint64_t now = coop::net::NowMs();
    if (g_shown[which] && now - g_shownAt[which] < kShowEveryMs) return;
    g_shown[which] = true;
    g_shownAt[which] = now;
    coop::chat_feed::Push(std::string(line), coop::chat_feed::Keep::Transient);
}

void ShowRefusal(uint8_t reason) {
    switch (static_cast<coop::net::PropActReason>(reason)) {
    case coop::net::PropActReason::Gone:
        Show(kLineGone, l10n::T("That item is gone."));
        return;
    case coop::net::PropActReason::WrongFamily:
        Show(kLineWrongFamily, l10n::T("You can't do that with this item."));
        return;
    case coop::net::PropActReason::Held:
        Show(kLineHeld, l10n::T("Someone else has that item."));
        return;
    case coop::net::PropActReason::Reach:
        Show(kLineReach, l10n::T("That item is too far away."));
        return;
    case coop::net::PropActReason::QueueFull:
        Show(kLineQueueFull, l10n::T("Too many actions at once, try again."));
        return;
    default:
        return;
    }
}

void ShowBadReason() { Show(kLineBadReason, l10n::T("That can't be done right now.")); }
void ShowNotAsked() { Show(kLineNotAsked, l10n::T("The host could not be asked; try again.")); }
void ShowNotReady() { Show(kLineNotReady, l10n::T("Not ready yet, try again in a moment.")); }
void ShowBusy() { Show(kLineBusy, l10n::T("Another player is using that item.")); }

// ---- the record -----------------------------------------------------------------------------------------------

void Close() { g_rec = Pending{}; }

// A commit the transport refused stays at the head and is sent again, so the host's reservation is always ended. The
// order of the queue is the order the host must see (the Default lane pairs by it).
struct QueuedCommit {
    coop::net::PropActCommitPayload payload;
    bool refusedLogged = false;
};
std::deque<QueuedCommit> g_commits;

// `released` sends a queue the drill is holding (dev::ReleaseHeldCommits).
void DrainCommits(bool released = false) {
    if (!g_session || (g_holdCommits && !released)) return;
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
        // A drill's second destroy, inside the latch: it is not the target, so the seam treats it as any destroy.
        if (void* const extra = g_destroyInReplay.Get()) E::DestroyActor(extra);
        g_destroyInReplay.Reset();
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
    g_lastStale = dev::LastReply{p.id, p.granted, p.reason, g_lastStale.count + 1};
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

// ---- the watches ----------------------------------------------------------------------------------------------

// Every verb that takes a shared prop into a player's hands, pockets or mouth, watched at the entry of its body by
// name. A Declared row is the class's own body: prop_food_C's is reached too by the descendants that inherit it.
UE_FACT_FN(kRowPocket, "prop_act", L"mainPlayer_C",      L"putObjectInventory2", Declared);
UE_FACT_FN(kRowHold,   "prop_act", L"mainPlayer_C",      L"Hold Object",         Declared);
UE_FACT_FN(kRowFood,   "prop_act", L"prop_food_C",       L"actionOptionIndex",   Declared);
UE_FACT_FN(kRowMre,    "prop_act", L"prop_food_mre_C",   L"actionOptionIndex",   Declared);
UE_FACT_FN(kRowPills,  "prop_act", L"prop_sleeppills_C", L"actionOptionIndex",   Declared);
UE_FACT_FN(kRowSoap,   "prop_act", L"prop_food_soap_C",  L"playerUsedOn",        Declared);
constexpr int kTagPocket = 0x50414331, kTagHold = 0x50414332, kTagFood = 0x50414333,
              kTagMre = 0x50414334, kTagPills = 0x50414335, kTagSoap = 0x50414336;   // "PAC1".."PAC6"

// The action index of eating, opening a food and taking pills.
constexpr uint8_t kEatAction = 4;

// Is this call the replay running now? The gate cannot tell a reflected call of ours from a drill's, so the latch does.
bool ReplayingTarget(void* target) { return g_latch.target && g_latch.target == target; }

// The trash lanes own the trash actors; a take or use of one is theirs.
bool InTrashLane(void* actor) { return ue_wrap::prop::IsTrashActor(actor) || ue_wrap::prop::IsTrashBitsPile(actor); }

bool HasKey(void* actor) {
    const std::wstring key = ue_wrap::prop::GetInteractableKeyString(actor);
    return !key.empty() && key != L"None";
}

template <class T>
bool ReadParam(R::ParamOffset& offset, const sg::Call& c, T& out) {
    const int32_t o = offset.Of(c.function);
    if (o < 0 || !c.locals) return false;
    std::memcpy(&out, c.locals + o, sizeof(T));
    return true;
}

// The parameters of an actionOptionIndex body, each in the offset cache of its own function.
struct ActionParams {
    R::ParamOffset action{L"action"};
    R::ParamOffset lookAt{L"lookAtComponent"};
};
ActionParams g_foodParams, g_mreParams, g_pillsParams;

struct SoapParams {
    R::ParamOffset holdObject{L"holdObject"};
    R::ParamOffset lookAt{L"lookAtComponent"};
    R::ParamOffset holdPropName{L"holdPropName"};
};
SoapParams g_soapParams;

// prop_sponge_C, found again when a world reload has re-created it.
ue_wrap::CachedObjRef g_spongeClass;
void* SpongeClass() {
    if (!g_spongeClass.Alive()) g_spongeClass.Set(ue_wrap::object_index::ClassByName(L"prop_sponge_C"));
    return g_spongeClass.Raw();
}

// Does the call end in a destroy of its prop? Read on the client's copy. A call the prediction cannot read asks the
// host anyway: its grant or its refusal is safe either way, and a needless ask aborts.
bool IsEat(ActionParams& params, const sg::Call& c) {
    uint8_t action = 0;
    return !ReadParam(params.action, c, action) || action == kEatAction;
}

bool FoodDestroys(const sg::Call& c) {
    uint8_t action = 0;
    if (ReadParam(g_foodParams.action, c, action) && action != kEatAction) return false;
    int32_t uses = 0;
    bool notEdible = false;
    if (!PU::ReadFoodUse(c.object, uses, notEdible)) return true;
    return !notEdible && uses <= 1;
}
bool MreDestroys(const sg::Call& c) { return IsEat(g_mreParams, c); }
bool PillsDestroys(const sg::Call& c) { return IsEat(g_pillsParams, c); }

// The soap's last use on a dry sponge in the hand: the body returns unless the hand holds a sponge with no soap.
bool SoapDestroys(const sg::Call& c) {
    void* hold = nullptr;
    if (!ReadParam(g_soapParams.holdObject, c, hold)) return true;
    if (!hold) return false;
    void* const sponge = SpongeClass();
    if (!sponge) return true;
    if (!ue_wrap::prop::WalksToBase(R::ClassOf(hold), sponge)) return false;
    float spongeSoap = 0.0f;
    if (PU::ReadSpongeSoap(hold, spongeSoap) && spongeSoap > 0.0f) return false;
    int32_t soap = 0;
    return !PU::ReadSoapAmount(c.object, soap) || soap == 1;
}

// A watch fires on the game thread. Null while no host link is up.
coop::net::Session* LinkedSession() { return g_session && g_session->connected() ? g_session : nullptr; }

sg::Verdict HostPre(void* target) {
    if (!target || !coop::prop_hold::ReservedByOther(target, 0)) return sg::Verdict::Run;
    ++g_hostCancels;
    ShowBusy();
    return sg::Verdict::Cancel;
}

enum class Gate : uint8_t { Run, Cancel, Ask };
sg::Verdict ToVerdict(Gate g) { return g == Gate::Cancel ? sg::Verdict::Cancel : sg::Verdict::Run; }

// What a client's call on `target` is: its own (Run), refused for now (Cancel), or the host's to grant (Ask, with the
// target's element id). `c.object` is the player of a pocket or hold, the prop of a use.
Gate ClientGate(const sg::Call& c, void* target, bool (*destroys)(const sg::Call&), EL::ElementId& eid) {
    if (ReplayingTarget(target)) return Gate::Run;
    if (!target || InTrashLane(target)) return Gate::Run;
    if (destroys && !destroys(c)) return Gate::Run;
    eid = EL::Registry::Get().EidForActor(target);
    const EL::Element* el = eid != EL::kInvalidId ? EL::Registry::Get().Get(eid) : nullptr;
    // The row decides, never the key: a client-born spawn is keyed and rowless and is the client's own.
    if (el && el->IsMirror() && el->GetType() == EL::ElementType::Prop) return Gate::Ask;
    // Before its world is announced a keyed prop with no row may be a shared one not bound yet, and a native take of
    // it would duplicate it.
    if (!el && !coop::net_pump::IsInAnnouncedWorld(c.object) && HasKey(target)) {
        ShowNotReady();
        return Gate::Cancel;
    }
    return Gate::Run;
}

// The hit parameter of one use function: its offset in the offset cache, its size read once per function. The
// function is held by slot and serial, as the offset cache holds it, so a freed function's size is never reused.
struct HitParam {
    R::ParamOffset offset{L"hit"};
    ue_wrap::CachedObjRef sizeFn;
    int32_t size = -1;
};
HitParam g_foodHit, g_mreHit, g_pillsHit, g_soapHit;

// The hit of a use, as the replay writes it back: only its component and place matter.
void CaptureHit(const sg::Call& c, HitParam& param, Pending& p) {
    const int32_t off = param.offset.Of(c.function);
    if (off < 0) return;
    if (!param.sizeFn.Alive() || param.sizeFn.Raw() != c.function || param.size <= 0) {
        param.sizeFn.Set(c.function);
        param.size = R::FindParamSize(c.function, L"hit");
    }
    const int32_t size = param.size;
    ue_wrap::hit_result::Hit hit;
    if (size <= 0 || !c.locals || !ue_wrap::hit_result::Parse(c.locals + off, size, &hit)) return;
    p.hitComponent.Set(hit.component);
    p.hitLocation = hit.location;
}

// A target parameter the call's frame does not carry leaves nothing to ask about: said once per row, then the call
// runs natively.
sg::Verdict RunUnreadable(const ue_wrap::facts::Function& row, bool& said) {
    if (!said) {
        said = true;
        UE_LOGW("prop_act: %ls.%ls's target parameter is unreadable -- the call runs natively", row.Class(), row.Name());
    }
    return sg::Verdict::Run;
}

// A take by the player's own hands or pocket: a puppet's body, a robot served for a client, runs untouched.
sg::Verdict PrePocket(const sg::Call& c) {
    coop::net::Session* const s = LinkedSession();
    if (!s || !coop::players::Registry::Get().IsLocal(c.object)) return sg::Verdict::Run;
    static R::ParamOffset sTarget{L"InputPin"};
    static R::ParamOffset sNoNotify{L"noNotify"};
    static bool sSaid = false;
    void* target = nullptr;
    const bool readable = ReadParam(sTarget, c, target);
    if (s->role() == coop::net::Role::Host) return HostPre(target);
    if (!readable) return RunUnreadable(kRowPocket, sSaid);
    EL::ElementId eid = EL::kInvalidId;
    const Gate g = ClientGate(c, target, nullptr, eid);
    if (g != Gate::Ask) return ToVerdict(g);
    Pending p;
    p.eid = eid;
    p.row = kPocket;
    p.target.Set(target);
    uint8_t noNotify = 0;
    ReadParam(sNoNotify, c, noNotify);
    p.noNotify = noNotify != 0;
    Ask(p, PropActFamily::Pocket);
    return sg::Verdict::Cancel;
}

// The target Hold Object takes, by the body's own rule: `manual`, else the grabbed actor when `useHold`, else the one
// looked at. `readable` is false when the frame lacks a parameter that decides it.
void* HoldTarget(const sg::Call& c, bool& readable) {
    static R::ParamOffset sManual{L"manual"};
    static R::ParamOffset sUseHold{L"useHold"};
    void* manual = nullptr;
    readable = ReadParam(sManual, c, manual);
    if (manual) return manual;
    uint8_t useHold = 0;
    readable = ReadParam(sUseHold, c, useHold) && readable;
    if (!useHold) return E::ReadMainPlayerLookAtActor(c.object);
    E::MainPlayerGrabState grab{};
    return E::ReadMainPlayerGrabState(c.object, grab) ? grab.grabbingActor : nullptr;
}

sg::Verdict PreHold(const sg::Call& c) {
    coop::net::Session* const s = LinkedSession();
    if (!s || !coop::players::Registry::Get().IsLocal(c.object)) return sg::Verdict::Run;
    static bool sSaid = false;
    bool readable = true;
    void* const target = HoldTarget(c, readable);
    if (s->role() == coop::net::Role::Host) return HostPre(target);
    if (!readable) return RunUnreadable(kRowHold, sSaid);
    EL::ElementId eid = EL::kInvalidId;
    const Gate g = ClientGate(c, target, nullptr, eid);
    if (g != Gate::Ask) return ToVerdict(g);
    Pending p;
    p.eid = eid;
    p.row = kHold;
    p.target.Set(target);
    Ask(p, PropActFamily::Hold);
    return sg::Verdict::Cancel;
}

// A use of the prop the body runs on: `c.object` is the prop, and on a client every such call is the local player's
// press or the replay's own.
sg::Verdict PreAction(const sg::Call& c, uint8_t row, ActionParams& params, HitParam& hitParam,
                      bool (*destroys)(const sg::Call&)) {
    coop::net::Session* const s = LinkedSession();
    if (!s) return sg::Verdict::Run;
    if (s->role() == coop::net::Role::Host) return HostPre(c.object);
    EL::ElementId eid = EL::kInvalidId;
    const Gate g = ClientGate(c, c.object, destroys, eid);
    if (g != Gate::Ask) return ToVerdict(g);
    Pending p;
    p.eid = eid;
    p.row = row;
    p.target.Set(c.object);
    ReadParam(params.action, c, p.action);
    void* lookAt = nullptr;
    ReadParam(params.lookAt, c, lookAt);
    p.lookAt.Set(lookAt);
    CaptureHit(c, hitParam, p);
    Ask(p, PropActFamily::Use);
    return sg::Verdict::Cancel;
}

sg::Verdict PreFood(const sg::Call& c) { return PreAction(c, kFood, g_foodParams, g_foodHit, &FoodDestroys); }
sg::Verdict PreMre(const sg::Call& c) { return PreAction(c, kMre, g_mreParams, g_mreHit, &MreDestroys); }
sg::Verdict PrePills(const sg::Call& c) { return PreAction(c, kPills, g_pillsParams, g_pillsHit, &PillsDestroys); }

// The replay passes the hand it finds then, not the one captured here: only the hit and the hand item's name travel.
sg::Verdict PreSoap(const sg::Call& c) {
    coop::net::Session* const s = LinkedSession();
    if (!s) return sg::Verdict::Run;
    if (s->role() == coop::net::Role::Host) return HostPre(c.object);
    EL::ElementId eid = EL::kInvalidId;
    const Gate g = ClientGate(c, c.object, &SoapDestroys, eid);
    if (g != Gate::Ask) return ToVerdict(g);
    Pending p;
    p.eid = eid;
    p.row = kSoap;
    p.target.Set(c.object);
    void* lookAt = nullptr;
    ReadParam(g_soapParams.lookAt, c, lookAt);
    p.lookAt.Set(lookAt);
    ReadParam(g_soapParams.holdPropName, c, p.holdPropName);
    CaptureHit(c, g_soapHit, p);
    Ask(p, PropActFamily::Use);
    return sg::Verdict::Cancel;
}

enum class WatchState : uint8_t { Pending, Registered, Refused, Settled };
struct Watch {
    const ue_wrap::facts::Function& row;
    int tag;
    sg::PreFn pre;
    const char* verb;   // the word for the line a dead watch prints
    WatchState state;
};
Watch g_watches[] = {
    {kRowPocket, kTagPocket, &PrePocket, "take", WatchState::Pending},
    {kRowHold,   kTagHold,   &PreHold,   "take", WatchState::Pending},
    {kRowFood,   kTagFood,   &PreFood,   "use",  WatchState::Pending},
    {kRowMre,    kTagMre,    &PreMre,    "use",  WatchState::Pending},
    {kRowPills,  kTagPills,  &PrePills,  "use",  WatchState::Pending},
    {kRowSoap,   kTagSoap,   &PreSoap,   "use",  WatchState::Pending},
};
bool g_watchesSettled = false;

// Says once per watch whether the gate took it. The names resolve on the game thread, so a tick drives them.
void SettleWatches() {
    if (g_watchesSettled) return;
    bool registered = false;
    for (const Watch& w : g_watches) registered = registered || w.state == WatchState::Registered;
    if (registered) sg::ResolvePendingNames();
    bool open = false;
    for (Watch& w : g_watches) {
        if (w.state == WatchState::Settled) continue;
        if (w.state == WatchState::Pending) {
            open = true;
        } else if (w.state == WatchState::Registered && sg::NameWatchLive(w.row, w.tag)) {
            w.state = WatchState::Settled;
            UE_LOGI("prop_act: watch %ls.%ls live", w.row.Class(), w.row.Name());
        } else if (w.state == WatchState::Refused || sg::NameWatchSettled(w.row, w.tag)) {
            w.state = WatchState::Settled;
            UE_LOGE("prop_act: watch %ls.%ls DEAD -- a client's %s of a shared prop reaches the host as a raw destroy",
                    w.row.Class(), w.row.Name(), w.verb);
        } else {
            open = true;
        }
    }
    g_watchesSettled = !open;
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

bool Ask(const Pending& filled, PropActFamily family) {
    UE_ASSERT_GAME_THREAD("prop_act_request::Ask");
    // A second press while one is pending is the player's double press: cancelled with nothing sent and no line.
    if (g_rec.open || !g_session) return false;
    uint16_t id = ++g_nextId;
    if (id == 0) id = ++g_nextId;
    const coop::net::PropActRequestPayload payload{id, static_cast<uint8_t>(family), 0, filled.eid};
    if (!g_session->SendReliableToSlot(0, ReliableKind::PropActRequest, &payload, sizeof(payload))) {
        ShowNotAsked();
        return false;
    }
    g_rec = filled;
    g_rec.open = true;
    g_rec.id = id;
    g_rec.sentMs = coop::net::NowMs();
    return true;
}

void Install(coop::net::Session* session) {
    g_session = session;
    // The gate refuses a watch it cannot keep for the process; a refused one is said dead at the settle.
    for (Watch& w : g_watches) {
        if (w.state != WatchState::Pending) continue;
        w.state = sg::WatchName(w.row, w.tag, w.pre, nullptr) ? WatchState::Registered : WatchState::Refused;
    }
}

void Tick() {
    SettleWatches();
    if (!g_rec.open && g_commits.empty()) return;
    UE_ASSERT_GAME_THREAD("prop_act_request::Tick");
    DrainCommits();
    if (!g_rec.open) return;
    if (g_rec.replayHeld) {
        // A drill parked this replay until the host's destroy reaches the mirror: the host then holds a reservation
        // for a prop that is gone, and the abort ends it.
        if (!g_rec.target.Get()) {
            SendCommit(0);
            Close();
        }
        return;
    }
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
        ShowBadReason();
        Close();
        return;
    case internal::ReplyAction::Refused:
        if (p.reason < coop::net::kPropActReasons) ++g_refused[p.reason];
        ShowRefusal(p.reason);
        Close();
        return;
    case internal::ReplyAction::Granted:
        if (g_holdReplay) {
            g_rec.replayHeld = true;
            return;
        }
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
    g_nextId = 0;
    g_commits.clear();
    g_latch = Latch{};
    g_expired.clear();
    g_lastStale = dev::LastReply{};
    g_lateGrantsAborted = 0;
    g_destroyInReplay.Reset();
    g_holdCommits = false;
    g_holdReplay = false;
    std::fill(std::begin(g_refused), std::end(g_refused), 0u);
    g_hostCancels = 0;
    for (size_t i = 0; i < kLineCount; ++i) g_shown[i] = false;
}

namespace dev {

LastReply LastStaleReply() { return g_lastStale; }
void DestroyInsideNextReplay(void* actor) { g_destroyInReplay.Set(actor); }
uint16_t PendingId() { return g_rec.open ? g_rec.id : 0; }
void HoldCommits(bool on) { g_holdCommits = on; }
void ReleaseHeldCommits() { DrainCommits(true); }
void HoldReplayUntilGone(bool on) { g_holdReplay = on; }
uint32_t RefusedCount(coop::net::PropActReason reason) {
    const size_t i = static_cast<size_t>(reason);
    return i < coop::net::kPropActReasons ? g_refused[i] : 0u;
}
uint32_t HostCancels() { return g_hostCancels; }
uint32_t LateGrantsAborted() { return g_lateGrantsAborted; }
size_t ExpiredPending() { return g_expired.size(); }
bool NotReadyShown() { return g_shown[kLineNotReady]; }

}  // namespace dev

}  // namespace coop::prop_act_request
