// coop/player/remote_player_ragdoll.cpp -- see coop/player/remote_player_ragdoll.h.

#include "coop/player/remote_player_ragdoll.h"

#include "ue_wrap/engine.h"
#include "ue_wrap/log.h"
#include "ue_wrap/puppet.h"
#include "ue_wrap/reflection.h"
#include "ue_wrap/types.h"

namespace coop {

namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;

namespace {

// Show/hide the puppet's TWO kel body meshes (native slot @0x0280 + its child
// mesh_playerVisible @0x04F8 -- both render, [[lesson-attachparent-visibility-two-body]]).
// VISIBILITY only (SetVisibility, propagate=false): the mesh ASSETS are untouched, so a
// pak-loaded custom-skin asset keeps its component reference and cannot be GC'd during
// the flop (mesh-clearing would drop the only strong ref). Forcing visible=true on the
// restore side is a safe no-op when nothing was hidden -- visible IS the puppet's normal
// state (spawn forces it), so the restore needs no bookkeeping.
void SetPuppetKelMeshesVisible(void* puppetActor, bool visible) {
    E::SetSceneComponentVisibility(
        ue_wrap::puppet::GetNativeBodyMeshComponent(puppetActor), visible, /*propagate=*/false);
    E::SetSceneComponentVisibility(
        ue_wrap::puppet::GetMeshPlayerVisibleComponent(puppetActor), visible, /*propagate=*/false);
}

}  // namespace

bool RagdollDisplay::OnWireBit(bool ragdollBit, void* puppetActor, int32_t puppetIdx) {
    if (ragdollBit == wireState_) return false;
    wireState_ = ragdollBit;
    if (ragdollBit) {
        Start(puppetActor, puppetIdx);
        return false;
    }
    Stop(puppetActor, puppetIdx);
    return true;
}

bool RagdollDisplay::SetPose(const coop::net::RagdollPoseSnapshot& snap) {
    // Velocity half of the v22 stream only: slave the plushie's pelvis rigid body to
    // the sender so the visible flop TRACKS the real ragdoll. The streamed pelvis
    // ROTATION is unused since the visible-plushy rework (it existed to tumble the
    // kel-attached actor); it stays on the wire untouched -- protocol stability.
    if (active_ && body_ && R::IsLiveByIndex(body_, bodyIdx_)) {
        E::DriveRagdollBodyPelvisVelocity(
            body_,
            ue_wrap::FVector{snap.linVelX, snap.linVelY, snap.linVelZ},
            ue_wrap::FVector{snap.angVelX, snap.angVelY, snap.angVelZ});
        return true;
    }
    return false;
}

RagdollDisplay::Drive RagdollDisplay::DriveAttached(void* puppetActor, int32_t puppetIdx) {
    if (!active_) return Drive::Inactive;
    if (body_ && R::IsLiveByIndex(body_, bodyIdx_)) {
        // The VISIBLE plushie body is the whole display; the (hidden-kel) puppet actor
        // just rides the pelvis attach. Nothing to drive per frame.
        return Drive::Attached;
    }
    // Self-heal (audit 2026-06-01): the body was GC-killed mid-ragdoll (e.g. a
    // level transition reaped it) -- recover NOW (un-hide + detach + clear) so the
    // puppet isn't stuck invisible on a dead attachment; the owner resumes pose-drive
    // this same tick. See [[project-ragdoll-sync]].
    Stop(puppetActor, puppetIdx);
    return Drive::StoppedNow;
}

void RagdollDisplay::TeardownForDestroy() {
    // The body is a SEPARATE actor that would otherwise outlive the puppet as
    // an orphan. IsLiveByIndex-guarded so a GC-recycled address isn't mistaken
    // for our body. No detach + no kel un-hide: the puppet actor (and its
    // hidden meshes) is destroyed right after.
    if (body_ && R::IsLiveByIndex(body_, bodyIdx_)) E::DestroyActor(body_);
    body_ = nullptr;
    bodyIdx_ = -1;
    wireState_ = false;  // a recycled puppet starts un-ragdolled + re-converges
    active_ = false;
}

void RagdollDisplay::Start(void* puppetActor, int32_t puppetIdx) {
    if (!puppetActor || !R::IsLiveByIndex(puppetActor, puppetIdx)) return;

    // Spawn VOTV's own playerRagdoll_C VISIBLE, co-located with the puppet -- the
    // game's plushie ragdoll (full 6-bone chain physics, the exact body SP shows in
    // mirrors). If it fails, leave the puppet UPRIGHT + pose-driving (active_ stays
    // false) -- graceful. The body's Player @0x248 = this puppet; the spawn is
    // DEATH-FREE (no ragdollMode).
    const ue_wrap::FVector loc = E::GetActorLocation(puppetActor);
    const ue_wrap::FRotator rot = E::GetActorRotation(puppetActor);
    void* body = E::SpawnPlayerRagdollBody(puppetActor, loc, rot);
    if (!body) {
        UE_LOGW("RagdollDisplay::Start: playerRagdoll_C spawn failed -- puppet stays upright (graceful)");
        return;
    }
    body_ = body;
    bodyIdx_ = R::InternalIndexOf(body);

    // PELVIS-attach the puppet actor to the body: the position anchor (nameplate +
    // recover hand-off follow the flop). The puppet itself goes INVISIBLE next -- the
    // plushie is the display. If the attach fails, destroy the body + stay upright.
    if (!E::AttachActorToRagdollBody(puppetActor, body)) {
        UE_LOGW("RagdollDisplay::Start: pelvis-attach failed -- destroying body, puppet stays upright");
        E::DestroyActor(body);
        body_ = nullptr; bodyIdx_ = -1;
        return;
    }
    // Hide the standing kel for the flop -- otherwise it double-images over the
    // plushie (the June "plushy on a stick" rejection, inverted: now the plushie is
    // the visible one).
    SetPuppetKelMeshesVisible(puppetActor, false);
    active_ = true;
    UE_LOGI("RagdollDisplay::Start: VISIBLE plushie body=%p spawned, puppet pelvis-attached + kel meshes hidden", body);
}

void RagdollDisplay::Stop(void* puppetActor, int32_t puppetIdx) {
    if (puppetActor && R::IsLiveByIndex(puppetActor, puppetIdx)) {
        // Un-hide the kel meshes FIRST (their normal state -- safe no-op if Start
        // never hid them), then detach (KeepWorld -- the puppet stays where the flop
        // left it; the next pose drives it back to the streamed pose) BEFORE
        // destroying the body it's attached to.
        SetPuppetKelMeshesVisible(puppetActor, true);
        E::DetachActorFromRagdollBody(puppetActor);
    }
    // Destroy the physics body (IsLiveByIndex-guarded against a recycled addr).
    if (body_ && R::IsLiveByIndex(body_, bodyIdx_)) E::DestroyActor(body_);
    body_ = nullptr;
    bodyIdx_ = -1;
    active_ = false;
    UE_LOGI("RagdollDisplay::Stop: kel meshes restored, puppet detached, plushie body destroyed");
}

}  // namespace coop
