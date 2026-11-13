// coop/dev/atv_gate_drill.cpp -- the sequencer of the ATV drill; see coop/dev/atv_gate_drill.h. The acts are
// atv_gate_drill_acts.cpp and the steps atv_gate_drill_steps.cpp.

#include "coop/dev/atv_gate_drill.h"

#include "atv_gate_drill_detail.h"  // co-located private header (src tree, not include/)

#include "harness/autotest.h"   // IsClientRole
#include "harness/autotest/peer_signal.h"

#include "coop/interactables/atv_drill_events.h"
#include "coop/interactables/atv_host_sync.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/session/net_pump.h"
#include "coop/session/rig_verdict.h"

#include "ue_wrap/core/log.h"

#include <cstdint>
#include <string>

namespace coop::dev::atv_gate_drill {

namespace PS = harness::autotest::peer_signal;
namespace RV = coop::rig_verdict;

namespace detail {
namespace {

std::atomic<bool> g_aborted{false};
std::atomic<bool> g_said{false};
Role g_me = kHost;

const char kEndMark[] = "atv_end";

// The verdict of an end text ("F ..." or "I ..."), said once, by the host.
void SayEnd(const std::string& text) {
    if (g_me != kHost || g_said.exchange(true)) return;
    const bool fail = !text.empty() && text[0] == 'F';
    const std::string why = text.size() > 2 ? text.substr(2) : std::string();
    RV::Say(kTag, fail ? RV::Outcome::Fail : RV::Outcome::Invalid, "%s", why.c_str());
}

}  // namespace

void SetRole(Role r) { g_me = r; }

// HOST: whether any request was answered with its slot's world not ready (the park's event, phase 2). Read on every wait of the
// run, not only at step 16: with the park off, the first such answer is a joiner's own adoption sync, which comes at its
// first join and leaves its gate shut, so the run would otherwise end INVALID at the next barrier (f7-atv_park_off).
bool AnsweredUnparked() {
    static uint32_t cursor = 0;
    DE::Rec r;
    while (DE::Next(DE::kPark, cursor, r))
        if (r.i1 == 2) return true;
    return false;
}

bool Aborted() {
    if (g_aborted.load()) return true;
    if (g_me == kHost && AnsweredUnparked()) {
        UE_LOGI("atv-drill: step 16 FAIL: the host answered a request from a slot whose world was not ready (it was not parked)");
        End('F', "step 16 (a request at the joiner's world-ready edge is parked) on the host: answered before the slot's world was ready");
        return true;
    }
    std::string text;
    if (!PS::Peek(kEndMark, text)) return false;
    g_aborted.store(true);
    SayEnd(text);
    return true;
}

void End(char kind, const std::string& text) {
    const std::string line = std::string(1, kind) + " " + text;
    UE_LOGI("atv-drill: the run ends %s: %s", kind == 'F' ? "FAIL" : "INVALID", text.c_str());
    if (!g_aborted.exchange(true)) PS::Mark(kEndMark, line);
    SayEnd(line);
}

bool AwaitMark(const std::string& name, std::string& out, int boundS) {
    return WaitUntil(("the signal '" + name + "'").c_str(), boundS, [&] { return PS::Peek(name.c_str(), out); });
}

void SleepStep() { PS::detail::SleepStep(); }

void Fact(Ctx& c, const std::string& text) { PS::Mark((c.id + "_f_" + RoleName(c.me)).c_str(), text); }

bool AwaitFact(Ctx& c, Role from, std::string& text) {
    return AwaitMark(c.id + "_f_" + RoleName(from), text, kEventS);
}

}  // namespace detail

namespace {

using namespace detail;

constexpr int kJoinS = 480;     // the clients' boot, join and world load
constexpr int kSignalS = 240;   // a step of another peer
constexpr int kWalkS = 420;     // another peer's director walk
constexpr int kBarrierS = 30;   // this copy's apply of the host's state

std::wstring g_key;

std::string Mk(const std::string& id, const char* what, int role = -1) {
    return id + "_" + what + (role >= 0 ? std::string("_") + RoleName(static_cast<Role>(role)) : std::string());
}

char OutCode(Out o) { return o == Out::Pass ? 'P' : o == Out::Fail ? 'F' : o == Out::Invalid ? 'I' : 'S'; }

// One step as this peer sees it. False ends this peer's sequence (the run was decided, or this peer has nothing more to do).
bool RunStep(const Step& s) {
    Ctx c;
    c.me = g_me;
    c.key = g_key;
    c.id = s.id;
    const bool host = c.me == kHost;
    std::string why;
    // The barrier: the host sets the step's state (its prelude) and says its state sequence; each peer waits to have applied
    // it and reads its own vitals, so nothing the step decides on is the state before it.
    if (host) {
        if (s.prelude && !s.prelude(c, why)) {
            End('I', std::string("step ") + s.id + ": " + why);
            return false;
        }
        OnGt([&] { c.seq0 = coop::atv_host_sync::CurrentStateSeq(c.key); });
        PS::Mark(Mk(c.id, "go").c_str(), std::to_string(c.seq0));
    } else {
        std::string t;
        if (!AwaitMark(Mk(c.id, "go"), t, kSignalS)) return false;
        c.seq0 = static_cast<uint32_t>(std::stoul(t));
        const bool applied = WaitUntil("this copy applies the host's state", kBarrierS, [&] {
            uint32_t a = 0;
            OnGt([&] { a = coop::atv_host_sync::AppliedStateSeq(c.key); });
            return static_cast<int32_t>(a - c.seq0) >= 0;
        });
        if (!applied) return false;
    }
    const Vitals v = ReadVitals(c.key);
    c.fuel0 = v.fuel;
    c.health0 = v.health;
    c.modules0 = v.modules;
    for (int e = 0; e < DE::kEventCount; ++e) c.cur[e] = DE::Count(static_cast<DE::Event>(e));
    // Setup, then the actor's go.
    const bool setUp = !s.setup || s.setup(c, why);
    if (!host) PS::Mark(Mk(c.id, "rdy", c.me).c_str(), setUp ? "ok" : "fail: " + why);
    else {
        if (!setUp) {
            End('I', std::string("step ") + s.id + ": " + why);
            return false;
        }
        for (int r = kClient; r < kRoles; ++r) {
            std::string t;
            if (!AwaitMark(Mk(c.id, "rdy", r), t, kSignalS)) return false;
            if (t != "ok") {
                End('I', std::string("step ") + s.id + " on the " + RoleName(static_cast<Role>(r)) + ": " + t);
                return false;
            }
        }
        PS::Mark(Mk(c.id, "act").c_str());
    }
    std::string t;
    if (!host && !AwaitMark(Mk(c.id, "act"), t, kSignalS)) return false;
    if (c.me == s.actor) {
        const bool acted = s.act && s.act(c, why);
        PS::Mark(Mk(c.id, "done").c_str(), acted ? "ok" : "fail: " + why);
        if (!acted) {
            End('I', std::string("step ") + s.id + ": the act could not be issued: " + why);
            return false;
        }
    } else {
        if (!AwaitMark(Mk(c.id, "done"), t, kSignalS)) return false;
        if (t != "ok") return false;   // the actor said INVALID
    }
    // The decision, on this peer's events: a FAIL or an INVALID ends the run here.
    const Result r = s.decide ? s.decide(c) : Pass("nothing to decide here");
    UE_LOGI("atv-drill: step %s %s: %s (%s)", s.id, r.out == Out::Pass ? "PASS" : r.out == Out::Fail ? "FAIL" : r.out == Out::Invalid ? "INVALID" : "stopped", r.text.c_str(), RoleName(c.me));
    if (s.teardown) s.teardown(c);
    if (r.out == Out::Fail || r.out == Out::Invalid) {
        End(OutCode(r.out), std::string("step ") + s.id + " (" + s.what + ") on the " + RoleName(c.me) + ": " + r.text);
        return false;
    }
    if (r.out == Out::Stopped) return false;
    PS::Mark(Mk(c.id, "res", c.me).c_str(), "ok");
    // Every peer decided: the host says the step, the others wait for it.
    if (host) {
        for (int p = kClient; p < kRoles; ++p) {
            std::string res;
            if (!AwaitMark(Mk(c.id, "res", p), res, kEventS + kSignalS)) return false;
        }
        UE_LOGI("atv-drill: step %s PASS: %s", s.id, s.what);
        PS::Mark(Mk(c.id, "verdict").c_str(), "pass");
    } else {
        if (!AwaitMark(Mk(c.id, "verdict"), t, kEventS + kSignalS)) return false;
    }
    return true;
}

// The announced world and the host's puppet exist here.
bool WorldUp() {
    return WaitUntil("the announced world", kJoinS, [] {
               bool ready = false;
               OnGt([&] { ready = coop::net_pump::HasAnnouncedWorldReady(); });
               return ready;
           }) &&
           WaitUntil("the host's puppet", 60, [] {
               bool have = false;
               OnGt([&] {
                   coop::RemotePlayer* p = coop::players::Registry::Get().Puppet(0);
                   have = p && p->valid();
               });
               return have;
           });
}

// Every peer's way into its place: the ATV chosen by the host, its copy resolved on each client, the director walking the
// client and then the host beside it.
bool Arrive() {
    std::string t;
    if (g_me == kHost) {
        for (const char* m : {"client_ready", "observer_ready"})
            if (!AwaitMark(m, t, kJoinS)) return false;
        g_key = ChooseAtv();
        if (g_key.empty()) {
            End('I', "the world holds no ATV a route from the host reaches");
            return false;
        }
        UE_LOGI("atv-drill: the host's ATV key='%ls'", g_key.c_str());
        PS::Mark("atv_key", std::string(g_key.begin(), g_key.end()));
        for (const char* m : {"client_has_atv", "observer_has_atv"})
            if (!AwaitMark(m, t, kSignalS)) return false;
        if (!AwaitMark("client_at_atv", t, kWalkS)) return false;
        std::string why;
        if (!WalkToAtv(g_key, why)) {
            End('I', why);
            return false;
        }
        PS::Mark("host_at_atv");
        return true;
    }
    if (!WorldUp()) return false;
    PS::Mark(g_me == kClient ? "client_ready" : "observer_ready");
    if (!AwaitMark("atv_key", t, kSignalS)) return false;
    g_key.assign(t.begin(), t.end());
    if (!WaitUntil("the ATV copy under the host's key", 120, [] { return CopyResolves(g_key); })) return false;
    PS::Mark(g_me == kClient ? "client_has_atv" : "observer_has_atv");
    if (g_me == kClient) {
        std::string why;
        if (!WalkToAtv(g_key, why)) {
            End('I', why);
            return false;
        }
        PS::Mark("client_at_atv");
    }
    return AwaitMark("host_at_atv", t, kWalkS);
}

void Run() {
    if (!Arrive()) return;
    for (int i = 0; i < kStepCount; ++i)
        if (!RunStep(kSteps[i])) return;
    if (g_me == kClient) LeaveForRejoin();
    if (g_me == kHost && HostRejoin(g_key)) RV::Say(kTag, RV::Outcome::Pass, "every step decided on its events");
}

}  // namespace

DWORD WINAPI Thread(LPVOID /*arg*/) {
    // The drill is the host's, slot 1's (the client) and slot 2's (the observer). A client learns its slot at its join.
    Role role = detail::kHost;
    if (harness::autotest::IsClientRole()) {
        uint8_t slot = coop::players::kPeerIdUnknown;
        for (int i = 0; i < 2400; ++i) {
            slot = coop::players::Registry::Get().LocalPeerId();
            if (slot != coop::players::kPeerIdUnknown) break;
            ::Sleep(100);
        }
        if (slot != 1 && slot != 2) {
            UE_LOGI("atv-drill: this client is slot %d: it takes no part", static_cast<int>(slot));
            return 0;
        }
        role = slot == 1 ? detail::kClient : detail::kObserver;
    }
    if (!PS::Begin(detail::kTag)) return 0;
    detail::SetRole(role);
    if (role == detail::kClient && detail::IsRejoinedLife()) detail::RunRejoinedClient();
    else Run();
    return 0;
}

}  // namespace coop::dev::atv_gate_drill
