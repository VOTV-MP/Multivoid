// coop/player/ko_respawn.cpp -- see coop/player/ko_respawn.h, which carries the
// measured death-chain bytecode and the reason this lane is PREVENTION and not a
// conversion. The short version: VOTV's death is a one-way latent chain that no
// flag can cancel, its single choke point is `ragdollMode`, and that function's
// first instruction is `IFNOT(canRagdoll) POP`. Hold that gate shut and no death
// can be authored at all; then a plain health poll is enough to notice that the
// player WOULD have died, and we author a survivable knock-out instead.

#include "coop/player/ko_respawn.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/ragdoll_gate.h"
#include "coop/session/teleport_client.h"
#include "ue_wrap/actors/vitals.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/core/types.h"

#include <atomic>
#include <chrono>
#include <cstdint>

namespace coop::ko_respawn {
namespace {

namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;
namespace V = ue_wrap::vitals;
namespace P = ue_wrap::profile;

// Cached Session. Only read to decide whether the lane is live at all.
std::atomic<coop::net::Session*> g_session{nullptr};

// Config cache (read in Install / OnSessionStart).
bool g_enabled = false;
int  g_ragdollSec = 5;
int  g_invulnSec  = 3;
bool g_spawnAtStart = true;

// One-shot latch for Install(), which the pump calls EVERY TICK. Never cleared:
// the per-session re-arm is OnSessionStart.
bool g_installed = false;

// KO state (game thread only).
bool     g_active = false;
void*    g_player = nullptr;
int32_t  g_playerIdx = -1;
uint64_t g_koStartedMs = 0;
uint64_t g_invulnUntilMs = 0;

// The KO triggers on a FALLING EDGE through zero, not on "health is zero". A
// pawn that has not finished loading its save reads health 0.0 for a few ticks,
// and a level-load KO would teleport the player mid-load. So the trigger arms
// only after we have SEEN this pawn alive with positive health.
bool g_armed = false;

// One-shot alarm for the fail-safe (see HandleLocalDeath): `dead` becoming true
// means the gate never took, and it will keep reading true until the travel, so
// the log line has to be latched or it is per-tick spam.
bool g_deathAlarmFired = false;

uint64_t NowMs() {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

void ReadConfig() {
    g_enabled = coop::config::ResolveFlag(coop::config_registry::rows::ko_respawn) != 0;
    g_ragdollSec = static_cast<int>(coop::config::ResolveInt(coop::config_registry::rows::ko_ragdoll_seconds));
    g_invulnSec  = static_cast<int>(coop::config::ResolveInt(coop::config_registry::rows::ko_invulnerable_seconds));
    g_spawnAtStart = coop::config::ResolveFlag(coop::config_registry::rows::ko_spawn_at_start) != 0;
}

// Drop the player into the NATIVE faint. Preconditions the caller guarantees:
// the gate is held (so `dead` is provably false) and the player is not already
// KO'd. Game thread.
void StartKO(void* mp) {
    if (!mp || g_active) return;

    // Park health at 1 BEFORE the flop. At 0 the game re-enters `kill()` on every
    // subsequent damage tick; the gate makes each of those harmless, but a
    // positive value keeps the HUD honest and stops the churn.
    V::Write(V::Field::Health, 1.f);

    // Borrow our own gate for exactly this call. `dead` is false, so
    // `fallen(false)` takes uber @39685's LIVING branch (@39704) instead of
    // jumping back into the death chain -- that is the whole difference between
    // this and the retired conversion.
    {
        coop::ragdoll_gate::ScopedOpen open(mp);
        E::SetMainPlayerRagdollMode(mp, /*ragdoll=*/true, /*passOut=*/true, /*death=*/false);
    }

    g_active = true;
    g_player = mp;
    g_playerIdx = R::InternalIndexOf(mp);
    g_koStartedMs = NowMs();
    const ue_wrap::FVector loc = E::GetActorLocation(mp);
    UE_LOGI("ko_respawn: KO STARTED player=%p at(%.0f,%.0f,%.0f) -- respawn in %d s",
            mp, loc.X, loc.Y, loc.Z, g_ragdollSec);
}

// The KO's tail: stand the player up, restore vitals, place them, arm immunity.
void FinishKO(void* mp, uint64_t now) {
    // forceWakeup, NOT forceGetUp: the latter runs a 0.2 s latent Delay and
    // lands in the branch that re-reads `dead` (uber @39625 -> @39685), while
    // forceWakeup restores movement/collision/camera/input unconditionally
    // (uber @25800). Reads no gate, so it works with the gate held.
    E::ForceMainPlayerWakeup(mp);
    V::Write(V::Field::Food,   100.f);
    V::Write(V::Field::Sleep,  100.f);
    V::Write(V::Field::Health, 100.f);
    if (g_spawnAtStart) {
        coop::teleport_client::ApplyLocally(
            {P::name::kKPPSpawnX, P::name::kKPPSpawnY, P::name::kKPPSpawnZ, 0.f, 0.f, 0.f});
    }
    g_invulnUntilMs = (g_invulnSec > 0)
                          ? (now + static_cast<uint64_t>(g_invulnSec) * 1000ull) : 0;
    g_active = false;
    UE_LOGI("ko_respawn: RESPAWNED %s -- vitals full%s",
            g_spawnAtStart ? "at KPP start" : "in place",
            g_invulnSec > 0 ? "; damage immunity armed" : "");
}

}  // namespace

bool Enabled() { return g_enabled; }
bool Active()  { return g_active; }

bool HandleLocalDeath(coop::net::Session& session, void* mainPlayer) {
    (void)session;
    (void)mainPlayer;
    if (!g_enabled) return false;  // legacy permadeath flow, feature off
    // Reaching here means `dead` is true, which this design makes unreachable:
    // the only writer is uber @37412, gated behind `ragdollMode`, gated behind
    // `canRagdoll`. So the gate never took on this pawn. There is NOTHING to
    // convert -- `dead := false` does not exist in the game and both latent
    // delays to the main menu are already armed and do not re-read the flag.
    // Return false and let the caller run the legacy flee: an honest kick to the
    // menu beats a silent claim to have saved a player the game is about to
    // travel out from under.
    if (!g_deathAlarmFired) {
        g_deathAlarmFired = true;
        UE_LOGW("ko_respawn: FAIL-SAFE -- dead=true reached us with ko_respawn ON. The "
                "canRagdoll gate did not take on this pawn (holders=%d), so the death "
                "chain ran and cannot be undone. Falling back to the permadeath flee.",
                coop::ragdoll_gate::Holds(coop::ragdoll_gate::Holder::KoRespawn) ? 1 : 0);
    }
    return false;
}

void Install(coop::net::Session* session) {
    // CALLED EVERY PUMP TICK -- everything here must be idempotent AND SILENT on
    // re-entry. (The v1 version logged unconditionally, ~30 lines/sec in a
    // 2026-08-29 smoke.) The session pointer is still refreshed on every call,
    // because a Stop()/Start() cycle in one process hands us a different Session.
    g_session.store(session, std::memory_order_release);
    if (g_installed) return;
    g_installed = true;
    ReadConfig();
    UE_LOGI("ko_respawn: install (enabled=%d ragdoll=%ds invuln=%ds spawnAtStart=%d)",
            g_enabled ? 1 : 0, g_ragdollSec, g_invulnSec, g_spawnAtStart ? 1 : 0);
}

void Tick() {
    if (!g_enabled) return;
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running()) return;

