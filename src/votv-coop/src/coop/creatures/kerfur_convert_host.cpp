// coop/kerfur_convert_host.cpp -- the HOST executor half of the kerfur
// conversion feature: KerfurConvertRequest execution (verb dispatch + the
// request-verb bracket) and the post-verb converge (new-form search/register,
// BindFormActor -> KerfurConvert, floppy express). The feature narrative and the
// blueprint ground truth live in kerfur_convert.h.
// Interfaces: kerfur_convert_host.h. Class pointers arrive via SetClasses
// every Install attempt; verb refs + the request latch via SetVerbs at the
// Install SUCCESS site only (two-layer handoff -- see the header, incl. the
// documented fail-closed DISABLED-state deviation).

#include "coop/creatures/kerfur_convert_host.h"

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
void* g_floppyClass     = nullptr;
void* g_dropPropFnBase     = nullptr;
void* g_dropPropFnCol      = nullptr;
void* g_dropPropFnColGamer = nullptr;
void* g_colClass           = nullptr;
void* g_colGamerClass      = nullptr;
void* g_spawnKerfuroFn     = nullptr;
int32_t g_killOff          = -1;
std::atomic<bool> g_ready{false};  // the request latch (flips with the residual's success g_installed)

// ---- host-side converge (kerfur redesign K-4b) -------------------------------
// After the verb ran on the host (its own radial menu OR a client request), drive the SOLE
// conversion signal KerfurConvert: find the new-form actor (the verb spawned it via EX_CallMath --
// PE-invisible, so it is UNTRACKED), register it SILENTLY at a host-range eid, release the dying form
// SILENTLY, and BindFormActor (rebinds the stable KerfurId IN PLACE + broadcasts KerfurConvert). No
// EntityDestroy / PropSpawn for the kerfur itself (redesign 10.3 -- the kerfur is one entity, not a
// destroy+create across two pipelines). The dropped FLOPPY (a normal prop, NOT part of the kerfur
// identity) still rides the ordinary keyed ExpressSpawnedProp -> PropSpawn. Game thread.

// Find the host's untracked, live kerfur actor of the requested form nearest (x,y,z): the verb's
// freshly-spawned new-form body. UNTRACKED = not yet a host element (g_actorToNpcId / the prop
// tracker) -- the verb output, before we register it. One cold GUObjectArray walk per conversion.
void* FindNewFormKerfurActor(bool wantNpc, float x, float y, float z) {
    void* base = wantNpc ? g_kerfurNpcClass : g_kerfurPropClass;
    if (!base) return nullptr;
    const int32_t n = R::NumObjects();
    constexpr float kR2 = 500.f * 500.f;  // the new form spawns at the kerfur's own transform
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
        const ue_wrap::FVector loc = ue_wrap::engine::GetActorLocation(obj);
        const float dx = loc.X - x, dy = loc.Y - y, dz = loc.Z - z;
        const float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 < bestD2) { bestD2 = d2; best = obj; }
    }
    return best;
}

// Silent teardown of a dying host PROP form (the actor is already dead -- pointer-as-map-key only, no
// deref, no PropDestroy broadcast). KerfurConvert carries oldEid for the clients.
void ReleaseHostPropSilent(void* deadActor) {
    if (!deadActor) return;
    PT::UnmarkProcessedInit(deadActor);
    PT::UnmarkKnownKeyedProp(deadActor);  // drains the Prop Element + frees its eid (ABBA-safe)
}

// Pick the most-derived dropKerfurProp declarer for this actor's class --
// ProcessEvent runs exactly the UFunction passed, so this IS the virtual
// dispatch the BP's own by-name call would have done.
void* PickDropPropFn(void* cls) {
    if (g_dropPropFnColGamer && g_colGamerClass &&
        R::IsDescendantOfAny(cls, &g_colGamerClass, 1)) return g_dropPropFnColGamer;
    if (g_dropPropFnCol && g_colClass &&
        R::IsDescendantOfAny(cls, &g_colClass, 1)) {
        return g_dropPropFnCol;
    }
    if (!g_dropPropFnColGamer || !g_dropPropFnCol) {
        // If the actor IS a col variant but its override never resolved, the
        // base body still converts correctly minus the collar drop -- log it.
        static std::atomic<bool> sWarned{false};
        if (g_colClass && R::IsDescendantOfAny(cls, &g_colClass, 1) &&
            !sWarned.exchange(true)) {
            UE_LOGW("kerfur_convert: col-variant kerfur converted via BASE dropKerfurProp (override unresolved at install latch)");
        }
    }
    return g_dropPropFnBase;
}

// (take-9) request-verb bracket + converge handshake between OnConvertRequest and the destroy-edge
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

