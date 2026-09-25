// coop/creatures/kerfur_convert_host.cpp -- the HOST executor half of the kerfur
// conversion feature: KerfurConvertRequest execution (verb dispatch + the
// request-verb bracket) and the post-verb converge (new-form search/register,
// BindFormActor -> KerfurConvert, floppy express). The feature narrative and the
// blueprint ground truth live in kerfur_convert.h.
// Interfaces: kerfur_convert_host.h. Class pointers arrive via SetClasses
// every Install attempt; verb refs + the request latch via SetVerbs at the
// Install SUCCESS site only (two-layer handoff -- see the header, incl. the
// documented fail-closed DISABLED-state deviation).

#include "coop/creatures/kerfur_convert_host.h"

#include "coop/creatures/kerfur_convert.h"  // LastLiveLocation/Rotation, the request's fallback

#include "coop/creatures/kerfur_form_assembler.h"
#include "coop/creatures/kerfur_entity.h"
#include "coop/creatures/npc_sync.h"
#include "coop/element/mirror_manager.h"
#include "coop/element/npc.h"
#include "coop/element/prop.h"
#include "coop/element/registry.h"
#include "coop/net/protocol.h"
#include "coop/props/prop_element_tracker.h"
#include "coop/props/prop_lifecycle.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"

#include <atomic>
#include <cstdint>
#include <string>

