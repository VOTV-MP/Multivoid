// coop/trash_collect_sync.cpp -- see coop/trash_collect_sync.h.
//
// One function: EnsureHeldItemBroadcast. net_pump's held-prop send calls it on
// the new-held edge. The collect itself (trashBitsPile_C::playerTryToCollect)
// is BP-internal (ProcessInternal) so it can't be observed; we instead act on
// the grabbing_actor the send already resolves -- a freshly-spawned, auto-
// grabbed Aprop_C with Key=None gets a stable Key + a PropSpawn here, then the
// existing pose stream mirrors it into the collector's hands.

#include "coop/trash_collect_sync.h"

#include "coop/kerfur_entity.h"  // K-5: IsKerfurActor (the held-kerfur class-gate)
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/prop_element_tracker.h"
#include "coop/prop_synth_key.h"
#include "coop/remote_prop.h"        // ResolveMirrorEidByActor (the pile-grab hook mirror eid resolve)
#include "coop/remote_prop_spawn.h"
#include "coop/trash_channel.h"      // NotePendingGrab (the VISIBLE-seam grab->clump link; docs/piles/08)
#include "ue_wrap/engine.h"
#include "ue_wrap/game_thread.h"     // RegisterPreObserver (the InpActEvt_use pile-grab observer)
#include "ue_wrap/log.h"
#include "ue_wrap/prop.h"
#include "ue_wrap/reflection.h"
#include "ue_wrap/sdk_profile.h"     // MainPlayerClass + MainPlayerUseInputEventFn
#include "ue_wrap/types.h"
#include "ue_wrap/ufunction_hook.h"  // the deterministic re-pile: BeginDeferred Func-patch (docs/piles/08)

#include <atomic>
#include <chrono>
#include <string>

