// coop/dev/atv_riders_drill.cpp -- the sequencer of the ATV riders drill; see coop/dev/atv_riders_drill.h. The verdict, the waits
// and the record cursor are here, the reads and the steps are atv_riders_drill_steps.cpp.
//
// The run, in the order the peers do it (the steps keep the numbers of the design, and run 0 1 2 4 3 5 6 8 7 9: step 4's
// events come with the first block after the kerfur's seat, so its red must fail before step 3's drive, and step 8 needs the
// driver seated, which step 7's host seat takes):
//   0 the host sits in the driver's first join window and the driver's hidden kerfur mirror rides at its reveal (the values
//     on, bind_ride_off and reveal_ride_off only)
//   1 the driver sits: the body is attached on the host and the observer      2 the host's kerfur rides on every peer
//   4 the observer takes the host's re-sent blocks with the rider's word      3 the driver drives 600 cm; both stay seated, rigid
//   5 the observer leaves and rejoins mid-ride: the stamp comes first, the birth and the bind apply it
//   6 the driver leaves by the game's own key: the body and the ride end      8 the driver sits again and ragdolls: the stop restores it
//   7 the host sits: its body is seated on the clients                        9 the host's ATV is destroyed: every seat and ride ends
// Each peer decides on its own events; the first FAIL or INVALID is said at once, marked for the others, and no later step runs.

#include "coop/dev/atv_riders_drill.h"

#include "atv_gate_drill_detail.h"   // co-located private header of the gate drill: ChooseAtv and CopyResolves
#include "atv_riders_drill_detail.h"

#include "coop/dev/atv_riders_arm.h"
#include "coop/dev/atv_riders_join_hold.h"
#include "coop/player/players_registry.h"
#include "coop/session/rig_verdict.h"

#include "harness/autotest.h"   // IsClientRole

#include "ue_wrap/core/log.h"

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <string>