namespace coop::kerfur_convert_host {
namespace {

namespace R  = ue_wrap::reflection;
namespace PT = coop::prop_element_tracker;

// Resolved kerfur refs (pushed by kerfur_convert::Install: SetClasses every
// attempt, SetVerbs at the success site; read-only on the game thread here).
void* g_kerfurNpcClass  = nullptr;
void* g_kerfurPropClass = nullptr;
void* g_spawnKerfuroFn     = nullptr;
int32_t g_killOff          = -1;
std::atomic<bool> g_ready{false};  // the request latch (flips with the residual's success g_installed)

// ---- host-side converge -------------------------------------------------------
// After the verb ran on the host (its own radial menu OR a client request), drive the SOLE
// conversion signal KerfurConvert: find the new-form actor (the verb spawned it via EX_CallMath --
// PE-invisible), register it SILENTLY at a host-range eid, release the dying form SILENTLY, and
// BindFormActor (rebinds the stable KerfurId IN PLACE + broadcasts KerfurConvert). No EntityDestroy
// / PropSpawn for a converted kerfur -- the kerfur is one entity, not a destroy+create across two
// pipelines; a turn-off that cannot bind its new form retires the NPC as a plain death instead. The
// dropped FLOPPY (a normal prop, NOT part of the kerfur identity) still rides the ordinary keyed
// ExpressSpawnedProp -> PropSpawn. Game thread.

// Find the host's untracked, live kerfur actor of the requested form nearest (x,y,z): the verb's
// freshly-spawned new-form body. UNTRACKED = not yet a host element (g_actorToNpcId / the prop
// tracker) -- the verb output, before we register it. One cold GUObjectArray walk per conversion.
void* FindNewFormKerfurActor(bool wantNpc, float x, float y, float z) {
    void* base = wantNpc ? g_kerfurNpcClass : g_kerfurPropClass;
    if (!base) return nullptr;
    const int32_t n = R::NumObjects();
    constexpr float kR2 = 500.f * 500.f;  // the verb's new form spawns about the old one (not in the flesh room)
    void* best = nullptr;
    float bestD2 = kR2;
    for (int32_t i = 0; i < n; ++i) {
        void* obj = R::ObjectAt(i);
        if (!obj) continue;
        void* cls = R::ClassOf(obj);
        if (!cls || !R::IsDescendantOfAny(cls, &base, 1)) continue;
        if (!R::IsLive(obj)) continue;
        if (R::NameStartsWith(R::NameOf(obj), L"Default__")) continue;
        const bool tracked = wantNpc
            ? (coop::npc_sync::GetNpcIdForActor(obj) != coop::element::kInvalidId)
            : (PT::GetPropElementIdForActor(obj) != coop::element::kInvalidId);
        if (tracked) continue;
        ue_wrap::FVector loc{};
        if (!ue_wrap::engine::TryGetActorLocation(obj, loc)) continue;  // unplaceable: never the nearest
        const float dx = loc.X - x, dy = loc.Y - y, dz = loc.Z - z;
        const float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 < bestD2) { bestD2 = d2; best = obj; }
    }
    return best;
}

// The pose a reject sends for the old form, which survived the verb: each half its own read, else that
// half of the pose this converge was given, the form's own last one (read before the verb, or by the
// death-watch while it lived). False when the rotation is unread and none was given: there is no pose
// to restore the form at. `what` names the case in the warning.
bool RejectPose(void* oldForm, const char* what, coop::element::ElementId oldEid, float px, float py, float pz,
                const ue_wrap::FRotator* rot0, ue_wrap::FVector& loc, ue_wrap::FRotator& rot) {
    const bool locRead = ue_wrap::engine::TryGetActorLocation(oldForm, loc);
    const bool rotRead = ue_wrap::engine::TryGetActorRotation(oldForm, rot);
    if (locRead && rotRead) return true;
    if (!rotRead && !rot0) {
        UE_LOGW("kerfur_convert: %s eid=%u -- its rotation could not be read and the converge was given none",
                what, static_cast<unsigned>(oldEid));
        return false;
    }
    UE_LOGW("kerfur_convert: %s eid=%u -- its %s could not be read; its last one is used", what,
            static_cast<unsigned>(oldEid), !locRead && !rotRead ? "location and rotation" : !locRead ? "location" : "rotation");
    if (!locRead) loc = ue_wrap::FVector{px, py, pz};
    if (!rotRead) rot = *rot0;
    return true;
}

// The pose the converge binds the new form at: its own, read whole. No pose of the old form's stands
// in for it, since the verb places the new form itself: the turn-on stands the NPC upright 50 cm above
// the prop's spawn point, keeping only its yaw, and the turn-off drops the prop at (0,0,20000) in the
// flesh room. `what` names the case in the warning.
bool NewFormPose(void* newForm, const char* what, coop::element::ElementId oldEid, ue_wrap::FVector& loc,
                 ue_wrap::FRotator& rot) {
    const bool locRead = ue_wrap::engine::TryGetActorLocation(newForm, loc);
    if (locRead && ue_wrap::engine::TryGetActorRotation(newForm, rot)) return true;
    UE_LOGW("kerfur_convert: %s eid=%u -- its %s could not be read; the converge fails", what,
            static_cast<unsigned>(oldEid), locRead ? "rotation" : "location");
    return false;
}

// Silent teardown of a dying host PROP form (the actor is already dead -- pointer-as-map-key only, no
// deref, no PropDestroy broadcast). KerfurConvert carries oldEid for the clients.
void ReleaseHostPropSilent(void* deadActor) {
    if (!deadActor) return;
    PT::UnmarkProcessedInit(deadActor);
    PT::UnmarkKnownKeyedProp(deadActor);  // drains the Prop Element + frees its eid (ABBA-safe)
}

// The most-derived dropKerfurProp declarer for this actor's class, a collar variant's override
// included: ProcessEvent runs exactly the UFunction passed, so this IS the virtual dispatch the BP's
// own by-name call would have done. The base declares it, so a kerfur always has one.
void* PickDropPropFn(void* cls) {
    return R::FindDispatchFunctionCached(cls, L"dropKerfurProp");
}

// Request-verb bracket + converge handshake between OnConvertRequest and the destroy-edge
// first refusal. The seam fires INSIDE the request-executed verb, so OnConvertRequest would run its
// explicit ConvergeAfterConversion a second time right after the verb returns -- and the fresh
// NPC, now tracked by the seam converge, would read as "no new kerfur NPC near" (a spurious
// release + WARN). g_requestVerbEid brackets the CallFunction; the capture records into
// g_seamConvergedEid ONLY when it fires inside the matching bracket: an unconditional write
// would leave one stale eid per host-OWN toggle with nobody to consume it, and a later request
// whose recycled eid matched would skip its converge AND its reject echo. Verbs are
// synchronous on the game thread, so single slots cannot be raced.
uint32_t g_requestVerbEid   = 0xFFFFFFFFu;  // eid of the request-path verb currently executing; GT-only
uint32_t g_seamConvergedEid = 0xFFFFFFFFu;  // set by the capture inside that bracket; GT-only

bool ConsumeSeamConverged(uint32_t eid) {
    if (g_seamConvergedEid != eid) return false;
    g_seamConvergedEid = 0xFFFFFFFFu;
    return true;
}

}  // namespace