namespace coop::trash_collect_sync {
namespace {

namespace R  = ue_wrap::reflection;
namespace PT = coop::prop_element_tracker;
namespace GT = ue_wrap::game_thread;
namespace P  = ue_wrap::profile;

// Cached Session for the pile-grab observer (the PRE callback can't take a param). Set by Install
// (re-cached every call for reconnect). Game-thread read inside the observer.
std::atomic<coop::net::Session*> g_session{nullptr};
bool g_grabObserverInstalled = false;  // InpActEvt_use PRE registration latch (stays for process life)
bool g_dropGrabThunkInstalled = false;  // read-only dropGrabObject Func-thunk latch (the PHC-grab release seam)

// RULE 1+2 (2026-06-21, docs/piles/08): the v81 MORPH (proximity FindNearestChipPile land-detect) is FULLY
// RETIRED -- it mis-bound on a NEIGHBOUR pile in a dense cluster. The host-grab sync is now: the InpActEvt
// PRE observer (OnPileGrabPre) records the aimed pile's eid (trash_channel::NotePendingGrab); the held-edge
// adopts the spawned clump onto that eid; identity is the host eid end-to-end (POSITION IS NEVER IDENTITY).
// (An earlier design caught the spawn at a host_spawn_watcher BeginDeferred POST -- RETIRED 2026-06-21: the
// chipPile/clump spawn is dispatched EX_CallMath, INVISIBLE to our ProcessEvent hook; it never fired.)

// (The proximity RE-PILE death-watch -- WatchClumpForRepile / Tick / FindNearestUntrackedChipPile_ /
// g_watchedClumps -- is RETIRED 2026-06-21, RULE 2. It converted on the clump's DEATH by a nearest-untracked
// pile search; the deterministic UFunction::Func thunk (OnBeginDeferredSpawnObserve) replaced it after a
// hands-on validated the thunk's *Result is ptr-for-ptr the same pile the death-watch found. The thunk
// converts the EXACT spawned pile the same tick -- no proximity, no reaper race. A consumed clump (no
// re-pile spawn) now just dies untracked -> the normal prop reaper PropDestroys it.)

bool g_repileThunkInstalled = false;          // process-lifetime Func-patch latch

// THE DETERMINISTIC RE-PILE (docs/piles/08 "thunk") -- the convert, validated GREEN 2026-06-21 (host log:
// many CLEAN [REPILE], the thunk's *Result ptr-for-ptr == the death-watch's FindNearest pile every isolated
// re-pile). On a host re-pile the clump's `EX_CallMath BeginDeferredActorSpawnFromClass(self=clump, pile)`
// fires our UFunction::Func patch -> (srcObj = FFrame::Object = the re-piling clump @0x18; newActor =
// *Result = the new chipPile). If the clump is a TRACKED trash entity (eid E) we convert E onto the new pile
// IN PLACE, the SAME tick the pile is constructed -- zero proximity, zero death-watch, no reaper race (so the
// old ~5s vanish-return is gone by construction). The GRAB case (srcObj=pile, newActor=clump) is NOT a clump
// source -> ignored here; the host grab stays on the InpActEvt PRE + held-edge adopt path. A wild srcObj (a
// wrong offset -- ruled out, IDA-pinned + validated) would fault in IsGarbageClump and the facility's
// RunCbSEH absorbs it -> no convert. Game thread (BeginDeferred is GT-only); host-only.
void OnBeginDeferredSpawnObserve(void* srcObj, void* newActor) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected() || s->role() != coop::net::Role::Host) return;  // host authors converts
    if (!srcObj || !newActor) return;
    if (!ue_wrap::prop::IsGarbageClump(srcObj)) return;   // re-pile source must be a clump (grab case is a pile -> skip)
    if (!ue_wrap::prop::IsChipPile(newActor)) return;     // ... spawning a chipPile
    const coop::element::ElementId E = PT::GetPropElementIdForActor(srcObj);
    if (E == coop::element::kInvalidId) return;           // an UNTRACKED clump (grab-adopt miss; the eid=0 gap) -> skip
    // The new pile is NOT positioned yet (FinishSpawning runs after BeginDeferred returns); the clump
    // (srcObj) IS at its sphere-traced resting point, so take loc/rot from it -- the convert position.
    const ue_wrap::FVector  loc      = ue_wrap::engine::GetActorLocation(srcObj);
    const ue_wrap::FRotator rot      = ue_wrap::engine::GetActorRotation(srcObj);
    const uint8_t           chipType = ue_wrap::prop::GetChipType(srcObj);
    UE_LOGI("[PILE] HOST RE-PILE(thunk) eid=%u clump=%p -> chipPile=%p convert IN PLACE (deterministic, same "
            "tick as the spawn -- no death-watch, no proximity)", static_cast<unsigned>(E), srcObj, newActor);
    coop::trash_channel::OnHostConvert(*s, E, coop::net::propconvert_kind::kToPile, newActor, loc, rot, chipType);
}

