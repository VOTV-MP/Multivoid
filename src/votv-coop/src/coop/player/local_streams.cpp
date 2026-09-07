// coop/local_streams.cpp -- the local player's outbound streams: the pose, the held prop (with
// its new-held and release edges) and the ragdoll pelvis physics. See coop/local_streams.h.

#include "coop/player/local_streams.h"

#include "coop/dev/perf_probe.h"
#include "coop/element/element.h"
#include "coop/config/config.h"
#include "coop/creatures/kerfur_entity.h"  // GetKerfurMirrorEidForActor, the held kerfur prop's eid
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/hand_item.h"     // the hotbar hand-item axis
#include "coop/player/skin_effects.h"  // own-body step FX at the wire-pose stride
#include "coop/props/trash_channel.h"   // CtxForEid, the trash sync-time context
#include "coop/props/prop_element_tracker.h"
#include "coop/props/prop_stick_sync.h"
#include "coop/props/remote_prop.h"     // ResolveMirrorEidByActor (the bound-clump held-pose eid fallback)
#include "coop/props/trash_collect_sync.h"

#include "ue_wrap/devices/atv.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/actors/puppet.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/actors/vitals.h"

#include <cmath>
#include <cstdint>
#include <string>

namespace coop::local_streams {
namespace {

namespace R = ue_wrap::reflection;

// The held-prop edge state, file scope rather than a static local in Tick: a static local would
// carry a stale prop and key across a session restart and send the old session's PropRelease on
// the new one. Cleared by OnSessionStart. A CachedObjRef, since the actor is set on the new-held
// edge and read on later edges.
ue_wrap::CachedObjRef g_lastHeldProp;
coop::net::WireKey g_lastHeldKey{};
// The held actor's wire eid, resolved once on the new-held edge (where the O(n) mirror lookup
// for a morph-bound clump may run) and reused per tick; a held actor's identity is stable for
// the carry. kInvalidId = unkeyed.
coop::element::ElementId g_lastHeldEid = coop::element::kInvalidId;
uint64_t g_propEmitCount = 0;

// The ragdoll-physics edge state, file scope for the same reason; cleared by OnSessionStart.
bool g_wasRagdolling = false;
uint64_t g_ragdollEmitCount = 0;

// The local player's pose, on the game thread at the send rate.
bool ReadLocalPose(void* local, void* controller, coop::net::PoseSnapshot& out) {
    if (!local) return false;
    const ue_wrap::FVector loc = ue_wrap::engine::GetActorLocation(local);
    const ue_wrap::FRotator actorRot = ue_wrap::engine::GetActorRotation(local);
    const ue_wrap::FVector vel = ue_wrap::engine::GetActorVelocity(local);
    // Body yaw from the actor (sending the controller yaw made the puppet body face the camera
    // while moving); on foot the actor yaw follows the camera almost immediately, so the receiver
    // synthesises the standing turn-in-place itself (RemotePlayer::UpdateBodyYaw). Head pitch from
    // the controller (an upright character's actor pitch is 0); net_pump caches the controller.
    out.x = loc.X;
    out.y = loc.Y;
    // Z is the actor's Z (the capsule centre); the receiver reconstructs the visible body offset
    // from its own mainPlayer_C's relative Z at puppet spawn, the same BP class on every peer.
    // MTA's shape: the streamed position is the capsule centre.
    out.z = loc.Z;
    // Yaw and pitch normalised into (-180, 180] before the wire: GetControlRotation returns the raw
    // ControlRotation, so looking 10 degrees down reads Pitch=350, which fails ValidatePose's bound
    // and dropped the whole packet (the puppet froze whenever the host looked below the horizon).
    out.yaw = ue_wrap::NormalizeAxis(actorRot.Yaw);
    const ue_wrap::FRotator ctlRot = controller
        ? ue_wrap::engine::GetControlRotation(controller)
        : actorRot;
    out.pitch = ue_wrap::NormalizeAxis(ctlRot.Pitch);
    // headYawDelta is the controller yaw's lead over the body yaw, in (-180, 180]; the puppet's
    // AnimBP head look-at reads it so the head turns where the source's camera looks.
    out.headYawDelta = ue_wrap::NormalizeAxis(ctlRot.Yaw - actorRot.Yaw);
    out.speed = std::sqrt(vel.X * vel.X + vel.Y * vel.Y);
    // The airborne bit: the receiver clears the puppet's leg IK for the airborne window so the foot
    // trace does not plant its feet while the source jumps or falls. Read through the wrapper,
    // which guards the actor and the movement component.
    out.stateBits = 0;
    if (ue_wrap::puppet::ReadCharacterIsFalling(local)) {
        out.stateBits |= coop::net::kStateBitInAir;
    }
    // The ragdoll bit: isRagdoll is the AnimBP gate every ragdoll cause flips (the C key, an
    // exhaustion faint, a KO), synced as a display flag so the puppet flops on the other screens; a
    // death is excluded by `dead`. One wrapper read.
    {
        bool isRagdoll = false, dead = false;
        if (ue_wrap::engine::ReadMainPlayerRagdollState(local, isRagdoll, dead) &&
            isRagdoll && !dead) {
            out.stateBits |= coop::net::kStateBitRagdoll;
        }
    }
    // The vitals (health fraction, food, sleep) in three bytes: ue_wrap::vitals reads this
    // machine's UsaveSlot_C, so each peer packs its own. Full (255) until the save resolves, so a
    // booting peer does not flash an empty bar; the receiver treats them as display only. O(1)
    // reads after the one-time resolve.
    out.healthFrac = out.foodFrac = out.sleepFrac = 255;
    {
        namespace V = ue_wrap::vitals;
        float health = 0.f, maxHealth = coop::net::kVitalScalarMax, food = 0.f, sleep = 0.f;
        bool gotHealth = false;
        if (V::Read(V::Field::Health, &health)) {
            gotHealth = true;
            V::Read(V::Field::MaxHealth, &maxHealth);  // best effort; 100 on a miss
            out.healthFrac = coop::net::QuantizeUnitFraction(
                maxHealth > 0.f ? health / maxHealth : 1.f);
        }
        if (V::Read(V::Field::Food, &food))
            out.foodFrac = coop::net::QuantizeUnitFraction(food * (1.f / coop::net::kVitalScalarMax));
        if (V::Read(V::Field::Sleep, &sleep))
            out.sleepFrac = coop::net::QuantizeUnitFraction(sleep * (1.f / coop::net::kVitalScalarMax));
        // One log line on the first successful health read, so a full bar is proven rather than a
        // silent default; game thread only, so a plain static latch.
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

// The wire eid of a held prop: a keyed prop is in the tracker's local map; a kerfur prop on a
// client is a host-owned mirror outside it, so its host-range eid comes from the kerfur held-pose
// map (its random BP key would not resolve, and the host's receiver drives the authoritative
// prop by eid); a garbage clump bound to its pile's eid by a client's morph is a mirror too, and
// resolves through the mirror lookup. kInvalidId if none. New-held edge only; the caller caches
// it.
coop::element::ElementId ResolveHeldPropEid(void* heldActor) {
    auto eid = coop::prop_element_tracker::GetPropElementIdForActor(heldActor);
    if (eid == coop::element::kInvalidId)
        eid = coop::kerfur_entity::GetKerfurMirrorEidForActor(heldActor);
    if (eid == coop::element::kInvalidId)
        eid = coop::remote_prop::ResolveMirrorEidByActor(heldActor);
    return eid;
}

}  // namespace

void NotifyPropEidRebound(void* actor) {
    // The held-eid cache refreshes when the held actor's identity rebinds mid-carry; otherwise it
    // is resolved only at the held edge.
    if (!actor || actor != g_lastHeldProp.Raw()) return;
    const coop::element::ElementId neweid = ResolveHeldPropEid(actor);
    if (neweid == g_lastHeldEid) return;
    UE_LOGI("local_streams: held prop %p eid rebind %u -> %u (identity fanout; carry stream follows the new id)",
            actor,
            (g_lastHeldEid == coop::element::kInvalidId) ? 0u : static_cast<unsigned>(g_lastHeldEid),
            (neweid == coop::element::kInvalidId) ? 0u : static_cast<unsigned>(neweid));
    g_lastHeldEid = neweid;
}

void* LastHeldActor() {
    // The local player's held actor, or null, for the trash channel's rest exclusion; live-guarded
    // so a stale pointer never aliases a recycled address.
    return g_lastHeldProp.Get();
}

void OnSessionStart() {
    g_lastHeldProp.Reset();
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
      if (ReadLocalPose(local, controller, mine)) {
          session.SetLocalPose(mine);
          // The skin step FX on the own body: the native stride calls lib_C::step through an
          // EX_CallMath the ProcessEvent hook cannot see, so the coop layer re-strides the pose
          // sample it just read.
          coop::skin_effects::TickStride(
              local, ue_wrap::FVector{mine.x, mine.y, mine.z}, mine.speed,
              (mine.stateBits & coop::net::kStateBitInAir) == 0);
      }
    }

    // The held-prop stream: mainPlayer.grabbing_actor (through the wrapper) becomes a
    // PropPoseSnapshot per tick, and the held-to-not-held edge sends a reliable PropRelease so the
    // peer re-enables physics without waiting for the 500 ms stream-stop timeout.
    ue_wrap::engine::MainPlayerGrabState gs{};
    void* heldActor = nullptr;
    void* hotbarProp = nullptr;  // holding_actor when it is the HAND item (Aprop_C)
    if (ue_wrap::engine::ReadMainPlayerGrabState(local, gs)) {
        heldActor = gs.grabbingActor;
        // grabbing_actor is the physics-handle grab and comes first; holding_actor is the
        // fallback, gated on IsKeyedInteractable since it can point at other carry targets. An
        // Aprop_C in holding_actor is the hotbar hand item, player expression rather than a world
        // entity (updateHold respawns it per switch), and goes to coop::hand_item; only the trash
        // lineage stays here.
        if (gs.holdingActor && R::IsLive(gs.holdingActor) &&
            ue_wrap::prop::IsKeyedInteractable(gs.holdingActor)) {
            if (ue_wrap::prop::IsDescendantOfProp(gs.holdingActor))
                hotbarProp = gs.holdingActor;
            else if (!heldActor)
                heldActor = gs.holdingActor;
        }
    }
    // The hand-item axis: the owner announces a change, the peers maintain their mirrors; both idle
    // cheaply.
    coop::hand_item::TickOwner(session, local, hotbarProp);
    coop::hand_item::TickMirrors();
    // The ATV belongs to coop::atv_sync: a grabbed ATV is not a keyed interactable and would emit
    // zero-key poses the receivers drop.
    if (heldActor && ue_wrap::atv::IsAtv(heldActor)) heldActor = nullptr;
    // The garbage-pickup probe (ini garbage_pickup_probe=1): which of grabbing_actor and
    // holding_actor a clump pickup sets, plus the held class, key and keyed verdict; while
    // something is held, about 4 Hz; one bool load when off.
    static const bool sProbeGarbage =
        ::coop::config::ResolveFlag(::coop::config_registry::rows::garbage_pickup_probe);
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
    // A held wall-attachable that just stuck (frozen or static; the engine hold lingers up to
    // 100 ms) ends the stream now: streamed poses could reach the receiver's unstick streak and rip
    // the mirror back off, and ending here fires the release edge in the same pump pass as, and
    // lane-ordered after, the PropStickState broadcast. A re-grab unstick clears the flags before
    // this read, so the stream resumes at once.
    if (heldActor && R::IsLive(heldActor) &&
        coop::prop_stick_sync::IsWallAttachable(heldActor) &&
        (ue_wrap::prop::IsFrozen(heldActor) || ue_wrap::prop::IsStatic(heldActor))) {
        heldActor = nullptr;
    }
    // The held-state diagnostic at the branch point: the pose branch, the release edge, or idle;
    // about 8 a second while something is or was held.
    if (heldActor || g_lastHeldProp.Raw()) {
        static uint32_t sHS = 0;
        if ((sHS++ % 15) == 0) {
            const bool hLive = heldActor && R::IsLive(heldActor);
            UE_LOGI("[HELD-STATE] heldActor=%p(live=%d) g_lastHeldProp=%p g_lastHeldEid=%u carrying=%d -> %s",
                    heldActor, hLive ? 1 : 0, g_lastHeldProp.Raw(),
                    (g_lastHeldEid == coop::element::kInvalidId) ? 0u : static_cast<unsigned>(g_lastHeldEid),
                    coop::trash_channel::IsCarrying(g_lastHeldEid) ? 1 : 0,
                    (heldActor && hLive) ? "MAIN(pose)" : (g_lastHeldProp.Raw() ? "RELEASE-EDGE" : "idle"));
        }
    }
    if (heldActor && R::IsLive(heldActor)) {
        // The held item: an Aprop_C carries a force-minted key; the non-keyable trash clump rides
        // the same pipeline identified by eid (key None), renders as the bare dirtball, floats in
        // front of the puppet through this stream and gets physics on release.
        if (heldActor != g_lastHeldProp.Raw()) {
            // The new-held edge. A held trash clump is adopted here onto the grabbed pile's eid:
            // the birth certificate the BeginDeferred thunk recorded at its spawn carries the pile
            // eid and chipType, and this edge consumes it and broadcasts the ToClump convert.
            // EnsureHeldItemBroadcast is false for a clump; an Aprop_C is broadcast as a fresh
            // keyed prop. The wire eid resolves once here and is cached for the carry.
            const bool mirrored = coop::trash_collect_sync::EnsureHeldItemBroadcast(heldActor, &session);
            // The host's grab adoption: the clump's identity is its birth certificate (every clump
            // is born from a chipPile, on an E-press or a use-hold alike). An existing tracked
            // clump re-entering the hand resolves by the tracker. A "no certificate mid-carry means
            // a churn re-grab of my last carry" guess bound a foreign clump to the wrong lane with
            // two clumps in flight, so no such guess.
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
                        // The game re-grabbed my carried entity (the just-re-piled pile morphs
                        // straight back): rebind and cancel the settle.
                        coop::trash_channel::OnHostRegrab(bornE, heldActor);
                        churnRegrab   = true;
                        g_lastHeldEid = bornE;
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
            // The carry-only invariant: every refusal in EnsureHeldItemBroadcast is an anti-dupe
            // gate that assumes the receiver resolves the item by another route (a kerfur prop by
            // its mirror eid, a clump by its pile's eid, a claimed save-loaded local by its shared
            // key). With no eid that precondition is false and the stream is undeliverable: the
            // receiver tries key then eid and gets a zero eid plus a key it may never have seen (in
            // the field, 91 poses in 1.5 s no peer could resolve, and an item invisible to everyone
            // but its holder). Reported once per held edge with the actor named, and not
            // suppressed: a key-only pose is legitimate when both peers share a save-loaded prop.
            if (!mirrored && g_lastHeldEid == coop::element::kInvalidId) {
                UE_LOGW("local_streams: CARRY-ONLY WITH NO WIRE IDENTITY -- held actor %p "
                        "cls='%ls' key='%ls' is streaming a held pose with eid=0 and no spawn "
                        "broadcast, so a receiver can resolve NEITHER term. Every carry-only "
                        "gate assumes an identity travels by another route; here none does. "
                        "role=%s",
                        heldActor, R::ClassNameOf(heldActor).c_str(),
                        ue_wrap::prop::GetInteractableKeyString(heldActor).c_str(),
                        (session.role() == coop::net::Role::Host) ? "host" : "client");
            }
            // The carry phase: the pile-to-clump convert went out on the edge above; this stream
            // carries the eid's pose.
            if (!mirrored && eidLog != 0 && ue_wrap::prop::IsGarbageClump(heldActor)) {
                UE_LOGI("[PILE] %s CARRY eid=%u clump in hand -> streaming carry pose (ctx-stamped); "
                        "clients drive their mirror of E",
                        (session.role() == coop::net::Role::Host) ? "HOST" : "CLIENT", eidLog);
            }
        }
        // The held world transform: key (None for the clump) and eid; the receiver resolves by key,
        // then eid.
        const std::wstring keyW = ue_wrap::prop::GetInteractableKeyString(heldActor);
        coop::net::PropPoseSnapshot pp{};
        pp.key.len = 0;
        for (size_t i = 0; i < keyW.size() && i < 31; ++i) {
            // Save keys are ASCII; the narrowing is lossless.
            pp.key.data[pp.key.len++] = static_cast<char>(keyW[i]);
        }
        // The eid cached on the new-held edge; the O(n) resolve never runs per tick.
        pp.elementId = (g_lastHeldEid == coop::element::kInvalidId) ? 0u
                                                                    : static_cast<uint32_t>(g_lastHeldEid);
        // The trash entity's sync-time context, so the receiver drops a carry pose that arrives
        // after a transition; 0 for a non-trash prop.
        pp.ctx = coop::trash_channel::CtxForEid(g_lastHeldEid);
        const auto loc = ue_wrap::engine::GetActorLocation(heldActor);
        const auto rot = ue_wrap::engine::GetActorRotation(heldActor);
        pp.x = loc.X; pp.y = loc.Y; pp.z = loc.Z;
        // Normalised at the wire boundary: a physics prop's rotation accumulates through quaternion
        // conversions to values like Yaw 359.8 that the receiver's guard rejects.
        pp.pitch = ue_wrap::NormalizeAxis(rot.Pitch);
        pp.yaw   = ue_wrap::NormalizeAxis(rot.Yaw);
        pp.roll  = ue_wrap::NormalizeAxis(rot.Roll);
        // Only a pose with a cross-peer identity (a key or an eid) is streamed: a clump grabbed
        // before quiescence that the broadcast declined has neither, and streaming it floods the
        // peer with unresolved-pose warnings for an actor it will never have. The stream resumes
        // the instant the item is expressed; g_lastHeldProp is still tracked so the release edge
        // works, and a PropRelease for a never-expressed prop is harmless.
        if (pp.key.len > 0 || pp.elementId != 0) {
            session.SetLocalPropPose(true, pp);
            // The first 3 and every 60th, matching the receiver's throttle so the two logs diff
            // line for line.
            const uint64_t n = ++g_propEmitCount;
            if (n <= 3 || (n % 60) == 0) {
                UE_LOGI("net: PropPose emit #%llu -> world(%.1f, %.1f, %.1f) rot(%.1f, %.1f, %.1f) key.len=%d eid=%u ctx=%u",
                        static_cast<unsigned long long>(n),
                        pp.x, pp.y, pp.z, pp.pitch, pp.yaw, pp.roll,
                        static_cast<int>(pp.key.len), pp.elementId, static_cast<unsigned>(pp.ctx));
            }
        } else {
            // The held clump has no identity, so the pose is not streamed; firing between grabs
            // means the cached eid went invalid, not a dead branch.
            static uint64_t sPK = 0;
            if ((sPK++ % 15) == 0)
                UE_LOGI("[POSE-SKIP] eid=0 key.len=0 -- held clump has NO identity to stream (g_lastHeldEid invalid -> carry frozen between E)");
        }
        // heldActor is live here, so Set's contract holds; the ref captures the index and serial.
        g_lastHeldProp.Set(heldActor);
        g_lastHeldKey = pp.key;
    } else if (g_lastHeldProp.Raw()) {
        // The carry latch owns "carrying", not the flickering holding_actor: a churn re-pile
        // destroys the held clump and the field is empty for a frame, and the held-puppet
        // recreation (updateHold rebuilds holding_actor) does the same, so this edge would clear
        // the cached eid and send a spurious PropRelease, churning the context until the client
        // held every carry pose and froze. No IsLive on the just-destroyed clump.
        const bool carrying_ = coop::trash_channel::IsCarrying(g_lastHeldEid);
        const bool pending_  = coop::trash_channel::HasPendingSettle(g_lastHeldEid);  // for the log line only
        // So the PropRelease is suppressed for the whole carry; the latch closes through the land
        // settle (a drop or throw's landing re-pile with no re-grab). No release verb is detected:
        // simulateDrop and dropGrabObject fire zero times for a clump release. Instead the stream
        // continues through it, one continuous eid stream that the client's interpolation shows as
        // the real arc.
        const bool relSkip   = carrying_;
        UE_LOGI("[REL-EDGE] eid=%u carrying=%d pendingSettle=%d -> %s",
                (g_lastHeldEid == coop::element::kInvalidId) ? 0u : static_cast<unsigned>(g_lastHeldEid),
                carrying_ ? 1 : 0, pending_ ? 1 : 0, relSkip ? "SKIP(carrying)" : "FIRE(release)");
        if (relSkip) {
            // Carry and flight are one stream: heldActor going null is either a one-frame flicker
            // mid-carry or a real release with the clump now flying, and in both the clump is
            // alive, so its pose keeps streaming under the same eid with no release verb and no
            // velocity. A churn re-pile destroys the clump (not alive), the gap: await the re-grab
            // or the land. The stream ends when the clump re-piles, wherever, and the ToPile
            // convert re-skins the proxy at the landed spot.
            if (g_lastHeldProp.Alive() &&
                ue_wrap::prop::IsGarbageClump(g_lastHeldProp.Raw()) &&
                g_lastHeldEid != coop::element::kInvalidId) {
                coop::net::PropPoseSnapshot pp{};
                // The same wire key the carry branch sends ("None" for the clump), not an empty
                // one: the receiver re-starts the drive, replaying the pickup click, whenever the
                // key changes mid-stream for a live drive, and an empty key here fired a spurious
                // grab and click on every throw.
                const std::wstring fkeyW = ue_wrap::prop::GetInteractableKeyString(g_lastHeldProp.Raw());
                pp.key.len = 0;
                for (size_t i = 0; i < fkeyW.size() && i < 31; ++i)
                    pp.key.data[pp.key.len++] = static_cast<char>(fkeyW[i]);
                pp.elementId = static_cast<uint32_t>(g_lastHeldEid);
                pp.ctx       = coop::trash_channel::CtxForEid(g_lastHeldEid);
                const auto loc = ue_wrap::engine::GetActorLocation(g_lastHeldProp.Raw());
                const auto rot = ue_wrap::engine::GetActorRotation(g_lastHeldProp.Raw());
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
                // The held cache is kept: the stream continues, and the land path ends the carry.
            } else {
                UE_LOGI("[PILE] HOST carry SUPPRESS release eid=%u -- !carrying gate; clump re-piled/gone "
                        "(!IsLive or not-a-clump) -> the gap; await the re-grab or the land-settle close",
                        static_cast<unsigned>(g_lastHeldEid));
            }
        } else {
        // The release edge (not carrying: the latch closed, or this is a non-trash prop). The
        // stream stops and a PropRelease goes out with the body's current velocity: the BP has
        // cleared grabbing_actor, released the handle and applied any impulse, and PhysX has not
        // stepped, so the body carries the kinematic-tracking velocity plus the impulse in one
        // value. The clump has no StaticMesh for GetPhysicsVelocity, so the generic root velocity
        // is the fallback.
        session.SetLocalPropPose(false, {});  // stop the held-pose stream (BOTH paths below)
        // A trash entity's throw is owned by the host-authoritative channel: the flight stream
        // carries the arc and the ToPile convert is the landing (it re-skins, snaps and clears the
        // client's drive). By this edge the carry has already closed, so a PropRelease here was
        // redundant and harmful: the client turned each into a proxy throw that churned the drive
        // and replayed the pickup sound. A tracked trash entity stops the stream and clears the
        // cache without a PropRelease; a non-trash prop keeps its velocity release.
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
            if (g_lastHeldProp.Alive()) {
                vel = ue_wrap::prop::GetPhysicsVelocity(g_lastHeldProp.Raw());
                if (!vel.ok) {
                    ue_wrap::FVector lin{}, ang{};
                    if (ue_wrap::engine::GetActorRootPhysicsVelocity(g_lastHeldProp.Raw(), lin, ang)) {
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
        g_lastHeldProp.Reset();
        g_lastHeldKey = {};
        g_lastHeldEid = coop::element::kInvalidId;  // the cached held eid goes with the release
        }
    }

    // The ragdoll physics stream: while the native ragdoll exists (the C key, a faint, a KO) the
    // pelvis world transform and velocities are published so each peer's mirror slaves its pelvis
    // to the real ragdoll instead of simulating its own flop; the read returns false when not
    // ragdolling, so the active-to-idle transition is the recover edge.
    {
        ue_wrap::FVector rdLoc{}, rdLin{}, rdAng{};
        ue_wrap::FRotator rdRot{};
        if (ue_wrap::engine::ReadLocalRagdollPelvisPhysics(local, rdLoc, rdRot, rdLin, rdAng)) {
            coop::net::RagdollPoseSnapshot rp{};
            rp.x = rdLoc.X; rp.y = rdLoc.Y; rp.z = rdLoc.Z;
            // The rotation normalised at the wire boundary; the velocities go raw.
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
