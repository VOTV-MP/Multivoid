// coop/dev/boar_hit_drill.cpp -- see coop/dev/boar_hit_drill.h.

#include "coop/dev/boar_hit_drill.h"

#include "coop/config/config.h"
#include "coop/dev/drill_hand.h"
#include "coop/net/net_clock.h"
#include "coop/net/session.h"
#include "coop/player/hit_intent.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/player/roster.h"
#include "coop/props/prop_snapshot.h"
#include "coop/save/join_window_baseline.h"

#include "ue_wrap/actors/int_objects.h"
#include "ue_wrap/core/asset_load.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/world_identity.h"
#include "ue_wrap/world/weapon_catalog.h"

#include <cmath>
#include <cstdint>

namespace coop::dev::boar_hit_drill {
namespace {

namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;

constexpr uint64_t kSettleMs = 8000;
constexpr uint64_t kWatchMs = 90000;
constexpr uint64_t kReadMs = 2000;
constexpr uint64_t kSwingMs = 700;
constexpr float kAheadCm = 260.f;
constexpr float kHoldCm = 130.f;
constexpr uint64_t kHoldMs = 300;

bool On() {
    static const bool on = coop::config::ResolveFlag(::coop::config_registry::rows::boar_hit_drill);
    return on;
}

ue_wrap::CachedObjRef g_boar;   // HOST: the boar it spawned. CLIENT: the copy it swings at.
uint64_t g_readyMs = 0;
uint64_t g_startMs = 0;
uint64_t g_nextMs = 0;
uint64_t g_nextLogMs = 0;
float    g_health0 = 0.f;
uint32_t g_applied0 = 0;
int      g_swings = 0;
bool     g_axe = false;
bool     g_done = false;

bool ClientInWorld(coop::net::Session& s) {
    for (int slot = 1; slot < static_cast<int>(coop::players::kMaxPeers); ++slot)
        if (s.IsSlotWorldReady(slot) && coop::prop_snapshot::IsBracketClosed(slot) &&
            !coop::join_window_baseline::IsLateWindowOpen(slot))
            return true;
    return false;
}

bool Health(void* boar, float& out) {
    const int32_t off = R::FindPropertyOffset(R::ClassOf(boar), L"health");
    if (off < 0) return false;
    out = *reinterpret_cast<const float*>(reinterpret_cast<const uint8_t*>(boar) + off);
    return true;
}

void* BoarClass() {
    if (void* cls = ue_wrap::object_index::ClassByName(L"grayboar_C")) return cls;
    if (void* cls = R::FindClass(L"grayboar_C")) return cls;
    for (const wchar_t* path : {L"/Game/objects/grayboar.grayboar_C", L"/Game/objects/npc/grayboar.grayboar_C"})
        if (void* cls = ue_wrap::asset_load::LoadObjectByPath(path)) return cls;
    return nullptr;
}

void HostTick(coop::net::Session& s, uint64_t now) {
    if (!g_startMs) {
        if (!ClientInWorld(s)) return;
        if (!g_readyMs) g_readyMs = now;
        if (now - g_readyMs < kSettleMs) return;
        coop::RemotePlayer* puppet = coop::players::Registry::Get().Puppet(1);
        ue_wrap::FVector at{};
        void* cls = BoarClass();
        if (!puppet || !puppet->valid() || !puppet->TryGetLocation(at)) return;
        if (!cls) {
            UE_LOGW("[BOAR-HIT-DRILL] [H] FAIL the gray boar's class did not load");
            g_done = true;
            return;
        }
        const ue_wrap::FVector aim = puppet->GetSyncedAimDirection();
        const float len = std::sqrt(aim.X * aim.X + aim.Y * aim.Y);
        const float fx = len > 0.001f ? aim.X / len : 1.f, fy = len > 0.001f ? aim.Y / len : 0.f;
        void* boar = E::SpawnActor(cls, {at.X + fx * kAheadCm, at.Y + fy * kAheadCm, at.Z + 40.f});
        if (!boar || !Health(boar, g_health0)) {
            UE_LOGW("[BOAR-HIT-DRILL] [H] FAIL the boar did not spawn, or has no health to read (%p)", boar);
            g_done = true;
            return;
        }
        g_boar.Set(boar);
        g_applied0 = coop::hit_intent::AppliedCount();
        g_startMs = now;
        UE_LOGI("[BOAR-HIT-DRILL] [H] a gray boar spawned in front of the client's puppet, health %.0f", g_health0);
        return;
    }
    void* boar = g_boar.Get();
    float health = -1.f;
    const bool alive = boar && Health(boar, health);
    // The boar is held where the client's swings can be judged: within reach, on the line the puppet looks along.
    if (alive) {
        coop::RemotePlayer* puppet = coop::players::Registry::Get().Puppet(1);
        ue_wrap::FVector at{};
        if (puppet && puppet->valid() && puppet->TryGetLocation(at)) {
            const ue_wrap::FVector aim = puppet->GetSyncedAimDirection();
            E::SetActorLocation(boar, {at.X + aim.X * kHoldCm, at.Y + aim.Y * kHoldCm, at.Z + 60.f + aim.Z * kHoldCm});
        }
    }
    if (now < g_nextLogMs) return;
    g_nextLogMs = now + kReadMs;
    const uint32_t applied = coop::hit_intent::AppliedCount() - g_applied0;
    UE_LOGI("[BOAR-HIT-DRILL] [H] t+%llus boar=%d health=%.0f judged hits applied=%u",
            static_cast<unsigned long long>((now - g_startMs) / 1000), alive ? 1 : 0, health, applied);
    if (applied >= 2 && (!alive || health < g_health0 - 150.f)) {
        UE_LOGI("[BOAR-HIT-DRILL] [H] PASS %u judged hits applied; the boar's health went from %.0f to %s", applied, g_health0,
                alive ? "less" : "its death");
        g_done = true;
    } else if (now - g_startMs > kWatchMs) {
        UE_LOGW("[BOAR-HIT-DRILL] [H] FAIL after %llu s: %u judged hits applied, health %.0f of %.0f",
                static_cast<unsigned long long>(kWatchMs / 1000), applied, health, g_health0);
        g_done = true;
    }
}

// CLIENT: the nearest boar copy of this world, once.
void* FindCopy(void* me) {
    void* cls = ue_wrap::object_index::ClassByName(L"grayboar_C");
    ue_wrap::FVector mine{};
    if (!cls || !E::TryGetActorLocation(me, mine)) return nullptr;
    struct Seek {
        void* world;
        ue_wrap::FVector at;
        void* best;
        float bestD2;
    } seek{ue_wrap::world_identity::CurrentWorld(), mine, nullptr, 800.f * 800.f};
    ue_wrap::object_index::ForEachInstance(cls, [](void* ctx, void* obj, int32_t) {
        Seek* k = static_cast<Seek*>(ctx);
        ue_wrap::FVector at{};
        if (!obj || !R::IsLive(obj) || ue_wrap::world_identity::WorldOf(obj) != k->world || !E::TryGetActorLocation(obj, at))
            return;
        const float dx = at.X - k->at.X, dy = at.Y - k->at.Y, dz = at.Z - k->at.Z;
        const float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 < k->bestD2) { k->bestD2 = d2; k->best = obj; }
    }, &seek);
    return seek.best;
}

void ClientTick(uint64_t now) {
    void* me = coop::players::Registry::Get().Local();
    if (!me) return;
    void* copy = g_boar.Get();
    if (!copy) {
        if (g_swings > 0) {
            UE_LOGI("[BOAR-HIT-DRILL] [C] DONE the copy is gone after %d swings", g_swings);
            g_done = true;
            return;
        }
        copy = FindCopy(me);
        if (!copy) return;
        g_boar.Set(copy);
        g_startMs = now;
        UE_LOGI("[BOAR-HIT-DRILL] [C] a boar copy stands within reach -- taking an axe");
        return;
    }
    if (!g_axe) {
        g_axe = coop::dev::drill_hand::TakeIntoHandNow(L"axe", L"prop_axe_C");
        g_startMs = now;  // the hand's item reaches the host before the first swing
        if (!g_axe) {
            UE_LOGW("[BOAR-HIT-DRILL] [C] FAIL the axe could not be taken");
            g_done = true;
        }
        return;
    }
    if (now - g_startMs < 3000) return;
    ue_wrap::weapon_catalog::Swing sw;
    const float damage = (ue_wrap::weapon_catalog::Lookup(L"axe", sw) && sw.maxDamage > 0.f) ? sw.maxDamage : 30.f;
    ue_wrap::FVector at{};
    E::TryGetActorLocation(copy, at);
    const bool ran = ue_wrap::int_objects::InvokeAddDamage(copy, me, damage, at, ue_wrap::FVector{});
    ++g_swings;
    if (g_swings <= 3 || g_swings % 10 == 0)
        UE_LOGI("[BOAR-HIT-DRILL] [C] swing %d at the copy, damage %.0f: %d", g_swings, damage, ran ? 1 : 0);
    if (g_swings >= 110) {
        UE_LOGI("[BOAR-HIT-DRILL] [C] DONE %d swings", g_swings);
        g_done = true;
    }
}

}  // namespace

void Tick(coop::net::Session* session) {
    if (g_done || !On() || !session || !session->connected()) return;
    const uint64_t now = coop::net::NowMs();
    if (now < g_nextMs) return;
    const bool host = coop::roster::LocalIsHost();
    g_nextMs = now + (host ? (g_startMs ? kHoldMs : kReadMs) : (g_axe ? kSwingMs : kReadMs));
    if (host) HostTick(*session, now);
    else ClientTick(now);
}

void OnDisconnect() {
    g_boar.Reset();
    g_readyMs = g_startMs = g_nextMs = g_nextLogMs = 0;
    g_swings = 0;
    g_axe = false;
}

}  // namespace coop::dev::boar_hit_drill
