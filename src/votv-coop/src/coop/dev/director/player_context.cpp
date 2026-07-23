// coop/dev/director/player_context.cpp -- the bot-director read-model (IPlayerContext
// analog). Reads the possessed local player's OWN state each tick. Game thread only.

#include "coop/dev/director/director.h"

#include "coop/player/players_registry.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/engine/engine.h"

#include <cstdint>

namespace coop::director {
namespace {
namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;
namespace P = ue_wrap::profile;
int32_t g_dropPlaceOff = -2;   // -2 uncomputed, -1 not found; cached process-wide
}  // namespace

bool PlayerContext::Refresh() {
    *this = PlayerContext{};   // clear
    void* p = coop::players::Registry::Get().Local();
    if (!p || !R::IsLive(p) || !E::GetController(p)) return false;   // GetController()!=null = possessed
    player = p;
    possessed = true;
    pos = E::GetActorLocation(p);
    E::MainPlayerGrabState gs{};
    if (E::ReadMainPlayerGrabState(p, gs)) { held = gs.grabbingActor; holding = gs.holdingActor; }
    if (g_dropPlaceOff == -2) {
        void* cls = R::FindClass(P::name::MainPlayerClass);
        g_dropPlaceOff = cls ? R::FindPropertyOffset(cls, L"drop_place") : -1;
    }
    if (g_dropPlaceOff >= 0) placeMode = *(reinterpret_cast<uint8_t*>(p) + g_dropPlaceOff) != 0;
    return true;
}

}  // namespace coop::director
