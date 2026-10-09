// coop/dev/event_drill.cpp -- see coop/dev/event_drill.h.

#include "coop/dev/event_drill.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/dev/set_clock.h"
#include "coop/net/session.h"
#include "coop/world/event_cue_sync.h"
#include "coop/world/event_fire_sync.h"
#include "coop/element/registry.h"

#include "ue_wrap/core/log.h"

#include <chrono>
#include <vector>

namespace coop::dev::event_drill {
namespace {

namespace EF = coop::event_fire_sync;

constexpr int kSlot = 1;              // the pair's client
constexpr int kRowDay = 2;            // starRain: the second day (displayed) at 00:17
constexpr int kRowMinute = 17;

enum class HostStep { PreJoinFire, SchedulerFire, Done };
HostStep g_host = HostStep::PreJoinFire;
bool g_clientDone = false;

void TickHost(coop::net::Session& s) {
    switch (g_host) {
    case HostStep::PreJoinFire:
        if (!s.IsSlotReady(kSlot)) return;  // the client's transport is not up yet
        if (s.IsSlotWorldReady(kSlot)) {
            UE_LOGW("[EVENT-DRILL] host FAIL -- the client's world was up before its transport was seen; the "
                    "pre-join fire cannot be placed");
            g_host = HostStep::Done;
            return;
        }
        {
            int h = 0, m = 0, d = 0;
            float frac = 0;
            if (!coop::dev::set_clock::ReadCurrent(h, m, d, frac)) return;  // the clock is not resolved yet
            if (d > kRowDay || (d == kRowDay && (h > 0 || m >= kRowMinute))) {
                UE_LOGW("[EVENT-DRILL] host FAIL -- the save is at day %d %02d:%02d, past starRain's day %d 00:%02d",
                        d, h, m, kRowDay, kRowMinute);
                g_host = HostStep::Done;
                return;
            }
        }
        UE_LOGI("[EVENT-DRILL] host: the client's transport is up and its world is not -- the dev fire of starRain");
        EF::HostFire(EF::FireKind::RunEvent, L"starRain", L"None");
        g_host = HostStep::SchedulerFire;
        return;
    case HostStep::SchedulerFire:
        // The world-ready handler sent the join snapshot before this tick could run.
        if (!s.IsSlotWorldReady(kSlot)) return;
        UE_LOGI("[EVENT-DRILL] host: the client's world is up -- the clock to day %d 00:%02d, so the scheduler "
                "fires starRain; then the dev fires of solar (replayed), arirGraff_0 (a special, replayed) and "
                "enasus (its props ride the prop lane: not replayed)", kRowDay, kRowMinute + 1);
        coop::dev::set_clock::SetClock(kRowDay, 0, kRowMinute + 1);
        EF::HostFire(EF::FireKind::RunEvent, L"solar", L"None");
        EF::HostFire(EF::FireKind::SpecialEvent, L"arirGraff_0", L"None");
        EF::HostFire(EF::FireKind::RunEvent, L"enasus", L"None");
        g_host = HostStep::Done;
        return;
    case HostStep::Done:
        return;
    }
}

void TickClient() {
    if (g_clientDone) return;
    const unsigned showers = coop::event_cue_sync::ReplayCount();
    const unsigned fires = EF::ReplayCount();
    if (showers < 2 || fires < 2) return;
    g_clientDone = true;
    UE_LOGI("[EVENT-DRILL] client DONE showers=%u fires=%u -- the join snapshot's shower and the scheduler's; "
            "solar and arirGraff_0 replayed", showers, fires);
}

// The egg arm (egg_drill): eggvasion's 121 eggs are the npc lane's largest population, and a still
// mirror must cost the client nothing once its pose stops changing. Readiness, not a clock: the
// host fires when the client's world is up, the client counts its mirrors and ends 30 s after the
// count first passes 100, so the [perf] lines of that window are the still eggs' cost.
bool g_eggFired = false;
bool g_eggDone = false;
std::chrono::steady_clock::time_point g_eggSeen{}, g_eggNextSay{};

void TickEggHost(coop::net::Session& s) {
    if (g_eggFired || !s.IsSlotWorldReady(kSlot)) return;
    g_eggFired = true;
    UE_LOGI("[EGG-DRILL] host: the client's world is up -- the dev fire of eggvasion");
    EF::HostFire(EF::FireKind::RunEvent, L"eggvasion", L"None");
}

void TickEggClient() {
    if (g_eggDone) return;
    const auto now = std::chrono::steady_clock::now();
    if (now < g_eggNextSay) return;
    g_eggNextSay = now + std::chrono::seconds(5);
    std::vector<coop::element::Registry::ActorIdPair> npcs;
    const size_t n = coop::element::Registry::Get().SnapshotActorsByType(coop::element::ElementType::Npc, npcs);
    UE_LOGI("[EGG-DRILL] client npc mirrors=%zu", n);
    if (n <= 100) return;
    if (g_eggSeen == std::chrono::steady_clock::time_point{}) { g_eggSeen = now; return; }
    if (now - g_eggSeen < std::chrono::seconds(30)) return;
    g_eggDone = true;
    UE_LOGI("[EGG-DRILL] client DONE mirrors=%zu -- read this window's [perf] reflected calls", n);
}

}  // namespace

void Tick(coop::net::Session* session) {
    static const bool s_on = coop::config::ResolveFlag(::coop::config_registry::rows::event_drill);
    static const bool s_eggs = coop::config::ResolveFlag(::coop::config_registry::rows::egg_drill);
    if ((!s_on && !s_eggs) || !session || !session->running()) return;
    const bool host = session->role() == coop::net::Role::Host;
    if (s_on) { if (host) TickHost(*session); else TickClient(); }
    if (s_eggs) { if (host) TickEggHost(*session); else TickEggClient(); }
}

}  // namespace coop::dev::event_drill