// DIAGNOSTIC read-only thunk on AmainPlayer_C::dropGrabObject -- the PHC-GRAB RELEASE verb (RE-confirmed
// 2026-06-22, mainPlayer.json: it writes `grabbing_actor = NoObject` @167013 + `grabHandle->ReleaseComponent()`
// @167059). This is the seam for releasing a PHYSICS-HANDLE-grabbed object: the chipPile clump rides
// grabbing_actor, NOT holding_actor (confirmed -- neither chipPile nor clump references holding_actor). It
// catches BOTH a plain drop AND a throw: a throw runs throwShit (SetPhysicsLinearVelocity) + thrown() on the
// SAME tick immediately BEFORE dropGrabObject, so by this POST the clump already carries its throw velocity (no
// need to classify drop-vs-throw here -- the release edge reads whatever velocity the clump has). The earlier
// simulateDrop thunk was the EQUIPMENT drop (throwHoldingProp -> simulateDrop) and NEVER fired for the clump:
// SIM-DROP=0 across 6 grabs + drops + throws, the live disproof. RULE 2: simulateDrop retired, this is correct.
// PostNativeCallback fires AFTER dropGrabObject runs, so grabbing_actor is already null -> the ROBUST signal is
// AnyCarryingEid() (our latch, untouched by dropGrabObject): carrying == the carry-eid in [HELD-STATE] == the
// release fired for the carried clump. The FLIP will close the latch HERE (read-only logs only now).
//
// CONFIRM before the flip: (a) [DROP-GRAB] fires on the real drop AND throw with carrying=<the carry-eid>;
// (b) it does NOT fire DURING the carry (at a churn re-grab -- the held clump ptr changes ~every 3 s). If it
// fires on churn, the flip would close the latch mid-carry (regress the smooth carry) -> the close needs a gate.
void OnDropGrabObserve(void* player, void* /*result*/) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected() || s->role() != coop::net::Role::Host) return;
    const coop::element::ElementId carryEid = coop::trash_channel::AnyCarryingEid();
    void* held = nullptr;
    coop::element::ElementId heldEid = coop::element::kInvalidId;
    ue_wrap::engine::MainPlayerGrabState gs{};
    if (player && ue_wrap::engine::ReadMainPlayerGrabState(player, gs)) {
        held = gs.grabbingActor ? gs.grabbingActor : gs.holdingActor;  // POST -> grabbing_actor already nulled by dropGrabObject
        if (held && R::IsLive(held)) heldEid = PT::GetPropElementIdForActor(held);
    }
    UE_LOGI("[DROP-GRAB] dropGrabObject fired player=%p grabbed=%p(POST) heldEid=%u carrying=%u -- READ-ONLY "
            "(cross-check carrying vs the carry-eid in [HELD-STATE]; MUST fire on a REAL drop/throw, NOT during "
            "the carry/churn; the FLIP closes the latch here -> the release edge then ships the throw velocity)",
            player, held,
            static_cast<unsigned>((heldEid  == coop::element::kInvalidId) ? 0u : heldEid),
            static_cast<unsigned>((carryEid == coop::element::kInvalidId) ? 0u : carryEid));
}

}  // namespace

