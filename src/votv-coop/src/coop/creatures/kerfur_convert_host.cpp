// coop/creatures/kerfur_convert_host.cpp -- the HOST executor half of the kerfur conversion: a
// KerfurConvertRequest runs the real verb on the host's copy, and every conversion verb's return, the
// host's own or a request's, converges on the successor its bracket captured (BindFormActor ->
// KerfurConvert). The feature narrative lives in kerfur_convert.h; interfaces in kerfur_convert_host.h.

#include "coop/creatures/kerfur_convert_host.h"

#include "coop/creatures/kerfur_entity.h"
#include "coop/creatures/kerfur_form_assembler.h"
#include "coop/creatures/npc_sync.h"
#include "coop/element/mirror_manager.h"
#include "coop/element/npc.h"
#include "coop/element/prop.h"
#include "coop/element/registry.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/props/prop_element_tracker.h"
#include "coop/props/prop_lifecycle.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>

namespace coop::kerfur_convert_host {
namespace {

namespace R  = ue_wrap::reflection;
namespace KE = coop::kerfur_entity;
namespace EL = coop::element;
namespace PT = coop::prop_element_tracker;

std::atomic<coop::net::Session*> g_session{nullptr};

// Resolved kerfur refs (pushed by kerfur_convert::Install: SetClasses once both classes resolve,
// SetVerbs at the success site; read-only on the game thread here).
void* g_kerfurNpcClass  = nullptr;
void* g_kerfurPropClass = nullptr;
void* g_spawnKerfuroFn  = nullptr;
int32_t g_killOff       = -1;
std::atomic<bool> g_ready{false};  // the request latch

// The request whose verb is running: its object, and the slot that asked. Set around the call, read
// at the verb's entry (RequestSlotFor). Verbs are synchronous on the game thread, so one pair does.
void* g_runningObject = nullptr;
int   g_runningSlot   = -1;

// The pose the converge binds the new form at: its own, read whole. No pose of the old form's stands
// in for it, since the verb places the new form itself: the turn-on stands the NPC upright 50 cm above
// the prop's spawn point, keeping only its yaw, and the turn-off drops the prop at the NPC's transform
// or at (0,0,20000) in the flesh room.
bool NewFormPose(void* newForm, const char* what, EL::ElementId oldEid, ue_wrap::FVector& loc,
                 ue_wrap::FRotator& rot) {
    const bool locRead = ue_wrap::engine::TryGetActorLocation(newForm, loc);
    if (locRead && ue_wrap::engine::TryGetActorRotation(newForm, rot)) return true;
    UE_LOGW("kerfur_convert: %s eid=%u -- the new form's %s could not be read; the converge fails", what,
            static_cast<unsigned>(oldEid), locRead ? "rotation" : "location");
    return false;
}

// The most-derived dropKerfurProp declarer for this actor's class, a collar variant's override
// included: ProcessEvent runs exactly the UFunction passed, so this IS the virtual dispatch the BP's
// own by-name call would have done. The base declares it, so a kerfur always has one.
void* PickDropPropFn(void* cls) {
    return R::FindDispatchFunctionCached(cls, L"dropKerfurProp");
}

// The dead prop form's element, released without a broadcast (the actor is a map key only, never
// dereferenced). Its destroy seam, which fired inside the verb, drained it already; the converge owns
// the old form's release all the same, and a second drain is a no-op.
void ReleaseHostPropSilent(void* deadActor) {
    if (!deadActor) return;
    PT::UnmarkProcessedInit(deadActor);
    PT::UnmarkKnownKeyedProp(deadActor);
}

// A turn-on whose converge fails after the prop's destroy seam held its death back, the successor
// being captured: the death reaches the peers here instead, by eid, and the kerfur's record goes.
void RetirePropFormAsDeath(void* deadActor, EL::ElementId eid) {
    ReleaseHostPropSilent(deadActor);
    if (eid != EL::kInvalidId) {
        auto* s = g_session.load(std::memory_order_acquire);
        if (s && s->connected()) {
            coop::net::PropDestroyPayload dp{};
            dp.key.len = 0;
            dp.elementId = static_cast<uint32_t>(eid);
            s->SendPropDestroy(dp);
        }
    }
    KE::ReleaseKerfurForEid(eid);
}

// The turn-off's converge: the NPC is gone, and `newProp` is the prop the verb made, or null.
void ConvergeTurnOff(void* oldActor, EL::ElementId oldEid, void* newProp, const char* who) {
    ue_wrap::FVector loc{};
    ue_wrap::FRotator rot{};
    const EL::ElementId newEid = (newProp && NewFormPose(newProp, "turn_off", oldEid, loc, rot))
                                     ? coop::prop_lifecycle::RegisterHostPropSilent(newProp)
                                     : EL::kInvalidId;
    if (newEid == EL::kInvalidId) {
        // No successor, or none that binds: the kerfur is gone, not converted. The NPC died inside the
        // verb, which no destroy relay saw, so its death is relayed here.
        UE_LOGW("kerfur_convert: turn_off eid=%u (%s) -- %s; the NPC is retired as a plain death",
                static_cast<unsigned>(oldEid), who,
                newProp ? "the new prop could not be bound" : "the verb captured no successor");
        coop::npc_sync::SyncDestroyedNpcByEid(oldEid, oldActor);
        return;
    }
    coop::npc_sync::ReleaseNpcElementSilent(oldEid);
    KE::BindFormActor(oldEid, newProp, R::InternalIndexOf(newProp), newEid, KE::Form::Prop,
                      R::ClassNameOf(newProp), loc.X, loc.Y, loc.Z, rot.Pitch, rot.Yaw, rot.Roll);
    UE_LOGI("kerfur_convert: turn_off eid=%u (%s) converged at the verb's return -> prop %p eid=%u",
            static_cast<unsigned>(oldEid), who, newProp, static_cast<unsigned>(newEid));
}

// The turn-on's converge: the prop is gone, and `newNpc` is the NPC the verb made, or null.
void ConvergeTurnOn(void* oldActor, EL::ElementId oldEid, void* newNpc, const char* who) {
    if (!newNpc) {
        // No successor: the prop's death rode the generic relay (its seam holds back only for a
        // captured successor), and its record goes with it.
        UE_LOGW("kerfur_convert: turn-on eid=%u (%s) -- the verb captured no successor; the prop's death "
                "was relayed as a plain one", static_cast<unsigned>(oldEid), who);
        ReleaseHostPropSilent(oldActor);
        KE::ReleaseKerfurForEid(oldEid);
        return;
    }
    ue_wrap::FVector loc{};
    ue_wrap::FRotator rot{};
    const std::wstring cls = R::ClassNameOf(newNpc);
    const EL::ElementId newEid = NewFormPose(newNpc, "turn-on", oldEid, loc, rot)
                                     ? coop::npc_sync::RegisterHostNpcSilent(newNpc, cls)
                                     : EL::kInvalidId;
    if (newEid == EL::kInvalidId) {
        UE_LOGW("kerfur_convert: turn-on eid=%u (%s) -- the new NPC %p could not be bound; the prop's death is "
                "relayed on its own", static_cast<unsigned>(oldEid), who, newNpc);
        RetirePropFormAsDeath(oldActor, oldEid);
        return;
    }
    ReleaseHostPropSilent(oldActor);
    KE::BindFormActor(oldEid, newNpc, R::InternalIndexOf(newNpc), newEid, KE::Form::Npc, cls,
                      loc.X, loc.Y, loc.Z, rot.Pitch, rot.Yaw, rot.Roll);
    UE_LOGI("kerfur_convert: turn-on eid=%u (%s) converged at the verb's return -> NPC %p eid=%u",
            static_cast<unsigned>(oldEid), who, newNpc, static_cast<unsigned>(newEid));
}

}  // namespace

int RequestSlotFor(void* object) {
    return (object && object == g_runningObject) ? g_runningSlot : -1;
}

void ConvergeAtReturn(void* oldActor, int32_t oldIdx, EL::ElementId oldEid, bool toProp, int requestSlot) {
    char who[32];
    if (requestSlot >= 0) std::snprintf(who, sizeof(who), "slot %d's request", requestSlot);
    else std::snprintf(who, sizeof(who), "the host's own");
    if (oldActor && R::IsLiveByIndex(oldActor, oldIdx)) {
        // The verb returned with its kerfur standing: the game refused (a sentient kerfur) or failed to
        // spawn. Nothing converted, and no peer converted either: a client never runs the verb. MTA
        // answers a refused vehicle request (Server CGame.cpp:3042, VEHICLE_ATTEMPT_FAILED) because its
        // client holds an entering state to reset (Client CPacketHandler.cpp:2135); a client here holds
        // none once its gate refused, so a refusal sends nothing.
        UE_LOGI("kerfur_convert: %s eid=%u (%s) refused by the game -- nothing converted",
                toProp ? "turn_off" : "turn-on", static_cast<unsigned>(oldEid), who);
        return;
    }
    // The old form is gone: the successor the verb's bracket captured is the new form. An old form
    // with no element id (a kerfur never enrolled) converges the same way: the bind mints its kerfur
    // id, and the successor, whose generic spawn its capture held back, reaches the peers by it.
    const auto cap = coop::kerfur_form_assembler::ConsumeCapturedForm(/*wantNpc=*/!toProp);
    if (toProp) ConvergeTurnOff(oldActor, oldEid, cap.actor, who);
    else ConvergeTurnOn(oldActor, oldEid, cap.actor, who);
}

void OnConvertRequest(const coop::net::KerfurConvertPayload& payload, uint8_t senderPeerSlot) {
    // Host-only (gated by the event_dispatch_intent router). Game thread (the event_feed drain): the
    // verb's dispatches, its gate watch's return included, run inside this call.
    if (!g_ready.load(std::memory_order_acquire)) {
        UE_LOGW("kerfur_convert: request before install resolved -- dropped");
        return;
    }
    // Every legitimate target is a host-range element (host props and NPCs are AllocHostId'd); an
    // out-of-range id is a spoof or a bug.
    if (!EL::Registry::IsAllowedHostAllocatedEid(static_cast<EL::ElementId>(payload.elementId))) {
        UE_LOGW("kerfur_convert: request eid=%u outside the host range -- dropped (slot %u)",
                payload.elementId, senderPeerSlot);
        return;
    }
    const auto eid = static_cast<EL::ElementId>(payload.elementId);
    const char* what = payload.toProp ? "turn_off" : "turn-on";
    EL::Element* el = payload.toProp
        ? static_cast<EL::Element*>(EL::MirrorManager<EL::Npc>::Instance().Get(eid))
        : static_cast<EL::Element*>(EL::MirrorManager<EL::Prop>::Instance().Get(eid));
    void* actor = el ? el->GetActor() : nullptr;
    if (!actor || !R::IsLiveByIndex(actor, el->GetInternalIdx())) {
        UE_LOGW("kerfur_convert: %s request eid=%u from slot %u -- no live kerfur of that form (already converted "
                "or stale) -- dropped", what, payload.elementId, senderPeerSlot);
        return;
    }
    void* cls = R::ClassOf(actor);
    void* base = payload.toProp ? g_kerfurNpcClass : g_kerfurPropClass;
    if (!cls || !R::IsDescendantOfAny(cls, &base, 1)) {
        UE_LOGW("kerfur_convert: %s request eid=%u targets no kerfur of that form -- dropped", what, payload.elementId);
        return;
    }
    // The BP's own guard before dropKerfurProp (actionName: `if (kill) return;`), replicated byte-exactly
    // from the disassembly, since the request calls the verb directly.
    if (payload.toProp && g_killOff >= 0 &&
        *(reinterpret_cast<const bool*>(reinterpret_cast<const uint8_t*>(actor) + g_killOff))) {
        UE_LOGI("kerfur_convert: turn_off eid=%u denied -- the kerfur is in kill mode (SP parity)", payload.elementId);
        return;
    }
    UE_LOGI("kerfur_convert: HOST running %s eid=%u for slot %u's request", what, payload.elementId, senderPeerSlot);
    uint8_t frame[16] = {};  // the verbs take no params (install-guarded); a zeroed frame for safety
    g_runningObject = actor;
    g_runningSlot = senderPeerSlot;
    R::CallFunction(actor, payload.toProp ? PickDropPropFn(cls) : g_spawnKerfuroFn, frame);
    g_runningObject = nullptr;
    g_runningSlot = -1;
}

void SetSession(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
}

void SetClasses(void* npcClass, void* propClass) {
    g_kerfurNpcClass  = npcClass;
    g_kerfurPropClass = propClass;
}

void SetVerbs(void* spawnKerfuroFn, int32_t killOff) {
    g_spawnKerfuroFn = spawnKerfuroFn;
    g_killOff        = killOff;
    g_ready.store(true, std::memory_order_release);
}

void OnDisconnect() {
    g_runningObject = nullptr;
    g_runningSlot = -1;
}

}  // namespace coop::kerfur_convert_host
