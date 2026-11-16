// coop/dev/droneunhook_probe.cpp -- [dev] the host unhooks the drone's sack (VOTVCOOP_DRONEUNHOOK_PROBE=1 on both
// peers), for GitHub issue "The client's drone keeps showing its sack after the host unhooks it".
//
// THE HOST, 24 s after a client is connected: its drone (drone_C) given a sack as a delivery has it (hasSack set, the
// sack mesh shown); at 34 s the drone's own dropSack, the unhook. EVERY PEER, every 2 s to 50 s: the drone's hasSack
// and whether its sack mesh is visible, and the loose sacks in the world (`[DRONEUNHOOK]`). VOTVCOOP_DRONEUNHOOK_REHOOK=host
// or client: at 44 s that peer puts the sack back, its copy of the loose sack set into the drone's sack box (`sackPhys`),
// whose overlap runs the drone's own putSackOn; the log runs to 70 s. VOTVCOOP_DRONEUNHOOK_REHOOK=hold: the host first
// brings its drone down 4 m before its player (20 s; with =client too), and at 40 s the client takes the dropped sack into its hand through
// its own `Hold Object` and steps until the held sack is in the drone's sack box, as a player carrying it there would.

#include "coop/dev/droneunhook_probe.h"

#include "coop/config/config.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/devices/drone.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_component.h"

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <string>

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

// Who puts the sack back: 'h', 'c' or none.
char Rehook() {
    static const std::string v = coop::config::ReadEnv("VOTVCOOP_DRONEUNHOOK_REHOOK");
    return v == "host" ? 'h' : v == "client" ? 'c' : v == "hold" ? 'o' : 0;
}

void* SackBox(void* drone) {
    const int32_t off = drone ? R::FindPropertyOffset(R::ClassOf(drone), L"sackPhys") : -1;
    return off >= 0 ? *reinterpret_cast<void**>(static_cast<char*>(drone) + off) : nullptr;
}

uint64_t g_startAt = 0, g_nextLog = 0;
int g_step = 0;
bool g_rehooked = false, g_droneMoved = false, g_held = false;

float Dist(const ue_wrap::FVector& a, const ue_wrap::FVector& b) {
    return std::sqrt((a.X - b.X) * (a.X - b.X) + (a.Y - b.Y) * (a.Y - b.Y) + (a.Z - b.Z) * (a.Z - b.Z));
}

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
    void* const me = coop::players::Registry::Get().Local();
    if ((Rehook() == 'o' || Rehook() == 'c') && host && drone && me && !g_droneMoved && t >= 20000u) {
        g_droneMoved = true;
        ue_wrap::FVector at{};
        const ue_wrap::FVector fwd = ue_wrap::engine::GetActorForwardVector(me);
        const bool ok = ue_wrap::engine::TryGetActorLocation(me, at) &&
                        ue_wrap::engine::SetActorLocation(drone, {at.X + fwd.X * 400.f, at.Y + fwd.Y * 400.f, at.Z + 150.f});
        UE_LOGI("[DRONEUNHOOK] host: the drone brought down before the player (ok=%d)", ok ? 1 : 0);
    }
    if (Rehook() == 'o' && !host && drone && me && t >= 40000u && t < 58000u) {
        void* const sack = Live(L"prop_dronesack_C");
        void* const box = SackBox(drone);
        if (!g_held && sack) {
            g_held = true;
            ue_wrap::ParamFrame f(R::FindDispatchFunctionCached(R::ClassOf(me), L"Hold Object"));
            const bool ran = f.valid() && f.Set<bool>(L"useHold", true) && f.Set<void*>(L"manual", sack) &&
                             ue_wrap::Call(me, f);
            UE_LOGI("[DRONEUNHOOK] client: took the loose sack into its hand (ran=%d)", ran ? 1 : 0);
        } else if (g_held && sack && box) {
            // Step the player by the held sack's miss, a little each frame, so the hand carries it into the box.
            ue_wrap::FVector s{}, b{}, p{};
            if (ue_wrap::engine::TryGetActorLocation(sack, s) && ue_wrap::engine::TryGetComponentLocation(box, b) &&
                ue_wrap::engine::TryGetActorLocation(me, p) && Dist(s, b) > 10.f)
                ue_wrap::engine::SetActorLocation(me, {p.X + (b.X - s.X) * 0.2f, p.Y + (b.Y - s.Y) * 0.2f,
                                                       p.Z + (b.Z - s.Z) * 0.2f});
        }
    }
    if (drone && !g_rehooked && t >= 44000u && Rehook() == (host ? 'h' : 'c')) {
        g_rehooked = true;
        void* const sack = Live(L"prop_dronesack_C");
        void* const box = SackBox(drone);
        ue_wrap::FVector at{};
        const bool moved = sack && box && ue_wrap::engine::TryGetComponentLocation(box, at) &&
                           ue_wrap::engine::SetActorLocation(sack, at);
        UE_LOGI("[DRONEUNHOOK] %s: the loose sack set into the drone's sack box (moved=%d, sackInteract %d)",
                host ? "host" : "client", moved ? 1 : 0, Bool(drone, L"sackInteract"));
    }
    if (t >= 20000u && t < (Rehook() ? 70000u : 50000u) && Now() >= g_nextLog) {
        g_nextLog = Now() + 2000;
        int sacks = 0;
        Live(L"prop_dronesack_C", &sacks);
        void* const sack = Live(L"prop_dronesack_C");
        ue_wrap::FVector s{}, b{};
        const float miss = sack && SackBox(drone) && ue_wrap::engine::TryGetActorLocation(sack, s) &&
                                   ue_wrap::engine::TryGetComponentLocation(SackBox(drone), b)
                               ? Dist(s, b)
                               : -1.f;
        UE_LOGI("[DRONEUNHOOK] %s t=%llu: drone %d, hasSack %d, sack mesh visible %d, loose sacks %d, sack to box %.0f",
                host ? "host" : "client", t / 1000ull, drone ? 1 : 0, Bool(drone, L"hasSack"), SackVisible(drone),
                sacks, miss);
    }
}

}  // namespace coop::dev::droneunhook_probe
