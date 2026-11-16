// coop/dev/blackfog_probe.cpp -- [dev] the host rolls a black fog (VOTVCOOP_BLACKFOG_PROBE=1 on both peers), for
// GitHub issue "Black fog (blackFog_C) the host rolls never reaches the client".
//
// THE HOST, 24 s after a client is connected: mainGamemode_C::spawnBlackFog, as the dev event menu fires it. EVERY
// PEER, every 2 s to 600 s: its live black fogs and eyer_C creatures (`[BLACKFOG]`).

#include "coop/dev/blackfog_probe.h"

#include "coop/config/config.h"
#include "coop/dev/event_trigger.h"
#include "coop/net/session.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"

#include <windows.h>

#include <cstdint>
#include <cstring>

namespace coop::dev::blackfog_probe {
namespace {

namespace R = ue_wrap::reflection;

uint64_t Now() { return ::GetTickCount64(); }

bool On() {
    static const bool v = coop::config::ReadEnv("VOTVCOOP_BLACKFOG_PROBE") == "1";
    return v;
}

int Live(const wchar_t* cls) {
    int n = 0;
    for (void* o : R::FindObjectsByClass(cls))
        if (o && R::IsLive(o) && !R::NameStartsWith(R::NameOf(o), L"Default__")) ++n;
    return n;
}

uint64_t g_startAt = 0, g_nextLog = 0;
bool g_fired = false;

}  // namespace

void Tick(coop::net::Session* session) {
    if (!On() || !session || !session->running() || !session->connected()) return;
    const bool host = session->role() == coop::net::Role::Host;
    if (!g_startAt) {
        g_startAt = Now();
        UE_LOGI("[BLACKFOG] probe armed role=%s", host ? "host" : "client");
    }
    const uint64_t t = Now() - g_startAt;
    if (host && !g_fired && t >= 24000u) {
        g_fired = true;
        bool ok = false;
        for (const auto& ev : coop::dev::event_trigger::Events())
            if (std::strcmp(ev.name, "spawnBlackFog") == 0) ok = coop::dev::event_trigger::Trigger(ev);
        UE_LOGI("[BLACKFOG] host: spawnBlackFog fired (ok=%d)", ok ? 1 : 0);
    }
    if (t >= 20000u && t < 600000u && Now() >= g_nextLog) {
        g_nextLog = Now() + 2000;
        UE_LOGI("[BLACKFOG] %s t=%llu: black fogs %d, eyers %d", host ? "host" : "client", t / 1000ull,
                Live(L"blackFog_C"), Live(L"eyer_C"));
    }
}

}  // namespace coop::dev::blackfog_probe
