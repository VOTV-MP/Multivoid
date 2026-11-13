// harness/autotest/autotest_cargo_lift.cpp -- TEST-ONLY: the cargo lift (the base's elevator), as each of two peers sees
// it.
//
// The host names its lift (the peers' lists may differ in order) and the client walks to it with the director. Then the
// host presses the lift's call button (its own actionOptionIndex, action 4, as a player's use runs it) and, once it has
// stopped, the client presses it. After each press each peer waits for its OWN copy to reach the direction the press
// wanted and stop, logging `cargo-lift: STATE <step>` once a second, then tells the other its `dir` and the platform's
// height over the lift's root. `cargo-lift: CHECK` lines judge that both peers end each press with the same `dir` and the
// platform at the same height; a failed check is said FAIL at once by each peer and no later leg runs, and the host says
// `rig: VERDICT cargo-lift PASS` when both steps held (coop/session/rig_verdict.h). The red is cargo_lift_client_runs=1 on
// the client, whose press then runs on its copy alone: the client sees at the press that nothing was sent to the host and
// nothing was cancelled, tells the host, and both say FAIL there. A press cancelled and not sent is INVALID, a readiness
// gap. Every time bound says INVALID only. Not covered: the lift's own obstruction reversal (the host publishes it, and a
// moving copy turns with it), since the drill has no way to hold the platform back. Armed by the cargo_lift_drill row
// (VOTVCOOP_RUN_CARGO_LIFT); the folder is VOTVCOOP_CAN_DIR.

#include "harness/autotest.h"
#include "harness/autotest/meet.h"
#include "harness/autotest/peer_signal.h"

