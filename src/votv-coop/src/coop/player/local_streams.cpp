// coop/local_streams.cpp -- see coop/local_streams.h.
//
// Extracted from net_pump.cpp 2026-06-12 (modular soft cap). Bodies moved
// VERBATIM with their rationale comments: ReadLocalPose, the held-prop
// stream + edges, and the v22 ragdoll pelvis-physics stream.

#include "coop/player/local_streams.h"

#include "coop/dev/perf_probe.h"
#include "coop/element/element.h"
#include "coop/session/ini_config.h"
#include "coop/creatures/kerfur_entity.h"  // K-5: GetKerfurMirrorEidForActor (held-kerfur-prop eid fallback)
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/hand_item.h"     // v105: the hotbar hand-item display axis (owner announce + mirrors)
#include "coop/player/skin_effects.h"  // own-body step FX at the wire-pose stride
#include "coop/props/trash_channel.h"   // docs/piles/08: CtxForEid -- the trash sync-time-context (carry stamp + trash-eid gate)
#include "coop/props/prop_element_tracker.h"
#include "coop/props/prop_stick_sync.h"
#include "coop/props/remote_prop.h"     // ResolveMirrorEidByActor (the bound-clump held-pose eid fallback)
#include "coop/props/trash_collect_sync.h"

#include "ue_wrap/atv.h"
#include "ue_wrap/engine.h"
#include "ue_wrap/log.h"
#include "ue_wrap/prop.h"
#include "ue_wrap/puppet.h"
#include "ue_wrap/reflection.h"
#include "ue_wrap/types.h"
#include "ue_wrap/vitals.h"

#include <cmath>
#include <cstdint>
#include <string>

