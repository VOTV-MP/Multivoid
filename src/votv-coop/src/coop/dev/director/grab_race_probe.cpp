// coop/dev/director/grab_race_probe.cpp -- the two-peer GRAB race, the measure of the one-holder rule
// (coop/props/prop_hold_claim). DEV-ONLY, env-gated; see coop/dev/director/director.h.
//
// The host picks a loose keyed prop near itself and names it in a shared file; both peers walk to it, aim until
// their own trace takes it, log ARRIVED and wait on the driver's GO instant, then run the use key's grab chain at
// it. Each peer then watches the prop for a few seconds -- does its own player hold it, which slot's drive moves
// its copy, and how many ticks both were true -- and logs a RESULT the driver compares across the two peers:
// one holder, and the other peer's copy driven by that holder's slot. "control" mode fires the host alone.
// VOTVCOOP_RACE_FIRE_DELAY_MS holds one peer's grab back.

#include "coop/dev/director/aimed_grab.h"
#include "coop/dev/director/director.h"

#include "coop/config/config.h"
#include "coop/player/players_registry.h"
#include "coop/props/prop_hold_claim.h"
#include "coop/props/prop_key_index.h"
#include "coop/props/remote_prop.h"
#include "coop/session/net_pump.h"   // HasAnnouncedWorldReady: a client's join has settled

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/actors/prop_gather.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"

#include <windows.h>

