// coop/dev/roomba_probe.cpp -- [dev] the robot cleaner on the rig (VOTVCOOP_ROOMBA_PROBE=1), for coop/props/roomba_sync.
//
// HOST, 25 s after a client is connected: drops a cleaner 3 m before its player and, 3 s later, switches it on through
// the cleaner's own action. CLIENT: 12 s after its copy of a cleaner runs, presses the switch the way the use key does
// (the cleaner's actionOptionIndex, action 5). BOTH, every second: each cleaner's element id, `active` and place
// (`[RMB] at`), so the host's and the client's lines can be paired.

#include "coop/dev/roomba_probe.h"

#include "coop/config/config.h"
#include "coop/dev/drill_props.h"
#include "coop/element/registry.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/engine/engine.h"

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <vector>

namespace coop::dev::roomba_probe {
namespace {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace OI = ue_wrap::object_index;
namespace EL = coop::element;

uint64_t Now() { return ::GetTickCount64(); }

bool On() {
    static const bool on = coop::config::ReadEnv("VOTVCOOP_ROOMBA_PROBE") == "1";
    return on;
}

std::vector<void*> Roombas() {
    std::vector<void*> out;
    if (void* cls = OI::ClassByName(L"prop_roomba_C"))
        OI::ForEachInstance(cls, [](void* ctx, void* obj, int32_t) {
            if (obj && R::IsLive(obj) && !R::NameStartsWith(R::NameOf(obj), L"Default__"))
                static_cast<std::vector<void*>*>(ctx)->push_back(obj);
        }, &out);
    return out;
}

bool Active(void* roomba) {
    int32_t off = -1;
    uint8_t mask = 0;
    return R::FindBoolProperty(R::ClassOf(roomba), L"active", off, mask) && off >= 0 &&
           (*(static_cast<uint8_t*>(roomba) + off) & mask) != 0;
}

bool PressSwitch(void* roomba) {
    ue_wrap::ParamFrame f(R::FindFunction(R::ClassOf(roomba), L"actionOptionIndex"));
    if (!f.valid()) return false;
    f.Set<uint8_t>(L"action", 5);
    return ue_wrap::Call(roomba, f);
}

uint64_t g_startAt = 0, g_reportAt = 0, g_onAt = 0, g_activeSince = 0;
int g_step = 0;
void* g_mine = nullptr;

void StepHost() {
    if (g_step == 0) {
        void* const me = coop::players::Registry::Get().Local();
        void* const cls = OI::ClassByName(L"prop_roomba_C");
        ue_wrap::FVector at{};
        if (!me || !cls || !E::TryGetActorLocation(me, at)) return;
        const ue_wrap::FVector fwd = E::GetActorForwardVector(me);
        void* r = nullptr;
        const auto d = coop::dev::drill_props::DropOnFloor(me, cls, nullptr, at.X + fwd.X * 300.f, at.Y + fwd.Y * 300.f,
                                                           at.Z, 400.f, 50.f, 20.f, &r);
        UE_LOGI("[RMB] host: dropped a cleaner (%d) %p", static_cast<int>(d), r);
        g_mine = r;
        g_onAt = Now() + 3000;
        g_step = 1;
    } else if (g_step == 1 && Now() >= g_onAt) {
        g_step = 2;
        for (void* r : Roombas())
            if (r == g_mine) UE_LOGI("[RMB] host: switched its cleaner on (ran=%d)", PressSwitch(r) ? 1 : 0);
    }
}

void StepClient() {
    if (g_step != 0) return;
    for (void* r : Roombas()) {
        if (!Active(r)) continue;
        if (!g_activeSince) g_activeSince = Now();
        if (Now() - g_activeSince < 12000) return;
        g_step = 1;
        UE_LOGI("[RMB] client: pressed the running cleaner's switch as the use key does (ran=%d)", PressSwitch(r) ? 1 : 0);
        return;
    }
}

void Report() {
    if (Now() < g_reportAt) return;
    g_reportAt = Now() + 1000;
    for (void* r : Roombas()) {
        ue_wrap::FVector at{};
        if (!E::TryGetActorLocation(r, at)) continue;
        UE_LOGI("[RMB] at eid=%u active=%d pos=(%.0f,%.0f,%.0f)", static_cast<unsigned>(EL::Registry::Get().EidForActor(r)),
                Active(r) ? 1 : 0, at.X, at.Y, at.Z);
    }
}

}  // namespace

void Tick(coop::net::Session* session) {
    if (!On() || !session || !session->running() || !session->connected()) return;
    if (!g_startAt) {
        g_startAt = Now();
        UE_LOGI("[RMB] probe armed role=%s", session->role() == coop::net::Role::Host ? "host" : "client");
    }
    if (Now() - g_startAt < 25000) return;
    if (session->role() == coop::net::Role::Host) StepHost();
    else StepClient();
    Report();
}

}  // namespace coop::dev::roomba_probe
