// coop/player/remote_player_ragdoll.cpp -- see coop/player/remote_player_ragdoll.h.
//
// [PATCH b208] NO HEADER CHANGE -- the hidden-state bookkeeping lives in a file-local map keyed by
// the RagdollDisplay instance, so this file compiles against the stock header unchanged.
//
// Why: RagdollDisplay::Start hid the puppet's kel meshes, and the only restore lived in Stop().
// TeardownForDestroy() deliberately skipped the restore ("the puppet actor is destroyed right
// after"), and any path that bypassed Stop() left the puppet invisible for good -- the reported
// "someone dies and another player's model disappears" (probabilistic, because it needs one of
// those paths to win the race).
//
// Invariant used below: hidden => active_. Start() sets both; Stop() and TeardownForDestroy()
// clear both. So no "hidden but nothing will ever drive it" state is left behind.

#include "coop/player/remote_player_ragdoll.h"

#include "coop/player/skin_effects.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/actors/puppet.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/types.h"

#include <unordered_map>

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

// [PATCH b208] Which puppet each display has hidden. Game-thread only (a RagdollDisplay is driven
// from the same update that already calls into this file), so no locking -- same as the rest of it.
struct HiddenPuppet {
    void*   actor = nullptr;
    int32_t idx   = -1;
};
std::unordered_map<const void*, HiddenPuppet> g_hidden;

void RememberHidden(const void* self, void* actor, int32_t idx) {
    HiddenPuppet h;
    h.actor = actor;
    h.idx = idx;
    g_hidden[self] = h;
}

bool IsHidden(const void* self) { return g_hidden.find(self) != g_hidden.end(); }

// The one restore. Idempotent: no entry -> nothing to undo. Both meshes plus the skin-effect rig
// (belly light / joint sparks) go back, because Start hides all three and floating sparks over an
// invisible body break the plushie trick the flop is doing.
void RestoreHidden(const void* self, const char* reason) {
    auto it = g_hidden.find(self);
    if (it == g_hidden.end()) return;
    void* actor = it->second.actor;
    const int32_t idx = it->second.idx;
    g_hidden.erase(it);

    if (actor && R::IsLiveByIndex(actor, idx)) {
        SetPuppetKelMeshesVisible(actor, true);
        coop::skin_effects::SetRigVisible(actor, true);
        UE_LOGI("ragdoll_visibility: RESTORED idx=%d reason=%s", idx, reason ? reason : "?");
    } else {
        // The actor is gone (its own teardown won the race): nothing to show, but say so -- a
        // silent skip is how the permanent-invisible case stayed invisible.
        UE_LOGW("ragdoll_visibility: RESTORED (actor gone) idx=%d reason=%s -- nothing left to show",
                idx, reason ? reason : "?");
    }
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
    Stop(puppetActor, puppetIdx);
    return Drive::StoppedNow;
}

void RagdollDisplay::TeardownForDestroy() {
    // The body is a SEPARATE actor that would otherwise outlive the puppet as
    // an orphan. IsLiveByIndex-guarded so a GC-recycled address isn't mistaken
    // for our body.
    //
    // [PATCH b208] Restore the kel meshes here too. The old comment assumed "the puppet actor is
    // destroyed right after" -- true for a real destroy, but this teardown also runs where the body
    // survives (an entry rebuilt for the same peer, a rejoin that keeps the actor). Restoring is a
    // safe no-op when the puppet still dies, and it is the difference between a visible player and
    // a permanently invisible one everywhere else.
    RestoreHidden(this, "destroy");

    if (body_ && R::IsLiveByIndex(body_, bodyIdx_)) E::DestroyActor(body_);
    body_ = nullptr;
    bodyIdx_ = -1;
    wireState_ = false;  // a recycled puppet starts un-ragdolled + re-converges
    active_ = false;
}

void RagdollDisplay::Start(void* puppetActor, int32_t puppetIdx) {
    if (!puppetActor || !R::IsLiveByIndex(puppetActor, puppetIdx)) return;

    // A flop that is already up must not hide twice; one owner of the hidden state.
    if (active_ || IsHidden(this)) return;

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

    // [PATCH b208] Record what we are about to hide BEFORE hiding it: every restore path (Stop,
    // TeardownForDestroy, the body-reaped self-heal) then knows which actor to put back, even when
    // the caller passes nothing useful.
    RememberHidden(this, puppetActor, puppetIdx);

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
    // Prefer the actor the hide recorded: the caller's copy can be empty (a guard calling in, or an
    // entry rebuilt under a new actor) while the flop is still attached to the recorded one.
    void* actor = puppetActor;
    int32_t idx = puppetIdx;
    auto it = g_hidden.find(this);
    if (it != g_hidden.end()) {
        actor = it->second.actor;
        idx = it->second.idx;
    }

    // Un-hide FIRST (their normal state -- a no-op if Start never hid them), then detach
    // (KeepWorld -- the puppet stays where the flop left it; the next pose drives it back to
    // the streamed pose) BEFORE destroying the body it's attached to.
    RestoreHidden(this, "stop");
    if (actor && R::IsLiveByIndex(actor, idx)) E::DetachActorFromRagdollBody(actor);

    // Destroy the physics body (IsLiveByIndex-guarded against a recycled addr).
    if (body_ && R::IsLiveByIndex(body_, bodyIdx_)) E::DestroyActor(body_);
    body_ = nullptr;
    bodyIdx_ = -1;
    active_ = false;
    UE_LOGI("RagdollDisplay::Stop: kel meshes restored, puppet detached, plushie body destroyed");
}

}  // namespace coop