bool EnsureHeldItemBroadcast(void* heldActor, coop::net::Session* s) {
    if (!heldActor || !s || !s->connected()) return false;
    if (!R::IsLive(heldActor)) return false;
    // K-5 kerfur class-gate: a kerfur prop is a host-owned entity (the host owns its KerfurId + its
    // host-range eid). A client must NEVER express/mint a peer-range eid for it -- that was the
    // grab-dupe root (redesign Failures #1/#3/#6: the minted peer-range eid is dropped by the host's
    // host-range conversion gate, then re-mirrored as a fresh client entity -> the dupe-and-drop loop).
    // Never mint here; the held-pose stream (local_streams) instead carries the kerfur prop MIRROR by
    // its host-range eid (kerfur_entity::GetKerfurMirrorEidForActor), which the host's remote_prop
    // receiver resolves to the authoritative kerfur prop + kinematic-drives -- no PropSpawn needed. On
    // the HOST this is redundant (its kerfur prop is tracker-known -> the express skip below already
    // returns false), but gating up-front is role-agnostic + explicit.
    if (coop::kerfur_entity::IsKerfurActor(heldActor)) return false;
    // docs/piles/08 class-gate: a garbageClump is a host-authoritative trash entity -- its identity is the
    // source pile's eid E, bound by trash_channel::OnHostConvert at the convert-spawn POST; the held-pose
    // stream then carries E (local_streams). The clump must NEVER be expressed here as a FRESH-eid keyless
    // PropSpawn (that second cross-peer entity = the dupe). A clump that reaches here (a stray pickup, or a
    // pile that had no eid to convert) is simply not authored -- carry-only, no dupe.
    if (ue_wrap::prop::IsGarbageClump(heldActor)) return false;
    // Any KEYED interactable: Aprop_C trash items AND the non-Aprop_C
    // garbageClump/chipPile (the actual trash the player carries). CRASH-SAFETY
    // is enforced on the RECEIVER, not by excluding them here: GetStaticMesh
    // returns null for non-Aprop_C, so the peer never runs physics on the clump
    // mirror -- it spawns physics-free and is driven KINEMATICALLY (the inverse
    // of the reverted-2a UAF). [[project-bug-trash-chippile-uaf-crash]]
    if (!ue_wrap::prop::IsKeyedInteractable(heldActor)) return false;
    // PART 2 (2026-06-18) CLIENT host-authority gate for SHARED TRASH (chipPile / garbageClump /
    // trashBitsPile -- the non-Aprop_C "world litter"). These are HOST-OWNED shared entities. When a
    // CLIENT grabs a host pile, its BP locally morphs it to a clump (EX_CallMath, un-hookable) and this
    // path would SendPropSpawn that clump -> the host fresh-spawns a DUPLICATE, and on the clump's later
    // land-convert (BroadcastConvertNear, reachable only if we WatchClump below) the host spawns yet
    // another pile = "a new pile born from the host's original, from the host's perspective." The client
    // must NEVER author a shared trash entity -- the exact host-authority rule the K-5 kerfur gate above
    // enforces. Suppressing here ALSO disarms the clump-convert watch (WatchClump is only reached past
    // this return), so neither the clump PropSpawn nor the PropConvert ever leaves the client -> the dupe
    // is killed at the SOURCE. The client's grab already removed the host's original via OnPileGrabPre's
    // PropDestroy (eid-resolved on the mirror), so the original IS removed -- no orphan. The client holds
    // the clump locally; a client-THROWN ground pile staying client-local is a known phase-2 gap (the
    // host-request spawn model) -- a benign divergence, NOT a dupe. Aprop_C items (IsDescendantOfProp)
    // are unaffected (per-player carry stays). The HOST still authors its own held trash below.
    // (Audit M1, 2026-06-18: this suppresses all 3 shared-trash bases but OnPileGrabPre's host-original
    // PropDestroy covers only chipPile -- safe because the other two are never carried-as-a-host-mirror:
    // a trashBitsPile dispenses an Aprop item + a separately-synced counter (not a held clump), and a
    // garbageClump only ever exists as a chipPile's morph product, whose chipPile grab already fired
    // OnPileGrabPre. So no host original is ever left orphaned.)
    if (s->role() == coop::net::Role::Client && !ue_wrap::prop::IsDescendantOfProp(heldActor)) {
        UE_LOGI("trash_collect: CLIENT holds shared trash cls='%ls' -- NOT authoring it (host owns world "
                "piles/clumps; the grab's PropDestroy already removed the host original). No dupe.",
                R::ClassNameOf(heldActor).c_str());
        return false;
    }

    // A divergence sweep is pending and this held actor is one of its candidates
    // (an in-universe, unclaimed save-loaded local the host did NOT express -- a
    // ghost awaiting adjudication, e.g. the save-transfer kerfur the host turned
    // ON before this client joined). Do NOT express it: a SendPropSpawn below
    // would fresh-spawn a host duplicate, and the RecordClaimIfTracking after it
    // would claim the ghost past the pending sweep (permanently rescuing it). Let
    // the deferred sweep destroy it. A genuinely client-originated drop is claimed
    // via the takeObj path, so it is NOT a candidate and streams normally here.
    if (coop::remote_prop_spawn::IsPendingSweepCandidate(heldActor)) {
        UE_LOGI("trash_collect: held item %p is a pending divergence-sweep candidate "
                "(unclaimed ghost) -- NOT expressing (the deferred sweep will destroy it)", heldActor);
        return false;
    }

    // Pre-quiescence JOIN WINDOW (2026-06-16 hands-on, the "kerfur off+grab dupe"). The guard above
    // only fires once the divergence sweep is ARMED (g_sweepPending). But a save-loaded in-universe
    // keyed prop the client grabs in the ~25 s window BEFORE the snapshot bracket opens slips past it
    // (g_sweepPending still false) -- and expressing it here SendPropSpawns a host duplicate the
    // client-side sweep can NEVER reclaim (it ran: host fresh-spawned a kerfur prop, eid 43938,
    // ownerSlot=1, permanent). Same divergence-universe membership, just not yet sweep-armed: until
    // load-tail quiescence the world is NOT reconciled, so a grabbed un-adjudicated local is not ours
    // to announce. Keep holding it locally (no pop); the host expresses its OWN copy in the bracket
    // (our held copy is then key/fuzzy-matched + claimed -> the held-pose stream mirrors it, no dupe),
    // or the sweep destroys our copy if the host converted it away. HasLoadTailQuiesced flips true the
    // instant the sweep fires, so this gates ONLY the join window -- never steady-state gameplay.
    if (!coop::remote_prop_spawn::HasLoadTailQuiesced() &&
        coop::remote_prop_spawn::IsInDivergenceUniverseUnclaimed(heldActor)) {
        UE_LOGI("trash_collect: held item %p cls='%ls' grabbed PRE-QUIESCENCE (join window not yet "
                "reconciled) -- NOT expressing (host expresses its own / the sweep adjudicates ours; "
                "prevents the off+grab host dupe)", heldActor, R::ClassNameOf(heldActor).c_str());
        return false;
    }

    // Express-if-unknown (ghost-twin fix, 2026-06-10 hands-on forensics). The
    // old predicate skipped ANY keyed held actor as "a normal world prop the
    // peer already has" -- but "has a key" is NOT "peer has it": a prop that
    // materialized in the world-load's late tail (keyless at sweep/seed time,
    // key self-minted by its Init AFTER both) is keyed yet completely unknown
    // to the wire -- never expressed, never bound, eid=0. The user carrying
    // one was invisible to the host (127 'no local match' PropPose warns) and
    // the wall stack diverged one-way. The correct skip test is TRACKER-KNOWN:
    // a prop with a live Element (seed-walked / Init-announced / previously
    // expressed) is genuinely shared -- the pose stream alone mirrors it. An
    // untracked one gets expressed RIGHT HERE, key and all, the moment a
    // player first touches it -- the self-heal for any straggler class.
    // (The peer's OnSpawn may fuzzy-bind it to a nearby same-class prop; for a
    // genuine EXTRA actor that is bounded weirdness vs the strictly-worse
    // one-way invisibility. The structural fix for the late-tail stragglers
    // themselves is tracked separately -- see the sweep's keyless histogram.)
    std::wstring keyStr = ue_wrap::prop::GetInteractableKeyString(heldActor);
    if (!keyStr.empty() && keyStr != L"None" &&
        PT::GetPropElementIdForActor(heldActor) != coop::element::kInvalidId) {
        return false;  // keyed AND tracker-known: the peer has it; pose stream suffices
    }

    const std::wstring cls = R::ClassNameOf(heldActor);
    // Aprop_C trash items get a force-minted Key (their UCS holds it). The non-Aprop_C
    // trash CLUMP (prop_garbageClump_C) is NON-KEYABLE -- setKey doesn't stick (the
    // source re-reads None, autotest 33e7f25) -- so it rides the SAME prop pipeline but
    // is identified by our EID instead: broadcast with key=None + the eid, and the
    // receiver resolves its mirror by eid (PropPoseSnapshot.elementId, v26). The clump
    // renders on its own (bare spawn = the 'dirtball' mesh), so no mesh transfer needed.
    // [[project-bug-trash-chippile-uaf-crash]]
    const bool isAprop = ue_wrap::prop::IsDescendantOfProp(heldActor);
    keyStr = coop::prop_synth_key::EnsureKeyForBroadcast(heldActor, keyStr, /*mintForAprop=*/isAprop);
    if (keyStr.empty() || keyStr == L"None") {
        if (isAprop) {
            UE_LOGW("trash_collect: Aprop item %p cls='%ls' Key still None after force-mint -- cannot mirror",
                    heldActor, cls.c_str());
            return false;
        }
        keyStr.clear();  // clump: wire key stays None; the eid is the cross-peer identity
    }
    // Create the Prop Element shadow + dedupe latch so the item's eventual
    // K2_DestroyActor unwinds it through the normal destroy path.
    PT::MarkProcessedInit(heldActor);
    PT::MarkPropElement(heldActor, keyStr, cls);

    coop::net::PropSpawnPayload p{};
    p.className.len = 0;
    for (size_t i = 0; i < cls.size() && i < 63; ++i)
        p.className.data[p.className.len++] = static_cast<char>(cls[i]);
    p.key.len = 0;
    for (size_t i = 0; i < keyStr.size() && i < 31; ++i)
        p.key.data[p.key.len++] = static_cast<char>(keyStr[i]);

    const ue_wrap::FVector  loc = ue_wrap::engine::GetActorLocation(heldActor);
    const ue_wrap::FRotator rot = ue_wrap::engine::GetActorRotation(heldActor);
    p.locX = loc.X; p.locY = loc.Y; p.locZ = loc.Z;
    p.rotPitch = ue_wrap::NormalizeAxis(rot.Pitch);
    p.rotYaw   = ue_wrap::NormalizeAxis(rot.Yaw);
    p.rotRoll  = ue_wrap::NormalizeAxis(rot.Roll);
    // v54: real scale (was hardcoded 1,1,1) + identity row below.
    const ue_wrap::FVector scl = ue_wrap::engine::GetActorScale3D(heldActor);
    p.scaleX = scl.X; p.scaleY = scl.Y; p.scaleZ = scl.Z;
    // Carry the trash VARIANT so the mirror shows the same chip/clump type the owner
    // grabbed (else a bare spawn defaults to variant 0 = the wrong-type bug). 0 for any
    // non-trash actor (GetChipType is a reflection no-op without a chipType property).
    p.chipType = ue_wrap::prop::GetChipType(heldActor);
    p.physFlags = coop::net::propspawn_flags::kSimulatePhysics;
    // IsHeavy/IsFrozen read Aprop_C struct offsets -- only meaningful on Aprop_C.
    // For a non-Aprop_C clump the receiver ignores physFlags (kinematic mirror),
    // so don't read stray bytes off it.
    p.propName.len = 0;
    if (ue_wrap::prop::IsDescendantOfProp(heldActor)) {
        if (ue_wrap::prop::IsHeavy(heldActor))  p.physFlags |= coop::net::propspawn_flags::kIsHeavy;
        if (ue_wrap::prop::IsFrozen(heldActor)) p.physFlags |= coop::net::propspawn_flags::kFrozen;
        if (ue_wrap::prop::IsStatic(heldActor)) p.physFlags |= coop::net::propspawn_flags::kStatic;
        if (ue_wrap::prop::IsSleeping(heldActor)) p.physFlags |= coop::net::propspawn_flags::kSleep;
        if (ue_wrap::prop::ReadRemoveWOrespawn(heldActor)) {
            p.physFlags |= coop::net::propspawn_flags::kRemoveWOrespawn;
        }
        // v54 identity: the list_props row -- a held GENERIC prop_C (e.g. a
        // cubicle gib panel) must mirror as itself, not the CDO 'cube'.
        const std::wstring nm = ue_wrap::prop::GetPropNameString(heldActor);
        for (size_t i = 0; i < nm.size() && i < 31; ++i) {
            p.propName.data[p.propName.len++] = static_cast<char>(nm[i]);
        }
    }
    p.initLinVelX = p.initLinVelY = p.initLinVelZ = 0.f;
    p.initAngVelX = p.initAngVelY = p.initAngVelZ = 0.f;
    {
        const coop::element::ElementId eid = PT::GetPropElementIdForActor(heldActor);
        p.elementId = (eid == coop::element::kInvalidId) ? 0u : eid;
    }
    UE_LOGI("trash_collect: BROADCAST held untracked item cls='%ls' key='%ls' loc=(%.1f,%.1f,%.1f) "
            "-- held-pose stream now mirrors it into the collector's hands",
            cls.c_str(), keyStr.c_str(), p.locX, p.locY, p.locZ);
    s->SendPropSpawn(p);
    // Fork B 2c: self-claim -- this peer just wire-expressed the held item;
    // an open bracket's sweep must not destroy it as "unclaimed".
    coop::remote_prop_spawn::RecordClaimIfTracking(heldActor);
    // (v81 MORPH V2: the former clump WatchClump enroll is GONE -- clumps return early above; the
    // bind-model pile_morph owns the clump's identity + its land conversion.)
    return true;
}