void RetireNpcFormAsDeath(coop::element::ElementId eid, void* actor) {
    coop::npc_sync::SyncDestroyedNpcByEid(eid, actor);   // the release and an EntityDestroy to every peer
    coop::kerfur_entity::ReleaseKerfurForEid(eid);
}

// turn_off may also drop the kerfur's carried FLOPPY (a normal prop_floppyDisc_C -- a plain keyed
// prop, NOT part of the kerfur identity). It too spawns BP-internally (Init POST misses it) -> express
// it the NORMAL keyed way (ExpressSpawnedProp -> PropSpawn), latch-deduped, only UNTRACKED, near the
// conversion site.
void ExpressConversionFloppies(float x, float y, float z) {
    void* floppyCls = ue_wrap::object_index::ClassByName(L"prop_floppyDisc_C");
    if (!floppyCls) return;
    const int32_t n = R::NumObjects();
    constexpr float kR2 = 500.f * 500.f;
    int ingested = 0;
    for (int32_t i = 0; i < n; ++i) {
        void* obj = R::ObjectAt(i);
        if (!obj) continue;
        void* cls = R::ClassOf(obj);
        if (!cls || !R::IsDescendantOfAny(cls, &floppyCls, 1)) continue;
        if (!R::IsLive(obj)) continue;
        if (PT::GetPropElementIdForActor(obj) != coop::element::kInvalidId) continue;  // tracked
        if (R::NameStartsWith(R::NameOf(obj), L"Default__")) continue;
        ue_wrap::FVector loc{};
        if (!ue_wrap::engine::TryGetActorLocation(obj, loc)) continue;  // unplaceable: not within the radius
        const float dx = loc.X - x, dy = loc.Y - y, dz = loc.Z - z;
        if (dx * dx + dy * dy + dz * dz > kR2) continue;
        coop::prop_lifecycle::ExpressSpawnedProp(obj);  // normal keyed PropSpawn (floppy is a plain prop)
        ++ingested;
    }
    if (ingested > 0)
        UE_LOGI("kerfur_convert: expressed %d dropped floppy prop(s) (normal keyed PropSpawn)", ingested);
}

