// coop/dev/sleepall_probe.cpp -- [dev] the host sleeps, then a client, then the client wakes, then the host
// (VOTVCOOP_SLEEPALL_PROBE=1 on both peers, VOTVCOOP_FILL_NEEDS=0; VOTVCOOP_SLEEPALL_RULE=1 on the host sets
// world.sleep_everyone on, unset leaves it off).
//
// Every phase follows a state the peer can read. HOST, 20 s after a client is connected: its nightmare chance held at
// 0 and its sleep need at 50 (the game's wake comes at 100), it goes to bed through the game's own sleep. CLIENT: 10 s
// after the host's sleeping flag, the same in the bed nearest it; it sleeps 15 s and then writes its need to 100, so the
// game's own poll wakes it. HOST: 10 s after the client's rest state is back to awake, it writes its need to 100 and
// wakes. HOST, every second from its bed to 15 s after its wake: the world's dilation, its own isSleep and the client's
// rest state (`[SLEEPALL]`). With the rule on the dilation should read 1 while the client is awake and 20 while both
// sleep; with it off, 20 for the host's whole sleep.

#include "coop/dev/sleepall_probe.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/sleep_sync.h"
#include "ue_wrap/actors/sleep.h"
#include "ue_wrap/actors/vitals.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/engine/engine.h"

#include <windows.h>

#include <cstdint>
#include <string>

namespace coop::dev::sleepall_probe {
namespace {

namespace SLP = ue_wrap::sleep;
namespace V   = ue_wrap::vitals;
using coop::net::SleepPeerState;

uint64_t Now() { return ::GetTickCount64(); }

bool On() {
    static const bool v = coop::config::ReadEnv("VOTVCOOP_SLEEPALL_PROBE") == "1";
    return v;
}

void* NearestBed() {
    ue_wrap::FVector from{};
    void* const body = coop::players::Registry::Get().Local();
    return body && ue_wrap::engine::TryGetActorLocation(body, from) ? SLP::NearestBed(from) : nullptr;
}

const char* StateName(SleepPeerState s) {
    return s == SleepPeerState::Awake ? "awake" : s == SleepPeerState::Resting ? "resting" : "dreaming";
}

uint64_t g_startAt = 0, g_nextLog = 0, g_mark = 0, g_logUntil = 0;
int g_step = 0;
bool g_hold = false;   // the sleep need held at 50 while this peer should stay asleep

}  // namespace

void Tick(coop::net::Session* session) {
    if (!On() || !session || !session->running() || !session->connected()) return;
    const bool host = session->role() == coop::net::Role::Host;
    if (!g_startAt) {
        g_startAt = Now();
        if (host) {
            const bool rule = coop::config::ReadEnv("VOTVCOOP_SLEEPALL_RULE") == "1";
            coop::config::SetValue(coop::config_registry::rows::world_sleep_everyone, rule ? "1" : "0");
        }
        UE_LOGI("[SLEEPALL] probe armed role=%s rule=%d", host ? "host" : "client",
                coop::config::ResolveFlag(coop::config_registry::rows::world_sleep_everyone) ? 1 : 0);
    }
    if (g_hold) V::Write(V::Field::Sleep, 50.f);
    const uint64_t t = Now() - g_startAt;
    if (host) {
        if (g_step == 0 && t >= 20000u) {
            g_step = 1;
            SLP::SetDreamProbability(0.f);
            V::Write(V::Field::Sleep, 50.f);
            g_hold = true;
            void* const bed = SLP::FindBed();
            UE_LOGI("[SLEEPALL] host: to bed (called=%d asleep=%d)", bed && SLP::CallSleep(bed) ? 1 : 0,
                    SLP::IsSleeping() ? 1 : 0);
            g_logUntil = ~0ull;
        }
        const SleepPeerState client = coop::sleep_sync::PeerState(1);
        if (g_step == 1 && client != SleepPeerState::Awake) g_step = 2;   // the client is asleep
        if (g_step == 2 && client == SleepPeerState::Awake) {
            g_step = 3;
            g_mark = Now();
        }
        if (g_step == 3 && Now() - g_mark >= 10000u) {
            g_step = 4;
            g_hold = false;
            V::Write(V::Field::Sleep, 100.f);
            g_logUntil = Now() + 15000u;
            UE_LOGI("[SLEEPALL] host: need to 100, the game's wake follows");
        }
        if (g_step >= 1 && Now() < g_logUntil && Now() >= g_nextLog) {
            g_nextLog = Now() + 1000;
            UE_LOGI("[SLEEPALL] host t=%llu: world dilation %.2f, host asleep %d, client %s", t / 1000ull,
                    SLP::GetGlobalTimeDilation(), SLP::IsSleeping() ? 1 : 0, StateName(client));
        }
        if (g_step == 4 && Now() >= g_logUntil) {
            g_step = 5;
            SLP::SetDreamProbability(-1.f);
            UE_LOGI("[SLEEPALL] host: DONE");
        }
        return;
    }
    // CLIENT
    if (g_step == 0 && coop::sleep_sync::HostAsleepNow()) {
        g_step = 1;
        g_mark = Now();
    }
    if (g_step == 1 && Now() - g_mark >= 10000u) {
        g_step = 2;
        V::Write(V::Field::Sleep, 50.f);
        g_hold = true;
        void* const bed = NearestBed();
        UE_LOGI("[SLEEPALL] client: to bed (called=%d asleep=%d)", bed && SLP::CallSleep(bed) ? 1 : 0,
                SLP::IsSleeping() ? 1 : 0);
        g_mark = Now();
    }
    if (g_step == 2 && Now() - g_mark >= 15000u) {
        g_step = 3;
        g_hold = false;
        V::Write(V::Field::Sleep, 100.f);
        UE_LOGI("[SLEEPALL] client: need to 100, the game's wake follows");
    }
    if (g_step == 3 && !SLP::IsSleeping()) {
        g_step = 4;
        UE_LOGI("[SLEEPALL] client: awake");
    }
}

}  // namespace coop::dev::sleepall_probe