namespace coop::dev::atv_riders_drill {
namespace detail {

namespace RV = coop::rig_verdict;

namespace {

std::atomic<bool> g_ended{false};
Role g_me = kHost;
const char kEndMark[] = "riders_end";

// The run's end, said once by the peer that decided it and marked for the others: 'F' FAIL, 'I' INVALID, 'P' PASS.
void End(char kind, const std::string& text) {
    if (g_ended.exchange(true)) return;
    PS::Mark(kEndMark, std::string(1, kind) + " " + text);
    RV::Say(kTag, kind == 'F' ? RV::Outcome::Fail : kind == 'P' ? RV::Outcome::Pass : RV::Outcome::Invalid, "%s",
            text.c_str());
}

}  // namespace

std::wstring g_key;
uint32_t g_eid = 0;
float g_attachCm = 0.f;

const char* RoleName(Role r) { return r == kHost ? "host" : r == kObserver ? "observer" : "driver"; }
Role MyRole() { return g_me; }
int32_t Tag() { return DE::KeyTag(g_key); }

std::string Narrow(const std::wstring& w) {
    std::string s;
    for (const wchar_t c : w) s.push_back(c < 128 ? static_cast<char>(c) : '?');
    return s;
}

bool Aborted() {
    if (g_ended.load()) return true;
    std::string text;
    if (!PS::Peek(kEndMark, text)) return false;
    g_ended.store(true);
    UE_LOGI("atv-riders: the run was ended by another peer: %s", text.c_str());
    return true;
}

void Fail(int step, const char* red, const std::string& why) {
    const std::string text = "step " + std::to_string(step) + (red && *red ? std::string(" (") + red + ")" : std::string()) +
                             " on the " + RoleName(g_me) + ": " + why;
    UE_LOGI("atv-riders: %s FAIL", text.c_str());
    End('F', text);
}

void Invalid(const std::string& why) {
    UE_LOGI("atv-riders: INVALID on the %s: %s", RoleName(g_me), why.c_str());
    End('I', std::string("on the ") + RoleName(g_me) + ": " + why);
}

// A peer's number mark, read without an exception: a malformed one ends the run INVALID.
bool ParseMark(const std::string& text, const char* what, uint32_t& out) {
    char* end = nullptr;
    errno = 0;
    const unsigned long v = std::strtoul(text.c_str(), &end, 10);
    if (text.empty() || end == text.c_str() || *end != '\0' || errno == ERANGE || v > 0xFFFFFFFFul) {
        Invalid(std::string("the signal '") + what + "' carries '" + text + "', not a number");
        return false;
    }
    out = static_cast<uint32_t>(v);
    return true;
}

void StepPass(int step, const std::string& what) {
    UE_LOGI("atv-riders: step %d PASS (%s): %s", step, RoleName(g_me), what.c_str());
}

bool PollFn(const char* what, int boundS, const std::function<int()>& fn, const std::function<std::string()>& atBound) {
    for (int i = 0; i < boundS * 10; ++i) {
        if (Aborted()) return false;
        const int r = fn();
        if (r > 0) return true;
        if (r < 0) return false;
        PS::detail::SleepStep();
    }
    if (!Aborted()) {
        std::string text = std::string(what) + " not within " + std::to_string(boundS) + " s";
        if (atBound) text += "; " + atBound();
        Invalid(text);
    }
    return false;
}

// Says what this peer is about to wait for, so a stall names itself in the log at once. Every wait on a peer's mark goes through
// PollFn, which also ends the wait the moment another peer has ended the run (the end mark).
bool AwaitMark(const std::string& name, std::string& out, int boundS, const char* ctx) {
    UE_LOGI("atv-riders: %s waits for %s (%s%s%d s)", RoleName(g_me), name.c_str(), ctx ? ctx : "", ctx ? ", " : "", boundS);
    return PollFn(("the signal '" + name + "'").c_str(), boundS, [&] { return PS::Peek(name.c_str(), out) ? 1 : 0; });
}

bool Barrier(const char* name, int boundS) {
    PS::Mark((std::string(name) + "_" + RoleName(g_me)).c_str());   // the one-argument mark writes "x": Peek refuses an empty file
    std::string t;
    const std::string ctx = std::string("barrier ") + name;
    for (int r = 0; r < kRoles; ++r) {
        if (r == g_me) continue;
        if (!AwaitMark(std::string(name) + "_" + RoleName(static_cast<Role>(r)), t, boundS, ctx.c_str())) return false;
    }
    return true;
}

Cur Snap() {
    Cur s;
    for (int e = 0; e < DE::kEventCount; ++e) s.c[e] = DE::Count(static_cast<DE::Event>(e));
    return s;
}

std::vector<DE::Rec> Recs(const Cur& cur, DE::Event e) {
    std::vector<DE::Rec> v;
    uint32_t c = cur.c[e];
    DE::Rec r;
    while (DE::Next(e, c, r)) v.push_back(r);
    return v;
}

}  // namespace detail

namespace {

using namespace detail;

// The key and the facts every first-life client takes from the host: the ATV copy and the kerfur mirror are here.
bool ClientArrive(Role me) {
    std::string t;
    if (!WorldUp()) return false;
    if (me == kDriver && coop::dev::atv_riders_arm::RunsFirstJoin()) {
        // The step's verdict is said, so the hold of the join's lift ends here whatever it was: a join that never saw the
        // kerfur bind completes.
        const bool decided = DriverFirstJoin();
        coop::dev::atv_riders_join_hold::ReleaseLift();
        if (!decided) return false;
    }
    PS::Mark(me == kObserver ? "o_ready" : "c_ready");
    if (!AwaitMark("atv-key", t, kSignalS)) return false;
    g_key.assign(t.begin(), t.end());
    if (!coop::dev::atv_gate_drill::detail::CopyResolves(g_key)) return false;
    if (!AwaitMark("kerfur-eid", t, kSignalS)) return false;
    uint32_t eid = 0;
    if (!ParseMark(t, "kerfur-eid", eid)) return false;
    if (!PollFn("the host's kerfur mirror", kSignalS, [&] { return KerfurMirrorUp(eid) ? 1 : 0; })) return false;
    PS::Mark(me == kObserver ? "o_has" : "c_has");
    return true;
}

bool RunHost() {
    std::string t, why;
    if (coop::dev::atv_riders_arm::RunsFirstJoin() && !HostFirstJoin()) return false;
    if (!AwaitMark("c_ready", t, kJoinS) || !AwaitMark("o_ready", t, kJoinS)) return false;
    if (g_key.empty()) g_key = coop::dev::atv_gate_drill::detail::ChooseAtv();   // step 0 chose it when it ran
    if (g_key.empty()) {
        Invalid("the world holds no ATV a route from the host reaches");
        return false;
    }
    UE_LOGI("atv-riders: the host's ATV key='%ls'", g_key.c_str());
    PS::Mark("atv-key", Narrow(g_key));
    if (!HostFixture()) return false;
    if (!AwaitMark("c_has", t, kSignalS) || !AwaitMark("o_has", t, kSignalS) || !AwaitMark("c_at_atv", t, kWalkS)) return false;
    if (!AD::WalkToAtv(g_key, why)) {
        Invalid(why);
        return false;
    }
    PS::Mark("h_at_atv");

    Cur cur = Snap();
    if (!Barrier("arm1") || !HostRide(cur) || !HostDrive() || !HostRejoin()) return false;
    cur = Snap();
    if (!Barrier("arm6") || !LeaveHost(cur)) return false;
    cur = Snap();
    if (!Barrier("arm8") || !RagdollWatch(kHost, cur)) return false;
    cur = Snap();
    if (!Barrier("arm7") || !HostSits(cur)) return false;
    cur = Snap();
    if (!Barrier("arm9") || !HostDestroys()) return false;
    if (!AwaitMark("c_done", t, kSignalS) || !AwaitMark("o_done", t, kSignalS)) return false;
    End('P', Control() ? "control: every step decided on its events, the host's drive with its kerfur idle and off the ATV"
                       : "every step decided on its events");
    return true;
}

bool RunDriver() {
    std::string t, why;
    if (!ClientArrive(kDriver)) return false;
    if (!AD::WalkToAtv(g_key, why)) {
        Invalid(why);
        return false;
    }
    PS::Mark("c_at_atv");
    if (!AwaitMark("h_at_atv", t, kWalkS)) return false;

    Cur cur = Snap();
    if (!Barrier("arm1") || !DriverSit(cur)) return false;
    if (!AwaitMark("rider-sat", t, kRideS) || !ParseMark(t, "rider-sat", g_eid)) return false;
    if (!ClientRide(kDriver, cur) || !DriverDrive()) return false;
    cur = Snap();
    // The observer reads the driver's `arm6` mark in its second life, and a mark written before that life began reads as absent
    // (peer_signal::Peek). So the driver marks it only after the observer's step 5 mark, the first thing that life writes after
    // the rejoin: the wait spans the observer's leaving, restart, boot and join.
    if (!AwaitMark("o2_s5", t, kRejoinS + kSignalS) || !Barrier("arm6") || !DriverLeave() || !LeaveWatch(kDriver, cur)) return false;
    cur = Snap();
    if (!Barrier("arm8") || !DriverRagdoll()) return false;
    cur = Snap();
    if (!Barrier("arm7") || !WatchHostSeat(kDriver, cur)) return false;
    cur = Snap();
    if (!Barrier("arm9") || !WatchExit(kDriver, cur)) return false;
    PS::Mark("c_done");
    return true;
}

// The observer's later steps, in either life: its key and the rider's eid are known.
bool ObserverLateSteps() {
    Cur cur = Snap();
    if (!Barrier("arm6") || !LeaveWatch(kObserver, cur)) return false;
    cur = Snap();
    if (!Barrier("arm8") || !RagdollWatch(kObserver, cur)) return false;
    cur = Snap();
    if (!Barrier("arm7") || !WatchHostSeat(kObserver, cur)) return false;
    cur = Snap();
    if (!Barrier("arm9") || !WatchExit(kObserver, cur)) return false;
    PS::Mark("o_done");
    return true;
}

bool RunObserver() {
    std::string t;
    if (coop::dev::atv_riders_join_hold::RejoinedLife()) return RejoinedObserver() && ObserverLateSteps();
    if (!ClientArrive(kObserver) || !AwaitMark("h_at_atv", t, kWalkS)) return false;
    Cur cur = Snap();
    if (!Barrier("arm1") || !ObserverSeat(cur)) return false;
    if (!AwaitMark("rider-sat", t, kRideS) || !ParseMark(t, "rider-sat", g_eid)) return false;
    if (!ClientRide(kObserver, cur)) return false;
    PS::Mark("o_drive_ready");
    if (!WatchDrive()) return false;
    // The rig restarts this client at the marker; the next life judges step 5 and the rest.
    UE_LOGI("atv-riders: REJOIN the observer leaves and is restarted by the rig");
    PS::Mark("o_leaving");
    PollFn("the rig's restart of the observer", kSignalS, [] { return 0; });
    return false;
}

}  // namespace

DWORD WINAPI Thread(LPVOID /*arg*/) {
    // The drill is the host's, slot 1's (the observer, which rejoins) and slot 2's (the driver). A client learns its slot at its join.
    detail::Role role = detail::kHost;
    if (harness::autotest::IsClientRole()) {
        uint8_t slot = coop::players::kPeerIdUnknown;
        for (int i = 0; i < 2400; ++i) {
            slot = coop::players::Registry::Get().LocalPeerId();
            if (slot != coop::players::kPeerIdUnknown) break;
            ::Sleep(100);
        }
        if (slot != detail::kSlotO && slot != detail::kSlotC) {
            UE_LOGI("atv-riders: this client is slot %d: it takes no part", static_cast<int>(slot));
            return 0;
        }
        role = slot == detail::kSlotO ? detail::kObserver : detail::kDriver;
    }
    if (!PS::Begin(kTag)) {
        coop::dev::atv_riders_join_hold::ReleaseLift();   // a drill that does not run holds nothing
        return 0;
    }
    detail::g_me = role;
    UE_LOGI("atv-riders: the %s starts (%s)", detail::RoleName(role), coop::dev::atv_riders_arm::Value().c_str());
    if (role == detail::kHost) RunHost();
    else if (role == detail::kDriver) RunDriver();
    else RunObserver();
    return 0;
}

}  // namespace coop::dev::atv_riders_drill
