// coop/dev/lift_probe.cpp -- [dev] a player standing on the cargo lift while it moves (VOTVCOOP_LIFT_PROBE=1 on both
// peers), for Ghezra/Multivoid#38: with a client on the platform the lift would not go up or down.
//
// BOTH PEERS, from 20 s after a client is connected: the first cargo lift's `dir`, `moving` and its platform's height
// (the `comp` component) are logged once a second. THE CLIENT at 20 s stands on the platform (teleportWObackrooms, 120 cm
// over it). THE HOST presses the lift at 26 s and again at 46 s, as its call button does. The lane's own lines
// (`cargo_lift`) say any reversal the lift made of itself.

#include "coop/dev/lift_probe.h"

#include "coop/config/config.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/devices/cargo_lift.h"
#include "ue_wrap/engine/engine_component.h"

#include <windows.h>

#include <cstdint>
#include <string>

namespace coop::dev::lift_probe {
namespace {

namespace R = ue_wrap::reflection;
namespace L = ue_wrap::cargo_lift;

uint64_t Now() { return ::GetTickCount64(); }

bool On() {
    static const bool v = !coop::config::ReadEnv("VOTVCOOP_LIFT_PROBE").empty();
    return v;
}

uint64_t g_startAt = 0, g_nextLog = 0;
int g_step = 0;

void* Lift() {
    for (void* o : R::FindObjectsByClass(L"cargoLift_C"))
        if (o && R::IsLive(o) && !R::NameStartsWith(R::NameOf(o), L"Default__")) return o;
    return nullptr;
}

void* Platform(void* lift) {
    const int32_t off = lift ? R::FindPropertyOffset(R::ClassOf(lift), L"comp") : -1;
    void* const c = off >= 0 ? *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(lift) + off) : nullptr;
    return c && R::IsLive(c) ? c : nullptr;
}

}  // namespace

void Tick(coop::net::Session* session) {
    if (!On() || !session || !session->running() || !session->connected()) return;
    const bool host = session->role() == coop::net::Role::Host;
    if (!g_startAt) {
        g_startAt = Now();
        UE_LOGI("[LIFT] probe armed role=%s", host ? "host" : "client");
    }
    const uint64_t t = Now() - g_startAt;
    if (t < 20000u) return;
    void* const lift = Lift();
    void* const plat = Platform(lift);
    ue_wrap::FVector pl{};
    if (plat) ue_wrap::engine::TryGetComponentLocation(plat, pl);
    if (Now() >= g_nextLog) {
        g_nextLog = Now() + 1000;
        bool up = false, moving = false;
        L::ReadDir(lift, up);
        L::ReadMoving(lift, moving);
        UE_LOGI("[LIFT] %s: dir=%s moving=%d platform z=%.0f", host ? "host" : "client", up ? "up" : "down", moving ? 1 : 0,
                pl.Z);
    }
    void* const me = coop::players::Registry::Get().Local();
    if (!host && g_step == 0 && plat && me) {
        g_step = 1;
        void* f = R::FindFunction(R::ClassOf(me), L"teleportWObackrooms");
        ue_wrap::ParamFrame pf(f);
        ue_wrap::FTransform tr{};
        tr.RotW = 1.f;
        tr.TX = pl.X; tr.TY = pl.Y; tr.TZ = pl.Z + 120.f;
        tr.SX = tr.SY = tr.SZ = 1.f;
        const bool ok = f && pf.valid() && pf.Set<ue_wrap::FTransform>(L"newTransform", tr) &&
                        pf.Set<bool>(L"useRotation", false) && ue_wrap::Call(me, pf);
        UE_LOGI("[LIFT] client: stands on the platform at (%.0f,%.0f,%.0f) (ok=%d)", pl.X, pl.Y, pl.Z + 120.f, ok ? 1 : 0);
    }
    if (host && ((g_step == 0 && t >= 26000u) || (g_step == 1 && t >= 46000u))) {
        ++g_step;
        UE_LOGI("[LIFT] host: press %d ran=%d", g_step, L::Press(lift) ? 1 : 0);
    }
}

}  // namespace coop::dev::lift_probe
