// coop/player/ko_respawn.cpp -- see coop/player/ko_respawn.h.
//
// The KO RESPAWN machine: lethal damage converts into a knock-out instead of the
// stock permadeath (death -> kick to the main menu -> rejoin). The lethal hit is
// canceled BEFORE the native death state enters (AddPlayerDamage interceptor); a
// death that slips through another source is caught by the net_pump backstop and
// converted the same way. Both paths call StartKO, which drops the player into the
// NATIVE faint state (ragdollMode(passOut=true, death=false)) -- the same state
// exhaustion-faint/sleep use, so no death screen and no native save-write. After
// ko_ragdoll_seconds the player forceGetUp's, vitals restore to full, and the
// player teleports to the КПП start point (or gets up in place).

#include "coop/player/ko_respawn.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/session/teleport_client.h"
#include "ue_wrap/actors/vitals.h"
#include "ue_wrap/core/game_thread.h"
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
namespace GT = ue_wrap::game_thread;
namespace rows = coop::config_registry::rows;

// Cached Session (same atomic pattern as player_damage; the interceptor reads it
// on every hit, never dereferences a live-null).
std::atomic<coop::net::Session*> g_session{nullptr};

// Config cache (read in Install / OnSessionStart). The interceptor + HandleLocalDeath
// gate on g_enabled; Tick uses the timing rows.
bool g_enabled = false;
int  g_ragdollSec = 5;
int  g_invulnSec  = 3;
bool g_spawnAtStart = true;

// AddPlayerDamage interceptor resolve/install latch (process-lifetime; the resolve
// retries in Tick until mainPlayer_C loads, then registers once -- the wisp shape).
bool g_interceptorInstalled = false;
int32_t g_damageOff = -1;  // "Add Player Damage" param `Damage` float offset

// KO state (game thread only).
bool     g_active = false;
void*    g_player = nullptr;
int32_t  g_playerIdx = -1;
uint64_t g_koStartedMs = 0;
uint64_t g_invulnUntilMs = 0;

uint64_t NowMs() {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

void ReadConfig() {
    g_enabled = coop::config::ResolveFlag(rows::ko_respawn) != 0;
    g_ragdollSec = static_cast<int>(coop::config::ResolveInt(rows::ko_ragdoll_seconds));
    g_invulnSec  = static_cast<int>(coop::config::ResolveInt(rows::ko_invulnerable_seconds));
    g_spawnAtStart = coop::config::ResolveFlag(rows::ko_spawn_at_start) != 0;
}

// Start the KO: drop the player into the native faint ragdoll (NOT death), park
// health at 1, and arm the respawn timer. Game thread. Idempotent (g_active gate).
void StartKO(void* mp) {
    if (!mp || g_active) return;
    g_active = true;
    g_player = mp;
    g_playerIdx = R::InternalIndexOf(mp);
    g_koStartedMs = NowMs();
    // Native faint: ragdollMode(true, passOut=true, death=false). The same function
    // the death path calls with death=true -- here it flips the player down WITHOUT
    // the death state (no death screen, no native save-write). On the net_pump
    // backstop this is also the mechanism that CLEARS the dead flag the death set.
    E::SetMainPlayerRagdollMode(mp, /*ragdoll=*/true, /*passOut=*/true, /*death=*/false);
    V::Write(V::Field::Health, 1.f);  // "almost died" meter state during the KO
    const ue_wrap::FVector loc = E::GetActorLocation(mp);
    UE_LOGI("ko_respawn: KO STARTED player=%p at(%.0f,%.0f,%.0f) -- respawn in %d s",
            mp, loc.X, loc.Y, loc.Z, g_ragdollSec);
}

// PRIMARY layer -- cancel a LETHAL "Add Player Damage" BEFORE the native death
// enters. Also cancels ALL damage while KO'd / during the post-respawn invuln
// window. Fires on the game thread (ProcessEvent dispatch), same contract as the
// wisp's AddPlayerDamage cancel. Multi-slot interceptor table: coexists with the
// wisp's cancel on the same UFunction.
bool OnAddPlayerDamagePre(void* self, void* params) {
    if (!g_enabled) return false;
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running()) return false;
    // KO'd or post-respawn invulnerable: cancel everything.
    if (g_active) return true;
    if (g_invulnUntilMs != 0 && NowMs() < g_invulnUntilMs) return true;
    // Only the LOCAL possessed pawn is ours to KO. A PUPPET taking damage in this
    // world is the wire-relay source (host-side enemy -> the OWNER's machine) -- the
    // owner's own "Add Player Damage" fires there, and this interceptor runs there.
    if (!coop::players::Registry::Get().IsLocal(self)) return false;
    float health = 0.f;
    if (!V::Read(V::Field::Health, &health) || health <= 0.f) return false;
    float damage = 0.f;
    if (g_damageOff >= 0)
        damage = *reinterpret_cast<float*>(static_cast<uint8_t*>(params) + g_damageOff);
    if (!(damage > 0.f) || damage < health) return false;  // non-lethal -> pass through
    UE_LOGI("ko_respawn: LETHAL hit %.1f >= health %.1f on local player -- canceling "
            "damage + KO", damage, health);
    StartKO(self);
    return true;  // cancel the native dispatch (the player never enters the death state)
}

}  // namespace