// ---- PROBE-A: the chipPile grab-intent diagnostic (docs/piles/08 Step 0) ----------------------------
//
// DECISIVE RE (votv-pile-grab-observable-hook-RE-2026-06-08-pass1.md): a chipPile grab is the E-press
// input action AmainPlayer_C::InpActEvt_use_..._41 -> icast(lookAtActor)->playerGrabbed (spawn clump +
// pickupObjectDirect(clump) + K2_DestroyActor(self)), all dispatched BP-internally (EX_LocalVirtualFunction
// -> ProcessInternal) and INVISIBLE to our ProcessEvent detour. The one ProcessEvent-observable edge with
// the pile STILL ALIVE is a PRE observer on InpActEvt_use (the native input->UFunction dispatch DOES hit
// ProcessEvent). docs/piles/08 makes the HOST grab sync WITHOUT acting here: the BP's own BeginDeferred is
// caught at the host_spawn_watcher convert-spawn POST (host-authoritative, zero-proximity). So this
// observer is now PROBE-A only (Increment 1): it logs the carry-slot the clump rides so the client-grab
// direction (v83) can suppress the native grab + send GrabIntent from this exact seam. Puppets are
// unpossessed -> never process input -> this only ever fires for the local player.
static void OnPileGrabPre(void* self, void* /*function*/, void* /*params*/) {
    if (!self) return;
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected()) return;  // BOTH roles -- whoever physically presses E
    void* aimed = ue_wrap::engine::ReadMainPlayerLookAtActor(self);  // the pile (PRE-conversion, alive)
    if (!aimed || !ue_wrap::prop::IsChipPile(aimed)) return;         // E-press not aimed at a pile
    // PROBE-A (docs/piles/08 Step 0): the HOST grab now syncs WITHOUT arming anything here -- the BP's own
    // BeginDeferred(clump) is caught at the host_spawn_watcher convert-spawn POST (WorldContextObject=pile
    // -> trash_channel ToClump; the land clump->pile likewise), zero-proximity + host-authoritative. This
    // observer's Increment-1 job is purely diagnostic: log the role, the aimed pile's eid (fwd + mirror),
    // and which carry SLOT the clump rides (grabbing_actor vs holding_actor -- the s34 correction left this
    // [?]) so the client-grab direction (v83) gates on the right slot. The client suppress-native +
    // GrabIntent send is added HERE in Increment 2. Read-only; fires only when aiming at a chipPile.
    coop::element::ElementId fwdEid = PT::GetPropElementIdForActor(aimed);
    coop::element::ElementId mirEid = coop::remote_prop::ResolveMirrorEidByActor(aimed);
    ue_wrap::engine::MainPlayerGrabState gs{};
    ue_wrap::engine::ReadMainPlayerGrabState(self, gs);
    const unsigned fwd = (fwdEid == coop::element::kInvalidId) ? 0u : static_cast<unsigned>(fwdEid);
    const unsigned mir = (mirEid == coop::element::kInvalidId) ? 0u : static_cast<unsigned>(mirEid);
    UE_LOGI("[PILE] %s E-PRESS on pile %p -- localEid=%u mirrorEid=%u %s (carry slots: grabbingActor=%p "
            "holdingActor=%p)",
            s->role() == coop::net::Role::Host ? "HOST" : "CLIENT", aimed, fwd, mir,
            (fwd != 0 || mir != 0) ? "[TRACKED -> grab will sync]"
                                   : "[UNTRACKED -> grab will NOT sync; tracking gap]",
            gs.grabbingActor, gs.holdingActor);

    // HOST: record the pending grab. The clump the BP spawns next (EX_CallMath -> invisible at spawn) is
    // adopted onto THIS pile's eid at the held-object edge (local_streams -> AdoptPendingGrabClump), which
    // broadcasts the PropConvert{ToClump}. The host owns its piles -> fwdEid is the authoritative local
    // element id (fall back to the mirror eid defensively). Client grab is Increment 2 (suppress + GrabIntent).
    if (s->role() == coop::net::Role::Host) {
        const coop::element::ElementId grabEid =
            (fwdEid != coop::element::kInvalidId) ? fwdEid : mirEid;
        if (grabEid != coop::element::kInvalidId)
            coop::trash_channel::NotePendingGrab(grabEid, ue_wrap::prop::GetChipType(aimed));
    }
}

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);  // re-cache every call (reconnect)

    // The deterministic re-pile thunk (READ-ONLY observe pass this cut): patch
    // BeginDeferredActorSpawnFromClass's UFunction::Func so we catch the clump's chipPile spawn
    // (EX_CallMath -> invisible to ProcessEvent). Independent latch -> retries until GameplayStatics
    // resolves, regardless of the grab-observer state. host_spawn_watcher's POST observer on the SAME
    // UFunction is unaffected (separate mechanism; our forwarder is transparent + the pinecone path's
    // result is not a chipPile/clump, so our cb ignores it).
    if (!g_repileThunkInstalled) {
        void* gsCls = R::FindClass(P::name::GameplayStaticsClass);
        void* bdFn  = gsCls ? R::FindFunction(gsCls, P::name::BeginDeferredSpawnFn) : nullptr;
        if (bdFn) {
            ue_wrap::ufunction_hook::InstallPostHook(bdFn, &OnBeginDeferredSpawnObserve);
            g_repileThunkInstalled = true;  // latch on attempt (InstallPostHook logs success/refusal)
            UE_LOGI("trash_collect: re-pile Func-patch armed on BeginDeferred -- the DETERMINISTIC converter "
                    "(clump re-pile -> OnHostConvert ToPile the same tick as the spawn; death-watch retired)");
        }
        // bdFn null -> GameplayStatics not loaded yet; retry next world-gated Install call.
    }

    // DIAGNOSTIC read-only: Func-thunk on AmainPlayer_C::dropGrabObject -- the PHC-GRAB RELEASE verb (the
    // correct seam; the prior simulateDrop thunk was the EQUIPMENT drop and NEVER fired for the PHC-grabbed
    // clump -- SIM-DROP=0 live, RULE 2 retired). dropGrabObject writes grabbing_actor = NoObject +
    // grabHandle->ReleaseComponent(); it is FUNC_BlueprintEvent dispatched EX_LocalVirtualFunction -> below
    // ProcessEvent, so a PRE observer can't see it -> the Func-thunk catches it. Logs carrying eid; the FLIP
    // will close the carry latch here so a real drop/throw lets the release edge ship the throw velocity.
    if (!g_dropGrabThunkInstalled) {
        void* pcls = R::FindClass(P::name::MainPlayerClass);
        void* dgFn = pcls ? R::FindFunction(pcls, L"dropGrabObject") : nullptr;
        if (dgFn) {
            ue_wrap::ufunction_hook::InstallPostHook(dgFn, &OnDropGrabObserve);
            g_dropGrabThunkInstalled = true;
            UE_LOGI("trash_collect: read-only dropGrabObject thunk armed (the PHC-grab release seam -- logs "
                    "carrying eid; drop AND throw both route through dropGrabObject; confirm it fires on a REAL "
                    "release NOT on a churn re-grab before the flip)");
        }
        // dgFn null -> mainPlayer_C not loaded yet; retry next world-gated Install call.
    }

    if (g_grabObserverInstalled) return;
    void* cls = R::FindClass(P::name::MainPlayerClass);
    if (!cls) return;  // mainPlayer_C not loaded yet -> retry on the next world-gated Install
    void* fn = R::FindFunction(cls, P::name::MainPlayerUseInputEventFn);
    if (!fn) {
        UE_LOGW("trash_collect: InpActEvt_use UFunction not found -- pile grabs cannot drop peer mirrors");
        g_grabObserverInstalled = true;  // permanent give-up (don't re-walk the class forever)
        return;
    }
    if (!GT::RegisterPreObserver(fn, &OnPileGrabPre)) {
        UE_LOGW("trash_collect: InpActEvt_use PRE observer register failed (table full?)");
        return;  // not latched -> retry next Install
    }
    g_grabObserverInstalled = true;
    UE_LOGI("trash_collect: pile grab observer installed on InpActEvt_use (records the aimed pile's eid "
            "as a pending grab -> the held-edge adopts the spawned clump; docs/piles/08)");
}

void OnDisconnect() {
    g_session.store(nullptr, std::memory_order_release);
}

}  // namespace coop::trash_collect_sync