namespace coop::local_streams {
namespace {

namespace R = ue_wrap::reflection;

// Held-prop edge detector: file-scope (NOT a static-local in Tick). A
// static-local would carry a stale prop pointer + key across a session stop/
// restart, causing the next pump to fire SendPropRelease for the OLD session's
// key on the NEW session -- a real bug found by the audit. Cleared by
// OnSessionStart on each session.Start.
void* g_lastHeldProp = nullptr;
coop::net::WireKey g_lastHeldKey{};
// v81 MORPH V2: the held actor's wire eid, resolved ONCE on the new-held edge (where the O(n)
// ResolveMirrorEidByActor fallback for a morph-bound clump may run) and reused for the per-tick stream so
// the hot path stays O(1) -- a held actor's identity is stable for the whole carry. kInvalidId = unkeyed.
coop::element::ElementId g_lastHeldEid = coop::element::kInvalidId;
// take-30 (audit): g_lastHeldProp is a raw UObject* cached ACROSS ticks (set on the new-held edge, read on the
// later release/flight edges). A GC purge between ticks can free the just-re-piled/dropped clump without firing
// K2_DestroyActor -> a bare R::IsLive(g_lastHeldProp) would deref freed memory (UAF). Cache the clump's stable
// GUObjectArray index at the new-held edge (while it is known live) and validate via R::IsLiveByIndex on the
// later edges -- the established cross-tick-pointer pattern (ResolveLiveActorByEid, audit 2026-05-30/06-03).
int32_t g_lastHeldPropIdx = -1;
uint64_t g_propEmitCount = 0;

// v22 ragdoll-physics edge detector (same file-scope rationale as g_lastHeldProp:
// a static-local would carry a stale "was ragdolling" across a session restart and
// emit a spurious recover-edge false on the next session). Cleared by OnSessionStart.
bool g_wasRagdolling = false;
uint64_t g_ragdollEmitCount = 0;

// Game thread, ~send-rate: read the local player's pose. Pulled in from
// harness.cpp 2026-05-28; full rationale comments preserved.
bool ReadLocalPose(void* local, void* controller, coop::net::PoseSnapshot& out) {
    if (!local) return false;
    const ue_wrap::FVector loc = ue_wrap::engine::GetActorLocation(local);
    const ue_wrap::FRotator actorRot = ue_wrap::engine::GetActorRotation(local);
    const ue_wrap::FVector vel = ue_wrap::engine::GetActorVelocity(local);
    // BODY yaw: read from the ACTOR (the real body facing direction). Earlier
    // attempt sent controller yaw -- sending it made the puppet body show the
    // CAMERA direction instead of the BODY direction while moving = sideways-
    // when-camera-leads (user-confirmed regression). NOTE (falsified 2026-06-11):
    // on foot the actor yaw FOLLOWS the camera ~immediately (standing actor.Yaw
    // == ctrl.Yaw, headYawDelta ~= 0), so the receiver does NOT get a natural
    // head-lead from this stream -- RemotePlayer::UpdateBodyYaw synthesizes the
    // standing turn-in-place presentation on the puppet instead.
    // HEAD pitch: read from the CONTROLLER (actor pitch is always 0 on an
    // upright character; the controller carries the real view pitch). Cached
    // by net_pump::Tick to skip re-resolving GetController every tick.
    out.x = loc.X;
    out.y = loc.Y;
    // Z = source actor.Z (capsule centre on an ACharacter). Receiver
    // reconstructs visible body offset from its own mainPlayer_C's
    // RelLoc.Z at puppet spawn -- same BP class on every peer so the
    // authored constant matches. MTA-fidelity shape (CEntitySA streams
    // matrix.vPos = capsule centre).
    out.z = loc.Z;
    // Normalize yaw and pitch into the canonical FRotator axis range (-180, 180]
    // BEFORE they go on the wire. UE4's AController::GetControlRotation returns
    // RAW ControlRotation unnormalized: looking 10 deg DOWN reads back as
    // Pitch=350. Without this normalize, pitch=350 fails coop::net::ValidatePose's
    // (-90, 90) bound and the ENTIRE packet is dropped on the receiver -- root
    // cause of the "puppet freezes when host looks below horizontal" bug.
    out.yaw = ue_wrap::NormalizeAxis(actorRot.Yaw);
    const ue_wrap::FRotator ctlRot = controller
        ? ue_wrap::engine::GetControlRotation(controller)
        : actorRot;
    out.pitch = ue_wrap::NormalizeAxis(ctlRot.Pitch);
    // headYawDelta: the source's controller-yaw LEAD over its body yaw, in
    // (-180, 180]. The puppet's AnimBP headLookAt yaw reads this so the puppet's
    // head turns to match where the source's CAMERA is looking (free-look /
    // camera-lead-body) -- decoupled from body facing.
    out.headYawDelta = ue_wrap::NormalizeAxis(ctlRot.Yaw - actorRot.Yaw);
    out.speed = std::sqrt(vel.X * vel.X + vel.Y * vel.Y);
    // Pack the source's airborne state. The receiver's BUA-POST observer
    // reads this bit to clear useLegIK on the puppet during the airborne
    // window so the foot-IK trace doesn't plant the puppet's feet to
    // ground while the source is jumping/falling. A-2 (2026-05-29 audit):
    // CMC pointer + MovementMode bytes accessed through ue_wrap::puppet::
    // ReadCharacterIsFalling -- symmetric with the write-side wrapper
    // DriveCharacterMovement (Principle 7: gameplay/coop layer reads
    // engine memory only through ue_wrap). The wrapper also adds IsLive
    // guards on actor + CMC that the prior inline code lacked -- raw
    // deref of a PendingKill mainPlayer_C (e.g. mid level transition)
    // was technically UB; false-positive rate in steady-state gameplay
    // is zero because the possessed mainPlayer_C never carries
    // PendingKill/Unreachable flags.
    out.stateBits = 0;
    if (ue_wrap::puppet::ReadCharacterIsFalling(local)) {
        out.stateBits |= coop::net::kStateBitInAir;
    }
    // v20 (Inc2b): piggyback the LOCAL player's ragdoll/faint state. isRagdoll
    // is the AnimBP gate flipped by EVERY ragdoll cause (manual C-key, exhaustion
    // faint, KO); we sync it as a per-peer DISPLAY flag so each peer's puppet
    // flops on the others' screens. Death is EXCLUDED -- a death-ragdoll routes
    // through VOTV's native SP menu flow + ends the session ([[project-coop-no-
    // host-migration]]), so it must not show as a recoverable faint on a puppet
    // that's about to be torn down. One wrapper read (a couple of derefs after
    // the offset cache fills) on this hot path -- same cost class as the
    // ReadCharacterIsFalling above it.
    {
        bool isRagdoll = false, dead = false;
        if (ue_wrap::engine::ReadMainPlayerRagdollState(local, isRagdoll, dead) &&
            isRagdoll && !dead) {
            out.stateBits |= coop::net::kStateBitRagdoll;
        }
    }
    // v19: piggyback the LOCAL player's vitals (health fraction + food + sleep)
    // in the 3 bytes that were `_pad` -- ZERO wire-size change. ue_wrap::vitals
    // reads THIS machine's UsaveSlot_C (one per machine), so the values are
    // per-peer-authoritative (host packs host's, each client its own). Default
    // to full (255) until the save resolves so a booting peer doesn't flash an
    // empty bar; overwrite per field only on a successful read. The receiver
    // treats these as DISPLAY-ONLY (puppet nameplate) -- never a saveSlot write.
    // After the one-time resolution cache fills, each Read is O(1) (a couple of
    // cheap derefs); 3-4 per pose-send tick is negligible on this hot path.
    out.healthFrac = out.foodFrac = out.sleepFrac = 255;
    {
        namespace V = ue_wrap::vitals;
        float health = 0.f, maxHealth = coop::net::kVitalScalarMax, food = 0.f, sleep = 0.f;
        bool gotHealth = false;
        if (V::Read(V::Field::Health, &health)) {
            gotHealth = true;
            V::Read(V::Field::MaxHealth, &maxHealth);  // best-effort; 100 fallback on miss
            out.healthFrac = coop::net::QuantizeUnitFraction(
                maxHealth > 0.f ? health / maxHealth : 1.f);
        }
        if (V::Read(V::Field::Food, &food))
            out.foodFrac = coop::net::QuantizeUnitFraction(food * (1.f / coop::net::kVitalScalarMax));
        if (V::Read(V::Field::Sleep, &sleep))
            out.sleepFrac = coop::net::QuantizeUnitFraction(sleep * (1.f / coop::net::kVitalScalarMax));
        // One-shot proof the vitals chain RESOLVED (vs silently defaulting to a
        // full bar). Latched on the first successful health read so it can't spam
        // this hot path. Game-thread only -> a plain static latch is safe here.
        static bool sVitalsLogged = false;
        if (gotHealth && !sVitalsLogged) {
            sVitalsLogged = true;
            UE_LOGI("vitals: first local read OK -- health=%.1f/%.1f food=%.1f sleep=%.1f "
                    "-> wire bytes h=%u f=%u s=%u", health, maxHealth, food, sleep,
                    out.healthFrac, out.foodFrac, out.sleepFrac);
        }
    }
    return true;
}

// Resolve the wire elementId for a held prop. An ordinary keyed prop is in prop_element_tracker's
// LOCAL map (g_actorToPropElementId). A kerfur prop on a CLIENT is a host-owned MIRROR (m_mirror=true)
// that is NOT in that local map -- fall back to its host-range eid from the kerfur held-pose map so the
// carry streams cross-peer by eid (its random BP key won't resolve; the host's remote_prop receiver
// resolves the authoritative kerfur prop by this eid + kinematic-drives it). K-5. kInvalidId if neither.
coop::element::ElementId ResolveHeldPropEid(void* heldActor) {
    auto eid = coop::prop_element_tracker::GetPropElementIdForActor(heldActor);
    if (eid == coop::element::kInvalidId)
        eid = coop::kerfur_entity::GetKerfurMirrorEidForActor(heldActor);
    // v81 MORPH V2: a held garbageClump bound to the grabbed pile's eid E by pile_morph (the host's own
    // morph rebinds the local tracker Element -> resolved above; a client's morph rebinds a MIRROR ->
    // resolved here). Without this the carried clump would stream eid=0 ("no local match" flood) and the
    // peers' clump mirror would never follow the hand. Reached only on the new-held edge + cached for the
    // per-tick stream by the caller, so the O(n) mirror snapshot is not a hot path.
    if (eid == coop::element::kInvalidId)
        eid = coop::remote_prop::ResolveMirrorEidByActor(heldActor);
    return eid;
}

}  // namespace

void* LastHeldActor() {
    // The rest-exclusion read for trash_channel::TickCarry (v106): the local
    // player's currently-held actor, or null. IsLive-guarded so a stale pointer
    // never aliases a recycled address into a wrong exclusion.
    return (g_lastHeldProp && ue_wrap::reflection::IsLive(g_lastHeldProp)) ? g_lastHeldProp : nullptr;
}

void OnSessionStart() {
    g_lastHeldProp = nullptr;
    g_lastHeldKey = {};
    g_lastHeldEid = coop::element::kInvalidId;
    g_lastHeldPropIdx = -1;
    g_propEmitCount = 0;
    g_wasRagdolling = false;
    g_ragdollEmitCount = 0;
}

void Tick(coop::net::Session& session, void* local, void* controller) {
    namespace PP = coop::dev::perf_probe;
    coop::net::PoseSnapshot mine;
    { PP::Scope _s{PP::Bucket::LocalSend};
      if (ReadLocalPose(local, controller, mine)) {
          session.SetLocalPose(mine);
          // Skin step FX on the OWN body (mynet burst / keljoy squeak at your
          // own feet): the native mainPlayer stride calls lib_C::step through
          // an EX_CallMath (invisible to the ProcessEvent hook), so the coop
          // layer re-strides the same pose sample it just read for the wire.
          coop::skin_effects::TickStride(
              local, ue_wrap::FVector{mine.x, mine.y, mine.z}, mine.speed,
              (mine.stateBits & coop::net::kStateBitInAir) == 0);
      }
    }

    // Held-prop replication. Read mainPlayer.grabbing_actor; if non-null,
    // build a PropPoseSnapshot from the prop's current world transform and
    // publish to the net thread. On the edge held -> not-held, send a
    // RELIABLE PropRelease so the peer re-enables SimulatePhysics (and we
    // never rely solely on the 500 ms stream-stop timeout).
    //
    // 2026-05-29 (post-A-4 follow-up): both fields read via
    // ue_wrap::engine::ReadMainPlayerGrabState (Principle 7 -- no inline raw
    // struct-offset derefs at coop layer). The wrapper resolves
    // mainPlayer.holding_actor via reflected_offset (MainPlayer_holding_actor
    // IS defined), so holdingActor is populated whenever the field exists.
    // Whether picking up a chipPile/clump actually SETS holding_actor (vs the
    // clump self-driving off its own holdPlayer) is a live-BP question
    // (Candidate A vs B); the [probe] below answers it from a real hands-on.
    ue_wrap::engine::MainPlayerGrabState gs{};
    void* heldActor = nullptr;
    void* hotbarProp = nullptr;  // holding_actor when it is the HAND item (Aprop_C)
    if (ue_wrap::engine::ReadMainPlayerGrabState(local, gs)) {
        heldActor = gs.grabbingActor;
        // 2026-05-27: chipPile/clump pickup sets mainPlayer.holding_actor
        // INSTEAD of grabbing_actor (their morph path doesn't use the
        // PhysicsHandle). Fall back to holding_actor so the PropPose
        // stream covers them too -- gated by IsKeyedInteractable since
        // holding_actor can also point at non-prop carry targets.
        // v105 (2026-07-06, RULE 2): an Aprop_C in holding_actor is the HOTBAR
        // HAND ITEM -- player expression, NOT a world entity (updateHold
        // respawns it per switch) -- and is routed to coop::hand_item instead
        // of the world-prop pipeline (whose PRE-QUIESCENCE gate never lifts on
        // the host: the never-mirrored-hand root, hands-on 12:40:05). Only the
        // non-Aprop_C trash lineage (clump/pile morph carry) stays here.
        if (gs.holdingActor && R::IsLive(gs.holdingActor) &&
            ue_wrap::prop::IsKeyedInteractable(gs.holdingActor)) {
            if (ue_wrap::prop::IsDescendantOfProp(gs.holdingActor))
                hotbarProp = gs.holdingActor;
            else if (!heldActor)
                heldActor = gs.holdingActor;
        }
    }
    // v105 hand-item axis: announce the hotbar hand on change + maintain the
    // peers' display mirrors (both are cheap idle no-ops).
    coop::hand_item::TickOwner(session, local, hotbarProp);
    coop::hand_item::TickMirrors();
    // v76: the ATV is owned by coop::atv_sync (occupant-OR-grabber pose stream + AtvRelease), NOT the
    // held-prop pipeline -- exclude it so a grabbed ATV doesn't emit zero-key PropPose packets here
    // (it is not a keyed interactable: EnsureHeldItemBroadcast fails, so receivers would just drop
    // them as "no local match"). One concept, one pipeline (RULE 2).
    if (heldActor && ue_wrap::atv::IsAtv(heldActor)) heldActor = nullptr;
    // [probe] garbage-ball pickup diagnostic (2026-05-31, ini
    // garbage_pickup_probe=1): resolves whether a chipPile/clump pickup sets
    // grabbing_actor or holding_actor (the open Candidate-A-vs-B question)
    // from a real hands-on carry, plus the held actor's class + key +
    // keyed-interactable verdict, proving the held-pose path can see the
    // clump. Only fires while something is held, throttled ~4 Hz; gated off
    // by default so steady-state cost is one atomic-bool load.
    static const bool sProbeGarbage =
        ::coop::ini_config::IsIniKeyTrue("garbage_pickup_probe");
    if (sProbeGarbage && (gs.grabbingActor || gs.holdingActor)) {
        static uint32_t sN = 0;
        if ((sN++ % 30) == 0) {
            void* ga = gs.grabbingActor;
            void* ha = gs.holdingActor;
            const bool gaLive = ga && R::IsLive(ga);
            const bool haLive = ha && R::IsLive(ha);
            const int haKeyed =
                haLive ? (int)ue_wrap::prop::IsKeyedInteractable(ha) : -1;
            const std::wstring hKey = (haKeyed == 1)
                ? ue_wrap::prop::GetInteractableKeyString(ha)
                : std::wstring(L"-");
            UE_LOGI("[probe garbage_pickup]: grabbing_actor=%p(%ls) "
                    "holding_actor=%p(%ls) holdingKeyed=%d holdingKey='%ls'",
                    ga, gaLive ? R::ClassNameOf(ga).c_str() : L"-",
                    ha, haLive ? R::ClassNameOf(ha).c_str() : L"-",
                    haKeyed, hKey.c_str());
        }
    }
    // v68 stick-sync (audit finding 2): a held wall-attachable that has just
    // STUCK (the commit set frozen/static; the engine hold lingers ~0-100 ms)
    // is stream-end NOW. Streaming poses for a frozen prop could reach the
    // receiver's 5-pose unstick streak and rip the just-stuck mirror back
    // off; ending the stream here also fires the release edge below in the
    // SAME pump pass as -- and therefore lane-ordered AFTER -- the
    // PropStickState broadcast (prop_stick_sync::Tick runs in TickGameplay,
    // before this stream tick), so the receiver's release gate already sees
    // frozen=true. A re-grab unstick (playerGrabbed_pre) clears the flags
    // before this read, so the carry stream resumes immediately.
    if (heldActor && R::IsLive(heldActor) &&
        coop::prop_stick_sync::IsWallAttachable(heldActor) &&
        (ue_wrap::prop::IsFrozen(heldActor) || ue_wrap::prop::IsStatic(heldActor))) {
        heldActor = nullptr;
    }
    // DIAGNOSTIC (2026-06-22): the held-state at the branch point -- which branch runs each frame.
    // MAIN(pose) = heldActor live -> the pose stream; RELEASE-EDGE = heldActor null but g_lastHeldProp set;
    // idle = neither. Distinguishes a dead main branch (heldActor undetected between E) from a firing
    // release-edge vs a pose-skip. Throttled ~8/s; only logs while something is/was held.
    if (heldActor || g_lastHeldProp) {
        static uint32_t sHS = 0;
        if ((sHS++ % 15) == 0) {
            const bool hLive = heldActor && R::IsLive(heldActor);
            UE_LOGI("[HELD-STATE] heldActor=%p(live=%d) g_lastHeldProp=%p g_lastHeldEid=%u carrying=%d -> %s",
                    heldActor, hLive ? 1 : 0, g_lastHeldProp,
                    (g_lastHeldEid == coop::element::kInvalidId) ? 0u : static_cast<unsigned>(g_lastHeldEid),
                    coop::trash_channel::IsCarrying(g_lastHeldEid) ? 1 : 0,
                    (heldActor && hLive) ? "MAIN(pose)" : (g_lastHeldProp ? "RELEASE-EDGE" : "idle"));
        }
    }
    if (heldActor && R::IsLive(heldActor)) {
        // New-held edge: broadcast a PropSpawn so each peer spawns a mirror. Aprop_C
        // items get a force-minted Key; the NON-KEYABLE trash CLUMP rides the SAME
        // prop pipeline identified by our EID (key=None -- setKey doesn't stick on
        // it). The clump renders on its own (a bare spawn = the 'dirtball' mesh),
        // floats in front of the puppet via this pose stream (like the mannequin),
        // and gets physics on release. [[project-bug-trash-chippile-uaf-crash]]
        if (heldActor != g_lastHeldProp) {
            // New-held edge. A held trash CLUMP (the chipPile's grab product) is ADOPTED HERE onto the
            // grabbed pile's eid E: the v106 birth certificate (recorded at the clump's BeginDeferred by
            // the UFunction::Func thunk -- EVERY dispatch route) carries {pile eid, chipType}; this edge
            // consumes it + broadcasts PropConvert{ToClump}. EnsureHeldItemBroadcast returns false for a clump (class gate at
            // trash_collect_sync.cpp); a normal Aprop_C item is broadcast as a fresh keyed prop. Resolve the
            // wire eid ONCE here (the only place the O(n) ResolveMirrorEidByActor
            // fallback for the eid-only clump may run) and CACHE it (g_lastHeldEid) so the carry stream is O(1).
            const bool mirrored = coop::trash_collect_sync::EnsureHeldItemBroadcast(heldActor, &session);
            // HOST grab adoption (v106, docs/piles/08): the clump's IDENTITY comes from its
            // BIRTH CERTIFICATE -- the BeginDeferred Func thunk records {source pile eid,
            // chipType} at the exact spawn (every clump is born from a chipPile; bytecode
            // toClump @141 / uber @3493), covering E-press AND use-HOLD grabs alike. The
            // held-edge CONSUMES it here. Fallback: an EXISTING tracked clump re-entering
            // the hand (a gate-aborted rest clump picked back up) resolves by the tracker.
            // (RULE 2: the InpActEvt-PRE pending-grab slot and the "no pending + mid-carry
            // => churn re-grab of MY last carry" heuristic are RETIRED -- with 2+ clumps in
            // flight the heuristic bound a foreign clump to the wrong lane; 2026-07-07
            // eid-4809 permanent HELD deny-lock, the client ghost-pile root.)
            coop::element::ElementId adoptedEid = coop::element::kInvalidId;
            bool churnRegrab = false;
            if (session.role() == coop::net::Role::Host && ue_wrap::prop::IsGarbageClump(heldActor)) {
                const auto cloc = ue_wrap::engine::GetActorLocation(heldActor);
                const auto crot = ue_wrap::engine::GetActorRotation(heldActor);
                coop::element::ElementId bornE = coop::element::kInvalidId;
                uint8_t bornChip = 0;
                if (!coop::trash_channel::TakeClumpBorn(heldActor, &bornE, &bornChip)) {
                    // No certificate: an existing tracked clump re-grabbed off the ground.
                    bornE    = coop::prop_element_tracker::GetPropElementIdForActor(heldActor);
                    bornChip = ue_wrap::prop::GetChipType(heldActor);
                }
                if (bornE != coop::element::kInvalidId) {
                    if (coop::trash_channel::IsCarrying(bornE)) {
                        // The game auto-re-grabbed MY carried entity (churn: the just-re-piled
                        // pile morphs straight back). Rebind + cancel the settle.
                        coop::trash_channel::OnHostRegrab(bornE, heldActor);
                        churnRegrab   = true;
                        g_lastHeldEid = bornE;  // deterministic (was: assumed unchanged)
                    } else {
                        adoptedEid = coop::trash_channel::AdoptBornClump(
                            session, bornE, heldActor, cloc, crot, bornChip);
                    }
                }
            }
            if (!churnRegrab)
                g_lastHeldEid = (adoptedEid != coop::element::kInvalidId) ? adoptedEid
                                                                          : ResolveHeldPropEid(heldActor);
            const unsigned eidLog =
                (g_lastHeldEid == coop::element::kInvalidId) ? 0u : static_cast<unsigned>(g_lastHeldEid);
            UE_LOGI("net: NEW held actor %p cls='%ls' key='%ls' eid=%u -> %s",
                    heldActor, R::ClassNameOf(heldActor).c_str(),
                    ue_wrap::prop::GetInteractableKeyString(heldActor).c_str(),
                    eidLog, mirrored ? "BROADCAST" : "carry-only(trash/clump)");
            // [PILE] carry phase: a trash clump (carry-only, eid-identified) is now in hand. The convert
            // (pile->clump) was broadcast by AdoptBornClump on the new-held edge above; this stream
            // just carries E's pose.
            if (!mirrored && eidLog != 0 && ue_wrap::prop::IsGarbageClump(heldActor)) {
                UE_LOGI("[PILE] %s CARRY eid=%u clump in hand -> streaming carry pose (ctx-stamped); "
                        "clients drive their mirror of E",
                        (session.role() == coop::net::Role::Host) ? "HOST" : "CLIENT", eidLog);
            }
        }
        // Stream the held world transform. key (None for the clump) + eid (the
        // clump's cross-peer identity); the receiver resolves the mirror by key,
        // falling back to eid.
        const std::wstring keyW = ue_wrap::prop::GetInteractableKeyString(heldActor);
        coop::net::PropPoseSnapshot pp{};
        pp.key.len = 0;
        for (size_t i = 0; i < keyW.size() && i < 31; ++i) {
            // The save UUIDs are ASCII; this lossless narrowing is fine.
            pp.key.data[pp.key.len++] = static_cast<char>(keyW[i]);
        }
        // v81 MORPH V2: use the eid CACHED on the new-held edge (O(1)) -- do NOT re-run the O(n)
        // ResolveHeldPropEid every tick (the CLAUDE.md per-frame full-scan hot-path regression).
        pp.elementId = (g_lastHeldEid == coop::element::kInvalidId) ? 0u
                                                                    : static_cast<uint32_t>(g_lastHeldEid);
        // v82 (docs/piles/08): stamp the trash entity's current sync-time-context so the receiver can DROP
        // a carry pose that arrives after a transition (re-pile/throw); 0 for a non-trash held prop.
        pp.ctx = coop::trash_channel::CtxForEid(g_lastHeldEid);
        const auto loc = ue_wrap::engine::GetActorLocation(heldActor);
        const auto rot = ue_wrap::engine::GetActorRotation(heldActor);
        pp.x = loc.X; pp.y = loc.Y; pp.z = loc.Z;
        // Normalize at the wire boundary: physics-prop rotation accumulates
        // through FQuat<->Euler conversions and can end up at Yaw=359.8 or
        // Pitch=-270; receiver's canonical (-180,180] guard would reject.
        pp.pitch = ue_wrap::NormalizeAxis(rot.Pitch);
        pp.yaw   = ue_wrap::NormalizeAxis(rot.Yaw);
        pp.roll  = ue_wrap::NormalizeAxis(rot.Roll);
        // Only STREAM a held-prop pose that carries a cross-peer IDENTITY (a Key OR an eid). A clump
        // grabbed PRE-QUIESCENCE that EnsureHeldItemBroadcast declined to express is keyless AND eid-less
        // -- the receiver can't resolve it, so streaming it floods the peer with 'no local match' warns
        // (~60/s) for an actor it will never have, and shows nothing. Skip the SEND until it gains an
        // identity; the stream resumes the instant the item is expressed. (Join-window flood fix
        // 2026-06-17. g_lastHeldProp is still tracked below so the release edge + re-acquire work; a
        // PropRelease for a never-expressed held prop is harmless -- the peer has no mirror to release.)
        if (pp.key.len > 0 || pp.elementId != 0) {
            session.SetLocalPropPose(true, pp);
            // Throttled emit log: first 3 + every 60th, matches receiver
            // throttle so the two logs can be diff'd line-for-line.
            const uint64_t n = ++g_propEmitCount;
            if (n <= 3 || (n % 60) == 0) {
                UE_LOGI("net: PropPose emit #%llu -> world(%.1f, %.1f, %.1f) rot(%.1f, %.1f, %.1f) key.len=%d eid=%u ctx=%u",
                        static_cast<unsigned long long>(n),
                        pp.x, pp.y, pp.z, pp.pitch, pp.yaw, pp.roll,
                        static_cast<int>(pp.key.len), pp.elementId, static_cast<unsigned>(pp.ctx));
            }
        } else {
            // DIAGNOSTIC (2026-06-22): the held clump has NO identity (eid=0 AND key.len=0) -> the pose is NOT
            // streamed. If this fires between E-events while a clump is held, the carry freeze is g_lastHeldEid
            // going invalid (the eid was cleared), NOT a dead main branch. Throttled ~8/s.
            static uint64_t sPK = 0;
            if ((sPK++ % 15) == 0)
                UE_LOGI("[POSE-SKIP] eid=0 key.len=0 -- held clump has NO identity to stream (g_lastHeldEid invalid -> carry frozen between E)");
        }
        g_lastHeldProp = heldActor;
        g_lastHeldKey = pp.key;
        // Cache the held actor's GUObjectArray index NOW (heldActor is live in this branch) so the later
        // release/flight edges can validate the cached pointer GC-safely via R::IsLiveByIndex (see decl).
        g_lastHeldPropIdx = R::InternalIndexOf(heldActor);
    } else if (g_lastHeldProp) {
        // CLOSE-B (2026-06-22): the LATCH owns "carrying", not the flickering holding_actor. A churn re-pile
        // DESTROYS the held clump -> holding_actor flickers empty for a frame -> this edge would clear
        // g_lastHeldEid + send a spurious PropRelease, breaking the carry binding (the "position dead between
        // E-events" symptom). Distinguish the flicker from a REAL release by the RE-PILE RECORD: a flicker is
        // immediately preceded by a re-pile that started a land-settle (HasPendingSettle); a real drop/throw is
        // an ALIVE clump with NO pending settle (a churn re-grab cancels the settle before the player could
        // ever throw -- so a real release never coincides with one). Suppress ONLY (carrying && HasPendingSettle);
        // a real release fires below + closes the latch so the landing converts as "not carrying". No R::IsLive
        // (UAF on the just-destroyed clump), no input-event RE (InpActEvt_drop is UI-gated, throwPath is aiming).
        const bool carrying_ = coop::trash_channel::IsCarrying(g_lastHeldEid);
        const bool pending_  = coop::trash_channel::HasPendingSettle(g_lastHeldEid);  // kept only for the diag log
        // B (2026-06-22, log-confirmed): the LATCH owns "carrying", NOT the flickering holding_actor. The
        // freeze is the held-puppet RECREATION (updateHold rebuilds holding_actor -> a 1-frame null) firing
        // this poll edge -> it cleared g_lastHeldEid + sent a spurious PropRelease -> ctx churn -> the client
        // held carry poses (ctx ahead of known) -> frozen. HasPendingSettle could not catch it (recreation
        // has no re-pile/settle). So SUPPRESS the PropRelease (send) for the WHOLE carry (!carrying). The latch
        // closes via the land-settle (a drop/throw's landing re-pile with no re-grab within K -> LAND COMMIT).
        // THROW ARC (2026-06-22): the release-VERB hunt is dead -- both simulateDrop AND dropGrabObject were
        // installed and fired ZERO times for the chipPile release (the clump's release path uses neither).
        // Instead of detecting the release, we STREAM THROUGH it: while carrying, if g_lastHeldProp is still a
        // LIVE clump (carry flicker OR post-release flight), keep streaming its pose under E (below) -- one
        // continuous E-stream, the client interp shows the real arc, no verb + no velocity needed. NO HasPendingSettle.
        const bool relSkip   = carrying_;
        UE_LOGI("[REL-EDGE] eid=%u carrying=%d pendingSettle=%d -> %s",
                (g_lastHeldEid == coop::element::kInvalidId) ? 0u : static_cast<unsigned>(g_lastHeldEid),
                carrying_ ? 1 : 0, pending_ ? 1 : 0, relSkip ? "SKIP(carrying)" : "FIRE(release)");
        if (relSkip) {
            // CARRY/FLIGHT CONTINUITY (throw-arc fix 2026-06-22): the release edge is suppressed while carrying
            // (the freeze fix). But heldActor going null does NOT mean the clump is gone -- it may be (a) a
            // 1-frame grab-state flicker mid-carry (the clump is still held at the carry position), or (b) the
            // player REALLY released it and it is now FLYING (physics) until it re-piles. In BOTH cases
            // g_lastHeldProp is still a LIVE clump, so KEEP STREAMING its pose under the SAME eid E -- the
            // carry and the flight are ONE continuous E-pose-stream (the client's fixed-delay interp drives the
            // same proxy throughout and shows the REAL arc). This needs NO release verb and NO velocity: an
            // ALIVE clump = carry-or-flight (stream it); a churn re-pile DESTROYS the clump (!IsLive) -> fall to
            // the gap below (await the re-grab or the land). That IsLive test is the churn/flight discriminator
            // the elusive drop-verb was meant to be -- a re-pile kills the clump (skip), a real release leaves
            // it flying (stream). The stream ends naturally when the clump re-piles (wherever -- end of arc OR a
            // mid-flight contact): the re-pile thunk's ToPile re-skins the proxy + snaps it to the landed spot.
            if (R::IsLiveByIndex(g_lastHeldProp, g_lastHeldPropIdx) &&
                ue_wrap::prop::IsGarbageClump(g_lastHeldProp) &&
                g_lastHeldEid != coop::element::kInvalidId) {
                coop::net::PropPoseSnapshot pp{};
                // SOUND FIX (take-29 #2a): stream the SAME wire key the carry main branch sends
                // (GetInteractableKeyString -> "None" for the clump), NOT key.len=0. The receiver's drive-
                // continuity gate (remote_prop.cpp KeyMatchesCache) re-StartDrives -- replaying the pickup
                // `use click` -- whenever the wire key CHANGES mid-stream for a live drive. Carry streamed
                // "None", this flight branch streamed "" -> the mismatch fired a spurious re-GRAB-IN + pickup
                // sound on EVERY throw (take-29: 2 use-clicks/cycle, 58 total). Identical key across carry+
                // flight = one continuous drive, one grab sound, no churn. The eid is still the identity.
                const std::wstring fkeyW = ue_wrap::prop::GetInteractableKeyString(g_lastHeldProp);
                pp.key.len = 0;
                for (size_t i = 0; i < fkeyW.size() && i < 31; ++i)
                    pp.key.data[pp.key.len++] = static_cast<char>(fkeyW[i]);
                pp.elementId = static_cast<uint32_t>(g_lastHeldEid);
                pp.ctx       = coop::trash_channel::CtxForEid(g_lastHeldEid);
                const auto loc = ue_wrap::engine::GetActorLocation(g_lastHeldProp);
                const auto rot = ue_wrap::engine::GetActorRotation(g_lastHeldProp);
                pp.x = loc.X; pp.y = loc.Y; pp.z = loc.Z;
                pp.pitch = ue_wrap::NormalizeAxis(rot.Pitch);
                pp.yaw   = ue_wrap::NormalizeAxis(rot.Yaw);
                pp.roll  = ue_wrap::NormalizeAxis(rot.Roll);
                session.SetLocalPropPose(true, pp);
                static uint64_t sFlight = 0;
                if ((sFlight++ % 30) == 0)
                    UE_LOGI("[PILE] HOST carry/flight CONTINUE eid=%u -> world(%.1f,%.1f,%.1f) (clump ALIVE: a "
                            "carry flicker OR the post-release FLIGHT -- one continuous E-stream until re-pile)",
                            static_cast<unsigned>(g_lastHeldEid), pp.x, pp.y, pp.z);
                // g_lastHeldProp/Eid are intentionally KEPT (not cleared) -- the stream continues; the land
                // path (re-pile thunk -> ToPile + the land-settle latch close) ends the carry.
            } else {
                UE_LOGI("[PILE] HOST carry SUPPRESS release eid=%u -- !carrying gate; clump re-piled/gone "
                        "(!IsLive or not-a-clump) -> the gap; await the re-grab or the land-settle close",
                        static_cast<unsigned>(g_lastHeldEid));
            }
        } else {
        // Edge: was holding, now not (NOT carrying -- the latch was already closed by the simulateDrop thunk
        // or the land-settle, or this is a non-trash prop). Stop the PropPose stream + send PropRelease.
        // Read the body's CURRENT linear+angular velocity. By the time this branch
        // runs, the engine has executed the BP graph clearing grabbing_actor, the
        // PHC.ReleaseComponent call, and any post-release AddImpulse the BP issues.
        // PhysX has not stepped yet, so the body still carries the inherited
        // kinematic-tracking velocity ("vzhukh" mouse-flick launch energy) PLUS any
        // impulse-derived velocity, summed into ONE velocity. prop::GetPhysicsVelocity
        // returns 0 for the non-Aprop_C clump (GetStaticMesh is null on it) -- fall
        // back to the GENERIC root-component velocity so the clump throws too.
        session.SetLocalPropPose(false, {});  // stop the held-pose stream (BOTH paths below)
        // SOUND/RELEASE FIX (take-29 #2b): a TRASH entity's throw is owned end-to-end by the host-authoritative
        // channel -- the flight-stream above carries the arc, and the re-pile thunk's ToPile convert is the
        // authoritative landing (it re-skins + snaps + ClearAnyDriveFor on the client). By the time this FIRE
        // edge runs for a trash eid, carrying has ALREADY closed (the land committed) -> this PropRelease is
        // REDUNDANT and HARMFUL: the client turned each one into a spurious `proxy throw` ClearAnyDriveFor that
        // churned the drive (re-GRAB-IN -> replayed pickup sound) -- take-29: 29 spurious releases + 0 whoosh.
        // The phase-1 NoCollision proxy must NOT simulate clump throw physics anyway. So for a tracked trash
        // entity (CtxForEid != 0): stop the stream (done) + clear the held cache (below), but DO NOT send a
        // PropRelease. A non-trash Aprop_C keeps its normal velocity release (the carry/throw it always had).
        const uint32_t relEid = (g_lastHeldEid == coop::element::kInvalidId)
                                    ? 0u : static_cast<uint32_t>(g_lastHeldEid);
        const bool isTrashEid = (g_lastHeldEid != coop::element::kInvalidId &&
                                 coop::trash_channel::CtxForEid(g_lastHeldEid) != 0);
        if (isTrashEid) {
            UE_LOGI("[PILE] HOST trash release eid=%u -- PropRelease SUPPRESSED (host-auth flight-stream + "
                    "ToPile convert own the throw end; client drives no clump physics, ToPile clears the drive)",
                    relEid);
        } else {
            ue_wrap::prop::VelocityState vel{};
            if (R::IsLiveByIndex(g_lastHeldProp, g_lastHeldPropIdx)) {
                vel = ue_wrap::prop::GetPhysicsVelocity(g_lastHeldProp);
                if (!vel.ok) {
                    ue_wrap::FVector lin{}, ang{};
                    if (ue_wrap::engine::GetActorRootPhysicsVelocity(g_lastHeldProp, lin, ang)) {
                        vel.linearCmS = lin; vel.angularDegS = ang; vel.ok = true;
                    }
                }
            }
            const float linMagSq = vel.linearCmS.X * vel.linearCmS.X +
                                   vel.linearCmS.Y * vel.linearCmS.Y +
                                   vel.linearCmS.Z * vel.linearCmS.Z;
            UE_LOGI("net: held -> released (vel.ok=%d linVel=(%.1f, %.1f, %.1f) |v|=%.1f cm/s angVel=(%.1f, %.1f, %.1f))",
                    vel.ok ? 1 : 0,
                    vel.linearCmS.X, vel.linearCmS.Y, vel.linearCmS.Z,
                    std::sqrt(linMagSq),
                    vel.angularDegS.X, vel.angularDegS.Y, vel.angularDegS.Z);
            session.SendPropRelease(g_lastHeldKey,
                                    vel.linearCmS.X, vel.linearCmS.Y, vel.linearCmS.Z,
                                    vel.angularDegS.X, vel.angularDegS.Y, vel.angularDegS.Z, relEid, /*relCtx=*/0u);
        }
        g_lastHeldProp = nullptr;
        g_lastHeldKey = {};
        g_lastHeldEid = coop::element::kInvalidId;  // v81: invalidate the cached held eid on release
        g_lastHeldPropIdx = -1;                      // take-30: invalidate the cached GC-safe index too
        }  // end real-release branch (CLOSE-B flicker gate above)
    }

    // v22 ragdoll PHYSICS stream. While the local player's native ragdoll
    // exists (C-key/faint/KO), read its pelvis world transform + linear+angular
    // velocity and publish so each peer's mirror body slaves its pelvis to the
    // real ragdoll (instead of free-simulating its own flop). Mirrors the held-
    // prop stream above: publish each frame while active, one false-edge on
    // recover. ReadLocalRagdollPelvisPhysics returns false when not ragdolling,
    // so the active->idle transition is the recover edge.
    {
        ue_wrap::FVector rdLoc{}, rdLin{}, rdAng{};
        ue_wrap::FRotator rdRot{};
        if (ue_wrap::engine::ReadLocalRagdollPelvisPhysics(local, rdLoc, rdRot, rdLin, rdAng)) {
            coop::net::RagdollPoseSnapshot rp{};
            rp.x = rdLoc.X; rp.y = rdLoc.Y; rp.z = rdLoc.Z;
            // Normalize the rotation at the wire boundary (the receiver drives
            // SetActorRotation off it + the canonical FRotator guard expects
            // (-180,180]); velocities are magnitudes, sent raw.
            rp.pitch = ue_wrap::NormalizeAxis(rdRot.Pitch);
            rp.yaw   = ue_wrap::NormalizeAxis(rdRot.Yaw);
            rp.roll  = ue_wrap::NormalizeAxis(rdRot.Roll);
            rp.linVelX = rdLin.X; rp.linVelY = rdLin.Y; rp.linVelZ = rdLin.Z;
            rp.angVelX = rdAng.X; rp.angVelY = rdAng.Y; rp.angVelZ = rdAng.Z;
            session.SetLocalRagdollPose(true, rp);
            g_wasRagdolling = true;
            const uint64_t n = ++g_ragdollEmitCount;
            if (n <= 3 || (n % 60) == 0) {
                const float linMag = std::sqrt(rdLin.X * rdLin.X + rdLin.Y * rdLin.Y + rdLin.Z * rdLin.Z);
                UE_LOGI("net: RagdollPose emit #%llu -> pelvis(%.0f, %.0f, %.0f) rot(%.0f, %.0f, %.0f) |linVel|=%.0f cm/s",
                        static_cast<unsigned long long>(n),
                        rp.x, rp.y, rp.z, rp.pitch, rp.yaw, rp.roll, linMag);
            }
        } else if (g_wasRagdolling) {
            session.SetLocalRagdollPose(false, {});
            g_wasRagdolling = false;
            UE_LOGI("net: RagdollPose stream STOP (local player recovered)");
        }
    }
}

}  // namespace coop::local_streams