// The successor B is the DETERMINISTIC one the form assembler captured in the verb's bracket at its
// FinishSpawningActor (coop/creatures/kerfur_form_assembler), when that is still live; else the
// FindNewFormKerfurActor(position) search. Everything downstream -- the silent mint, the silent
// release, BindFormActor -> KerfurConvert -- is the same either way: the capture only fixes WHICH B.
void ConvergeAfterConversion(void* oldActor, int32_t oldIdx, coop::element::ElementId oldEid,
                             uint8_t toProp, float px, float py, float pz, const ue_wrap::FRotator* rot0) {
    namespace KE = coop::kerfur_entity;
    // A turn-ON wants the NPC successor, a turn-OFF the prop. An empty slot (already consumed by the
    // destroy-edge first refusal, or a genuinely absent B) -> the search.
    const auto cap = coop::kerfur_form_assembler::ConsumeCapturedForm(/*wantNpc=*/toProp == 0);
    void* const capturedForm = cap.actor;
    const int32_t capturedIdx = cap.idx;
    const bool haveCaptured = capturedForm && R::IsLiveByIndex(capturedForm, capturedIdx);
    if (haveCaptured)
        UE_LOGI("kerfur_convert: 2a-capture converge (%s) eid=%u -> captured successor %p "
                "(deterministic; FindNewFormKerfurActor bypassed)",
                toProp ? "turn_off" : "turn-on", static_cast<unsigned>(oldEid), capturedForm);
    if (toProp) {
        // turn_off: NPC -> prop. The NPC should have died; a kerfur prop (+ maybe floppy) spawned at
        // its transform, or at (0,0,20000) in the flesh room. A SENTIENT kerfur refused -> the NPC is
        // still live -> echo a reject.
        if (oldActor && R::IsLiveByIndex(oldActor, oldIdx)) {
            // The reject still goes out: unread, at the form's own last pose; with no rotation at all it
            // cannot place the restored form, and is not sent.
            ue_wrap::FVector loc{};
            ue_wrap::FRotator rot{};
            if (!RejectPose(oldActor, "turn_off reject (the NPC)", oldEid, px, py, pz, rot0, loc, rot)) return;
            void* cls = R::ClassOf(oldActor);
            KE::BroadcastConvertRejected(oldEid, KE::Form::Npc, loc.X, loc.Y, loc.Z,
                                         rot.Pitch, rot.Yaw, rot.Roll,
                                         cls ? R::ToString(R::NameOf(cls)) : std::wstring());
            return;
        }
        // The NPC died inside the verb, a destroy npc_sync's observer does not see. A converge that
        // cannot bind the new prop retires it as a plain death: no KerfurConvert will carry its eid,
        // and a silent release would orphan every client's mirror of it.
        void* newProp = haveCaptured ? capturedForm : FindNewFormKerfurActor(/*wantNpc=*/false, px, py, pz);
        if (!newProp) {
            UE_LOGW("kerfur_convert: turn_off converge -- no new kerfur prop near (%.0f,%.0f,%.0f); dead NPC eid=%u "
                    "retired as a plain death", px, py, pz, static_cast<uint32_t>(oldEid));
            RetireNpcFormAsDeath(oldEid, oldActor);   // no successor: the kerfur is gone, not converted
            return;
        }
        ue_wrap::FVector loc{};
        ue_wrap::FRotator rot{};
        if (!NewFormPose(newProp, "turn_off converge (the new prop)", oldEid, loc, rot)) {
            RetireNpcFormAsDeath(oldEid, oldActor);
            return;
        }
        void* ncls = R::ClassOf(newProp);
        const std::wstring cls = ncls ? R::ToString(R::NameOf(ncls)) : std::wstring();
        const coop::element::ElementId newEid = coop::prop_lifecycle::RegisterHostPropSilent(newProp);
        if (newEid == coop::element::kInvalidId) {
            UE_LOGW("kerfur_convert: turn_off converge -- RegisterHostPropSilent failed for prop %p; dead NPC eid=%u "
                    "retired as a plain death", newProp, static_cast<uint32_t>(oldEid));
            RetireNpcFormAsDeath(oldEid, oldActor);   // the bind never runs: nothing re-points the record
            return;
        }
        coop::npc_sync::ReleaseNpcElementSilent(oldEid);
        KE::BindFormActor(oldEid, newProp, R::InternalIndexOf(newProp), newEid, KE::Form::Prop, cls,
                          loc.X, loc.Y, loc.Z, rot.Pitch, rot.Yaw, rot.Roll);
        ExpressConversionFloppies(px, py, pz);
    } else {
        // turn on: prop -> NPC. The prop should have died; a kerfur NPC spawned. Spawn failure (the BP
        // hint path) leaves the prop alive -> echo a reject.
        if (oldActor && R::IsLiveByIndex(oldActor, oldIdx)) {
            ue_wrap::FVector loc{};   // the reject still goes out, as above
            ue_wrap::FRotator rot{};
            if (!RejectPose(oldActor, "turn-on reject (the prop)", oldEid, px, py, pz, rot0, loc, rot)) return;
            void* cls = R::ClassOf(oldActor);
            KE::BroadcastConvertRejected(oldEid, KE::Form::Prop, loc.X, loc.Y, loc.Z,
                                         rot.Pitch, rot.Yaw, rot.Roll,
                                         cls ? R::ToString(R::NameOf(cls)) : std::wstring());
            return;
        }
        void* newNpc = haveCaptured ? capturedForm : FindNewFormKerfurActor(/*wantNpc=*/true, px, py, pz);
        if (!newNpc) {
            UE_LOGW("kerfur_convert: turn-on converge -- no new kerfur NPC near (%.0f,%.0f,%.0f); releasing dead prop eid=%u (no broadcast)",
                    px, py, pz, static_cast<uint32_t>(oldEid));
            ReleaseHostPropSilent(oldActor);
            KE::ReleaseKerfurForEid(oldEid);  // no successor: the kerfur is gone, not converted
            return;
        }
        ue_wrap::FVector loc{};
        ue_wrap::FRotator rot{};
        if (!NewFormPose(newNpc, "turn-on converge (the new NPC)", oldEid, loc, rot)) {
            ReleaseHostPropSilent(oldActor);   // as a failed register: nothing re-points the record
            KE::ReleaseKerfurForEid(oldEid);
            return;
        }
        void* ncls = R::ClassOf(newNpc);
        const std::wstring cls = ncls ? R::ToString(R::NameOf(ncls)) : std::wstring();
        const coop::element::ElementId newEid = coop::npc_sync::RegisterHostNpcSilent(newNpc, cls);
        if (newEid == coop::element::kInvalidId) {
            UE_LOGW("kerfur_convert: turn-on converge -- RegisterHostNpcSilent failed for NPC %p; releasing prop eid=%u",
                    newNpc, static_cast<uint32_t>(oldEid));
            ReleaseHostPropSilent(oldActor);
            KE::ReleaseKerfurForEid(oldEid);  // the bind never runs: nothing will re-point the record
            return;
        }
        ReleaseHostPropSilent(oldActor);
        KE::BindFormActor(oldEid, newNpc, R::InternalIndexOf(newNpc), newEid, KE::Form::Npc, cls,
                          loc.X, loc.Y, loc.Z, rot.Pitch, rot.Yaw, rot.Roll);
    }
}

