// coop/dev/event_drill.cpp -- see coop/dev/event_drill.h.

#include "coop/dev/event_drill.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/dev/set_clock.h"
#include "coop/net/session.h"
#include "coop/world/event_cue_sync.h"
#include "coop/world/event_fire_sync.h"

#include "ue_wrap/core/log.h"

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

}  // namespace

void Tick(coop::net::Session* session) {
    static const bool s_on = coop::config::ResolveFlag(::coop::config_registry::rows::event_drill);
    if (!s_on || !session || !session->running()) return;
    if (session->role() == coop::net::Role::Host) TickHost(*session);
    else TickClient();
}

}  // namespace coop::dev::event_drill