#include <atomic>
#include <cstdlib>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace coop::director {
namespace {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace GT = ue_wrap::game_thread;

constexpr float kReachCm   = 140.f;
constexpr float kPickMinCm = 250.f;    // far enough that both players walk to it
constexpr float kPickMaxCm = 1500.f;
constexpr int   kWatchMs   = 4000;

const char* g_faultedStep = nullptr;

template <class Fn>
int Step(const char* name, Fn&& body) {
    const int r = GT::RunAndWait(std::forward<Fn>(body));
    if (r == GT::kTaskFaulted && !g_faultedStep) g_faultedStep = name;
    return r;
}

uint64_t NowUnixMs() {   // the wall clock, shared by both peers on one machine
    FILETIME ft;
    ::GetSystemTimeAsFileTime(&ft);
    const uint64_t t = (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    return (t - 116444736000000000ULL) / 10000ULL;
}

// The driver's GO sentinel: a future Unix time in ms, written once both peers logged ARRIVED.
bool WaitForGo(const std::string& goFile, uint64_t timeoutMs) {
    const uint64_t start = NowUnixMs();
    uint64_t targetMs = 0;
    while (NowUnixMs() - start < timeoutMs) {
        if (targetMs == 0) {
            std::ifstream f(goFile, std::ios::binary);
            unsigned long long v = 0;
            if (f && (f >> v) && v > 0) targetMs = v;
        }
        if (targetMs != 0) {
            if (NowUnixMs() >= targetMs) return true;
            ::Sleep(1);
        } else {
            ::Sleep(20);
        }
    }
    return false;
}

// No other keyed prop this close to a target: a trace aimed at it would take a neighbour at the slightest shift.
constexpr float kIsolationCm = 60.f;

bool Isolated(void* a, const ue_wrap::FVector& p, const std::vector<coop::prop_key_index::DebugEntry>& all) {
    for (const auto& o : all) {
        if (o.actor == a || !o.actor || !R::IsLive(o.actor)) continue;
        ue_wrap::FVector q{};
        if (!E::TryGetActorLocation(o.actor, q)) continue;
        if (std::fabs(q.X - p.X) < kIsolationCm && std::fabs(q.Y - p.Y) < kIsolationCm &&
            std::fabs(q.Z - p.Z) < kIsolationCm)
            return false;
    }
    return true;
}

// HOST: the nearest loose keyed prop a hand can hold, at walking distance, with no neighbour at hand. Game thread.
void* PickTarget(void* player, std::wstring& outKey) {
    ue_wrap::FVector me{};
    if (!E::TryGetActorLocation(player, me)) return nullptr;
    std::vector<coop::prop_key_index::DebugEntry> all;
    coop::prop_key_index::DebugCollect(all);
    void* best = nullptr;
    float bestD = 1e30f;
    for (const auto& e : all) {
        void* a = e.actor;
        if (!a || !R::IsLive(a) || !coop::prop_hold_claim::IsArbitrated(a)) continue;
        if (ue_wrap::prop::IsFrozen(a) || ue_wrap::prop::IsStatic(a) || ue_wrap::prop::IsHeavy(a)) continue;
        ue_wrap::prop_gather::HoldFacts hf{};
        if (!ue_wrap::prop_gather::ReadHoldFacts(a, hf) || hf.child || !hf.canHold || hf.heavy || hf.pickupRefused)
            continue;
        ue_wrap::FVector p{};
        if (!E::TryGetActorLocation(a, p)) continue;
        const float d = std::sqrt((p.X - me.X) * (p.X - me.X) + (p.Y - me.Y) * (p.Y - me.Y) +
                                  (p.Z - me.Z) * (p.Z - me.Z));
        if (d < kPickMinCm || d > kPickMaxCm || std::fabs(p.Z - me.Z) > 150.f) continue;
        if (d < bestD && Isolated(a, p, all)) { bestD = d; best = a; outKey = e.key; }
    }
    return best;
}

}  // namespace

void RunGrabRace() {
    const std::string mode = coop::config::ReadEnv("VOTVCOOP_RACE_MODE");      // "race" | "control"
    const std::string role = coop::config::ReadEnv("VOTVCOOP_RACE_ROLE");      // "host" | "client"
    const std::string goFile = coop::config::ReadEnv("VOTVCOOP_RACE_GO_FILE");
    const std::string targetFile = coop::config::ReadEnv("VOTVCOOP_RACE_TARGET_FILE");
    const bool isHost = role == "host";
    const bool shouldFire = mode != "control" || isHost;
    UE_LOGI("grab-race: START mode=%s role=%s fire=%d", mode.c_str(), role.c_str(), shouldFire ? 1 : 0);
    if (targetFile.empty() || goFile.empty()) {
        UE_LOGW("grab-race: VERDICT ABORT -- VOTVCOOP_RACE_GO_FILE and VOTVCOOP_RACE_TARGET_FILE must be set");
        return;
    }
    ::Sleep(15000);   // the world, and on a client the join, settle
    g_faultedStep = nullptr;

    auto player = std::make_shared<void*>(nullptr);
    for (int i = 0; i < 90 && !*player && !g_faultedStep; ++i) {
        Step("the player", [player](std::atomic<int>& d) {
            void* p = coop::players::Registry::Get().Local();
            if (p && R::IsLive(p) && E::GetController(p)) *player = p;
            d.store(1);
        });
        if (!*player) ::Sleep(1000);
    }
    if (!*player) { UE_LOGW("grab-race: VERDICT ABORT -- no possessed local player (role=%s)", role.c_str()); return; }

    // The shared target: the host picks and names it; the client reads the name and finds its copy.
    auto prop = std::make_shared<void*>(nullptr);
    auto key = std::make_shared<std::wstring>();
    if (isHost) {
        Step("the pick", [player, prop, key](std::atomic<int>& d) { *prop = PickTarget(*player, *key); d.store(1); });
        if (!*prop) { UE_LOGW("grab-race: VERDICT ABORT -- no holdable keyed prop within reach of a walk"); return; }
        std::ofstream(targetFile, std::ios::binary | std::ios::trunc) << std::string(key->begin(), key->end());
    } else {
        // The join's corrections replace and move props for a while after the world is up: the target is read once
        // this client announced its world ready, and some seconds after.
        auto ready = std::make_shared<bool>(false);
        for (int i = 0; i < 240 && !*ready && !g_faultedStep; ++i) {
            Step("the join", [ready](std::atomic<int>& d) { *ready = coop::net_pump::HasAnnouncedWorldReady(); d.store(1); });
            if (!*ready) ::Sleep(500);
        }
        ::Sleep(10000);
        for (int i = 0; i < 240 && !*prop && !g_faultedStep; ++i) {
            std::string k;
            { std::ifstream f(targetFile, std::ios::binary); if (f) std::getline(f, k); }
            if (!k.empty()) {
                *key = std::wstring(k.begin(), k.end());
                Step("the find", [prop, key](std::atomic<int>& d) { *prop = coop::prop_key_index::Find(*key); d.store(1); });
            }
            if (!*prop) ::Sleep(500);
        }
        if (!*prop) { UE_LOGW("grab-race: VERDICT ABORT -- the host's target never resolved here"); return; }
    }
    ue_wrap::FVector targetPos{};
    Step("the target's place", [prop, &targetPos](std::atomic<int>& d) {
        E::TryGetActorLocation(*prop, targetPos);
        d.store(1);
    });
    UE_LOGI("grab-race: TARGET role=%s key=%ls at (%.0f,%.0f,%.0f)", role.c_str(), key->c_str(), targetPos.X,
            targetPos.Y, targetPos.Z);

    DirectorGoal goal;
    goal.reachCm = kReachCm;
    goal.targetActor = *prop;
    goal.targetPos = targetPos;
    {
        ControlManager mgr;
        AddWalkToProcesses(mgr, goal);
        mgr.Run(goal, /*maxSeconds=*/180);
    }
    if (!goal.reached) {
        UE_LOGW("grab-race: VERDICT ABORT -- did not reach the target (role=%s reason=%s)", role.c_str(),
                goal.failReason);
        return;
    }

    // Aim until the trace takes the prop, and hold there.
    auto grab = std::make_shared<AimedGrab>(*player, *prop);
    grab->HoldAtAim();
    auto state = std::make_shared<GrabState>(GrabState::Working);
    for (int i = 0; i < 1200 && !grab->Aimed() && !g_faultedStep; ++i) {
        Step("the aim", [player, prop, key, grab, state](std::atomic<int>& d) {
            // A prop the join or the game replaced is the same prop under the same key: aim at the new actor.
            if (!R::IsLive(*prop)) {
                if (void* now = coop::prop_key_index::Find(*key)) {
                    *prop = now;
                    *grab = AimedGrab(*player, now);
                    grab->HoldAtAim();
                }
            }
            *state = grab->Tick();
            d.store(1);
        });
        if (*state == GrabState::Failed) break;
        ::Sleep(16);
    }
    if (!grab->Aimed()) {
        UE_LOGW("grab-race: VERDICT ABORT -- the aim never took the prop (role=%s: %s)", role.c_str(), grab->Why());
        return;
    }
    UE_LOGI("grab-race: ARRIVED role=%s -- aimed, waiting for GO", role.c_str());
    // The aim is held through the wait: a trace that drifts off the target (the other player walking up, a
    // neighbour settling) aims again, so the chain at GO acts on the target. The GO instant itself is spun to.
    const uint64_t waitStart = NowUnixMs();
    uint64_t goMs = 0;
    auto reaims = std::make_shared<int>(0);
    while (!g_faultedStep && NowUnixMs() - waitStart < 120000) {
        if (goMs == 0) {
            std::ifstream f(goFile, std::ios::binary);
            unsigned long long v = 0;
            if (f && (f >> v) && v > 0) goMs = v;
        }
        if (goMs != 0 && NowUnixMs() + 100 >= goMs) break;   // the last 100 ms are spun below, unaimed
        Step("the held aim", [player, prop, grab, state, reaims](std::atomic<int>& d) {
            if (grab->Aimed() && E::ReadMainPlayerHitActor(*player) != *prop) { grab->Reaim(); ++*reaims; }
            if (!grab->Aimed()) *state = grab->Tick();
            d.store(1);
        });
        ::Sleep(16);
    }
    if (goMs == 0 || !WaitForGo(goFile, 5000)) {
        UE_LOGW("grab-race: VERDICT ABORT -- no GO (role=%s)", role.c_str());
        return;
    }
    UE_LOGI("grab-race: GO role=%s -- %d re-aim(s) while waiting", role.c_str(), *reaims);

    // What this player's own trace takes at the GO instant: the grab chain acts on it, not on the prop.
    Step("the trace at GO", [player, prop](std::atomic<int>& d) {
        void* const hit = E::ReadMainPlayerHitActor(*player);
        UE_LOGI("grab-race: AT GO the trace takes %p (%ls)%s", hit, hit ? R::ClassNameOf(hit).c_str() : L"nothing",
                hit == *prop ? " -- the target" : " -- NOT the target");
        d.store(1);
    });
    if (shouldFire) {
        // VOTVCOOP_RACE_FIRE_DELAY_MS holds this peer's grab back, so the other peer's hold arrives first.
        const int fireDelayMs = std::atoi(coop::config::ReadEnv("VOTVCOOP_RACE_FIRE_DELAY_MS").c_str());
        if (fireDelayMs > 0) ::Sleep(static_cast<DWORD>(fireDelayMs));
        grab->Fire();
        for (int i = 0; i < 60 && *state == GrabState::Working && !g_faultedStep; ++i) {
            Step("the grab", [grab, state](std::atomic<int>& d) { *state = grab->Tick(); d.store(1); });
            ::Sleep(16);
        }
        UE_LOGI("grab-race: FIRED role=%s at %llu -- local grab %s", role.c_str(),
                static_cast<unsigned long long>(NowUnixMs()),
                *state == GrabState::Grabbed ? "took" : (*state == GrabState::Failed ? grab->Why() : "pending"));
    }

    // The watch: who holds this copy now.
    auto holds = std::make_shared<int>(0);
    auto driver = std::make_shared<int>(-1);
    auto both = std::make_shared<int>(0);
    auto samples = std::make_shared<int>(0);
    auto heldSamples = std::make_shared<int>(0);
    auto drivenBy = std::make_shared<std::vector<int>>(coop::players::kMaxPeers, 0);   // samples per driving slot
    const uint64_t end = NowUnixMs() + kWatchMs;
    while (NowUnixMs() < end && !g_faultedStep) {
        Step("the watch", [player, prop, holds, driver, both, samples, heldSamples, drivenBy](std::atomic<int>& d) {
            void* const a = *prop;
            const bool mine = a && R::IsLive(a) && Grabbing(*player) == a;
            const int slot = (a && R::IsLive(a)) ? coop::remote_prop::DrivingSlotOf(a) : -1;
            *holds = mine ? 1 : 0;
            *driver = slot;
            if (mine && slot >= 0) ++*both;
            if (mine) ++*heldSamples;
            if (slot >= 0 && slot < static_cast<int>(drivenBy->size())) ++(*drivenBy)[slot];
            ++*samples;
            d.store(1);
        });
        ::Sleep(16);
    }
    // Over the watch, not at its last sample: the other peer's watch can end, and its release land, first.
    int topSlot = -1, topCount = 0;
    for (int s = 0; s < static_cast<int>(drivenBy->size()); ++s)
        if ((*drivenBy)[s] > topCount) { topCount = (*drivenBy)[s]; topSlot = s; }
    UE_LOGI("grab-race: RESULT role=%s mode=%s held=%d/%d driven_by_slot=%d (%d) both_ticks=%d | at the end: "
            "holds=%d driver_slot=%d", role.c_str(), mode.c_str(), *heldSamples, *samples, topSlot, topCount, *both,
            *holds, *driver);
    ::Sleep(2000);   // the other peer's watch may still be running: the hold outlasts it
    Step("the let go", [player, prop](std::atomic<int>& d) {
        if (*prop && R::IsLive(*prop)) E::ReleaseMainPlayerGrabIfHolding(*player, *prop);
        d.store(1);
    });
}

DWORD WINAPI GrabRaceThread(LPVOID /*arg*/) {
    RunGrabRace();
    return 0;
}

}  // namespace coop::director
