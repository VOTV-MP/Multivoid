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

    actor_ = E::SpawnActor(cls, loc);
    if (!actor_) {
        UE_LOGE("RemotePlayer::Spawn: SpawnActor failed");
        return false;
    }
    UE_LOGI("RemotePlayer::Spawn: 2nd mainPlayer_C spawned actor=%p at (%.0f,%.0f,%.0f)",
            actor_, loc.X, loc.Y, loc.Z);
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

}  // namespace coop
