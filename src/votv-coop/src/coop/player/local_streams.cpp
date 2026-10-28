// coop/player/local_streams.cpp -- the local player's outbound streams: the pose, the held prop
// (with its new-held and release edges) and the ragdoll pelvis physics. See
// coop/player/local_streams.h.

#include "coop/player/local_streams.h"

#include "coop/dev/perf_probe.h"
#include "coop/dev/swinger_drill.h"  // NoteReleased
#include "coop/element/element.h"
#include "coop/element/quiescence_drain.h"  // SupersedePosCorrection: the own release is the newest pose
#include "coop/element/registry.h"          // EidForActor: the keyed eid of the released prop
#include "coop/interactables/interactable_sync.h"  // ContainerKey: a dragged swinger's portable key
#include "coop/config/config.h"
#include "coop/creatures/kerfur_entity.h"  // GetKerfurMirrorEidForActor, the held kerfur prop's eid
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/hand_item.h"     // the hotbar hand-item axis
#include "coop/player/players_registry.h"
#include "coop/player/skin_effects.h"  // own-body step FX at the wire-pose stride
#include "coop/props/trash_channel.h"   // CtxForEid, the trash sync-time context
#include "coop/props/prop_element_tracker.h"
#include "coop/props/prop_syncer.h"        // Claim: a dragged swinger's parent prop
#include "coop/props/prop_wire_parity.h"  // PhysFlagsOf, the flags a release carries
#include "coop/props/remote_prop.h"     // ResolveMirrorEidByActor (the bound-clump held-pose eid fallback)
#include "coop/props/trash_collect_sync.h"

#include "ue_wrap/devices/atv.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_attach.h"   // IsActorRootBodyAtRest: a released swinger stops coasting
#include "ue_wrap/core/log.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/actors/puppet.h"
#include "ue_wrap/actors/swinger.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/actors/vitals.h"

#include <chrono>
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
bool g_heldSwinger = false;   // the held actor is a swinger, set at each new-held edge
bool g_swingerKeySaid = false;   // the hold's key problem has been said
// The held actor's wire eid, resolved once on the new-held edge (where the O(n) mirror lookup
// for a morph-bound clump may run) and reused per tick; a held actor's identity is stable for
// the carry. kInvalidId = unkeyed.
coop::element::ElementId g_lastHeldEid = coop::element::kInvalidId;
// This peer's hold generation: a new one at every new-held edge, carried by each pose and closed by
// the release, so a receiver can tell a late pose of an ended hold from the next hold. Wraps, skips 0.
uint16_t g_holdGen = 0;
uint64_t g_propEmitCount = 0;

// The ragdoll-physics edge state, file scope for the same reason; cleared by OnSessionStart.
bool g_wasRagdolling = false;
uint64_t g_ragdollEmitCount = 0;

// The release branch below re-enters EVERY pump tick for the whole flight of a thrown clump --
// it deliberately keeps the held cache so the stream continues -- while its verdict stays
// constant across that window. These remember the verdict already reported so the branch logs
// one line per DECISION rather than one per tick at the ~60 Hz pump rate. File scope, cleared by
// OnSessionStart, for the same reason the edge state above is.
coop::element::ElementId g_relEdgeLoggedEid = coop::element::kInvalidId;
int  g_relEdgeLoggedSkip = -1;  // -1 = nothing reported yet, else the last relSkip reported
coop::element::ElementId g_gapLoggedEid = coop::element::kInvalidId;