// turn_off may also drop the kerfur's carried FLOPPY (a normal prop_floppyDisc_C -- a plain keyed
// prop, NOT part of the kerfur identity). It too spawns BP-internally (Init POST misses it) -> express
// it the NORMAL keyed way (ExpressSpawnedProp -> PropSpawn), latch-deduped, only UNTRACKED, near the
// conversion site.
void ExpressConversionFloppies(float x, float y, float z) {
    if (!g_floppyClass) return;
    const int32_t n = R::NumObjects();
    constexpr float kR2 = 500.f * 500.f;
    int ingested = 0;
    for (int32_t i = 0; i < n; ++i) {
        void* obj = R::ObjectAt(i);
        if (!obj) continue;
        void* cls = R::ClassOf(obj);
        if (!cls || !R::IsDescendantOfAny(cls, &g_floppyClass, 1)) continue;
        if (!R::IsLive(obj)) continue;
        if (PT::GetPropElementIdForActor(obj) != coop::element::kInvalidId) continue;  // tracked
        if (R::NameStartsWith(R::NameOf(obj), L"Default__")) continue;
        const ue_wrap::FVector loc = ue_wrap::engine::GetActorLocation(obj);
        const float dx = loc.X - x, dy = loc.Y - y, dz = loc.Z - z;
        if (dx * dx + dy * dy + dz * dz > kR2) continue;
        coop::prop_lifecycle::ExpressSpawnedProp(obj);  // normal keyed PropSpawn (floppy is a plain prop)
        ++ingested;
    }
    if (ingested > 0)
        UE_LOGI("kerfur_convert: expressed %d dropped floppy prop(s) (normal keyed PropSpawn)", ingested);
}

