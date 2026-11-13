// coop/dev/sky_tp_probe.cpp -- [dev] the earth teleport on the rig (VOTVCOOP_SKY_TP_PROBE=1), for coop/world/sky_sync.
//
// HOST, 25 s after a client is connected: fires `earthTp` through the eventer's runEvent, as the scheduler does.
// CLIENT, 25 s after it is connected: calls its own newsky_C.tp, which sky_sync refuses. BOTH, every 2 s: the sky's
// black hole (`[SKYTP] hole`), so the host's and the client's lines can be paired.

#include "coop/dev/sky_tp_probe.h"

#include "coop/config/config.h"
#include "coop/dev/event_trigger.h"
#include "coop/net/session.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/world/skysphere.h"

#include <windows.h>

#include <cstdint>
#include <cstring>

namespace coop::dev::sky_tp_probe {
namespace {

namespace R = ue_wrap::reflection;

uint64_t Now() { return ::GetTickCount64(); }

bool On() {
    static const bool on = coop::config::ReadEnv("VOTVCOOP_SKY_TP_PROBE") == "1";
    return on;
}

uint64_t g_startAt = 0, g_reportAt = 0;
bool g_fired = false;

}  // namespace

void Tick(coop::net::Session* session) {
    if (!On() || !session || !session->running() || !session->connected()) return;
    const bool host = session->role() == coop::net::Role::Host;
    if (!g_startAt) {
        g_startAt = Now();
        UE_LOGI("[SKYTP] probe armed role=%s", host ? "host" : "client");
    }
    if (Now() - g_startAt < 25000) return;
    if (!g_fired) {
        g_fired = true;
        if (host) {
            bool ok = false;
            for (const auto& ev : coop::dev::event_trigger::Events())
                if (std::strcmp(ev.name, "earthTp") == 0) ok = coop::dev::event_trigger::Trigger(ev);
            UE_LOGI("[SKYTP] host: fired earthTp through the eventer (ok=%d)", ok ? 1 : 0);
        } else {
            void* sky = ue_wrap::skysphere::Sky();
            void* fn = sky ? R::FindDispatchFunctionCached(R::ClassOf(sky), L"tp") : nullptr;
            const bool ran = fn && R::CallFunction(sky, fn, nullptr);
            UE_LOGI("[SKYTP] client: called its own newsky_C.tp (dispatched=%d)", ran ? 1 : 0);
        }
    }
    if (Now() < g_reportAt) return;
    g_reportAt = Now() + 2000;
    bool hole = false;
    if (ue_wrap::skysphere::ReadBlackHole(hole)) UE_LOGI("[SKYTP] hole=%d", hole ? 1 : 0);
}

}  // namespace coop::dev::sky_tp_probe