#include "coop/interactables/cargo_lift.h"
#include "coop/props/join_membership_sweep.h"
#include "coop/player/players_registry.h"
#include "coop/session/host_world_settle.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"
#include "coop/session/rig_verdict.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_component.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace harness::autotest {
namespace {

namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;
namespace PS = harness::autotest::peer_signal;
namespace RV = coop::rig_verdict;

constexpr const char* kTag = "cargo-lift";
constexpr int kStepBound = 120;      // the bound on one step of the other peer's
constexpr int kWalkBound = 240;      // the client's staged walk (meet.cpp: StageWalk), which took about 40 s on the rig; INVALID only
constexpr int kMoveBound = 40;       // a lift's travel is about twelve seconds; a copy that has not stopped by then is INVALID
constexpr float kReachCm = 250.f;    // the client's walk ends this near the lift's root; the host judges a press within 400

void* g_lift = nullptr;   // game thread only


template <class Fn>
void OnGameThread(Fn&& fn) {
    ue_wrap::game_thread::RunAndWait([&fn](std::atomic<int>& d) { fn(); d.store(1); });
}

template <class Fn>
auto OnGameThreadValue(Fn&& fn) {
    decltype(fn()) out{};
    OnGameThread([&] { out = fn(); });
    return out;
}

const char* Who() { return IsClientRole() ? "client" : "host"; }
const char* Other() { return IsClientRole() ? "host" : "client"; }

template <class T>
bool Field(void* obj, const wchar_t* name, T& out) {
    const int32_t off = obj && R::IsLive(obj) ? R::FindPropertyOffset(R::ClassOf(obj), name) : -1;
    if (off < 0) return false;
    std::memcpy(&out, static_cast<const uint8_t*>(obj) + off, sizeof(T));
    return true;
}

void* Obj(void* obj, const wchar_t* name) {
    void* o = nullptr;
    Field(obj, name, o);
    return o && R::IsLive(o) ? o : nullptr;
}

std::vector<void*> Lifts() {
    std::vector<void*> out;
    for (void* o : R::FindObjectsByClass(L"cargoLift_C"))
        if (o && R::IsLive(o) && !R::NameStartsWith(R::NameOf(o), L"Default__")) out.push_back(o);
    return out;
}

struct Seen {
    bool dir = false, moving = false;
    float alpha = -1.f, attachZ = 0.f, platformZ = 0.f;
};

Seen Read() {
    Seen v;
    OnGameThread([&] {
        if (!g_lift || !R::IsLive(g_lift)) return;
        Field(g_lift, L"dir", v.dir);
        Field(g_lift, L"moving", v.moving);
        Field(g_lift, L"alpha", v.alpha);
        ue_wrap::FVector root{}, at{};
        E::TryGetActorLocation(g_lift, root);
        if (void* a = Obj(g_lift, L"attach"); a && E::TryGetComponentLocation(a, at)) v.attachZ = at.Z - root.Z;
        if (void* c = Obj(g_lift, L"lift")) {
            if (void* p = Obj(c, L"ChildActor"); p && E::TryGetActorLocation(p, at)) v.platformZ = at.Z - root.Z;
        }
    });
    return v;
}

void Press() {
    OnGameThread([] {
        void* f = R::FindFunction(R::ClassOf(g_lift), L"actionOptionIndex");
        ue_wrap::ParamFrame pf(f);
        const bool ok = f && pf.valid() && pf.Set<uint8_t>(L"action", uint8_t{4}) && ue_wrap::Call(g_lift, pf);
        UE_LOGI("cargo-lift: %hs pressed the lift's button (ok=%d)", Who(), ok ? 1 : 0);
    });
}

// One press: both peers read their lift, the presser presses, then each waits for its own copy to reach the direction the
// press wanted and stop, and judges against the other's. False when the other peer's step never came (peer_signal said
// INVALID).
bool Step(bool hostPresses, const std::string& name) {
    const bool mine = hostPresses != IsClientRole();
    const Seen before = Read();
    PS::Mark((name + "_before_" + Who()).c_str());
    std::string how;
    if (mine) {
        if (!PS::Await((name + "_before_" + Other()).c_str(), kStepBound)) return false;
        const uint64_t sent0 = OnGameThreadValue([] { return coop::cargo_lift::PressesSent(); });
        const uint64_t cancelled0 = OnGameThreadValue([] { return coop::cargo_lift::PressesCancelled(); });
        Press();
        // A client's press is asked of the host: the gate sends the intent inside the call. A press that sent nothing
        // either was cancelled (the lift not indexed yet, a refused send: a readiness gap, INVALID) or ran on this copy
        // alone (the red: FAIL). Either is known at the press, and the host is told which.
        how = "sent";
        if (IsClientRole() && OnGameThreadValue([] { return coop::cargo_lift::PressesSent(); }) == sent0)
            how = OnGameThreadValue([] { return coop::cargo_lift::PressesCancelled(); }) != cancelled0 ? "cancelled"
                                                                                                         : "alone";
        PS::Mark((name + "_pressed").c_str(), how);
    } else if (!PS::AwaitText((name + "_pressed").c_str(), how, kStepBound)) {
        return false;
    }
    if (how == "cancelled") {
        RV::Say(kTag, RV::Outcome::Invalid, "the client's press was cancelled and not sent (the lift not indexed there "
                                            "yet, or the session refused the send)");
        return false;
    }
    if (how == "alone") {
        UE_LOGI("cargo-lift: CHECK %hs %hs FAIL -- the client's press ran on its own copy and was not sent to the host",
                name.c_str(), Who());
        RV::Say(kTag, RV::Outcome::Fail, "%s: the client's press ran on its own copy and was not sent to the host",
                name.c_str());
        return false;
    }
    const bool wanted = !before.dir;
    int polls = 0;
    // The lift's own end of the move is nothing a drill can bind to (its Blueprint clears `moving` inside a loop of its
    // graph), so this peer's copy is read until it has the direction and stands still. The bound says INVALID only.
    if (!PS::WaitFor("this peer's lift to stop", kMoveBound, [&] {
            const Seen v = Read();
            if (polls++ % 10 == 0)
                UE_LOGI("cargo-lift: STATE %hs %hs dir=%d moving=%d alpha=%.2f attachZ=%.0f platformZ=%.0f", name.c_str(),
                        Who(), v.dir ? 1 : 0, v.moving ? 1 : 0, v.alpha, v.attachZ, v.platformZ);
            return v.dir == wanted && !v.moving;
        }))
        return false;
    const Seen v = Read();
    char mineText[64];
    std::snprintf(mineText, sizeof(mineText), "%d %.0f", v.dir ? 1 : 0, v.platformZ);
    PS::Mark((name + "_" + Who()).c_str(), mineText);
    std::string other;
    if (!PS::AwaitText((name + "_" + Other()).c_str(), other, kStepBound)) return false;
    char* rest = nullptr;
    const int odir = static_cast<int>(std::strtol(other.c_str(), &rest, 10));
    const float oz = std::strtof(rest, nullptr);
    const bool ok = v.dir == wanted && !v.moving && odir == (v.dir ? 1 : 0) && std::fabs(oz - v.platformZ) < 20.f;
    UE_LOGI("cargo-lift: CHECK %hs %hs %hs -- this peer dir=%d (wanted %d) platform %.0f cm, the other dir=%d platform %.0f cm",
            name.c_str(), Who(), ok ? "PASS" : "FAIL", v.dir ? 1 : 0, wanted ? 1 : 0, v.platformZ, odir, oz);
    // A failed check is the verdict: each peer holds both values and says it, and no later leg runs.
    if (!ok)
        RV::Say(kTag, RV::Outcome::Fail, "%s: dir %d (wanted %d) at %.0f cm here, dir %d at %.0f cm there", name.c_str(),
                v.dir ? 1 : 0, wanted ? 1 : 0, v.platformZ, odir, oz);
    return ok;
}

bool Steps() { return Step(true, "1-host-press") && Step(false, "2-client-press"); }

void RunHost() {
    if (!PS::WaitFor("the host's player and a settled world", 240, [&] {
            bool ready = false;
            OnGameThread([&] {
                ready = coop::players::Registry::Get().Local() && coop::host_world_settle::Settled() &&
                        ue_wrap::object_index::IsSeeded() && ue_wrap::object_index::Backlog() == 0;
            });
            return ready;
        }))
        return;
    std::wstring name;
    OnGameThread([&] {
        const std::vector<void*> lifts = Lifts();
        for (void* l : lifts) {
            ue_wrap::FVector at{};
            E::TryGetActorLocation(l, at);
            UE_LOGI("cargo-lift: host found %ls at (%.0f,%.0f,%.0f)", R::ToString(R::NameOf(l)).c_str(), at.X, at.Y, at.Z);
        }
        g_lift = lifts.empty() ? nullptr : lifts.front();
        if (g_lift) name = R::ToString(R::NameOf(g_lift));
    });
    if (!g_lift) {
        RV::Say(kTag, RV::Outcome::Invalid, "the host has no cargo lift");
        return;
    }
    // A signal counts only when written after the reader's own Begin, and the client's drill begins when it boots, after
    // the host's: the lift is named only once the client has said it is here.
    if (!PS::Await("client_begin")) return;
    PS::Mark("lift", std::string(name.begin(), name.end()));
    if (!PS::Await("client_ready", kWalkBound)) return;
    if (!Steps()) return;
    std::string clientWord;
    if (!PS::AwaitText("client_done", clientWord, kStepBound) || clientWord != "ok") return;
    RV::Say(kTag, RV::Outcome::Pass, "a host's press and a client's both moved the lift on both peers to the same height");
}

void RunClient() {
    PS::Mark("client_begin");
    if (!PS::WaitFor("the client's world", 240, [&] {
            bool ready = false;
            OnGameThread([&] {
                ready = coop::players::Registry::Get().Local() && coop::net_pump::HasAnnouncedWorldReady() &&
                        coop::join_progress::CurrentPhase() == coop::join_progress::Phase::Idle &&
                        coop::net_pump::IsWorldSettled() && coop::join_membership_sweep::HasLoadTailQuiesced();
            });
            return ready;
        }))
        return;
    std::string named;
    if (!PS::AwaitText("lift", named, kStepBound)) return;
    ue_wrap::FVector at{};
    bool found = false;
    OnGameThread([&] {
        for (void* l : Lifts())
            if (R::ToString(R::NameOf(l)) == std::wstring(named.begin(), named.end())) g_lift = l;
        found = g_lift && E::TryGetActorLocation(g_lift, at);
    });
    if (!found) {
        RV::Say(kTag, RV::Outcome::Invalid, "the client has no cargo lift named '%s'", named.c_str());
        return;
    }
    if (!meet::WalkToPoint(kTag, at, kReachCm, "the cargo lift")) {
        RV::Say(kTag, RV::Outcome::Invalid, "the client could not walk to the cargo lift");
        return;
    }
    PS::Mark("client_ready");
    // The client's end of both steps, told to the host: a step that failed has said its own verdict.
    PS::Mark("client_done", Steps() ? "ok" : "fail");
}

}  // namespace

DWORD WINAPI CargoLiftThread(LPVOID /*arg*/) {
    if (!PS::Begin(kTag)) return 0;
    if (IsClientRole()) RunClient();
    else RunHost();
    return 0;
}

}  // namespace harness::autotest
