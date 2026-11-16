// coop/dev/vent_probe.cpp -- [dev] a player crawling in a vent (VOTVCOOP_VENT_PROBE=client|host, the crawler, on both
// peers), for Ghezra/Multivoid#41: a player's steps in a vent reached no other player.
//
// THE HOST fires the vent knocker's event at 14 s. BOTH PEERS, 20 s after a client is connected: the vent knocker's (kocker_C) and the vent crawler event's
// (ventCrawler_C) places are logged, and every vent cover's (prop_vent_C). THE CRAWLER crouches through its own Crouch,
// at 21 s goes to the knocker's place, or the first vent cover's (VOTVCOOP_VENT_AT="x,y,z" instead, when set), through
// teleportWObackrooms, and from 24 s walks 3 s each way along VOTVCOOP_VENT_DIR="x,y" and back, four times, through
// AddMovementInput, its place logged each second. The other peer's lines `puppet_footsteps:` say
// which of the crawler's steps found ground.

#include "coop/dev/vent_probe.h"

#include "coop/config/config.h"
#include "coop/dev/event_trigger.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_nav.h"

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace coop::dev::vent_probe {
namespace {

namespace R = ue_wrap::reflection;

uint64_t Now() { return ::GetTickCount64(); }

const std::string& Who() {
    static const std::string v = coop::config::ReadEnv("VOTVCOOP_VENT_PROBE");
    return v;
}

uint64_t g_startAt = 0, g_nextLog = 0;
int g_step = 0;
ue_wrap::FVector g_at{};
bool g_haveAt = false;

void* First(const wchar_t* cls) {
    for (void* o : R::FindObjectsByClass(cls))
        if (o && R::IsLive(o) && !R::NameStartsWith(R::NameOf(o), L"Default__")) return o;
    return nullptr;
}

bool PlaceMe(void* me, const ue_wrap::FVector& at) {
    void* f = R::FindFunction(R::ClassOf(me), L"teleportWObackrooms");
    ue_wrap::ParamFrame pf(f);
    ue_wrap::FTransform t{};
    t.RotW = 1.f;
    t.TX = at.X; t.TY = at.Y; t.TZ = at.Z;
    t.SX = t.SY = t.SZ = 1.f;
    return f && pf.valid() && pf.Set<ue_wrap::FTransform>(L"newTransform", t) && pf.Set<bool>(L"useRotation", false) &&
           ue_wrap::Call(me, pf);
}

}  // namespace

void Tick(coop::net::Session* session) {
    if (Who().empty() || !session || !session->running() || !session->connected()) return;
    const bool host = session->role() == coop::net::Role::Host;
    if (!g_startAt) {
        g_startAt = Now();
        UE_LOGI("[VENT] probe armed role=%s crawler=%s", host ? "host" : "client", Who().c_str());
    }
    const uint64_t t = Now() - g_startAt;
    void* const me = coop::players::Registry::Get().Local();
    if (!me) return;
    // The host fires the vent knocker's event first (as the dev event menu does): its knocker stands in a vent.
    static bool s_fired = false;
    if (host && !s_fired && t >= 14000u) {
        s_fired = true;
        bool ok = false;
        for (const auto& ev : coop::dev::event_trigger::Events())
            if (std::strcmp(ev.name, "ventKnocker") == 0) ok = coop::dev::event_trigger::Trigger(ev);
        UE_LOGI("[VENT] host: ventKnocker fired (ok=%d)", ok ? 1 : 0);
    }
    if (g_step == 0 && t >= 20000u) {
        g_step = 1;
        ue_wrap::FVector k{}, c{};
        void* const kocker = First(L"kocker_C");
        void* const crawler = First(L"ventCrawler_C");
        const bool hk = kocker && ue_wrap::engine::TryGetActorLocation(kocker, k);
        const bool hc = crawler && ue_wrap::engine::TryGetActorLocation(crawler, c);
        UE_LOGI("[VENT] knocker %d at (%.0f,%.0f,%.0f), vent crawler %d at (%.0f,%.0f,%.0f)", hk ? 1 : 0, k.X, k.Y, k.Z,
                hc ? 1 : 0, c.X, c.Y, c.Z);
        if (Who() != (host ? "host" : "client")) return;
        int n = 0;
        ue_wrap::FVector vent{};
        bool haveVent = false;
        for (void* v : R::FindObjectsByClass(L"prop_vent_C")) {
            ue_wrap::FVector vl{};
            if (!v || !R::IsLive(v) || R::NameStartsWith(R::NameOf(v), L"Default__") ||
                !ue_wrap::engine::TryGetActorLocation(v, vl))
                continue;
            const ue_wrap::FVector fw = ue_wrap::engine::GetActorForwardVector(v);
            UE_LOGI("[VENT] vent cover %d '%ls' at (%.0f,%.0f,%.0f) facing (%.2f,%.2f,%.2f)", n++,
                    R::ToString(R::NameOf(v)).c_str(), vl.X, vl.Y, vl.Z, fw.X, fw.Y, fw.Z);
            if (!haveVent) { vent = vl; haveVent = true; }
        }
        ue_wrap::FVector at = hk ? k : vent;
        const std::string want = coop::config::ReadEnv("VOTVCOOP_VENT_AT");
        if (!want.empty()) std::sscanf(want.c_str(), "%f,%f,%f", &at.X, &at.Y, &at.Z);
        // Crouched first: a standing capsule teleported into a duct is a clip the game answers with the backrooms.
        ue_wrap::ParamFrame cf(R::FindDispatchFunctionCached(R::ClassOf(me), L"Crouch"));
        const bool crouched = cf.valid() && cf.Set<bool>(L"bClientSimulation", false) && ue_wrap::Call(me, cf);
        g_at = at;
        g_haveAt = hk || haveVent || !want.empty();
        UE_LOGI("[VENT] crawler: crouch ran=%d, going to (%.0f,%.0f,%.0f)", crouched ? 1 : 0, at.X, at.Y, at.Z);
    }
    if (g_step == 1 && t >= 21000u && Who() == (host ? "host" : "client")) {
        g_step = 2;
        const bool placed = g_haveAt && PlaceMe(me, g_at);
        UE_LOGI("[VENT] crawler: placed=%d", placed ? 1 : 0);
    }
    if (Who() != (host ? "host" : "client") || g_step < 1 || t < 24000u || t >= 48000u) return;
    // The legs go along VOTVCOOP_VENT_DIR="x,y" and back, four times (+x and back when unset).
    static ue_wrap::FVector s_dir{1.f, 0.f, 0.f};
    static bool s_dirRead = false;
    if (!s_dirRead) {
        s_dirRead = true;
        const std::string d = coop::config::ReadEnv("VOTVCOOP_VENT_DIR");
        if (!d.empty()) std::sscanf(d.c_str(), "%f,%f", &s_dir.X, &s_dir.Y);
    }
    const int leg = static_cast<int>((t - 24000u) / 3000u);
    const float sign = (leg % 2) == 0 ? 1.f : -1.f;
    ue_wrap::engine::AddMovementInput(me, {s_dir.X * sign, s_dir.Y * sign, 0.f}, 1.f, true);
    if (Now() >= g_nextLog) {
        g_nextLog = Now() + 1000;
        ue_wrap::FVector at{};
        ue_wrap::engine::TryGetActorLocation(me, at);
        UE_LOGI("[VENT] crawler: leg %d at (%.0f,%.0f,%.0f)", leg, at.X, at.Y, at.Z);
    }
}

}  // namespace coop::dev::vent_probe