    void* mp = coop::players::Registry::Get().Local();
    if (!mp || !R::IsLive(mp)) return;

    // (1) THE DEATH GATE. Held for the whole session; re-asserted on the current
    // pawn because a world load hands us a fresh mainPlayer_C back at its class
    // default. Both calls are no-ops once settled.
    coop::ragdoll_gate::Hold(coop::ragdoll_gate::Holder::KoRespawn);
    coop::ragdoll_gate::Tick(mp);

    // (2) FAIL-SAFE probe. With the gate held this is unreachable; if it fires,
    // net_pump's death policy will see the same flag and run HandleLocalDeath.
    bool isRag = false, dead = false;
    const bool haveState = E::ReadMainPlayerRagdollState(mp, isRag, dead);

    float hp = 0.f;
    const bool haveHp = V::Read(V::Field::Health, &hp);

    if (!g_active) {
        // (3) POST-RESPAWN IMMUNITY. Enforced by PINNING health, not by
        // cancelling damage: the game's own damage entries are invisible to a
        // ProcessEvent interceptor (all seven `Add Player Damage` call sites are
        // EX_LocalVirtualFunction), so a cancel could only ever cover damage WE
        // relay. A pin covers every source, seen or unseen.
        if (g_invulnUntilMs != 0) {
            if (NowMs() < g_invulnUntilMs) {
                if (haveHp && hp < 100.f) V::Write(V::Field::Health, 100.f);
            } else {
                g_invulnUntilMs = 0;
            }
        }

        // (4) THE KO TRIGGER: a falling edge through zero on the CURRENT pawn.
        if (mp != g_player) { g_player = mp; g_playerIdx = R::InternalIndexOf(mp); g_armed = false; }
        if (!haveHp) return;
        if (hp > 0.f) { g_armed = true; return; }
        if (!g_armed) return;  // never seen this pawn alive -- still loading
        UE_LOGI("ko_respawn: local health reached %.2f -- the game would have killed us; "
                "converting to a knock-out", hp);
        StartKO(mp);
        return;
    }

    // ---- a KO is in progress --------------------------------------------------
    if (!R::IsLiveByIndex(g_player, g_playerIdx) || g_player != mp) {
        UE_LOGW("ko_respawn: KO'd player gone (GC / level change) -- clearing KO state");
        g_active = false; g_player = nullptr; g_playerIdx = -1; g_armed = false;
        return;
    }
    // Keep health parked so nothing re-enters the damage math while we are down.
    if (haveHp && hp != 1.f) V::Write(V::Field::Health, 1.f);
    // Pin the flop: a faint can let the player stand early, and the respawn must
    // fire from a downed state. Borrow the gate for the re-flop, same as StartKO.
    if (haveState && !isRag && !dead) {
        coop::ragdoll_gate::ScopedOpen open(g_player);
        E::SetMainPlayerRagdollMode(g_player, /*ragdoll=*/true, /*passOut=*/true, /*death=*/false);
    }
    const uint64_t now = NowMs();
    if (now - g_koStartedMs < static_cast<uint64_t>(g_ragdollSec) * 1000ull) return;
    FinishKO(g_player, now);
}

void OnSessionStart() {
    ReadConfig();
    g_active = false; g_player = nullptr; g_playerIdx = -1;
    g_koStartedMs = 0; g_invulnUntilMs = 0;
    g_armed = false; g_deathAlarmFired = false;
}

void OnDisconnect() {
    g_active = false; g_player = nullptr; g_playerIdx = -1;
    g_koStartedMs = 0; g_invulnUntilMs = 0;
    g_armed = false; g_deathAlarmFired = false;
    // Never strand the player un-ragdollable past the session -- that would block
    // every ragdoll cause in single-player too, including a real death the stock
    // game is entitled to run.
    coop::ragdoll_gate::Release(coop::ragdoll_gate::Holder::KoRespawn);
}

}  // namespace coop::ko_respawn
