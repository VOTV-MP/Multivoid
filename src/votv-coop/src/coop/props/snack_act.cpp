// coop/props/snack_act.cpp -- see coop/props/snack_act.h.

#include "coop/props/snack_act.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/element/element.h"
#include "coop/element/registry.h"
#include "coop/net/session.h"
#include "coop/props/prop_echo_suppress.h"
#include "coop/props/prop_save_data.h"
#include "coop/props/prop_snapshot.h"
#include "coop/props/remote_prop.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/actors/snack.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/ufunction_hook.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/facts/function_row.h"
#include "ue_wrap/world/votv_lib.h"

#include <string>
#include <vector>

namespace coop::snack_act {

namespace internal {

bool WellFormedSnackCommit(uint8_t committed, uint8_t flags) {
    if (committed == 1) return flags == kFlagSpent;
    if (committed == kCommitApplied) return (flags & ~kFlagMask) == 0;
    return false;
}

Bounded Bound(int32_t uses, bool open, bool beer, uint8_t flags) {
    Bounded b{uses, open};
    // A use is spent only from the copy's state BEFORE the open: openBottle spends nothing, so a report of both from a
    // closed bottle is never honest and cuts to the open alone.
    const bool couldSpend = open || !beer;
    if ((flags & kFlagOpened) && beer && !b.open) b.open = true;
    if ((flags & kFlagSpent) && couldSpend && b.uses >= 1) --b.uses;
    return b;
}

}  // namespace internal

CopyState ReadCopy(void* actor) {
    CopyState s;
    s.read = ue_wrap::snack::ReadSnackState(actor, s.uses, s.open);
    return s;
}

CommitWords CommitFor(bool ran, bool destroyed, const CopyState& before, const CopyState& after) {
    if (destroyed) return {1, kFlagSpent};
    if (!ran || !before.read || !after.read) return {0, 0};
    uint8_t flags = 0;
    if (after.uses < before.uses) flags |= kFlagSpent;
    if (!before.open && after.open) flags |= kFlagOpened;
    return {kCommitApplied, flags};
}

namespace {

namespace R  = ue_wrap::reflection;
namespace UH = ue_wrap::ufunction_hook;

// The beer's spill is a second reader of the shared copy: its own tick spends a use and births a grimeProjectile_C whose
// source object is the beer, a call the host's own actions never announce. The birth is the mark.
UE_FACT_FN(kRowFinish, "snack_act", L"GameplayStatics", L"FinishSpawningActor", Declared);
constexpr int kHookTries = 10;   // pump ticks the hook's function may take to resolve

// The snacks the host owes a publication: an announced event marks one, the one drain per tick publishes it. Coalescing
// announced events is the shape of spill_sync's pours and the tick-deferred PublishHostBirth; a mark is an actor
// by slot and serial, so the set is bounded by the live world snacks and a dead one is dropped at the drain.
std::vector<ue_wrap::CachedObjRef> g_marks;
bool g_hostSeen = false;   // the session's role at the last tick: the hook marks only on the host
bool g_hookLive = false;
bool g_hookBroken = false;
int g_hookTries = 0;
ue_wrap::CachedObjRef g_grimeClass;

void Mark(void* actor) {
    for (const ue_wrap::CachedObjRef& m : g_marks)
        if (m.Is(actor)) return;
    g_marks.emplace_back();
    g_marks.back().Set(actor);
}

// A world snack: a prop of the host's own with an element and no mirror, which a hand view never is.
bool IsWorldSnack(void* actor) {
    if (!ue_wrap::snack::IsSnack(actor)) return false;
    const coop::element::ElementId eid = coop::element::Registry::Get().EidForActor(actor);
    const coop::element::Element* const el = eid != coop::element::kInvalidId ? coop::element::Registry::Get().Get(eid) : nullptr;
    return el && el->GetType() == coop::element::ElementType::Prop && !el->IsMirror();
}

// The post-hook of FinishSpawningActor, for every spawn in the world: a grime born by a beer's script marks that beer.
// It does no work inside the engine's call.
void OnFinishPost(void*, void* source, void* result) {
    if (!g_hostSeen || !source || !result) return;
    // The class is resolved by the tick (a world change makes it dead there and it is found again), never per spawn.
    void* const grime = g_grimeClass.Get();
    if (!grime || R::ClassOf(result) != grime || !ue_wrap::snack::IsBeerFamily(source)) return;
    Mark(source);
}

// CLIENT: the replay of a snack's last use runs the game's own script, which births a wrapper on this peer too. While a
// replay is latched for `g_refuseSource`, a spawn that script makes (its FinishSpawningActor, source the snack) is
// destroyed at the post-hook and never published, so the host's wrapper is the only one. The pointer is compared only.
void* g_refuseSource = nullptr;
bool g_refuseHookLive = false;
bool g_refuseHookBroken = false;
int g_refuseTries = 0;
void* g_refuseFn = nullptr;

void OnRefusePost(void*, void* source, void* result) {
    if (!g_refuseSource || source != g_refuseSource || !result) return;
    coop::prop_echo_suppress::MarkIncomingDestroy(result);
    ue_wrap::engine::DestroyActor(result);
}

void InstallRefusalHook() {
    if (g_refuseHookLive || g_refuseHookBroken) return;
    const ue_wrap::facts::Resolved fn = ue_wrap::facts::Resolve(kRowFinish);
    if (fn && UH::InstallPostHook(fn, &OnRefusePost, /*armed=*/false)) {
        g_refuseFn = fn.Fn();
        g_refuseHookLive = true;
        return;
    }
    if (++g_refuseTries >= kHookTries) {
        g_refuseHookBroken = true;
        UE_LOGE("snack_act: the wrapper-refusal post-hook did not install -- a replayed last use leaves a second wrapper");
    }
}

void InstallSpillHook() {
    if (g_hookLive || g_hookBroken) return;
    const ue_wrap::facts::Resolved fn = ue_wrap::facts::Resolve(kRowFinish);
    if (fn && UH::InstallPostHook(fn, &OnFinishPost)) {
        g_hookLive = true;
        return;
    }
    if (++g_hookTries >= kHookTries) {
        g_hookBroken = true;
        UE_LOGE("snack_act: the FinishSpawningActor post-hook did not install -- a beer's spill is not published");
    }
}

// The wrapper a snack's last use leaves, by the snack script's own decision (its eat's empty branch): the INSTANCE's
// spawnAsObject decides. True: lib_C's Prop To Object names the trash's class, and a class spawns (adjusting where it
// can) and, for a prop, has its name stamped and its init() run after the finish; no class and a food row spawns a
// prop_food_C with that row's data and the name set before the finish; any other a plain prop_C the same way. False: a
// plain prop_C with the name set before the finish and the class's own collision. Null when it could not be born.
void* BirthWrapper(void* snack) {
    namespace S = ue_wrap::snack;
    constexpr uint8_t kClassDefault = 0, kAdjust = 2;
    S::WrapperFacts f;
    if (!S::ReadWrapperFacts(snack, f)) return nullptr;
    void* const propClass = ue_wrap::object_index::ClassByName(L"prop_C");
    void* actor = nullptr;
    bool stamp = false;
    if (!f.spawnAsObject) {
        actor = propClass ? S::BeginWrapperActor(propClass, f, kClassDefault, f.trash, nullptr) : nullptr;
    } else {
        ue_wrap::votv_lib::PropObject obj;
        if (!ue_wrap::votv_lib::PropToObject(f.trash, snack, obj)) return nullptr;
        if (obj.objectClass) {
            actor = S::BeginWrapperActor(obj.objectClass, f, kAdjust, R::FName{0, 0}, nullptr);
            stamp = obj.parseNameToObject;
        } else if (obj.isFood) {
            void* const foodClass = ue_wrap::object_index::ClassByName(L"prop_food_C");
            actor = foodClass ? S::BeginWrapperActor(foodClass, f, kAdjust, f.trash, &obj.foodData) : nullptr;
        } else {
            actor = propClass ? S::BeginWrapperActor(propClass, f, kAdjust, f.trash, nullptr) : nullptr;
        }
    }
    if (!actor || !S::FinishWrapperActor(actor, f)) return nullptr;
    // The object path stamps the finished prop's name and runs its init(), as the script does through asProp (a prop's
    // own, which returns itself).
    if (stamp && !S::StampNameAndInit(actor, f.trash)) UE_LOGW("snack_act: the wrapper's name stamp or init() did not run");
    return actor;
}

// Dev red arm (never set): the host does not re-send a birth to a requester whose copy ended, so its copy stays gone.
bool ResendOff() {
    static const bool off = coop::config::ResolveFlag(coop::config_registry::rows::snack_resend_off);
    return off;
}

// A snack with no use left is gone on every peer, and the host births the one wrapper it leaves (its seams carry the
// birth to every peer, the name stamped before the row is built) before its copy ends. No publish follows the destroy.
void EndOnEveryPeer(coop::net::Session* s, void* actor, const coop::net::PropActCommitPayload& p, uint8_t senderSlot) {
    if (!BirthWrapper(actor))
        UE_LOGE("snack_act: eid %u -- the last use's wrapper could not be born on the host", static_cast<unsigned>(p.eid));
    ue_wrap::CachedObjRef before;
    before.Set(actor);
    coop::net::PropDestroyPayload dp{};
    dp.elementId = p.eid;
    coop::remote_prop::DestroyJudged(dp, senderSlot);
    if (before.Is(actor)) {
        UE_LOGE("snack_act: the host could not destroy eid %u for slot %u -- the snack stands", static_cast<unsigned>(p.eid),
                static_cast<unsigned>(senderSlot));
        return;
    }
    // A requester whose copy ended already has nothing to destroy; one whose copy lived has it destroyed here.
    s->SendReliable(coop::net::ReliableKind::PropDestroy, &dp, sizeof(dp), p.committed == 1 ? senderSlot : -1);
}

}  // namespace

void OnCommit(coop::net::Session* s, void* actor, const coop::net::PropActCommitPayload& p, uint8_t senderSlot) {
    int32_t uses = 0;
    bool open = false;
    if (!s || !ue_wrap::snack::ReadSnackState(actor, uses, open)) {
        // Nothing is applied, but the requester is not left with a copy the replay changed or gone: the host's state is
        // published, and a requester whose copy ended gets the birth again.
        UE_LOGW("snack_act: slot %u eid %u -- the host copy's state is unreadable, nothing applied, its state published",
                static_cast<unsigned>(senderSlot), static_cast<unsigned>(p.eid));
        Mark(actor);
        if (s && p.committed == 1 && !ResendOff()) coop::prop_snapshot::SendPropBirthToSlot(actor, senderSlot);
        return;
    }
    const internal::Bounded b = internal::Bound(uses, open, ue_wrap::snack::IsBeerFamily(actor), p.flags);
    UE_LOGI("snack_act: slot %u eid %u committed %u flags %u: host copy uses %d open %d -> uses %d open %d",
            static_cast<unsigned>(senderSlot), static_cast<unsigned>(p.eid), static_cast<unsigned>(p.committed),
            static_cast<unsigned>(p.flags), uses, open ? 1 : 0, b.uses, b.open ? 1 : 0);
    if (b.uses <= 0) {
        EndOnEveryPeer(s, actor, p, senderSlot);
        return;
    }
    if ((b.uses != uses || b.open != open) && !ue_wrap::snack::PatchSnackRecord(actor, b.uses, b.open))
        UE_LOGW("snack_act: eid %u -- the host copy's record could not be patched", static_cast<unsigned>(p.eid));
    // The requester's replayed copy, and every other peer's, is put back to the host's state, a refused delta included.
    Mark(actor);
    // The requester's copy ended while the host's lives: its element is gone there, so the host sends that one peer the
    // snack's birth again, with the host's own count.
    if (p.committed == 1 && !ResendOff() && !coop::prop_snapshot::SendPropBirthToSlot(actor, senderSlot))
        UE_LOGW("snack_act: eid %u -- the snack's birth could not be re-sent to slot %u", static_cast<unsigned>(p.eid),
                static_cast<unsigned>(senderSlot));
}

void NoteAborted(void* actor) {
    if (IsWorldSnack(actor)) Mark(actor);
}

void NoteHostUse(void* actor) {
    if (IsWorldSnack(actor)) Mark(actor);
}

// Dev red arm (never set): the replay keeps the wrapper its own script births, beside the host's.
static bool RefusalOff() {
    static const bool off = coop::config::ResolveFlag(coop::config_registry::rows::snack_wrapper_refusal_off);
    return off;
}

WrapperRefusal::WrapperRefusal(void* snack) : armed_(snack && g_refuseHookLive && !RefusalOff()) {
    if (!armed_) return;
    g_refuseSource = snack;
    UH::SetArmed(g_refuseFn, &OnRefusePost, true);
}

WrapperRefusal::~WrapperRefusal() {
    if (!armed_) return;
    UH::SetArmed(g_refuseFn, &OnRefusePost, false);
    g_refuseSource = nullptr;
}

void Tick(coop::net::Session* s) {
    g_hostSeen = s && s->role() == coop::net::Role::Host;
    if (s && !g_hostSeen) InstallRefusalHook();
    if (!g_hostSeen) return;
    InstallSpillHook();
    if (!g_grimeClass.Alive()) g_grimeClass.Set(ue_wrap::object_index::ClassByName(L"grimeProjectile_C"));
    if (g_marks.empty()) return;
    // A mark leaves once its record has gone out. A declined Publish with no peer ready to take it (nobody in the world yet)
    // keeps the mark for the next tick; one the transport refused is registered by the lane and sent again by it.
    size_t kept = 0;
    for (size_t i = 0; i < g_marks.size(); ++i) {
        void* const actor = g_marks[i].Get();
        if (!actor || !IsWorldSnack(actor)) continue;
        if (!coop::prop_save_data::Publish(s, actor, ue_wrap::prop::GetInteractableKeyString(actor)) &&
            !s->AnyWorldReadyPeer())
            g_marks[kept++] = g_marks[i];
    }
    g_marks.resize(kept);
}

namespace dev {
void MarkForPublication(void* actor) { Mark(actor); }
bool MarksPending() { return !g_marks.empty(); }
}  // namespace dev

void OnDisconnect() {
    g_marks.clear();
    g_hostSeen = false;
}

}  // namespace coop::snack_act
