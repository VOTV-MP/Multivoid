// coop/local_streams.cpp -- see coop/local_streams.h.
//
// Extracted from net_pump.cpp 2026-06-12 (modular soft cap). Bodies moved
// VERBATIM with their rationale comments: ReadLocalPose, the held-prop
// stream + edges, and the v22 ragdoll pelvis-physics stream.

#include "coop/local_streams.h"

#include "coop/dev/perf_probe.h"
#include "coop/element/element.h"
#include "coop/ini_config.h"
#include "coop/kerfur_entity.h"  // K-5: GetKerfurMirrorEidForActor (held-kerfur-prop eid fallback)
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/trash_channel.h"   // docs/piles/08: CtxForEid (carry) + OnHostRelease (throw) -- the trash sync-time-context
#include "coop/prop_element_tracker.h"
#include "coop/prop_stick_sync.h"
#include "coop/remote_prop.h"     // ResolveMirrorEidByActor (the bound-clump held-pose eid fallback)
#include "coop/trash_collect_sync.h"

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

void OnSessionStart() {
    g_lastHeldProp = nullptr;
    g_lastHeldKey = {};
    g_lastHeldEid = coop::element::kInvalidId;
    g_propEmitCount = 0;
    g_wasRagdolling = false;
    g_ragdollEmitCount = 0;
}