// `capturedForm` is the DETERMINISTIC successor B, captured in-bracket at its
// FinishSpawningActor (coop/creatures/kerfur_form_assembler); when supplied and live it replaces
// the probabilistic FindNewFormKerfurActor(position) search. nullptr falls back to that search.
// Everything downstream -- the silent mint, the silent release, BindFormActor -> KerfurConvert --
// is the same either way: the capture only fixes WHICH B, not the converge.
void ConvergeAfterConversion(void* oldActor, int32_t oldIdx, coop::element::ElementId oldEid,
                             uint8_t toProp, float px, float py, float pz,
                             void* capturedForm, int32_t capturedIdx) {
    namespace KE = coop::kerfur_entity;
    // 2a-capture: if no explicit successor B was threaded in, pull the assembler's DETERMINISTIC
    // in-bracket B (a turn-ON wants the NPC successor; a turn-OFF wants the prop). This replaces
    // FindNewFormKerfurActor's proximity search on the request route. An empty slot (already
    // consumed by the destroy-edge first refusal, or a genuinely absent B) -> the legacy search.
    if (!capturedForm) {
        auto cap = coop::kerfur_form_assembler::ConsumeCapturedForm(/*wantNpc=*/toProp == 0);
        capturedForm = cap.actor;
        capturedIdx  = cap.idx;
    }
    const bool haveCaptured = capturedForm && R::IsLiveByIndex(capturedForm, capturedIdx);
    if (haveCaptured)
        UE_LOGI("kerfur_convert: 2a-capture converge (%s) eid=%u -> captured successor %p "
                "(deterministic; FindNewFormKerfurActor bypassed)",
                toProp ? "turn_off" : "turn-on", static_cast<unsigned>(oldEid), capturedForm);
    if (toProp) {
        // turn_off: NPC -> prop. The NPC should have died; a kerfur prop (+ maybe floppy) spawned at
        // its position. A SENTIENT kerfur refused -> the NPC is still live -> echo a reject.
        if (oldActor && R::IsLiveByIndex(oldActor, oldIdx)) {
            const auto loc = ue_wrap::engine::GetActorLocation(oldActor);
            const auto rot = ue_wrap::engine::GetActorRotation(oldActor);
            void* cls = R::ClassOf(oldActor);
            KE::BroadcastConvertRejected(oldEid, KE::Form::Npc, loc.X, loc.Y, loc.Z,
                                         rot.Pitch, rot.Yaw, rot.Roll,
                                         cls ? R::ToString(R::NameOf(cls)) : std::wstring());
            return;
        }
        void* newProp = haveCaptured ? capturedForm : FindNewFormKerfurActor(/*wantNpc=*/false, px, py, pz);
        if (!newProp) {
            UE_LOGW("kerfur_convert: turn_off converge -- no new kerfur prop near (%.0f,%.0f,%.0f); releasing dead NPC eid=%u (no broadcast)",
                    px, py, pz, static_cast<uint32_t>(oldEid));
            coop::npc_sync::ReleaseNpcElementSilent(oldEid);
            KE::ReleaseKerfurForEid(oldEid);  // no successor: the kerfur is gone, not converted
            return;
        }
        const auto loc = ue_wrap::engine::GetActorLocation(newProp);
        const auto rot = ue_wrap::engine::GetActorRotation(newProp);
        void* ncls = R::ClassOf(newProp);
        const std::wstring cls = ncls ? R::ToString(R::NameOf(ncls)) : std::wstring();
        const coop::element::ElementId newEid = coop::prop_lifecycle::RegisterHostPropSilent(newProp);
        if (newEid == coop::element::kInvalidId) {
            UE_LOGW("kerfur_convert: turn_off converge -- RegisterHostPropSilent failed for prop %p; releasing NPC eid=%u",
                    newProp, static_cast<uint32_t>(oldEid));
            coop::npc_sync::ReleaseNpcElementSilent(oldEid);
            KE::ReleaseKerfurForEid(oldEid);  // the bind never runs: nothing will re-point the record
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
            const auto loc = ue_wrap::engine::GetActorLocation(oldActor);
            const auto rot = ue_wrap::engine::GetActorRotation(oldActor);
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
        const auto loc = ue_wrap::engine::GetActorLocation(newNpc);
        const auto rot = ue_wrap::engine::GetActorRotation(newNpc);
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
    // Host-only (gated by the event_dispatch_state router). Game thread (the
    // event_feed drain) -- ProcessEvent calls are legal here, and because the
    // drain runs INSIDE the pump task (t_inPump set), the verb's nested
    // dispatches cannot re-enter the pump: the converge below always runs
    // strictly after the verb returns.
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
    // 2a-capture: start this request's CallFunction bracket with an EMPTY capture slot (the 0x45
    // route self-clears at OnVerbEntry; this route is 0x45-blind, so clear here) -- the successor B
    // spawned INSIDE the verb below is the only capture the destroy-edge / converge may consume.
    coop::kerfur_form_assembler::ClearCapturedForm();
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
        // can no longer read it; the new-form prop spawns at this position (position continuity).
        const ue_wrap::FVector pos0 = ue_wrap::engine::GetActorLocation(actor);
        uint8_t frame[16] = {};  // verbs take no params (install-guarded); zeroed frame for safety
        R::CallFunction(actor, PickDropPropFn(cls), frame);
        ConvergeAfterConversion(actor, idx, eid, /*toProp=*/1, pos0.X, pos0.Y, pos0.Z);
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
        // Capture the prop's pose BEFORE the verb -- it spawns the NPC at this position then
        // K2_DestroyActor's the prop (position continuity for the converge).
        const ue_wrap::FVector pos0 = ue_wrap::engine::GetActorLocation(actor);
        uint8_t frame[16] = {};  // spawnKerfuro takes no params (install-guarded)
        // (take-9) bracket the verb: the prop's K2_DestroyActor INSIDE it hits the destroy seam,
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
        ConvergeAfterConversion(actor, idx, eid, /*toProp=*/0, pos0.X, pos0.Y, pos0.Z);
    }
}

coop::element::ElementId ActiveRequestVerbEid() {
    // GT-only marker set around the OnConvertRequest CallFunction bracket (above). The 0x45
    // assembler is blind to the CallFunction dispatch, so it consults this to recognize the
    // host-exec-client-request bracket as a SECOND capture scope. See the header.
    return (g_requestVerbEid == 0xFFFFFFFFu)
               ? coop::element::kInvalidId
               : static_cast<coop::element::ElementId>(g_requestVerbEid);
}

// Record the converge for OnConvertRequest ONLY when we are inside ITS verb bracket -- a
// host-OWN toggle has no consumer and an unconditional write would leave a stale eid that a
// later recycled-eid request could falsely consume. One-way owner API for the residual destroy
// seam; the bracket predicate lives here, next to the state.
void RecordSeamConvergedInBracket(coop::element::ElementId dyingEid) {
    if (g_requestVerbEid == static_cast<uint32_t>(dyingEid))
        g_seamConvergedEid = static_cast<uint32_t>(dyingEid);
}

void SetClasses(void* npcClass, void* propClass, void* floppyClass) {
    g_kerfurNpcClass  = npcClass;
    g_kerfurPropClass = propClass;
    g_floppyClass     = floppyClass;
}

void SetVerbs(void* dropPropFnBase, void* dropPropFnCol, void* dropPropFnColGamer,
              void* colClass, void* colGamerClass, void* spawnKerfuroFn, int32_t killOff) {
    g_dropPropFnBase     = dropPropFnBase;
    g_dropPropFnCol      = dropPropFnCol;
    g_dropPropFnColGamer = dropPropFnColGamer;
    g_colClass           = colClass;
    g_colGamerClass      = colGamerClass;
    g_spawnKerfuroFn     = spawnKerfuroFn;
    g_killOff            = killOff;
    g_ready.store(true, std::memory_order_release);
}

void OnDisconnect() {
    g_seamConvergedEid = 0xFFFFFFFFu;  // GT-only destroy-edge converge handshake (take-9)
    g_requestVerbEid   = 0xFFFFFFFFu;
}

}  // namespace coop::kerfur_convert_host
