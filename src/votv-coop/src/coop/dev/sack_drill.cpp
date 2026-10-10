// coop/dev/sack_drill.cpp -- see coop/dev/sack_drill.h.

#include "coop/dev/sack_drill.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/dev/director/aim_fan.h"
#include "coop/items/point_sack_intent.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/puppet_drive.h"
#include "coop/player/remote_player.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"  // HasAnnouncedWorldReady
#include "ue_wrap/core/asset_load.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/world/economy.h"
#include "ue_wrap/world/world_singleton.h"

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>

namespace coop::dev::sack_drill {
namespace {

namespace E = ue_wrap::engine;
namespace R = ue_wrap::reflection;

constexpr const wchar_t* kSackPath  = L"/Game/objects/prop_pointSack.prop_pointSack_C";
constexpr const wchar_t* kSackClass = L"prop_pointSack_C";
constexpr float    kAheadCm      = 100.f;   // in front of the puppet: inside the player's interaction reach
constexpr float    kDropCm       = 20.f;    // above its feet's height, so it settles on the floor
constexpr uint64_t kPayBoundMs   = 60000;   // HOST: the spawn to the pay (the client's find, aim and press)
constexpr uint64_t kFindBoundMs  = 60000;   // CLIENT: world-ready to the sack's mirror
constexpr uint64_t kSeenBoundMs  = 30000;   // CLIENT: the press to its mirrored balance rising
constexpr uint64_t kReadyBoundMs = 3000;    // CLIENT: the aim to the player's action list holding the sack's
constexpr uint64_t kCheckEveryMs = 250;
constexpr uint64_t kSettleBoundMs = 10000;  // CLIENT: the mirror's find to it at rest (it is spawned above the floor)
constexpr int      kRestChecks    = 4;      // ...that many reads in a row, a check apart, within a centimetre

enum class Step : uint8_t { Arm, Find, Settle, Aim, Ready, Press, Seen, Done };
Step     g_step = Step::Arm;
uint64_t g_stepMs = 0;
uint64_t g_nextCheckMs = 0;
int      g_session = 1;
int32_t  g_points0 = 0;      // this peer's balance before the sack
int32_t  g_sackPoints = 0;   // HOST: the sack's own `points`
uint64_t g_paid0 = 0;        // HOST: paid redemptions before the sack
ue_wrap::CachedObjRef g_sack;   // CLIENT: the sack's mirror
coop::director::AimFan g_fan;
ue_wrap::FVector g_lastAt{};
int      g_restReads = 0;
uint64_t g_nextReadMs = 0;

bool Enabled() {
    static const bool on = coop::config::ResolveString(::coop::config_registry::rows::sack_drill) == "run";
    return on;
}
void Next(Step s) { g_step = s; g_stepMs = ::GetTickCount64(); }
bool Expired(uint64_t bound) { return ::GetTickCount64() - g_stepMs > bound; }
void Fail(const char* what) {
    UE_LOGW("[SACK-DRILL] FAIL in session %d: %s", g_session, what);
    g_step = Step::Done;
}
void Abandon(const char* why) {
    UE_LOGW("[SACK-DRILL] ABANDONED in session %d: %s", g_session, why);
    g_step = Step::Done;
}

// The sack class's `points`, the sum its own action pays. -1 unread.
int32_t SackPoints(void* sack) {
    if (!sack || !R::IsLive(sack)) return -1;
    const int32_t off = R::FindPropertyOffset(R::ClassOf(sack), L"points");
    if (off < 0) return -1;
    int32_t v = 0;
    std::memcpy(&v, static_cast<const char*>(sack) + off, sizeof(v));
    return v;
}

// The player's own look state, named when its action list never takes the sack: which of
// selectedAction's gates held (its look-at actor, the look flag, an open interface, the trace's hit).
void SayLookState(void* player, void* sack) {
    void* cls = R::ClassOf(player);
    const int32_t lookOff = R::FindPropertyOffset(cls, L"lookAtActor");
    const int32_t uiOff = R::FindPropertyOffset(cls, L"activeInterface");
    int32_t flagOff = -1;
    uint8_t flagMask = 0;
    void* look = nullptr;
    void* ui = nullptr;
    if (lookOff >= 0) std::memcpy(&look, static_cast<const char*>(player) + lookOff, sizeof(look));
    if (uiOff >= 0) std::memcpy(&ui, static_cast<const char*>(player) + uiOff, sizeof(ui));
    const bool flagRead = R::FindBoolProperty(cls, L"isLookingAt", flagOff, flagMask);
    const int flag = flagRead ? ((static_cast<const uint8_t*>(player)[flagOff] & flagMask) ? 1 : 0) : -1;
    UE_LOGW("[SACK-DRILL] client: the look state -- lookAtActor=%p (the sack %p) isLookingAt=%d activeInterface=%p "
            "trace hit=%p", look, sack, flag, ui, E::ReadMainPlayerHitActor(player));
}

void HostTick(coop::net::Session* s) {
    if (g_step == Step::Done) return;
    const uint64_t now = ::GetTickCount64();
    if (now < g_nextCheckMs) return;
    g_nextCheckMs = now + kCheckEveryMs;
    if (g_step == Step::Arm) {
        if (!s->IsSlotWorldReady(1)) return;
        void* puppet = coop::puppet_drive::Puppet(1).GetActor();   // spawned once the peer's pose arrives
        ue_wrap::FVector at{};
        if (!puppet || !R::IsLive(puppet) || !E::TryGetActorLocation(puppet, at)) return;
        void* cls = ue_wrap::asset_load::LoadObjectByPath(kSackPath);
        if (!cls) { Abandon("the point sack's class does not load"); return; }
        const ue_wrap::FVector fwd = E::GetActorForwardVector(puppet);
        void* sack = E::SpawnActor(cls, {at.X + fwd.X * kAheadCm, at.Y + fwd.Y * kAheadCm, at.Z + kDropCm});
        g_sackPoints = SackPoints(sack);
        if (!sack || g_sackPoints <= 0) { Abandon("the point sack did not spawn or its points are unread"); return; }
        if (!ue_wrap::economy::ReadPoints(&g_points0)) { Abandon("the host's balance is unread"); return; }
        g_paid0 = coop::point_sack_intent::PaidCount();
        UE_LOGI("[SACK-DRILL] host: a point sack worth %d spawned before the client's puppet; balance %d",
                g_sackPoints, g_points0);
        Next(Step::Seen);
        return;
    }
    // Seen: the client's redemption paid here, once, by the sack's own sum.
    const uint64_t paid = coop::point_sack_intent::PaidCount();
    if (paid == g_paid0) {
        if (Expired(kPayBoundMs)) Fail("the client's sack was not paid on the host within 60 s");
        return;
    }
    int32_t now1 = 0;
    if (!ue_wrap::economy::ReadPoints(&now1)) return;
    if (paid != g_paid0 + 1 || now1 - g_points0 != g_sackPoints) {
        char why[160];
        std::snprintf(why, sizeof(why), "the host paid %llu redemption(s) and its balance moved by %d, not one and %d",
                      static_cast<unsigned long long>(paid - g_paid0), now1 - g_points0, g_sackPoints);
        Fail(why);
        return;
    }
    UE_LOGI("[SACK-DRILL] host DONE in session %d: the client's sack paid here once, %d -> %d (+%d)", g_session,
            g_points0, now1, g_sackPoints);
    g_step = Step::Done;
}

void ClientTick() {
    if (g_step == Step::Done) return;
    void* player = coop::players::Registry::Get().Local();
    if (!player) return;
    switch (g_step) {
    case Step::Arm:
        if (!coop::net_pump::HasAnnouncedWorldReady() ||
            coop::join_progress::CurrentPhase() != coop::join_progress::Phase::Idle)
            return;
        Next(Step::Find);
        return;
    case Step::Find: {
        void* sack = ue_wrap::world_singleton::Find(kSackClass);
        if (!sack) {
            if (Expired(kFindBoundMs)) Abandon("no point sack's mirror reached this client within 60 s");
            return;
        }
        if (!ue_wrap::economy::ReadPoints(&g_points0)) return;
        g_sack.Set(sack);
        g_restReads = 0;
        UE_LOGI("[SACK-DRILL] client: the sack's mirror is here; balance %d", g_points0);
        Next(Step::Settle);
        return;
    }
    case Step::Settle: {   // spawned above the floor: aimed at only once it lies still
        const uint64_t now = ::GetTickCount64();
        if (now < g_nextReadMs) return;
        g_nextReadMs = now + kCheckEveryMs;
        ue_wrap::FVector at{};
        void* sack = g_sack.Get();
        if (!sack || !E::TryGetActorLocation(sack, at)) { Abandon("the sack's mirror died"); return; }
        const float moved = std::fabs(at.X - g_lastAt.X) + std::fabs(at.Y - g_lastAt.Y) + std::fabs(at.Z - g_lastAt.Z);
        g_restReads = moved < 1.f ? g_restReads + 1 : 0;
        g_lastAt = at;
        if (g_restReads >= kRestChecks) {
            g_fan = coop::director::AimFan();
            Next(Step::Aim);
            return;
        }
        if (Expired(kSettleBoundMs)) Abandon("the sack's mirror never came to rest");
        return;
    }
    case Step::Aim: {
        ue_wrap::FVector at{};
        void* sack = g_sack.Get();
        if (!sack || !E::TryGetActorLocation(sack, at)) { Abandon("the sack's mirror died"); return; }
        const bool aimed = E::ReadMainPlayerHitActor(player) == sack;
        const auto st = g_fan.Tick(player, at, aimed);
        if (st == coop::director::AimFan::State::Failed) { Abandon("no pose of the fan put the trace on the sack"); return; }
        if (st == coop::director::AimFan::State::Aimed) Next(Step::Ready);
        return;
    }
    case Step::Ready:   // the player's own action list, rebuilt after the look changed, holds the sack's action
        if (E::MainPlayerHasSelectedAction(player)) { Next(Step::Press); return; }
        if (E::ReadMainPlayerHitActor(player) != g_sack.Get()) {   // the trace left it: aim again
            g_fan = coop::director::AimFan();
            Next(Step::Aim);
            return;
        }
        if (Expired(kReadyBoundMs)) {
            SayLookState(player, g_sack.Get());
            Abandon("the player's action list never held an action for the sack");
        }
        return;
    case Step::Press:
        if (!E::CallMainPlayerUseSelectedAction(player)) { Abandon("useSelectedAction did not dispatch"); return; }
        UE_LOGI("[SACK-DRILL] client pressed the sack");
        Next(Step::Seen);
        return;
    case Step::Seen: {
        int32_t now1 = 0;
        if (ue_wrap::economy::ReadPoints(&now1) && now1 > g_points0) {
            UE_LOGI("[SACK-DRILL] client: the mirrored balance rose %d -> %d", g_points0, now1);
            g_step = Step::Done;
            return;
        }
        if (Expired(kSeenBoundMs)) Fail("this client's mirrored balance did not rise within 30 s of the press");
        return;
    }
    case Step::Done:
        return;
    }
}

}  // namespace

void Tick(coop::net::Session* s) {
    if (!Enabled() || !s) return;
    if (s->role() == coop::net::Role::Host) HostTick(s);
    else if (s->connected()) ClientTick();
}

void OnDisconnect() {
    if (!Enabled()) return;
    ++g_session;
    g_step = Step::Arm;
    g_stepMs = g_nextCheckMs = 0;
    g_points0 = g_sackPoints = 0;
    g_paid0 = 0;
    g_sack.Reset();
    g_restReads = 0;
    g_nextReadMs = 0;
    g_lastAt = {};
}

}  // namespace coop::dev::sack_drill
