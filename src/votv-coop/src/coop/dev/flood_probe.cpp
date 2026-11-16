// coop/dev/flood_probe.cpp -- [dev] the basement flood (VOTVCOOP_FLOOD_PROBE=1 on both peers), for Ghezra/Multivoid#40:
// a flooded basement never reached a client, and a client's repair of a burst pipe never reached the host.
//
// THE HOST, 24 s after a client is connected: its first burst pipe (burstWaterPipe_C) bursts through the pipe's own State,
// as its addDamage does. THE CLIENT, at 60 s: takes a toolbox into its hand, stands beside that pipe and runs the pipe's own
// toolboxFix with it, as a finished repair does. EVERY PEER, every 2 s to 90 s: each pipe's damaged bit and each flood
// volume's (waterVolume_basementFlooder_C) level (`[FLOOD]`).

#include "coop/dev/flood_probe.h"

#include "coop/config/config.h"
#include "coop/dev/drill_hand.h"
#include "coop/net/session.h"
#include "coop/player/hand_item.h"
#include "coop/player/players_registry.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/engine/engine.h"

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace coop::dev::flood_probe {
namespace {

namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;

uint64_t Now() { return ::GetTickCount64(); }

bool On() {
    static const bool v = coop::config::ReadEnv("VOTVCOOP_FLOOD_PROBE") == "1";
    return v;
}

std::vector<void*> All(const wchar_t* cls) {
    std::vector<void*> out;
    for (void* o : R::FindObjectsByClass(cls))
        if (o && R::IsLive(o) && !R::NameStartsWith(R::NameOf(o), L"Default__")) out.push_back(o);
    return out;
}

int Damaged(void* pipe) {
    int32_t off = -1;
    uint8_t mask = 0;
    if (!pipe || !R::FindBoolProperty(R::ClassOf(pipe), L"damaged", off, mask)) return -1;
    return (static_cast<const uint8_t*>(pipe)[off] & mask) ? 1 : 0;
}

float Level(void* v) {
    ue_wrap::ParamFrame f(R::FindDispatchFunctionCached(R::ClassOf(v), L"getLevel"));
    return f.valid() && ue_wrap::Call(v, f) ? f.Get<float>(L"level") : -1.f;
}

// The pipe the probe works on: the first by object name, the same on every peer.
void* Pipe() {
    void* best = nullptr;
    std::wstring bestName;
    for (void* p : All(L"burstWaterPipe_C")) {
        const std::wstring n = R::ToString(R::NameOf(p));
        if (!best || n < bestName) { best = p; bestName = n; }
    }
    return best;
}

uint64_t g_startAt = 0, g_nextLog = 0;
int g_step = 0;
ue_wrap::FVector g_spot{};

}  // namespace

void Tick(coop::net::Session* session) {
    if (!On() || !session || !session->running() || !session->connected()) return;
    const bool host = session->role() == coop::net::Role::Host;
    if (!g_startAt) {
        g_startAt = Now();
        UE_LOGI("[FLOOD] probe armed role=%s", host ? "host" : "client");
    }
    const uint64_t t = Now() - g_startAt;
    void* const pipe = t >= 20000u ? Pipe() : nullptr;
    if (host && pipe && g_step == 0 && t >= 24000u) {
        g_step = 1;
        ue_wrap::ParamFrame f(R::FindDispatchFunctionCached(R::ClassOf(pipe), L"State"));
        const bool ran = f.valid() && f.Set<bool>(L"damaged", true) && ue_wrap::Call(pipe, f);
        UE_LOGI("[FLOOD] host: pipe '%ls' burst (ran=%d)", R::ToString(R::NameOf(pipe)).c_str(), ran ? 1 : 0);
        const int32_t off = R::FindPropertyOffset(R::ClassOf(pipe), L"waterVolume");
        void* const vol = off >= 0 ? *reinterpret_cast<void**>(static_cast<char*>(pipe) + off) : nullptr;
        UE_LOGI("[FLOOD] host: the pipe's waterVolume is '%ls' of class %ls", vol ? R::ToString(R::NameOf(vol)).c_str() : L"-",
                vol ? R::ClassNameOf(vol).c_str() : L"-");
    }
    void* const me = coop::players::Registry::Get().Local();
    if (!host && pipe && me && g_step == 0 && t >= 56000u) {
        g_step = 1;
        ue_wrap::FVector at{};
        bool moved = false;
        if (E::TryGetActorLocation(pipe, at)) {
            // A spot the engine's teleport takes: around the pipe, from its height down to a floor below it.
            const float dz[] = {0.f, -100.f, -200.f, -300.f};
            const float dxy[][2] = {{150.f, 0.f}, {-150.f, 0.f}, {0.f, 150.f}, {0.f, -150.f}, {250.f, 0.f}, {-250.f, 0.f}};
            for (float z : dz)
                for (const auto& d : dxy)
                    if (!moved && E::TeleportTo(me, {at.X + d[0], at.Y + d[1], at.Z + z}, {})) {
                        moved = true;
                        E::TryGetActorLocation(me, g_spot);
                    }
        }
        const bool held = coop::dev::drill_hand::TakeIntoHandNow(L"toolbox", L"prop_toolbox_C");
        UE_LOGI("[FLOOD] client: beside pipe '%ls' (moved=%d), a toolbox in the hand (%d)",
                R::ToString(R::NameOf(pipe)).c_str(), moved ? 1 : 0, held ? 1 : 0);
    }
    // Held beside the pipe until the repair: the basement floor may not hold a teleported player.
    if (!host && pipe && me && g_step == 1 && g_spot.Z != 0.f) E::SetActorLocation(me, g_spot);
    if (!host && pipe && me && g_step == 1 && t >= 60000u) {
        g_step = 2;
        void* const box = coop::hand_item::LocalHandActor();
        ue_wrap::ParamFrame f(R::FindDispatchFunctionCached(R::ClassOf(pipe), L"toolboxFix"));
        const bool ran = box && f.valid() && f.Set<void*>(L"toolbox", box) && ue_wrap::Call(pipe, f);
        ue_wrap::FVector pl{}, ml{};
        E::TryGetActorLocation(pipe, pl);
        E::TryGetActorLocation(me, ml);
        UE_LOGI("[FLOOD] client: at (%.0f,%.0f,%.0f), the pipe at (%.0f,%.0f,%.0f)", ml.X, ml.Y, ml.Z, pl.X, pl.Y, pl.Z);
        UE_LOGI("[FLOOD] client: toolboxFix on '%ls' (ran=%d, returned %d)", R::ToString(R::NameOf(pipe)).c_str(),
                ran ? 1 : 0, ran && f.Get<bool>(L"return") ? 1 : 0);
    }
    if (t >= 20000u && t < 90000u && Now() >= g_nextLog) {
        g_nextLog = Now() + 2000;
        std::wstring pipes, levels;
        for (void* p : All(L"burstWaterPipe_C")) pipes += R::ToString(R::NameOf(p)) + L"=" + std::to_wstring(Damaged(p)) + L" ";
        for (void* v : All(L"waterVolume_basementFlooder_C"))
            levels += R::ToString(R::NameOf(v)) + L"=" + std::to_wstring(static_cast<int>(Level(v))) + L" ";
        UE_LOGI("[FLOOD] %s t=%llu: pipes [%ls] levels [%ls]", host ? "host" : "client", t / 1000ull, pipes.c_str(),
                levels.c_str());
    }
}

}  // namespace coop::dev::flood_probe
