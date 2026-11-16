// coop/dev/droneunhook_probe.cpp -- [dev] the host unhooks the drone's sack (VOTVCOOP_DRONEUNHOOK_PROBE=1 on both
// peers), for GitHub issue "The client's drone keeps showing its sack after the host unhooks it".
//
// THE HOST, 24 s after a client is connected: its drone (drone_C) given a sack as a delivery has it (hasSack set, the
// sack mesh shown); at 34 s the drone's own dropSack, the unhook. EVERY PEER, every 2 s to 50 s: the drone's hasSack
// and whether its sack mesh is visible, and the loose sacks in the world (`[DRONEUNHOOK]`).

#include "coop/dev/droneunhook_probe.h"

#include "coop/config/config.h"
#include "coop/net/session.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/devices/drone.h"

#include <windows.h>

#include <cstdint>

namespace coop::dev::droneunhook_probe {
namespace {

namespace R = ue_wrap::reflection;

uint64_t Now() { return ::GetTickCount64(); }

bool On() {
    static const bool v = coop::config::ReadEnv("VOTVCOOP_DRONEUNHOOK_PROBE") == "1";
    return v;
}

void* Live(const wchar_t* cls, int* count = nullptr) {
    void* first = nullptr;
    int n = 0;
    for (void* o : R::FindObjectsByClass(cls))
        if (o && R::IsLive(o) && !R::NameStartsWith(R::NameOf(o), L"Default__")) {
            if (!first) first = o;
            ++n;
        }
    if (count) *count = n;
    return first;
}

int Bool(void* o, const wchar_t* name) {
    int32_t off = -1;
    uint8_t mask = 0;
    if (!o || !R::FindBoolProperty(R::ClassOf(o), name, off, mask)) return -1;
    return (static_cast<const uint8_t*>(o)[off] & mask) ? 1 : 0;
}

int SackVisible(void* drone) {
    const int32_t off = drone ? R::FindPropertyOffset(R::ClassOf(drone), L"sack") : -1;
    void* sack = off >= 0 ? *reinterpret_cast<void**>(static_cast<char*>(drone) + off) : nullptr;
    void* sc = R::FindClass(L"SceneComponent");
    void* fn = sc ? R::FindFunction(sc, L"IsVisible") : nullptr;
    if (!sack || !fn) return -1;
    ue_wrap::ParamFrame f(fn);
    return f.valid() && ue_wrap::Call(sack, f) ? (f.Get<bool>(L"ReturnValue") ? 1 : 0) : -1;
}

uint64_t g_startAt = 0, g_nextLog = 0;
int g_step = 0;

}  // namespace

void Tick(coop::net::Session* session) {
    if (!On() || !session || !session->running() || !session->connected()) return;
    const bool host = session->role() == coop::net::Role::Host;
    if (!g_startAt) {
        g_startAt = Now();
        UE_LOGI("[DRONEUNHOOK] probe armed role=%s", host ? "host" : "client");
    }
    const uint64_t t = Now() - g_startAt;
    void* const drone = Live(L"drone_C");
    if (host && drone && g_step == 0 && t >= 24000u) {
        g_step = 1;
        int32_t off = -1;
        uint8_t mask = 0;
        if (R::FindBoolProperty(R::ClassOf(drone), L"hasSack", off, mask)) static_cast<uint8_t*>(drone)[off] |= mask;
        UE_LOGI("[DRONEUNHOOK] host: the drone given a sack (hasSack %d, mesh shown %d)", Bool(drone, L"hasSack"),
                ue_wrap::drone::SetSackVisible(drone, true) ? 1 : 0);
    }
    if (host && drone && g_step == 1 && t >= 34000u) {
        g_step = 2;
        ue_wrap::ParamFrame f(R::FindDispatchFunctionCached(R::ClassOf(drone), L"dropSack"));
        UE_LOGI("[DRONEUNHOOK] host: the drone's dropSack (ran=%d)", f.valid() && ue_wrap::Call(drone, f) ? 1 : 0);
    }
    if (t >= 20000u && t < 50000u && Now() >= g_nextLog) {
        g_nextLog = Now() + 2000;
        int sacks = 0;
        Live(L"prop_dronesack_C", &sacks);
        UE_LOGI("[DRONEUNHOOK] %s t=%llu: drone %d, hasSack %d, sack mesh visible %d, loose sacks %d",
                host ? "host" : "client", t / 1000ull, drone ? 1 : 0, Bool(drone, L"hasSack"), SackVisible(drone),
                sacks);
    }
}

}  // namespace coop::dev::droneunhook_probe
