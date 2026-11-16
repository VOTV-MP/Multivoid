// coop/interactables/atv_fleet.cpp -- see coop/interactables/atv_fleet.h.

#include "coop/interactables/atv_fleet.h"

#include "coop/config/config.h"
#include "coop/net/net_clock.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/trace.h"
#include "ue_wrap/devices/atv.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/world_identity.h"

#include <cmath>

namespace coop::atv_fleet {
namespace {

namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;

constexpr uint64_t kCheckMs = 5000;
constexpr float kAsideCm = 450.f;     // from the first ATV's centre to the second's
constexpr float kEyeCm = 120.f;       // the height the line to a spot is traced at
constexpr float kGroundReachCm = 400.f;
constexpr float kLevelCm = 250.f;     // how far above or below the first ATV the ground of a spot may lie
constexpr float kLiftCm = 80.f;
constexpr float kRoomCm = 170.f;      // half the room a parked ATV is given

uint64_t g_nextMs = 0;
bool     g_settled = false;   // this session's world has its two ATVs, or a spawn was tried

int Threshold() {
    static const int n = coop::config::ResolveInt(::coop::config_registry::rows::net_second_atv_players);
    return n;
}

struct Census {
    void* world;
    void* first;
    int   count;
};

// A spot beside `atv` a second one can stand on. False when none of the eight has a clear line and ground.
bool FindSpot(void* atv, const ue_wrap::FVector& at, ue_wrap::FVector& out) {
    for (int i = 0; i < 8; ++i) {
        const float rad = static_cast<float>(i) * 0.785398163f;
        const ue_wrap::FVector spot{at.X + std::cos(rad) * kAsideCm, at.Y + std::sin(rad) * kAsideCm, at.Z};
        if (ue_wrap::trace::LineBlockedStatDyn(atv, {at.X, at.Y, at.Z + kEyeCm}, {spot.X, spot.Y, spot.Z + kEyeCm}) != 0)
            continue;
        ue_wrap::trace::Hit ground;
        if (!ue_wrap::trace::LineHitStatDyn(atv, {spot.X, spot.Y, spot.Z + kEyeCm}, {spot.X, spot.Y, spot.Z - kGroundReachCm},
                                            &ground) ||
            !ground.blocked || std::fabs(ground.point.Z - at.Z) > kLevelCm)
            continue;
        // Room for the whole vehicle: four lines from the spot's middle to its length and width away, clear.
        const ue_wrap::FVector mid{spot.X, spot.Y, ground.point.Z + kEyeCm};
        bool room = true;
        for (int k = 0; k < 4 && room; ++k) {
            const float a = static_cast<float>(k) * 1.570796327f;
            room = ue_wrap::trace::LineBlockedStatDyn(atv, mid, {mid.X + std::cos(a) * kRoomCm, mid.Y + std::sin(a) * kRoomCm, mid.Z}) == 0;
        }
        if (!room) continue;
        out = {spot.X, spot.Y, ground.point.Z + kLiftCm};
        return true;
    }
    return false;
}

}  // namespace

void Tick(coop::net::Session* session) {
    if (!session || g_settled || Threshold() <= 0) return;
    const uint64_t now = coop::net::NowMs();
    if (now < g_nextMs) return;
    g_nextMs = now + kCheckMs;
    int players = 1;  // the host
    for (int slot = 1; slot < coop::players::kMaxPeers; ++slot)
        if (session->IsSlotWorldReady(slot)) ++players;
    if (players < Threshold()) return;
    Census census{ue_wrap::world_identity::CurrentWorld(), nullptr, 0};
    if (!census.world) return;
    const size_t loading = ue_wrap::atv::ForEachAtv([](void* ctx, void* actor, int32_t) {
        Census* c = static_cast<Census*>(ctx);
        // The plain ATV only: the lineage holds other vehicles.
        if (ue_wrap::world_identity::WorldOf(actor) != c->world || R::ClassNameOf(actor) != L"ATV_C") return;
        if (!c->first) c->first = actor;
        ++c->count;
    }, &census);
    if (loading > 0 || census.count == 0) return;  // an ATV still loading, or none to stand beside: asked again
    if (census.count >= 2) {
        g_settled = true;
        return;
    }
    ue_wrap::FVector at{}, spot{};
    void* cls = ue_wrap::object_index::ClassByName(L"ATV_C");
    if (!cls || !E::TryGetActorLocation(census.first, at)) return;
    if (!FindSpot(census.first, at, spot)) {
        UE_LOGI("atv_fleet: %d players and one ATV, and no clear spot beside it at (%.0f,%.0f,%.0f) -- asked again", players,
                at.X, at.Y, at.Z);
        return;
    }
    void* spawned = E::SpawnActor(cls, spot);
    g_settled = true;  // a spawn that fails in a ready world is not retried
    UE_LOGI("atv_fleet: %d players and one ATV -- a second %s at (%.0f,%.0f,%.0f), beside the first", players,
            spawned ? "spawned" : "did NOT spawn", spot.X, spot.Y, spot.Z);
}

void OnDisconnect() {
    g_nextMs = 0;
    g_settled = false;
}

}  // namespace coop::atv_fleet
