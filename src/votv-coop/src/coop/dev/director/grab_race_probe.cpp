// coop/dev/director/grab_race_probe.cpp -- the two-peer GRAB race, the measure of the one-holder rule
// (coop/props/prop_hold_claim). DEV-ONLY, env-gated; see coop/dev/director/director.h.
//
// The host picks a loose keyed prop near itself and names it in a peer_signal signal (VOTVCOOP_CAN_DIR); both peers
// walk to it, aim until their own trace takes it and log ARRIVED. The host, once the client arrived, names a GO
// instant in a signal and both run the use key's grab chain at it. Each peer then watches the prop for a few
// seconds -- does its own player hold it, which slot's drive moves its copy, and how many ticks both were true --
// and logs a RESULT. The client hands its numbers to the host, which judges the two and logs VERDICT PASS or FAIL:
// one holder, and the other peer's copy driven by that holder's slot. "control" mode fires the host alone.
// VOTVCOOP_RACE_FIRE_DELAY_MS holds the grab of the peer VOTVCOOP_RACE_DELAY_ROLE names (host|client) back.

#include "coop/dev/director/aimed_grab.h"
#include "coop/dev/director/director.h"

#include "harness/autotest.h"   // IsClientRole
#include "harness/autotest/peer_signal.h"

#include "coop/config/config.h"
#include "coop/element/quiescence_drain.h"   // HasPendingWork: the join's reconcile work has drained
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/props/prop_hold_claim.h"
#include "coop/props/prop_key_index.h"
#include "coop/props/remote_prop.h"
#include "coop/session/net_pump.h"   // IsWorldSettled: a client's join has settled

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
#include <memory>
#include <string>
#include <vector>