// The local player's pose, on the game thread at the send rate.
bool ReadLocalPose(void* local, void* controller, coop::net::PoseSnapshot& out) {
    ue_wrap::FVector loc{};
    ue_wrap::FRotator actorRot{};
    if (!local || !ue_wrap::engine::TryGetActorLocation(local, loc) ||
        !ue_wrap::engine::TryGetActorRotation(local, actorRot))
        return false;
    const ue_wrap::FVector vel = ue_wrap::engine::GetActorVelocity(local);
    // Body yaw from the actor (sending the controller yaw made the puppet body face the camera
    // while moving); on foot the actor yaw follows the camera almost immediately, so the receiver
    // synthesises the standing turn-in-place (puppet_body_yaw::State::Update). Head pitch from
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
    // The burning bit: the body's own fire runs while its burningTime is above zero (the condition
    // mainPlayer_C's tick tests before it turns the flame off), so the other screens show the flame on
    // the puppet. Display only: a receiver never ignites anything. ue_wrap::vitals reads it off the
    // gamemode's own player, the body `local` names but for a few frames across a respawn.
    {
        float burn = 0.f;
        if (ue_wrap::vitals::Read(ue_wrap::vitals::Field::BurningTime, &burn) && burn > 0.f)
            out.stateBits |= coop::net::kStateBitBurning;
    }
    // The vitals (health fraction, food, sleep) in three bytes: ue_wrap::vitals reads this
    // machine's UsaveSlot_C, so each peer packs its own. Full (255) until the save resolves, so a
    // booting peer does not flash an empty bar; the receiver draws them, and the host's hit judge
    // reads a health of 0 as dead. O(1) reads after the one-time resolve.
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

// The held-prop pose of the hold: the identity (key, eid, the trash context), the hold's generation and
// the actor's place with its rotation normalised at the wire boundary. The hold branch and the swinger
// coast both stream through it, so a coast pose is the hold's pose.
coop::net::PropPoseSnapshot HeldPose(const coop::net::WireKey& key, coop::element::ElementId eid,
                                     const ue_wrap::FVector& loc, const ue_wrap::FRotator& rot) {
    coop::net::PropPoseSnapshot pp{};
    pp.key = key;
    // The eid cached on the new-held edge; the O(n) resolve never runs per tick.
    pp.elementId = (eid == coop::element::kInvalidId) ? 0u : static_cast<uint32_t>(eid);
    // The trash entity's sync-time context, so the receiver drops a carry pose that arrives
    // after a transition; 0 for a non-trash prop.
    pp.ctx = coop::trash_channel::CtxForEid(eid);
    pp.holdGen = g_holdGen;
    pp.x = loc.X; pp.y = loc.Y; pp.z = loc.Z;
    // Normalised at the wire boundary: a physics prop's rotation accumulates through quaternion
    // conversions to values like Yaw 359.8 that the receiver's guard rejects.
    pp.pitch = ue_wrap::NormalizeAxis(rot.Pitch);
    pp.yaw   = ue_wrap::NormalizeAxis(rot.Yaw);
    pp.roll  = ue_wrap::NormalizeAxis(rot.Roll);
    return pp;
}

// A swinger with nothing to hang on (a barrel's cap, a crate's lid) falls when it is let go, and each peer's own
// physics would carry it somewhere else. So the peer that let it go keeps the held pose stream running for it, from
// its own copy, until that copy is at rest, and only then sends the release: the other peers follow the same fall
// parked and let go of a door already where it stopped. At rest is the body asleep, or the same place and
// orientation for kCoastStillTicks equal reads (about half a second at the pump's rate), measured from the pose
// where that still run began, so a creep past the band is not rest. The release branch below is the trash flight's neighbour:
// that one keeps streaming a thrown clump under its eid; this one keeps a released swinger under its key.
// Divergence from MTA (CUnoccupiedVehicleSync.cpp, client and server): there the syncer sends while the
// vehicle changes and the server reassigns the syncer. Here the releaser is the syncer until rest, because no
// server-side physics arbiter exists and the pose carries no velocity, so the receivers stay parked through the
// fall instead of simulating from a velocity.
constexpr int   kCoastStillTicks = 30;   // a count of equal reads, not a duration
// A lid rocking against a contact cycles between two poses about 0.6 cm and 0.5 deg apart, both at rest, so the
// band is wider than that rock.
constexpr float kCoastStillCm    = 1.0f;
constexpr float kCoastStillDeg   = 1.0f;
// The bound on a coast: ten seconds from the release edge, a deadline and not a tick count, since the pump runs at
// about 60 Hz (the timeline loop sleeps 16 ms, harness/session_runtime.cpp) and slows with the game thread. A lid that
// keeps moving (on a moving carrier, or creeping past the band) is released at its current pose with its measured
// velocity, and every peer's own physics takes it from there.
constexpr auto  kCoastMax = std::chrono::seconds(10);
std::chrono::steady_clock::time_point g_coastDeadline{};
bool g_coasting = false;               // the release edge has begun a coast
ue_wrap::FVector  g_stillFromLoc{};    // where the current still run began
ue_wrap::FRotator g_stillFromRot{};
int g_coastStill = 0;                  // ticks within tolerance of that pose

// A swinger on a prop (a barrel's cap on its barrel) is joined to it by a constraint, so the drag rolls the parent
// too, and no channel carries that motion: the dragging peer syncs the parent (prop_syncer.h), claimed every tick of
// the hold and of the coast. A parent that does not simulate (a static frame) is not claimed.
void FeedSwingerParent(void* door) {
    static const bool sRedNoClaim =
        ::coop::config::ResolveEnum(::coop::config_registry::rows::swinger_drill_red) == "no_parent_claim";
    if (sRedNoClaim) return;
    void* const parent = ue_wrap::engine::ParentActorOf(door);
    if (!parent || !R::IsLive(parent) || !ue_wrap::engine::IsActorRootSimulatingPhysics(parent)) return;
    coop::prop_syncer::Claim(parent, coop::net::PropSyncReason::SwingerParent);
}

// True while the released swinger is still moving: its pose goes out this tick and the release waits.
bool CoastSwinger(coop::net::Session& session) {
    if (!g_heldSwinger || !g_lastHeldProp.Alive()) return false;
    // A lid that streamed nothing while held has nothing to wait for.
    if (g_lastHeldKey.len == 0 && g_lastHeldEid == coop::element::kInvalidId) return false;
    void* const door = g_lastHeldProp.Raw();
    ue_wrap::FVector loc{};
    ue_wrap::FRotator rot{};
    if (!ue_wrap::engine::TryGetActorLocation(door, loc) || !ue_wrap::engine::TryGetActorRotation(door, rot))
        return false;
    if (!g_coasting) {   // the first tick after the release edge starts the still run afresh
        g_coasting = true;
        g_coastDeadline = std::chrono::steady_clock::now() + kCoastMax;
        g_coastStill = 0;
        g_stillFromLoc = loc;
        g_stillFromRot = rot;
    }
    if (std::chrono::steady_clock::now() > g_coastDeadline) {
        UE_LOGI("local_streams: swinger '%.*s' still moving after %lld s of coasting -- released at its current pose",
                static_cast<int>(g_lastHeldKey.len), g_lastHeldKey.data, static_cast<long long>(kCoastMax.count()));
        return false;
    }
    const float dx = loc.X - g_stillFromLoc.X, dy = loc.Y - g_stillFromLoc.Y, dz = loc.Z - g_stillFromLoc.Z;
    const bool still = dx * dx + dy * dy + dz * dz <= kCoastStillCm * kCoastStillCm &&
                       ue_wrap::engine::RotatorAngleDeg(rot, g_stillFromRot) <= kCoastStillDeg;
    if (still) {
        ++g_coastStill;
    } else {
        g_coastStill = 0;
        g_stillFromLoc = loc;
        g_stillFromRot = rot;
    }
    // The body's rest flag is read only on a still tick: a body in motion is not at rest.
    if (g_coastStill >= kCoastStillTicks || (still && ue_wrap::engine::IsActorRootBodyAtRest(door))) return false;
    session.SetLocalPropPose(true, HeldPose(g_lastHeldKey, g_lastHeldEid, loc, rot));
    FeedSwingerParent(door);
    return true;
}

// The release of the held prop: the held-pose stream stops, a PropRelease goes out (a trash entity's is left to
// its channel) and the edge state clears. The release edge sends it, and so does a grab of another item while a
// swinger still coasts, so a coasting lid's release goes out once either way.
void SendHeldRelease(coop::net::Session& session) {
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
    // redundant and harmful: the client turned each into a trash throw that churned the drive
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
        // The prop's flags as the hold left them: not frozen after a grab, frozen when a slot or
        // a dock took it from the hand. A pocketed prop is gone and reports none, a trash clump has
        // none.
        coop::net::PropReleasePayload rel{};
        rel.key = g_lastHeldKey;
        rel.linVelX = vel.linearCmS.X; rel.linVelY = vel.linearCmS.Y; rel.linVelZ = vel.linearCmS.Z;
        rel.angVelX = vel.angularDegS.X; rel.angVelY = vel.angularDegS.Y; rel.angVelZ = vel.angularDegS.Z;
        rel.elementId = relEid;  // a keyless clump is routed by eid (key None cannot disambiguate)
        rel.holdGen = g_holdGen;
        if (g_lastHeldProp.Alive()) {
            rel.physFlags = coop::prop_wire_parity::PhysFlagsOf(g_lastHeldProp.Raw());
            // Where the hold let go, so a receiver whose last poses were lost lets go from here;
            // unread, the release carries no pose and the receiver lets go from its own.
            ue_wrap::FVector loc{};
            ue_wrap::FRotator rot{};
            if (ue_wrap::engine::TryGetActorLocation(g_lastHeldProp.Raw(), loc) &&
                ue_wrap::engine::TryGetActorRotation(g_lastHeldProp.Raw(), rot)) {
                rel.locX = loc.X; rel.locY = loc.Y; rel.locZ = loc.Z;
                rel.rotPitch = rot.Pitch; rel.rotYaw = rot.Yaw; rel.rotRoll = rot.Roll;
                rel.hasPose = 1;
            }
        }
        session.SendPropRelease(rel);
        coop::dev::swinger_drill::NoteReleased(g_lastHeldProp.Raw());
        // This peer's own release: a join-window correction armed before the hold is older than
        // where the prop was put down, and would snap it back once the drain's grab deferral ends.
        if (session.role() == coop::net::Role::Client && g_lastHeldProp.Alive()) {
            const coop::element::ElementId heldEid =
                coop::element::Registry::Get().EidForActor(g_lastHeldProp.Raw());
            if (heldEid != coop::element::kInvalidId && heldEid != 0u)
                coop::element::quiescence_drain::SupersedePosCorrection(heldEid, "own release");
        }
    }
    g_lastHeldProp.Reset();
    g_lastHeldKey = {};
    g_heldSwinger = false;
    g_coasting = false;
    g_lastHeldEid = coop::element::kInvalidId;  // the cached held eid goes with the release
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

uint16_t CurrentHoldGen() { return g_holdGen; }

void* LastHeldActor() {
    // The local player's held actor, or null, for the trash channel's rest exclusion; live-guarded
    // so a stale pointer never aliases a recycled address. A released swinger is returned while it
    // coasts; the readers (prop_hold, prop_drive_host, prop_drive_stream, prop_stick_sync and the
    // trash carry tick) act on props and trash only, and a swinger is neither.
    return g_lastHeldProp.Get();
}

void OnSessionStart() {
    g_lastHeldProp.Reset();
    g_lastHeldKey = {};
    g_heldSwinger = false;
    g_swingerKeySaid = false;
    g_coasting = false;
    g_coastStill = 0;
    g_coastDeadline = {};
    g_stillFromLoc = {};
    g_stillFromRot = {};
    g_lastHeldEid = coop::element::kInvalidId;
    g_propEmitCount = 0;
    g_wasRagdolling = false;
    g_ragdollEmitCount = 0;
    g_relEdgeLoggedEid = coop::element::kInvalidId;
    g_relEdgeLoggedSkip = -1;
    g_gapLoggedEid = coop::element::kInvalidId;
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
    if (hotbarProp) {   // once per process: the first hand item given to the hand lane, and its two gates
        static bool s_said = false;
        if (!s_said) {
            s_said = true;
            UE_LOGI("local_streams: first hand item to the hand lane cls=%ls connected=%d self=%u",
                    R::ClassNameOf(hotbarProp).c_str(), session.connected() ? 1 : 0,
                    static_cast<unsigned>(coop::players::Registry::Get().LocalPeerId()));
        }
    }
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
    // The held-state diagnostic at the branch point: the pose branch, the release edge, or idle;
    // about 4 a second while something is or was held.
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
            // A swinger let go and still coasting (or held) when another item is taken: its release goes
            // out now, with its own hold's generation, key and eid, before this edge replaces them.
            if (g_heldSwinger && g_lastHeldProp.Raw()) SendHeldRelease(session);
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
            // The adoption expresses the clump where it is: an unreadable one is not adopted and its
            // certificate stays (the local grab cannot be refused), so this carry goes unmirrored.
            const bool hostClump =
                session.role() == coop::net::Role::Host && ue_wrap::prop::IsGarbageClump(heldActor);
            ue_wrap::FVector cloc{};
            ue_wrap::FRotator crot{};
            if (hostClump && (!ue_wrap::engine::TryGetActorLocation(heldActor, cloc) ||
                              !ue_wrap::engine::TryGetActorRotation(heldActor, crot))) {
                UE_LOGW("local_streams: host clump %p not adopted -- its location or rotation could not be read",
                        heldActor);
            } else if (hostClump) {
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
                        coop::trash_channel::OnHostRegrab(session, bornE, heldActor);
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
            // A new hold, and a new generation for it -- except the game's churn re-grab of the
            // entity this peer is carrying, which is the same carry in a new clump actor: no
            // release closed the last one, and a receiver must not replay the grab.
            if (!churnRegrab && ++g_holdGen == 0) g_holdGen = 1;
            const unsigned eidLog =
                (g_lastHeldEid == coop::element::kInvalidId) ? 0u : static_cast<unsigned>(g_lastHeldEid);
            UE_LOGI("net: NEW held actor %p cls='%ls' key='%ls' eid=%u -> %s",
                    heldActor, R::ClassNameOf(heldActor).c_str(),
                    ue_wrap::prop::GetInteractableKeyString(heldActor).c_str(),
                    eidLog, mirrored ? "BROADCAST" : "carry-only(trash/clump)");
            g_heldSwinger = ue_wrap::swinger::EnsureResolved() && ue_wrap::swinger::IsSwinger(heldActor);
            g_swingerKeySaid = false;
            // The carry-only invariant: every refusal in EnsureHeldItemBroadcast is an anti-dupe
            // gate that assumes the receiver resolves the item by another route (a kerfur prop by
            // its mirror eid, a clump by its pile's eid, a claimed save-loaded local by its shared
            // key). With no eid that precondition is false and the stream is undeliverable: the
            // receiver tries key then eid and gets a zero eid plus a key it may never have seen (in
            // the field, 91 poses in 1.5 s no peer could resolve, and an item invisible to everyone
            // but its holder). Reported once per held edge with the actor named, and not
            // suppressed: a key-only pose is legitimate when both peers share a save-loaded prop.
            // A dragged swinger is the exception: its identity is the container lane's key, which the receiver finds
            // in that lane's index (EnsureHeldItemBroadcast leaves it alone by design).
            if (!mirrored && g_lastHeldEid == coop::element::kInvalidId &&
                !(g_heldSwinger && !coop::interactable_sync::ContainerKey(heldActor).empty())) {
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
        // A dragged swinger goes under the container lane's key: the portable `mv_` token where the lid has an
        // identity, since the game's own Key differs between peers for most lids. A lid the lane has not indexed has
        // no key (it streams nothing), and one it filed under the game Key has only the key that may differ: each said
        // once per hold.
        const std::wstring keyW = g_heldSwinger ? coop::interactable_sync::ContainerKey(heldActor)
                                                : ue_wrap::prop::GetInteractableKeyString(heldActor);
        if (g_heldSwinger && !g_swingerKeySaid && (keyW.empty() || keyW.rfind(L"mv_", 0) != 0)) {
            g_swingerKeySaid = true;
            UE_LOGW("local_streams: dragged swinger %p %s", heldActor,
                    keyW.empty() ? "is not indexed by the container lane: it streams nothing"
                                 : "has no portable key: it streams under its game Key, which may differ on the other peers");
        }
        coop::net::WireKey key{};
        for (size_t i = 0; i < keyW.size() && i < 31; ++i) {
            // Save keys are ASCII; the narrowing is lossless.
            key.data[key.len++] = static_cast<char>(keyW[i]);
        }
        ue_wrap::FVector loc{};
        ue_wrap::FRotator rot{};
        const bool poseRead = ue_wrap::engine::TryGetActorLocation(heldActor, loc) &&
                              ue_wrap::engine::TryGetActorRotation(heldActor, rot);
        const coop::net::PropPoseSnapshot pp = HeldPose(key, g_lastHeldEid, loc, rot);
        // Only a pose with a cross-peer identity (a key or an eid) is streamed: a clump grabbed
        // before quiescence that the broadcast declined has neither, and streaming it floods the
        // peer with unresolved-pose warnings for an actor it will never have. The stream resumes
        // the instant the item is expressed; g_lastHeldProp is still tracked so the release edge
        // works, and a PropRelease for a never-expressed prop is harmless.
        if (!poseRead) {
            // An unreadable held prop streams no pose this tick; its identity is kept below.
        } else if (pp.key.len > 0 || pp.elementId != 0) {
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
                UE_LOGI("%s", g_heldSwinger ? "[POSE-SKIP] a dragged swinger has no container-lane key: nothing is streamed"
                            : "[POSE-SKIP] eid=0 key.len=0 -- held clump has NO identity to stream (g_lastHeldEid invalid -> carry frozen between E)");
        }
        // heldActor is live here, so Set's contract holds; the ref captures the index and serial.
        g_lastHeldProp.Set(heldActor);
        g_lastHeldKey = pp.key;
        if (g_heldSwinger) FeedSwingerParent(heldActor);
        g_coasting = false;  // a re-grab of the coasting door ends the coast; the next one starts afresh
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
        if (g_lastHeldEid != g_relEdgeLoggedEid || (relSkip ? 1 : 0) != g_relEdgeLoggedSkip) {
            g_relEdgeLoggedEid  = g_lastHeldEid;
            g_relEdgeLoggedSkip = relSkip ? 1 : 0;
            UE_LOGI("[REL-EDGE] eid=%u carrying=%d pendingSettle=%d -> %s",
                    (g_lastHeldEid == coop::element::kInvalidId) ? 0u : static_cast<unsigned>(g_lastHeldEid),
                    carrying_ ? 1 : 0, pending_ ? 1 : 0, relSkip ? "SKIP(carrying)" : "FIRE(release)");
        }
        if (relSkip) {
            // Carry and flight are one stream: heldActor going null is either a one-frame flicker
            // mid-carry or a real release with the clump now flying, and in both the clump is
            // alive, so its pose keeps streaming under the same eid with no release verb and no
            // velocity. A churn re-pile destroys the clump (not alive), the gap: await the re-grab
            // or the land. The stream ends when the clump re-piles, wherever, and the ToPile
            // convert materialises the pile at the landed spot.
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
                pp.holdGen   = g_holdGen;  // one continuous stream: the carry's own hold
                ue_wrap::FVector loc{};
                ue_wrap::FRotator rot{};
                if (ue_wrap::engine::TryGetActorLocation(g_lastHeldProp.Raw(), loc) &&
                    ue_wrap::engine::TryGetActorRotation(g_lastHeldProp.Raw(), rot)) {   // unread: no pose this tick
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
                }
                // The held cache is kept: the stream continues, and the land path ends the carry.
            } else if (g_lastHeldEid != g_gapLoggedEid) {
                // Entering the gap for this eid. The branch itself re-enters per tick until the
                // carry latch closes, so the line reports the transition, not the state.
                g_gapLoggedEid = g_lastHeldEid;
                UE_LOGI("[PILE] HOST carry SUPPRESS release eid=%u -- !carrying gate; clump re-piled/gone "
                        "(!IsLive or not-a-clump) -> the gap; await the re-grab or the land-settle close",
                        static_cast<unsigned>(g_lastHeldEid));
            }
        } else if (!CoastSwinger(session)) {   // a swinger still falling keeps streaming; the release waits
            SendHeldRelease(session);
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