bool Enabled() { return g_enabled; }
bool Active()  { return g_active; }

bool HandleLocalDeath(coop::net::Session& session, void* mainPlayer) {
    // net_pump death-policy BACKSTOP: dead=true fired from a source that bypassed
    // "Add Player Damage" (fall / fire / radiation / a verb the interceptor missed).
    // Convert it into the same KO instead of the permadeath flee.
    (void)session;
    if (!g_enabled) return false;   // legacy permadeath flow
    if (g_active) return true;      // already down -- absorb the re-entry
    UE_LOGI("ko_respawn: death backstop fired (dead=true) -- converting death into a KO");
    StartKO(mainPlayer);
    return true;
}

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    ReadConfig();
    // The interceptor registers lazily in Tick once mainPlayer_C + the UFunction
    // resolve (AddPlayerDamageFunctionPtr self-gates until the class loads).
    g_interceptorInstalled = false;
    UE_LOGI("ko_respawn: install (enabled=%d ragdoll=%ds invuln=%ds spawnAtStart=%d)",
            g_enabled ? 1 : 0, g_ragdollSec, g_invulnSec, g_spawnAtStart ? 1 : 0);
}

void Tick() {
    // Lazy resolve + install the AddPlayerDamage interceptor (retry until mainPlayer
    // loads). Give-up latch on a missing param (like trash_use_intercept) -- the
    // net_pump backstop still covers every death, so a failed interceptor degrades
    // to the backstop-only path, never to permadeath.
    if (!g_interceptorInstalled && g_enabled) {
        void* fn = E::AddPlayerDamageFunctionPtr();
        if (fn) {
            g_damageOff = R::FindParamOffset(fn, L"Damage");
            if (g_damageOff < 0) {
                UE_LOGW("ko_respawn: AddPlayerDamage 'Damage' param not found -- "
                        "interceptor cannot read the hit; using the death backstop only");
                g_interceptorInstalled = true;
            } else if (GT::RegisterInterceptor(fn, &OnAddPlayerDamagePre)) {
                g_interceptorInstalled = true;
                UE_LOGI("ko_respawn: installed AddPlayerDamage PRE interceptor @ %p (Damage@%d)",
                        fn, g_damageOff);
            }
        }
    }
    if (!g_active) return;
    if (!R::IsLiveByIndex(g_player, g_playerIdx)) {
        UE_LOGW("ko_respawn: KO'd player gone (GC/level change) -- clearing KO state");
        g_active = false; g_player = nullptr; g_playerIdx = -1;
        return;
    }
    const uint64_t now = NowMs();
    // Pin: keep the player ragdolled for the whole KO (a faint can let the player
    // stand early -- re-flop so the respawn fires from a downed state). Cheap: only
    // runs while g_active.
    bool isRag = false, dead = false;
    if (E::ReadMainPlayerRagdollState(g_player, isRag, dead) && !isRag && !dead) {
        E::SetMainPlayerRagdollMode(g_player, /*ragdoll=*/true, /*passOut=*/true, /*death=*/false);
    }
    if (now - g_koStartedMs < static_cast<uint64_t>(g_ragdollSec) * 1000ull) return;

    // ---- RESPAWN --------------------------------------------------------------
    E::ForceMainPlayerGetUp(g_player);
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
            g_spawnAtStart ? "at КПП start" : "in place",
            g_invulnSec > 0 ? "; damage immunity armed" : "");
}

void OnSessionStart() {
    ReadConfig();
    g_active = false; g_player = nullptr; g_playerIdx = -1;
    g_koStartedMs = 0; g_invulnUntilMs = 0;
}

void OnDisconnect() {
    g_active = false; g_player = nullptr; g_playerIdx = -1;
    g_koStartedMs = 0; g_invulnUntilMs = 0;
}

}  // namespace coop::ko_respawn