namespace coop::director {
namespace {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace GT = ue_wrap::game_thread;
namespace PS = harness::autotest::peer_signal;

constexpr float kReachCm   = 140.f;
constexpr float kPickMinCm = 250.f;    // far enough that both players walk to it
constexpr float kPickMaxCm = 1500.f;
constexpr int   kWatchMs   = 4000;
constexpr int   kGoLeadMs  = 3000;     // the GO instant is this far ahead of the host's word; both re-aim until it
// How long the refused peer's own grab may hold: the one round trip to the host's word plus a margin.
constexpr int   kRefusedHoldMaxMs = 250;
// The share of a watch a result must hold for: the watch starts at fire time, so the first ticks race the grab.
constexpr double kMajority = 0.8;

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

// What one peer measured over its watch: the client sends it as text, the host reads its own and the client's.
struct Result {
    int held = 0, samples = 0, slot = -1, slotCount = 0, both = 0;
};

std::string Format(const Result& r) {
    return std::to_string(r.held) + " " + std::to_string(r.samples) + " " + std::to_string(r.slot) + " " +
           std::to_string(r.slotCount) + " " + std::to_string(r.both);
}

bool Parse(const std::string& text, Result& r) {
    int* const fields[] = {&r.held, &r.samples, &r.slot, &r.slotCount, &r.both};
    const char* at = text.c_str();
    for (int* f : fields) {
        char* next = nullptr;
        *f = static_cast<int>(std::strtol(at, &next, 10));
        if (next == at) return false;
        at = next;
    }
    return true;
}

bool Most(int n, int of) { return of > 0 && n >= kMajority * of; }

// The failed clause of the one-holder rule over the two watches, or nullptr on a PASS; `winner` is the holder.
const char* Judge(bool control, const Result& host, const Result& client, const char*& winner) {
    winner = "host";
    if (host.both != 0 || client.both != 0) return "both_ticks above 0";
    if (control) {
        if (!Most(host.held, host.samples)) return "the host did not hold";
        if (client.held != 0) return "the client held";
        if (client.slot != 0 || !Most(client.slotCount, client.samples))
            return "the client's copy was not driven by slot 0";
        return nullptr;
    }
    const bool hostHolds = Most(host.held, host.samples);
    const bool clientHolds = Most(client.held, client.samples);
    if (hostHolds == clientHolds) return hostHolds ? "both peers held" : "no peer held";
    const Result& other = hostHolds ? client : host;
    winner = hostHolds ? "host" : "client";
    // The refused grab is the game's own and holds until the host's word arrives, one round trip (docs/props.md's
    // known limit; measured 1 sample of 157 on loopback): a hold past kRefusedHoldMaxMs of the watch is the defect.
    // The watch runs kWatchMs, so its samples convert to time at samples / kWatchMs.
    const double allowedSamples = std::ceil(kRefusedHoldMaxMs * static_cast<double>(other.samples) / kWatchMs);
    if (other.held > allowedSamples) return "the other peer also held";
    if (other.slot != (hostHolds ? 0 : 1) || !Most(other.slotCount, other.samples))
        return "the other peer's copy was not driven by the holder's slot";
    return nullptr;
}

}  // namespace

void RunGrabRace() {
    const std::string mode = coop::config::ReadEnv("VOTVCOOP_RACE_MODE");      // "race" | "control"
    // The peer's own role, from the session it launched as: one env shared by both peers cannot name two roles.
    const std::string role = harness::autotest::IsClientRole() ? "client" : "host";
    const bool isHost = role == "host";
    const bool control = mode == "control";
    const bool shouldFire = !control || isHost;
    UE_LOGI("grab-race: START mode=%s role=%s fire=%d", mode.c_str(), role.c_str(), shouldFire ? 1 : 0);
    if (!PS::Begin("grab-race")) return;
    g_faultedStep = nullptr;

    // The world is up and the player possessed; a client also has the host's puppet (its join settled).
    auto player = std::make_shared<void*>(nullptr);
    PS::WaitFor("a possessed local player", 120, [&] {
        Step("the player", [player](std::atomic<int>& d) {
            void* p = coop::players::Registry::Get().Local();
            if (p && R::IsLive(p) && E::GetController(p)) *player = p;
            d.store(1);
        });
        return *player || g_faultedStep;
    }, false);
    if (!*player) { UE_LOGW("grab-race: VERDICT ABORT -- no possessed local player (role=%s)", role.c_str()); return; }
    if (!isHost && !PS::WaitFor("the host's puppet", 60, [&] {
            auto have = std::make_shared<bool>(false);
            Step("the puppet", [have](std::atomic<int>& d) {
                coop::RemotePlayer* pp = coop::players::Registry::Get().Puppet(0);
                *have = pp && pp->valid();
                d.store(1);
            });
            return *have;
        }))
        return;

    // The shared target: the host picks and names it; the client reads the name and finds its copy.
    auto prop = std::make_shared<void*>(nullptr);
    auto key = std::make_shared<std::wstring>();
    if (isHost) {
        Step("the pick", [player, prop, key](std::atomic<int>& d) { *prop = PickTarget(*player, *key); d.store(1); });
        if (!*prop) { UE_LOGW("grab-race: VERDICT ABORT -- no holdable keyed prop within reach of a walk"); return; }
        // Named once the client is in: a signal counts only if written after the reader's drill began, and the
        // host picks before the client has even launched (measured: the client never saw a target written early).
        if (!PS::Await("client_ready")) return;
        PS::Mark("target", std::string(key->begin(), key->end()));
    } else {
        // The join's corrections replace and move props for a while after the world is up. The target is read once
        // the join has settled (the world announced, no world change waiting) and the reconcile queue the join armed
        // is drained: both are announced state (net_pump::IsWorldSettled, quiescence_drain::HasPendingWork).
        auto settled = std::make_shared<bool>(false);
        const bool joined = PS::WaitFor("this client's join to settle", 120, [&] {
            Step("the join", [settled](std::atomic<int>& d) {
                *settled = coop::net_pump::IsWorldSettled() && !coop::element::quiescence_drain::HasPendingWork();
                d.store(1);
            });
            return *settled || g_faultedStep;
        });
        if (!joined) return;
        if (!*settled) { UE_LOGW("grab-race: VERDICT ABORT -- a game-thread step faulted (%s) before the join settled", g_faultedStep); return; }
        PS::Mark("client_ready");
        std::string k;
        if (!PS::AwaitText("target", k)) return;
        *key = std::wstring(k.begin(), k.end());
        for (int i = 0; i < 240 && !*prop && !g_faultedStep; ++i) {
            Step("the find", [prop, key](std::atomic<int>& d) { *prop = coop::prop_key_index::Find(*key); d.store(1); });
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
    PS::Mark((role + "_arrived").c_str());
    uint64_t goMs = 0;
    if (isHost) {
        // The shared instant is the race's definition, not a readiness wait: both peers fire at it.
        if (!control && !PS::Await("client_arrived")) return;
        goMs = NowUnixMs() + kGoLeadMs;
        PS::Mark("go", std::to_string(goMs));
    } else {
        std::string goText;
        if (!PS::AwaitText("go", goText)) return;
        goMs = std::strtoull(goText.c_str(), nullptr, 10);
    }
    // The aim is held through the lead: a trace that drifts off the target (the other player walking up, a
    // neighbour settling) aims again, so the chain at GO acts on the target. The GO instant itself is spun to.
    auto reaims = std::make_shared<int>(0);
    while (!g_faultedStep && goMs != 0 && NowUnixMs() + 100 < goMs) {   // the last 100 ms are spun below, unaimed
        Step("the held aim", [player, prop, grab, state, reaims](std::atomic<int>& d) {
            if (grab->Aimed() && E::ReadMainPlayerHitActor(*player) != *prop) { grab->Reaim(); ++*reaims; }
            if (!grab->Aimed()) *state = grab->Tick();
            d.store(1);
        });
        ::Sleep(16);
    }
    if (goMs == 0) {
        UE_LOGW("grab-race: VERDICT ABORT -- no GO (role=%s)", role.c_str());
        return;
    }
    while (NowUnixMs() < goMs) ::Sleep(1);
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
        // VOTVCOOP_RACE_DELAY_ROLE names the one peer it applies to (both peers share one environment).
        const int fireDelayMs = coop::config::ReadEnv("VOTVCOOP_RACE_DELAY_ROLE") == role
                                    ? std::atoi(coop::config::ReadEnv("VOTVCOOP_RACE_FIRE_DELAY_MS").c_str()) : 0;
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
    Result mine;
    mine.held = *heldSamples;
    mine.samples = *samples;
    mine.slot = topSlot;
    mine.slotCount = topCount;
    mine.both = *both;
    auto letGo = [&] {
        Step("the let go", [player, prop](std::atomic<int>& d) {
            if (*prop && R::IsLive(*prop)) E::ReleaseMainPlayerGrabIfHolding(*player, *prop);
            d.store(1);
        });
    };
    if (!isHost) {
        PS::Mark("client_result", Format(mine));
        // The host's watch may still be running and the hold outlasts it: the host marks its own result when
        // that watch is over.
        const bool hostDone = PS::Await("host_result");
        letGo();
        if (!hostDone) return;
        UE_LOGI("grab-race: DONE");
        return;
    }
    PS::Mark("host_result");
    // The host's hold outlasts the client's watch: the client's result is written when that watch is over.
    std::string clientText;
    Result theirs;
    const bool got = PS::AwaitText("client_result", clientText);
    letGo();
    if (!got) return;
    if (!Parse(clientText, theirs)) {
        UE_LOGW("grab-race: VERDICT ABORT -- the client's result is unreadable ('%s')", clientText.c_str());
        return;
    }
    const char* winner = "host";
    if (const char* failed = Judge(control, mine, theirs, winner)) {
        UE_LOGW("grab-race: VERDICT FAIL %s host=%s client=%s", failed, Format(mine).c_str(), Format(theirs).c_str());
    } else {
        UE_LOGI("grab-race: VERDICT PASS winner=%s", winner);
    }
    UE_LOGI("grab-race: DONE");   // after the verdict: a run ends on DONE
}

DWORD WINAPI GrabRaceThread(LPVOID /*arg*/) {
    RunGrabRace();
    return 0;
}

}  // namespace coop::director