void OnConvertRequest(const coop::net::KerfurConvertPayload& payload,
                      uint8_t senderPeerSlot) {
    // Host-only (gated by the event_dispatch_intent router). Game thread (the
    // event_feed drain) -- ProcessEvent calls are legal here, and because the
    // drain runs INSIDE the pump task, the verb's dispatches are nested in the
    // one that drains it and cannot re-enter the pump: the converge below
    // always runs strictly after the verb returns.
    if (!g_ready.load(std::memory_order_acquire)) {
        UE_LOGW("kerfur_convert: request before install resolved -- dropped");
        return;
    }
    // Every legitimate target is a host-range element (host props and NPCs are
    // AllocHostId'd); reject out-of-range ids loudly (a spoof or a bug).
    if (!coop::element::Registry::IsAllowedHostAllocatedEid(
            static_cast<coop::element::ElementId>(payload.elementId))) {
        UE_LOGW("kerfur_convert: request eid=%u outside the host range -- dropped (slot %u)",
                payload.elementId, senderPeerSlot);
        return;
    }
    const auto eid = static_cast<coop::element::ElementId>(payload.elementId);
    if (payload.toProp) {
        auto* el = coop::element::MirrorManager<coop::element::Npc>::Instance().Get(eid);
        void* actor = el ? el->GetActor() : nullptr;
        const int32_t idx = el ? el->GetInternalIdx() : -1;
        if (!actor || !R::IsLiveByIndex(actor, idx)) {
            UE_LOGW("kerfur_convert: turn_off request eid=%u from slot %u -- no live NPC (already converted / stale) -- dropped",
                    payload.elementId, senderPeerSlot);
            return;
        }
        void* cls = R::ClassOf(actor);
        if (!cls || !R::IsDescendantOfAny(cls, &g_kerfurNpcClass, 1)) {
            UE_LOGW("kerfur_convert: turn_off request eid=%u targets a non-kerfur element -- dropped", payload.elementId);
            return;
        }
        // The BP's own guard (actionName ubergraph: `if (kill) return;` before
        // dropKerfurProp) -- replicated byte-exactly from the disassembly.
        if (g_killOff >= 0 &&
            *(reinterpret_cast<const bool*>(reinterpret_cast<const uint8_t*>(actor) + g_killOff))) {
            UE_LOGI("kerfur_convert: turn_off eid=%u denied -- kerfur is in kill mode (SP parity)", payload.elementId);
            return;
        }
        UE_LOGI("kerfur_convert: HOST executing turn_off eid=%u (slot %u)", payload.elementId, senderPeerSlot);
        // Capture the kerfur's pose BEFORE the verb -- it K2_DestroyActor's the NPC, so the converge
        // can no longer read it. The location centres the converge's successor search and floppy
        // express, and a reject restores the kerfur at this pose; each half unread falls back to the
        // death-watch's last live one, and a kerfur with no location is refused.
        ue_wrap::FVector pos0{};
        ue_wrap::FRotator rot0{};
        const bool posKnown = ue_wrap::engine::TryGetActorLocation(actor, pos0) ||
                              coop::kerfur_convert::LastLiveLocation(payload.elementId, actor, pos0);
        const bool rotKnown = ue_wrap::engine::TryGetActorRotation(actor, rot0) ||
                              coop::kerfur_convert::LastLiveRotation(payload.elementId, actor, rot0);
        if (!posKnown) {
            UE_LOGW("kerfur_convert: turn_off request eid=%u from slot %u refused -- the kerfur has no readable or "
                    "last live location for the converge", payload.elementId, senderPeerSlot);
            return;
        }
        void* const dropFn = PickDropPropFn(cls);
        if (!dropFn) {
            UE_LOGW("kerfur_convert: turn_off request eid=%u from slot %u refused -- its class declares no "
                    "dropKerfurProp", payload.elementId, senderPeerSlot);
            return;
        }
        uint8_t frame[16] = {};  // verbs take no params (install-guarded); zeroed frame for safety
        R::CallFunction(actor, dropFn, frame);
        ConvergeAfterConversion(actor, idx, eid, /*toProp=*/1, pos0.X, pos0.Y, pos0.Z, rotKnown ? &rot0 : nullptr);
    } else {
        auto* el = coop::element::MirrorManager<coop::element::Prop>::Instance().Get(eid);
        void* actor = el ? el->GetActor() : nullptr;
        const int32_t idx = el ? el->GetInternalIdx() : -1;
        if (!actor || !R::IsLiveByIndex(actor, idx)) {
            UE_LOGW("kerfur_convert: turn-on request eid=%u from slot %u -- no live prop (already converted / stale) -- dropped",
                    payload.elementId, senderPeerSlot);
            return;
        }
        void* cls = R::ClassOf(actor);
        if (!cls || !R::IsDescendantOfAny(cls, &g_kerfurPropClass, 1)) {
            UE_LOGW("kerfur_convert: turn-on request eid=%u targets a non-kerfur-prop element -- dropped", payload.elementId);
            return;
        }
        UE_LOGI("kerfur_convert: HOST executing turn-on eid=%u (slot %u)", payload.elementId, senderPeerSlot);
        // Capture the prop's pose BEFORE the verb -- it spawns the NPC above the prop's spawn point and
        // then K2_DestroyActor's the prop -- for the converge, as above.
        ue_wrap::FVector pos0{};
        ue_wrap::FRotator rot0{};
        const bool posKnown = ue_wrap::engine::TryGetActorLocation(actor, pos0) ||
                              coop::kerfur_convert::LastLiveLocation(payload.elementId, actor, pos0);
        const bool rotKnown = ue_wrap::engine::TryGetActorRotation(actor, rot0) ||
                              coop::kerfur_convert::LastLiveRotation(payload.elementId, actor, rot0);
        if (!posKnown) {
            UE_LOGW("kerfur_convert: turn-on request eid=%u from slot %u refused -- the prop has no readable or "
                    "last live location for the converge", payload.elementId, senderPeerSlot);
            return;
        }
        uint8_t frame[16] = {};  // spawnKerfuro takes no params (install-guarded)
        // Bracket the verb: the prop's K2_DestroyActor INSIDE it hits the destroy seam,
        // whose kerfur first refusal converges inline and records the eid -- consumed just below
        // instead of double-converging (the NPC is tracked by then; the search in
        // ConvergeAfterConversion would spuriously fail and release the eid with a WARN).
        g_requestVerbEid = static_cast<uint32_t>(eid);
        R::CallFunction(actor, g_spawnKerfuroFn, frame);
        g_requestVerbEid = 0xFFFFFFFFu;
        if (ConsumeSeamConverged(static_cast<uint32_t>(eid))) {
            UE_LOGI("kerfur_convert: turn-on eid=%u already converged at the destroy edge (first refusal) -- request satisfied",
                    payload.elementId);
            return;
        }
        ConvergeAfterConversion(actor, idx, eid, /*toProp=*/0, pos0.X, pos0.Y, pos0.Z, rotKnown ? &rot0 : nullptr);
    }
}

// Record the converge for OnConvertRequest ONLY when we are inside ITS verb bracket -- a
// host-OWN toggle has no consumer and an unconditional write would leave a stale eid that a
// later recycled-eid request could falsely consume. One-way owner API for the residual destroy
// seam; the bracket predicate lives here, next to the state.
void RecordSeamConvergedInBracket(coop::element::ElementId dyingEid) {
    if (g_requestVerbEid == static_cast<uint32_t>(dyingEid))
        g_seamConvergedEid = static_cast<uint32_t>(dyingEid);
}

void SetClasses(void* npcClass, void* propClass) {
    g_kerfurNpcClass  = npcClass;
    g_kerfurPropClass = propClass;
}

void SetVerbs(void* spawnKerfuroFn, int32_t killOff) {
    g_spawnKerfuroFn     = spawnKerfuroFn;
    g_killOff            = killOff;
    g_ready.store(true, std::memory_order_release);
}

void OnDisconnect() {
    g_seamConvergedEid = 0xFFFFFFFFu;  // GT-only destroy-edge converge handshake
    g_requestVerbEid   = 0xFFFFFFFFu;
}

}  // namespace coop::kerfur_convert_host
