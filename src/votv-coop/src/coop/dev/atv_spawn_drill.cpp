// coop/dev/atv_spawn_drill.cpp -- see coop/dev/atv_spawn_drill.h.

#include "coop/dev/atv_spawn_drill.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/interactables/atv_sync.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"  // HasAnnouncedWorldReady
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/engine/engine.h"

#include <windows.h>

#include <cstdint>
#include <string>

namespace coop::dev::atv_spawn_drill {
namespace {

namespace E = ue_wrap::engine;

constexpr float    kAheadCm       = 600.f;   // in front of the host's player, clear of it
constexpr float    kLiftCm        = 50.f;
constexpr uint64_t kAnnounceBound = 30000;   // HOST: the spawn to the lane's announce
constexpr uint64_t kMirrorBound   = 60000;   // CLIENT: world-ready to the mirror
constexpr uint64_t kCheckEveryMs  = 250;

enum class Step : uint8_t { Arm, Announce, Mirror, Done };
Step     g_step = Step::Arm;
uint64_t g_stepMs = 0;
uint64_t g_nextCheckMs = 0;
int      g_session = 1;
uint32_t g_announced0 = 0;   // HOST: runtime ATVs announced before the drill's
uint32_t g_parked0 = 0;      // CLIENT: spawns parked before this session's first tick
bool     g_baselined = false;

const std::string& Mode() {
    static const std::string s = coop::config::ResolveString(::coop::config_registry::rows::atv_spawn_drill);
    return s;
}
bool Enabled() { return Mode() == "run" || Mode() == "join" || Mode() == "park"; }
void Next(Step s) { g_step = s; g_stepMs = ::GetTickCount64(); }
bool Expired(uint64_t bound) { return ::GetTickCount64() - g_stepMs > bound; }
void Fail(const char* what) {
    UE_LOGW("[ATV-SPAWN-DRILL] FAIL in session %d: %s", g_session, what);
    g_step = Step::Done;
}
void Abandon(const char* why) {
    UE_LOGW("[ATV-SPAWN-DRILL] ABANDONED in session %d: %s", g_session, why);
    g_step = Step::Done;
}
bool Due() {
    const uint64_t now = ::GetTickCount64();
    if (now < g_nextCheckMs) return false;
    g_nextCheckMs = now + kCheckEveryMs;
    return true;
}

void HostTick(coop::net::Session* s) {
    if (g_step == Step::Done || !Due()) return;
    if (g_step == Step::Arm) {
        // join: the spawn lands while slot 1 is seated and loading, so only the world-ready replay carries it.
        const bool ready = Mode() == "join" ? (s->IsSlotReady(1) && !s->IsSlotWorldReady(1)) : s->IsSlotWorldReady(1);
        if (!ready) return;
        void* me = coop::players::Registry::Get().Local();
        ue_wrap::FVector at{};
        if (!me || !E::TryGetActorLocation(me, at)) return;
        void* cls = ue_wrap::object_index::ClassByName(L"ATV_C");
        if (!cls) { Abandon("the ATV's class is not loaded"); return; }
        g_announced0 = coop::atv_sync::RuntimeAnnounced();
        const ue_wrap::FVector fwd = E::GetActorForwardVector(me);
        if (!E::SpawnActor(cls, {at.X + fwd.X * kAheadCm, at.Y + fwd.Y * kAheadCm, at.Z + kLiftCm})) {
            Abandon("the ATV did not spawn");
            return;
        }
        UE_LOGI("[ATV-SPAWN-DRILL] host (%s): an ATV spawned in front of this player", Mode().c_str());
        Next(Step::Announce);
        return;
    }
    if (coop::atv_sync::RuntimeAnnounced() > g_announced0) {
        UE_LOGI("[ATV-SPAWN-DRILL] host: the ATV lane announced the spawn under a synthetic key");
        g_step = Step::Done;
        return;
    }
    if (Expired(kAnnounceBound)) Fail("the host's ATV lane did not announce the runtime spawn within 30 s");
}

void ClientTick() {
    if (g_step == Step::Done) return;
    // The park baseline is taken at the session's first tick, before this world loads: the host spawns on its own
    // view of the slot's world-ready, which can come before this peer's own, so the spawn may land before the arm.
    if (!g_baselined) {
        g_baselined = true;
        g_parked0 = coop::atv_sync::ParkedEver();
    }
    if (g_step == Step::Arm) {
        if (!coop::net_pump::HasAnnouncedWorldReady() ||
            coop::join_progress::CurrentPhase() != coop::join_progress::Phase::Idle)
            return;
        Next(Step::Mirror);
        return;
    }
    if (!Due()) return;
    // Both ways in have run once nothing waits in the park: the broadcast or replay spawned, and a park handed back.
    const int mirrors = coop::atv_sync::RuntimeMirrors();
    const bool parkedOk = Mode() != "park" || coop::atv_sync::ParkedEver() > g_parked0;
    if (mirrors >= 1 && coop::atv_sync::ParkPending() == 0 && parkedOk) {
        if (mirrors != 1) {
            char why[120];
            std::snprintf(why, sizeof(why), "%d runtime ATV mirrors stand here for one spawn", mirrors);
            Fail(why);
            return;
        }
        UE_LOGI("[ATV-SPAWN-DRILL] client DONE in session %d (%s): one runtime ATV mirror stands here%s -- PASS",
                g_session, Mode().c_str(), Mode() == "park" ? ", the park having held and handed back its spawn" : "");
        g_step = Step::Done;
        return;
    }
    if (Expired(kMirrorBound))
        Fail(mirrors < 1 ? "no runtime ATV mirror stood here within 60 s of world-ready"
             : !parkedOk ? "the park never held the runtime ATV's spawn"
                         : "a runtime ATV spawn still waited in the park 60 s after world-ready");
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
    g_announced0 = g_parked0 = 0;
    g_baselined = false;
}

}  // namespace coop::dev::atv_spawn_drill