void Tick(coop::net::Session& session, void* local, void* controller) {
    namespace PP = coop::dev::perf_probe;
    coop::net::PoseSnapshot mine;
    { PP::Scope _s{PP::Bucket::LocalSend};
      if (ReadLocalPose(local, controller, mine)) session.SetLocalPose(mine); }

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
    if (ue_wrap::engine::ReadMainPlayerGrabState(local, gs)) {
        heldActor = gs.grabbingActor;
        // 2026-05-27: chipPile/clump pickup sets mainPlayer.holding_actor
        // INSTEAD of grabbing_actor (their morph path doesn't use the
        // PhysicsHandle). Fall back to holding_actor so the PropPose
        // stream covers them too -- gated by IsKeyedInteractable since
        // holding_actor can also point at non-prop carry targets.
        if (!heldActor && gs.holdingActor && R::IsLive(gs.holdingActor) &&
            ue_wrap::prop::IsKeyedInteractable(gs.holdingActor)) {
            heldActor = gs.holdingActor;
        }
    }
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
    if (heldActor && R::IsLive(heldActor)) {
        // New-held edge: broadcast a PropSpawn so each peer spawns a mirror. Aprop_C
        // items get a force-minted Key; the NON-KEYABLE trash CLUMP rides the SAME
        // prop pipeline identified by our EID (key=None -- setKey doesn't stick on
        // it). The clump renders on its own (a bare spawn = the 'dirtball' mesh),
        // floats in front of the puppet via this pose stream (like the mannequin),
        // and gets physics on release. [[project-bug-trash-chippile-uaf-crash]]
        if (heldActor != g_lastHeldProp) {
            // New-held edge. A held trash CLUMP (the chipPile's grab product) is ADOPTED HERE onto the
            // grabbed pile's eid E (AdoptPendingGrabClump, below): the clump's BeginDeferred spawn is
            // EX_CallMath -> invisible to our ProcessEvent hook, so the grab is caught at the InpActEvt_use
            // PRE (NotePendingGrab) and the clump is bound + the PropConvert{ToClump} broadcast at THIS
            // held-edge -- NOT at any spawn POST (docs/piles/08; the host_spawn_watcher convert-POST was
            // disproven 2026-06-21). EnsureHeldItemBroadcast returns false for a clump (class gate at
            // trash_collect_sync.cpp); a normal Aprop_C item is broadcast as a fresh keyed prop. Resolve the
            // wire eid ONCE here (the only place the O(n) ResolveMirrorEidByActor
            // fallback for the eid-only clump may run) and CACHE it (g_lastHeldEid) so the carry stream is O(1).
            const bool mirrored = coop::trash_collect_sync::EnsureHeldItemBroadcast(heldActor, &session);
            // HOST grab adoption (docs/piles/08): a freshly-grabbed trash CLUMP is eid-less here -- bind it
            // to the pile's eid recorded at the InpActEvt PRE seam (the clump's BeginDeferred spawn is
            // EX_CallMath -> invisible, so we can't catch it at spawn). AdoptPendingGrabClump rebinds E onto
            // the clump + broadcasts PropConvert{ToClump} + returns E; we cache E as the carry eid.
            coop::element::ElementId adoptedEid = coop::element::kInvalidId;
            if (session.role() == coop::net::Role::Host && ue_wrap::prop::IsGarbageClump(heldActor)) {
                const auto cloc = ue_wrap::engine::GetActorLocation(heldActor);
                const auto crot = ue_wrap::engine::GetActorRotation(heldActor);
                adoptedEid = coop::trash_channel::AdoptPendingGrabClump(session, heldActor, cloc, crot);
                // (The re-pile death-watch enroll is GONE 2026-06-21, RULE 2: the clump's re-pile is now
                // caught deterministically at its EX_CallMath BeginDeferred via the UFunction::Func thunk
                // -- trash_collect_sync::OnBeginDeferredSpawnObserve -- so no proximity watch is needed.)
            }
            g_lastHeldEid = (adoptedEid != coop::element::kInvalidId) ? adoptedEid
                                                                      : ResolveHeldPropEid(heldActor);
            const unsigned eidLog =
                (g_lastHeldEid == coop::element::kInvalidId) ? 0u : static_cast<unsigned>(g_lastHeldEid);
            UE_LOGI("net: NEW held actor %p cls='%ls' key='%ls' eid=%u -> %s",
                    heldActor, R::ClassNameOf(heldActor).c_str(),
                    ue_wrap::prop::GetInteractableKeyString(heldActor).c_str(),
                    eidLog, mirrored ? "BROADCAST" : "carry-only(trash/clump)");
            // [PILE] carry phase: a trash clump (carry-only, eid-identified) is now in hand. The convert
            // (pile->clump) was broadcast by AdoptPendingGrabClump on the new-held edge above; this stream
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
        }
        g_lastHeldProp = heldActor;
        g_lastHeldKey = pp.key;
    } else if (g_lastHeldProp) {
        // Edge: was holding, now not. Stop the PropPose stream + send PropRelease.
        // Read the body's CURRENT linear+angular velocity. By the time this branch
        // runs, the engine has executed the BP graph clearing grabbing_actor, the
        // PHC.ReleaseComponent call, and any post-release AddImpulse the BP issues.
        // PhysX has not stepped yet, so the body still carries the inherited
        // kinematic-tracking velocity ("vzhukh" mouse-flick launch energy) PLUS any
        // impulse-derived velocity, summed into ONE velocity. prop::GetPhysicsVelocity
        // returns 0 for the non-Aprop_C clump (GetStaticMesh is null on it) -- fall
        // back to the GENERIC root-component velocity so the clump throws too.
        session.SetLocalPropPose(false, {});
        ue_wrap::prop::VelocityState vel{};
        if (R::IsLive(g_lastHeldProp)) {
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
        // v82 (docs/piles/08): the HOST owns the trash entity's sync-time-context -- bump it on the throw
        // (HELD -> FLYING) and stamp the PropRelease so a stale carry can't re-apply post-throw. 0 on a
        // client / a non-trash release (OnHostRelease returns 0 for an eid it doesn't author). The eid
        // routes the keyless clump throw (key=None can't disambiguate one clump from another).
        const uint32_t relEid = (g_lastHeldEid == coop::element::kInvalidId)
                                    ? 0u : static_cast<uint32_t>(g_lastHeldEid);
        const uint8_t relCtx = (session.role() == coop::net::Role::Host)
                                   ? coop::trash_channel::OnHostRelease(g_lastHeldEid) : 0u;
        if (relEid != 0 && relCtx != 0) {
            UE_LOGI("[PILE] HOST THROW eid=%u -> FLYING (PropRelease ctx=%u |v|=%.0f cm/s) -- clients re-enable physics on the clump mirror",
                    relEid, static_cast<unsigned>(relCtx), std::sqrt(linMagSq));
        }
        session.SendPropRelease(g_lastHeldKey,
                                vel.linearCmS.X, vel.linearCmS.Y, vel.linearCmS.Z,
                                vel.angularDegS.X, vel.angularDegS.Y, vel.angularDegS.Z, relEid, relCtx);
        g_lastHeldProp = nullptr;
        g_lastHeldKey = {};
        g_lastHeldEid = coop::element::kInvalidId;  // v81: invalidate the cached held eid on release
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
