// coop/interactables/atv_spawn_ask.cpp -- see coop/interactables/atv_spawn_ask.h.

#include "coop/interactables/atv_spawn_ask.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_pawn.h"

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <vector>

namespace coop::atv_spawn_ask {
namespace {

namespace N = coop::net;
namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;

constexpr float kReachCm = 2000.f;
constexpr uint64_t kAskGapMs = 3000;

N::Session* g_session = nullptr;
std::vector<ue_wrap::CachedObjRef> g_remove;   // CLIENT: the asked ATVs, removed on the next tick
uint64_t g_lastAsk[N::kMaxPeers] = {};         // HOST: each slot's last granted ask
uint32_t g_asked = 0, g_granted = 0, g_refused = 0;

uint64_t Now() { return ::GetTickCount64(); }

bool Finite(const N::AtvSpawnAskPayload& p) {
    return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z) && std::isfinite(p.yaw);
}

}  // namespace

void Install(N::Session* session) { g_session = session; }

void OnPlayerSpawnedAtv(void* atv) {
    if (!g_session || g_session->role() != N::Role::Client || !atv || !R::IsLive(atv)) return;
    ue_wrap::FVector at{};
    ue_wrap::FRotator rot{};
    if (!E::TryGetActorLocation(atv, at)) return;
    E::TryGetActorRotation(atv, rot);
    N::AtvSpawnAskPayload p{at.X, at.Y, at.Z, rot.Yaw};
    const bool sent = g_session->SendReliable(N::ReliableKind::AtvSpawnAsk, &p, sizeof(p));
    ue_wrap::CachedObjRef ref;
    ref.Set(atv);
    g_remove.push_back(ref);
    ++g_asked;
    UE_LOGI("atv_spawn_ask: this player's spawn menu made an ATV at (%.0f, %.0f, %.0f) -- asked of the host (sent=%d), "
            "this copy removed", at.X, at.Y, at.Z, sent ? 1 : 0);
}

void Tick() {
    if (g_remove.empty()) return;
    for (ue_wrap::CachedObjRef& r : g_remove)
        if (r.Alive()) E::DestroyActor(r.Raw());
    g_remove.clear();
}

void OnAsk(N::Session& session, const N::AtvSpawnAskPayload& p, int slot) {
    const char* why = nullptr;
    coop::RemotePlayer* const rp = coop::players::Registry::Get().Puppet(static_cast<uint8_t>(slot));
    void* const body = rp && rp->valid() ? rp->actor() : nullptr;
    ue_wrap::FVector at{};
    const uint64_t now = Now();
    if (!session.IsSlotWorldReady(slot)) why = "its world is not ready";
    else if (!Finite(p)) why = "the place is not finite";
    else if (!body || !E::TryGetActorLocation(body, at)) why = "no body for it here";
    else if (std::hypot(p.x - at.X, p.y - at.Y, p.z - at.Z) > kReachCm) why = "the place is out of its reach";
    else if (g_lastAsk[slot] && now - g_lastAsk[slot] < kAskGapMs) why = "it asked under 3 s ago";
    void* atv = nullptr;
    if (!why) {
        void* const cls = ue_wrap::object_index::ClassByName(L"ATV_C");
        const ue_wrap::FVector loc{p.x, p.y, p.z};
        const ue_wrap::FRotator rot{0.f, p.yaw, 0.f};
        atv = cls ? E::BeginDeferredSpawn(cls, loc, rot) : nullptr;
        if (atv && !E::FinishDeferredSpawn(atv, loc, rot)) atv = nullptr;
        if (!atv) why = "the spawn failed";
    }
    if (why) {
        ++g_refused;
        UE_LOGW("atv_spawn_ask: slot %d's spawn-menu ATV refused: %s", slot, why);
        return;
    }
    g_lastAsk[slot] = now;
    ++g_granted;
    UE_LOGI("atv_spawn_ask: slot %d's spawn-menu ATV made here at (%.0f, %.0f, %.0f) -- the ATV lane announces it",
            slot, p.x, p.y, p.z);
}

void OnDisconnect() {
    g_remove.clear();
    for (uint64_t& t : g_lastAsk) t = 0;
    if (g_asked || g_granted || g_refused)
        UE_LOGI("atv_spawn_ask: session over -- %u asked here, %u made, %u refused", g_asked, g_granted, g_refused);
    g_asked = g_granted = g_refused = 0;
}

}  // namespace coop::atv_spawn_ask
