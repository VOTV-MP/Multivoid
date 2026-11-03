// coop/player/remote_player_ragdoll.cpp -- see coop/player/remote_player_ragdoll.h.
//
// [PATCH b208] visibility is now a transaction with one idempotent restore:
//   * the hide happens only AFTER the plushie body spawned and the pelvis attach succeeded;
//   * every teardown path (Stop, TeardownForDestroy, the body-reaped self-heal) goes through
//     RestoreVisibility(), which is a no-op when nothing was hidden;
//   * TickRestoreGuard() is the safety net for the paths that bypass Stop entirely (a revive
//     racing the flop, an entry rebuilt under a new RagdollDisplay, an element teardown) -- it
//     restores a puppet left hidden and clears a stale flop.

#include "coop/player/remote_player_ragdoll.h"

#include "coop/player/skin_effects.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/actors/puppet.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/types.h"

namespace coop {

namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;

namespace {

// Show/hide the puppet's TWO kel body meshes -- the native mesh slot and its
// mesh_playerVisible child, both of which render. VISIBILITY only (SetVisibility,
// propagate=false): the mesh ASSETS are untouched, so a pak-loaded custom-skin asset keeps its
// component reference and cannot be GC'd during the flop, which clearing the mesh would risk by
// dropping the only strong ref. Forcing visible=true on the restore side is a safe no-op when
// nothing was hidden -- visible IS the puppet's normal state, forced at spawn -- so the restore
// needs no bookkeeping.
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
    // Velocity half of the pose stream only: slave the plushie's pelvis rigid body to the sender so
    // the visible flop TRACKS the real ragdoll. The streamed pelvis ROTATION is unused since the
    // visible-plushy rework -- it existed to tumble the kel-attached actor -- and stays on the wire
    // untouched, for protocol stability.
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
    // Self-heal: the body was GC-killed mid-ragdoll, a level transition having reaped it. Recover
    // NOW -- un-hide, detach, clear -- so the puppet is not stuck invisible on a dead attachment;
    // the owner resumes pose-drive this same tick.
    Stop(puppetActor ? puppetActor : hiddenActor_, puppetActor ? puppetIdx : hiddenIdx_);
    return Drive::StoppedNow;
}

void RagdollDisplay::TeardownForDestroy() {
    // The body is a SEPARATE actor that would otherwise outlive the puppet as
    // an orphan. IsLiveByIndex-guarded so a GC-recycled address isn't mistaken
    // for our body.
    //
    // [PATCH b208] ALSO restore the kel meshes here. The old code relied on "the puppet actor is
    // destroyed right after" -- true for a real destroy, but this teardown also runs on paths
    // where the puppet body survives (an entry rebuilt for the same peer, a rejoin that keeps the
    // actor). Restoring is a safe no-op when the puppet still dies, and it is the difference
    // between a visible player and a permanently invisible one on every other path.
    RestoreVisibility("destroy");
    if (body_ && R::IsLiveByIndex(body_, bodyIdx_)) E::DestroyActor(body_);
    body_ = nullptr;
    bodyIdx_ = -1;
    wireState_ = false;  // a recycled puppet starts un-ragdolled + re-converges
    active_ = false;
}

void RagdollDisplay::Start(void* puppetActor, int32_t puppetIdx) {
    if (!puppetActor || !R::IsLiveByIndex(puppetActor, puppetIdx)) return;

    // A flop that is already up must not hide twice; re-running Start on the same actor keeps
    // one owner of the hidden state.
    if (active_ && hidden_) return;

    // Spawn VOTV's own playerRagdoll_C VISIBLE, co-located with the puppet: the game's plushie
    // ragdoll, the full six-bone chain physics and the exact body single-player shows in mirrors.
    // If it fails, leave the puppet UPRIGHT and pose-driving (active_ stays false) -- graceful. The
    // body's Player field points at this puppet, and the spawn is DEATH-FREE: no ragdollMode.
    ue_wrap::FVector loc{};
    ue_wrap::FRotator rot{};
    if (!E::TryGetActorLocation(puppetActor, loc) || !E::TryGetActorRotation(puppetActor, rot)) {
        UE_LOGW("RagdollDisplay::Start: the puppet's location or rotation could not be read -- puppet stays upright "
                "(graceful)");
        return;
    }
    void* body = E::SpawnPlayerRagdollBody(puppetActor, loc, rot);
    if (!body) {
        UE_LOGW("RagdollDisplay::Start: playerRagdoll_C spawn failed -- puppet stays upright (graceful)");
        return;
    }
    body_ = body;
    bodyIdx_ = R::InternalIndexOf(body);

    // PELVIS-attach the puppet actor to the body: the position anchor (nameplate +
    // recover hand-off follow the flop). If the attach fails, destroy the body + stay upright --
    // and the kel meshes were never hidden, so nothing has to be undone.
    if (!E::AttachActorToRagdollBody(puppetActor, body)) {
        UE_LOGW("RagdollDisplay::Start: pelvis-attach failed -- destroying body, puppet stays upright");
        E::DestroyActor(body);
        body_ = nullptr; bodyIdx_ = -1;
        return;
    }

    // [PATCH b208] Record what we are about to hide BEFORE hiding it, so every restore path knows
    // which actor to put back even when the caller passes nothing useful.
    hiddenActor_ = puppetActor;
    hiddenIdx_ = puppetIdx;
    hidden_ = true;

    // Hide the standing kel for the flop -- otherwise it double-images over the
    // plushie (the June "plushy on a stick" rejection, inverted: now the plushie is
    // the visible one). The skin effect rig (belly light / joint sparks) hides
    // with it -- floating sparks over an invisible body break the plushie trick.
    SetPuppetKelMeshesVisible(puppetActor, false);
    coop::skin_effects::SetRigVisible(puppetActor, false);
    active_ = true;
    UE_LOGI("RagdollDisplay::Start: VISIBLE plushie body=%p spawned, puppet pelvis-attached + kel meshes hidden", body);
}

void RagdollDisplay::Stop(void* puppetActor, int32_t puppetIdx) {
    void* actor = puppetActor;
    int32_t idx = puppetIdx;
    if (!actor || !R::IsLiveByIndex(actor, idx)) {
        // [PATCH b208] The caller may have no usable actor (entry rebuilt, or a guard calling in).
        // Fall back to the actor the hide recorded, which is what the flop is attached to.
        actor = hiddenActor_;
        idx = hiddenIdx_;
    }
    if (actor && R::IsLiveByIndex(actor, idx)) {
        // Un-hide FIRST (their normal state -- a no-op if Start never hid them), then detach
        // (KeepWorld -- the puppet stays where the flop left it; the next pose drives it back to
        // the streamed pose) BEFORE destroying the body it's attached to.
        RestoreVisibility("stop");
        E::DetachActorFromRagdollBody(actor);
    } else {
        // The actor is gone: clear our bookkeeping so a recycled index cannot hide a new body.
        RestoreVisibility("stop");
    }
    // Destroy the physics body (IsLiveByIndex-guarded against a recycled addr).
    if (body_ && R::IsLiveByIndex(body_, bodyIdx_)) E::DestroyActor(body_);
    body_ = nullptr;
    bodyIdx_ = -1;
    active_ = false;
    UE_LOGI("RagdollDisplay::Stop: kel meshes restored, puppet detached, plushie body destroyed");
}

void RagdollDisplay::RestoreVisibility(const char* reason) {
    if (!hidden_) return;  // idempotent: nothing was hidden, nothing to undo
    if (hiddenActor_ && R::IsLiveByIndex(hiddenActor_, hiddenIdx_)) {
        SetPuppetKelMeshesVisible(hiddenActor_, true);
        coop::skin_effects::SetRigVisible(hiddenActor_, true);
        UE_LOGI("ragdoll_visibility: RESTORED idx=%d reason=%s", hiddenIdx_, reason ? reason : "?");
    } else {
        UE_LOGW("ragdoll_visibility: RESTORED (actor gone) idx=%d reason=%s -- nothing left to show",
                hiddenIdx_, reason ? reason : "?");
    }
    hidden_ = false;
    hiddenActor_ = nullptr;
    hiddenIdx_ = -1;
}

bool RagdollDisplay::Hidden() const { return hidden_; }

bool RagdollDisplay::TickRestoreGuard() {
    if (!hidden_) return false;

    // The flop's body is the display: with the body gone there is nothing to show, so a puppet
    // that is still hidden must be restored no matter how we got here (revive raced the flop, the
    // element ended, the entry was rebuilt under a new RagdollDisplay, a world load reaped the
    // body without our Stop).
    const bool bodyAlive = body_ && R::IsLiveByIndex(body_, bodyIdx_);
    if (!bodyAlive) {
        RestoreVisibility("watchdog-body-gone");
        active_ = false;
        body_ = nullptr;
        bodyIdx_ = -1;
        wireState_ = false;  // re-converge from the wire on the next sample
        UE_LOGW("ragdoll_visibility: WATCHDOG restored a puppet left hidden (plushie body gone)");
        return true;
    }

    // The sender already says "upright" (the bit went false) but our Stop never ran -- the same
    // race, one step earlier. Stop is idempotent and does the full teardown.
    if (!wireState_) {
        Stop(hiddenActor_, hiddenIdx_);
        UE_LOGW("ragdoll_visibility: WATCHDOG stopped a stale flop (wire says upright)");
        return true;
    }
    return false;
}

}  // namespace coop
