#include "coop/remote_player.h"

#include "ue_wrap/engine.h"
#include "ue_wrap/log.h"
#include "ue_wrap/reflection.h"
#include "ue_wrap/sdk_profile.h"

namespace coop {

namespace P = ue_wrap::profile;
namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;

bool RemotePlayer::Spawn() {
    void* cls = R::FindClass(P::name::MainPlayerClass);
    if (!cls) {
        UE_LOGW("RemotePlayer::Spawn: %ls class not loaded (not in gameplay yet)",
                P::name::MainPlayerClass);
        return false;
    }

    // Spawn offset from the local player so it lands in the world, not at origin.
    ue_wrap::FVector loc{};
    void* local = R::FindObjectByClass(P::name::MainPlayerClass);
    if (local) {
        loc = E::GetActorLocation(local);
        loc.X += 200.f;  // 2m to the side of the local player
    } else {
        UE_LOGW("RemotePlayer::Spawn: no local mainPlayer_C; spawning at origin");
    }

    actor_ = E::SpawnActor(cls, loc, /*inertPawn=*/true);  // no auto PLAYER controller (no hijack)
    if (!actor_) {
        UE_LOGE("RemotePlayer::Spawn: SpawnActor failed");
        return false;
    }
    UE_LOGI("RemotePlayer::Spawn: 2nd mainPlayer_C spawned actor=%p at (%.0f,%.0f,%.0f)",
            actor_, loc.X, loc.Y, loc.Z);

    // Give it an AIController so the body's AnimBP has a possessing controller and
    // poses correctly (an unpossessed body collapses to a "stick"). An AIController
    // has NO viewport/input/camera, so it does not hijack the local player (unlike
    // the PlayerController auto-possess we prevented above).
    E::SpawnDefaultController(actor_);
    if (void* ctrl = E::GetController(actor_)) {
        UE_LOGI("RemotePlayer::Spawn: controller = %ls (want AIController)",
                R::ClassNameOf(ctrl).c_str());
    } else {
        UE_LOGW("RemotePlayer::Spawn: SpawnDefaultController gave no controller "
                "(AIControllerClass null?) -- body may not pose");
    }
    return true;
}

bool RemotePlayer::valid() const { return actor_ != nullptr; }

bool RemotePlayer::SetLocation(const ue_wrap::FVector& location) {
    if (!actor_) return false;
    return E::SetActorLocation(actor_, location);
}

ue_wrap::FVector RemotePlayer::GetLocation() const {
    if (!actor_) return {};
    return E::GetActorLocation(actor_);
}

int RemotePlayer::ShowBody() {
    if (!actor_) return 0;
    int shown = 0;
    for (const auto& comp : R::ChildObjectsOf(actor_)) {
        if (comp.className != L"SkeletalMeshComponent") continue;
        if (E::SetComponentVisible(comp.object, true)) {
            // Force the pose to always tick (belt-and-suspenders so the body keeps
            // posing even when not the rendered viewpoint). The actual posing is
            // driven by the AIController possession path (see Spawn).
            E::SetAnimTickAlways(comp.object);
            UE_LOGI("RemotePlayer::ShowBody: shown %ls", comp.name.c_str());
            ++shown;
        }
    }
    return shown;
}

int RemotePlayer::NeuterLocalSystems() {
    if (!actor_) return 0;
    int stripped = 0;
    // NOTE: we deliberately KEEP the orphan's AIController (Spawn gives it one so
    // the body poses). The hijack we prevented was the PLAYER controller
    // (inertPawn zeroes AutoPossessPlayer); an AIController is harmless.
    // NOTE: we deliberately do NOT disable the actor tick. A blunt tick-freeze
    // collapses the skeletal mesh (the "stick + frozen head" look) because the
    // AnimInstance stops posing. The gamma/view stomp it was masking is now fixed
    // at the root (inertPawn: no controller, never a view target), so the tick can
    // run and animate the body normally. Re-investigate per-site only if a
    // specific tick behaviour still leaks onto the local screen.
    for (const auto& comp : R::ChildObjectsOf(actor_)) {
        // A remote pawn's unbound post-process stomps the LOCAL screen's
        // gamma/exposure -- destroy it (the remote pawn renders nothing for the
        // local viewport; only its body matters).
        if (comp.className != L"PostProcessComponent") continue;
        if (E::DestroyComponent(comp.object, actor_)) {
            UE_LOGI("RemotePlayer::NeuterLocalSystems: destroyed %ls (%ls)",
                    comp.name.c_str(), comp.className.c_str());
            ++stripped;
        }
    }
    return stripped;
}

int RemotePlayer::HideGizmos() {
    if (!actor_) return 0;
    int hidden = 0;
    for (const auto& comp : R::ChildObjectsOf(actor_)) {
        // Only the editor-debug visualizer primitive types -- never gameplay
        // meshes (principle 4: targeted). These render as the red arrow / white
        // rod / "ball" on an unpossessed orphan.
        if (comp.className != L"ArrowComponent" && comp.className != L"BillboardComponent")
            continue;
        if (E::SetComponentVisible(comp.object, false)) {
            UE_LOGI("RemotePlayer::HideGizmos: hid %ls (%ls)",
                    comp.name.c_str(), comp.className.c_str());
            ++hidden;
        }
    }
    return hidden;
}

}  // namespace coop
