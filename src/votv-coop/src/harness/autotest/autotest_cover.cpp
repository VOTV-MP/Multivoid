// harness/autotest/autotest_cover.cpp -- TEST-ONLY: the signal desk's back plates and a transformer station's door, as
// each of two peers sees them (cover_drill row, VOTVCOOP_COVER_DRILL; the folder is VOTVCOOP_CAN_DIR).
//
// The host changes a plate and the first station's door before the client's world is up (arm `join`). The client walks to
// the desk with the director and, at each arm, acts as a player does and judges its own copy: `unbolt`, `stale` (the same
// unbolt asked again: Already, the plate stays), `takeoff`, `putback` (the prop carried to its lid is eaten), `carried` (a
// put-back of a prop nobody carried back is refused); then at the station `door` and `reach` (a desk ask from out of reach
// is answered OutOfReach and changes nothing). After each arm the client tells the host what its copy shows and the host
// compares it with its own. A failed arm is said FAIL at once and no later arm runs; the host says `rig: VERDICT cover
// PASS` when all held. A verdict is decided at its event (the answer, or the change a red lets through); a time bound
// says INVALID only. Reds are host rows: cover_judge_off (`reach` FAILs), cover_wanted_off (`stale`), cover_hold_judge_off
// (`carried`). cover_drill=measure runs the same arms with cover_measure logging on both peers.


#include "harness/autotest.h"
#include "harness/autotest/cover_measure.h"
#include "harness/autotest/meet.h"
#include "harness/autotest/peer_signal.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/dev/director/aimed_grab.h"
#include "coop/net/protocol.h"
#include "coop/player/players_registry.h"
#include "coop/props/join_membership_sweep.h"
#include "coop/session/host_world_settle.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"
#include "coop/session/rig_verdict.h"
#include "coop/world/cover_sync.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/desk/analog_desk.h"
#include "ue_wrap/devices/generator.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_component.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>

namespace harness::autotest {
namespace {

namespace R   = ue_wrap::reflection;
namespace E   = ue_wrap::engine;
namespace PS  = harness::autotest::peer_signal;
namespace RV  = coop::rig_verdict;
namespace AD  = ue_wrap::analog_desk;
namespace GEN = ue_wrap::generator;
namespace CS  = coop::cover_sync;
namespace N   = coop::net;

constexpr const char* kTag = "cover";
constexpr int kStepBound = 120;   // the bound on one step of the other peer's; INVALID only
constexpr int kWalkBound = 240;   // the client's staged walk; INVALID only
constexpr int kArmBound = 40;     // an arm's own event; INVALID only
constexpr float kDeskStandCm = 180.f;
constexpr float kStationStandCm = 300.f;
constexpr float kFarCm = 800.f;   // the station must be this far from a plate for `reach` to ask from out of reach
const wchar_t* const kTakeOffClass = L"prop_computerBackPanel_tl_C";

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

void* Desk() { return AD::Find(); }
void* Station() { return GEN::At(0); }

// What a peer's copy shows: the desk's plates (on in bits 0-3, unbolted in bits 4-7), the live plate props of the top left
// class and the first station's door (-1 unread).
struct Snap {
    unsigned bits = 0xFFu;
    int props = 0;
    int opened = -1;
};

Snap ReadHere() {
    Snap s;
    uint8_t b = 0;
    if (AD::ReadPlates(Desk(), b)) s.bits = b;
    for (void* o : R::FindObjectsByClass(kTakeOffClass))
        if (o && R::IsLive(o) && !R::NameStartsWith(R::NameOf(o), L"Default__")) ++s.props;
    bool open = false;
    if (GEN::ReadOpened(Station(), open)) s.opened = open ? 1 : 0;
    return s;
}

// From the drill's thread or, inside an arm's act and poll, from the game thread already.
Snap Read() { return ue_wrap::game_thread::IsGameThread() ? ReadHere() : OnGameThreadValue(ReadHere); }

bool Loose(const Snap& s, int plate) { return (s.bits >> (4 + plate)) & 1u; }
bool On(const Snap& s, int plate) { return (s.bits >> plate) & 1u; }

std::string Text(const Snap& s) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "bits=%x props=%d opened=%d", s.bits, s.props, s.opened);
    return buf;
}

bool Parse(const std::string& t, Snap& s) {
    const char* const b = std::strstr(t.c_str(), "bits=");
    const char* const p = std::strstr(t.c_str(), "props=");
    const char* const o = std::strstr(t.c_str(), "opened=");
    if (!b || !p || !o) return false;
    s.bits = static_cast<unsigned>(std::strtoul(b + 5, nullptr, 16));
    s.props = static_cast<int>(std::strtol(p + 6, nullptr, 10));
    s.opened = static_cast<int>(std::strtol(o + 7, nullptr, 10));
    return true;
}

void SetLooking(void* desk, int plate) {
    const int32_t off = desk ? R::FindPropertyOffset(R::ClassOf(desk), L"lookingAtPanel") : -1;
    if (off >= 0) *reinterpret_cast<int32_t*>(static_cast<char*>(desk) + off) = plate;
}

bool Verb(void* obj, const wchar_t* fn) {
    void* const f = obj ? R::FindDispatchFunctionCached(R::ClassOf(obj), fn) : nullptr;
    return f && R::CallFunction(obj, f, nullptr);
}

// A player's E press on `obj` (actionOptionIndex, action 4, as the use key runs it).
bool PressE(void* obj) {
    void* const me = coop::players::Registry::Get().Local();
    ue_wrap::ParamFrame f(obj ? R::FindDispatchFunctionCached(R::ClassOf(obj), L"actionOptionIndex") : nullptr);
    return me && f.valid() && f.Set<void*>(L"player", me) && f.Set<uint8_t>(L"action", uint8_t{4}) && ue_wrap::Call(obj, f);
}

// What the arms compare between the two peers.
enum : unsigned { kBits = 1, kProps = 2, kOpened = 4 };

// The host's half of an arm: the client's report, compared with the host's own copy.
bool HostArm(const std::string& name, unsigned compare) {
    std::string said;
    if (!PS::AwaitText((name + "_c").c_str(), said, kStepBound)) return false;
    if (said.rfind("fail", 0) == 0) {
        RV::Say(kTag, RV::Outcome::Fail, "%s: %s", name.c_str(), said.c_str());
        return false;
    }
    Snap theirs;
    const Snap mine = Read();
    const bool parsed = Parse(said, theirs);
    const bool same = parsed && (!(compare & kBits) || theirs.bits == mine.bits) &&
                      (!(compare & kProps) || theirs.props == mine.props) &&
                      (!(compare & kOpened) || theirs.opened == mine.opened);
    UE_LOGI("cover: CHECK %s host %s -- the host's copy %s, the client's %s", name.c_str(), same ? "PASS" : "FAIL",
            Text(mine).c_str(), said.c_str());
    PS::Mark((name + "_h").c_str(), same ? "ok" : "fail");
    if (!same) {
        RV::Say(kTag, RV::Outcome::Fail, "%s: the host's copy shows %s, the client's %s", name.c_str(), Text(mine).c_str(),
                said.c_str());
        return false;
    }
    return true;
}

// What an arm's poll says: still waiting, the arm held, or it failed with `why`.
enum class Judge { Pending, Pass, Fail };

// The client's half of an arm: `act` runs on the game thread (false: a precondition is missing, INVALID), `poll` judges its
// own copy every 100 ms until it settles, then the report goes to the host, whose answer gates the next arm.
bool ClientArm(const std::string& name, const std::function<bool(std::string&)>& act,
               const std::function<Judge(std::string&)>& poll) {
    std::string why;
    if (!OnGameThreadValue([&] { return act(why); })) {
        RV::Say(kTag, RV::Outcome::Invalid, "%s: %s", name.c_str(), why.c_str());
        return false;
    }
    Judge verdict = Judge::Pending;
    if (!PS::WaitFor(name.c_str(), kArmBound, [&] {
            verdict = OnGameThreadValue([&] { return poll(why); });
            return verdict != Judge::Pending;
        }))
        return false;
    if (verdict == Judge::Fail) {
        UE_LOGI("cover: CHECK %s client FAIL -- %s", name.c_str(), why.c_str());
        PS::Mark((name + "_c").c_str(), "fail " + why);
        RV::Say(kTag, RV::Outcome::Fail, "%s: %s", name.c_str(), why.c_str());
        return false;
    }
    const Snap mine = Read();
    UE_LOGI("cover: CHECK %s client PASS -- %s", name.c_str(), Text(mine).c_str());
    PS::Mark((name + "_c").c_str(), Text(mine));
    std::string ack;
    return PS::AwaitText((name + "_h").c_str(), ack, kStepBound) && ack == "ok";
}

uint32_t Seen(N::CoverReason r) { return CS::AnswersSeen(static_cast<uint8_t>(r)); }

// The answers that refuse an ask, summed.
uint32_t Refusals() {
    return Seen(N::CoverReason::OutOfReach) + Seen(N::CoverReason::Busy) + Seen(N::CoverReason::NotHeld) +
           Seen(N::CoverReason::NoStation);
}

void* PlateProp() {
    for (void* o : R::FindObjectsByClass(kTakeOffClass))
        if (o && R::IsLive(o) && !R::NameStartsWith(R::NameOf(o), L"Default__")) return o;
    return nullptr;
}

// ---- the host ------------------------------------------------------------------------------------------------------

bool WaitHostReady() {
    return PS::WaitFor("the host's player and a settled world", 240, [&] {
        return OnGameThreadValue([] {
            return coop::players::Registry::Get().Local() && coop::host_world_settle::Settled() &&
                   ue_wrap::object_index::IsSeeded() && ue_wrap::object_index::Backlog() == 0;
        });
    });
}

// The host's own change before the client's world is up: the bottom right plate unbolted through the game's verb, the first
// station's door opened through its own openDoor.
bool HostChange() {
    const bool started = OnGameThreadValue([] {
        void* const desk = Desk();
        void* const gen = Station();
        if (!desk || !gen) return false;
        SetLooking(desk, 3);
        const bool unbolted = Verb(desk, L"unscrewPanel");
        const bool swung = GEN::CallOpenDoor(gen, true);
        return unbolted && swung;
    });
    if (!started) {
        RV::Say(kTag, RV::Outcome::Invalid, "the host has no desk or station, or its verbs did not run");
        return false;
    }
    return PS::WaitFor("the host's own plate and door change", kArmBound, [&] {
        const Snap s = Read();
        bool moving = true;
        OnGameThread([&] { GEN::ReadMoving(Station(), moving); });
        return Loose(s, 3) && s.opened == 1 && !moving;
    });
}

void RunHost(bool measure) {
    if (!WaitHostReady()) return;
    if (measure) cover_measure::Start();
    if (!PS::Await("client_begin")) return;
    if (!HostChange()) return;
    PS::Mark("host_changed");
    if (!HostArm("join", kBits | kOpened)) return;
    if (measure) cover_measure::Census();
    if (!PS::Await("client_at_desk", kWalkBound)) return;
    if (!HostArm("unbolt", kBits) || !HostArm("stale", kBits) || !HostArm("takeoff", kBits | kProps) ||
        !HostArm("putback", kBits | kProps) || !HostArm("carried", kBits | kProps))
        return;
    if (!PS::Await("client_at_station", kWalkBound)) return;
    if (!HostArm("door", kBits | kOpened) || !HostArm("reach", kBits)) return;
    std::string word;
    if (!PS::AwaitText("client_done", word, kStepBound) || word != "ok") return;
    RV::Say(kTag, RV::Outcome::Pass,
            "the join, an unbolt, a repeated unbolt, a take-off, a put-back, a refused put-back, a door and an out-of-reach "
            "ask held on both peers");
}

// ---- the client ----------------------------------------------------------------------------------------------------

bool WaitClientReady() {
    return PS::WaitFor("the client's world", 240, [&] {
        return OnGameThreadValue([] {
            return coop::players::Registry::Get().Local() && coop::net_pump::HasAnnouncedWorldReady() &&
                   coop::join_progress::CurrentPhase() == coop::join_progress::Phase::Idle &&
                   coop::net_pump::IsWorldSettled() && coop::join_membership_sweep::HasLoadTailQuiesced();
        });
    });
}

bool ClientJoin() {
    if (!PS::Await("host_changed", kStepBound)) return false;
    return ClientArm("join", [](std::string&) { return true; }, [](std::string&) {
        const Snap s = Read();
        return Loose(s, 3) && s.opened == 1 ? Judge::Pass : Judge::Pending;
    });
}

bool WalkNear(const char* what, const ue_wrap::FVector& at, float reachCm) {
    if (!meet::WalkToPoint(kTag, at, reachCm, what)) {
        RV::Say(kTag, RV::Outcome::Invalid, "the client could not walk to %s", what);
        return false;
    }
    return true;
}

bool ArmUnbolt() {
    return ClientArm("unbolt", [](std::string& why) {
        const Snap s = Read();
        if (!On(s, 0) || Loose(s, 0)) { why = "plate 0 is not on and bolted at the start"; return false; }
        SetLooking(Desk(), 0);
        return Verb(Desk(), L"unscrewPanel") || (why = "unscrewPanel did not run", false);
    }, [](std::string&) { return Loose(Read(), 0) ? Judge::Pass : Judge::Pending; });
}

// The unbolt asked again with the state it already holds, as a second client or a stale one asks it.
bool ArmStale() {
    static uint32_t already0 = 0;
    return ClientArm("stale", [](std::string& why) {
        already0 = Seen(N::CoverReason::Already);
        return CS::DrillAsk(N::kCoverOpUnscrew, N::kCoverDesk, 0, 1, 0) || (why = "the ask was not sent", false);
    }, [](std::string& why) {
        if (!Loose(Read(), 0)) { why = "the repeated unbolt bolted plate 0 again"; return Judge::Fail; }
        return Seen(N::CoverReason::Already) > already0 ? Judge::Pass : Judge::Pending;
    });
}

bool ArmTakeOff() {
    static uint32_t refused0 = 0;
    return ClientArm("takeoff", [](std::string& why) {
        refused0 = Refusals();
        SetLooking(Desk(), 0);
        return PressE(Desk()) || (why = "the E press did not run", false);
    }, [](std::string& why) {
        if (Refusals() > refused0) { why = "the host refused the take-off of plate 0"; return Judge::Fail; }
        const Snap s = Read();
        return !On(s, 0) && s.props >= 1 && CS::LastTakeOffEid() != 0 ? Judge::Pass : Judge::Pending;
    });
}

// The plate prop taken in hand as the use key grabs it, then carried onto its lid: the plate is on and the prop gone.
bool ArmPutBack() {
    static coop::director::AimedGrab* s_grab = nullptr;
    static bool s_carrying = false;
    return ClientArm("putback", [](std::string& why) {
        void* const prop = PlateProp();
        void* const me = coop::players::Registry::Get().Local();
        if (!prop || !me) { why = "no plate prop or no player"; return false; }
        delete s_grab;
        s_grab = new coop::director::AimedGrab(me, prop);
        s_carrying = false;
        return true;
    }, [](std::string& why) {
        void* const prop = PlateProp();
        if (On(Read(), 0)) { delete s_grab; s_grab = nullptr; return Judge::Pass; }
        if (!prop) { why = "the plate prop is gone and plate 0 is still off"; return Judge::Fail; }
        if (!s_carrying) {
            const coop::director::GrabState g = s_grab->Tick();
            if (g == coop::director::GrabState::Failed) {
                why = std::string("the grab failed: ") + s_grab->Why();
                delete s_grab;
                s_grab = nullptr;
                return Judge::Fail;
            }
            s_carrying = g == coop::director::GrabState::Grabbed;
            return Judge::Pending;
        }
        ue_wrap::FVector lidAt{};
        void* const lid = AD::Lid(Desk(), 0);
        if (lid && E::TryGetComponentLocation(lid, lidAt)) E::SetActorLocation(prop, lidAt);
        return Judge::Pending;
    });
}

// A put-back asked for a plate prop nobody has carried back: refused, and the plate stays off.
bool ArmCarried() {
    static uint32_t refused0 = 0;
    static int stage = 0;
    return ClientArm("carried", [](std::string& why) {
        refused0 = Refusals();
        stage = 0;
        SetLooking(Desk(), 0);
        return PressE(Desk()) || (why = "the E press did not run", false);
    }, [](std::string& why) {
        const Snap s = Read();
        if (stage == 0) {
            if (Refusals() > refused0) { why = "the host refused the second take-off of plate 0"; return Judge::Fail; }
            const uint32_t eid = CS::LastTakeOffEid();
            if (On(s, 0) || s.props < 1 || eid == 0) return Judge::Pending;
            refused0 = Refusals();
            stage = CS::DrillAsk(N::kCoverOpPutBack, N::kCoverDesk, 0, 0, eid) ? 1 : -1;
            return stage < 0 ? Judge::Fail : Judge::Pending;
        }
        if (On(s, 0)) { why = "a put-back nobody carried was applied: plate 0 is on"; return Judge::Fail; }
        return Refusals() > refused0 ? Judge::Pass : Judge::Pending;
    });
}

bool ArmDoor() {
    static int wanted = 0;
    static uint32_t refused0 = 0;
    return ClientArm("door", [](std::string& why) {
        refused0 = Refusals();
        bool open = false;
        void* const gen = Station();
        int32_t off = -1;
        uint8_t mask = 0;
        if (!GEN::ReadOpened(gen, open) || !R::FindBoolProperty(R::ClassOf(gen), L"lookAtDoor", off, mask)) {
            why = "the station's door or its look-at flag did not read";
            return false;
        }
        wanted = open ? 0 : 1;
        // The look that marks the door as the one under the trace, as getActionOptions writes it.
        uint8_t& b = static_cast<uint8_t*>(gen)[off];
        b = static_cast<uint8_t>(b | mask);
        const bool ran = PressE(gen);
        b = static_cast<uint8_t>(b & ~mask);
        return ran || (why = "the E press did not run", false);
    }, [](std::string& why) {
        if (Refusals() > refused0) { why = "the host refused the door"; return Judge::Fail; }
        bool moving = true;
        GEN::ReadMoving(Station(), moving);
        return Read().opened == wanted && !moving ? Judge::Pass : Judge::Pending;
    });
}

// A desk verb asked from the station, out of reach of the desk: answered OutOfReach, and plate 1 stays bolted.
bool ArmReach() {
    static uint32_t out0 = 0;
    return ClientArm("reach", [](std::string& why) {
        ue_wrap::FVector me{}, plate{};
        void* const mesh = AD::PlateMesh(Desk(), 1);
        if (!E::TryGetActorLocation(coop::players::Registry::Get().Local(), me) || !mesh || !E::TryGetComponentLocation(mesh, plate)) {
            why = "the player or plate 1 did not read";
            return false;
        }
        const float d = std::sqrt((me.X - plate.X) * (me.X - plate.X) + (me.Y - plate.Y) * (me.Y - plate.Y) +
                                  (me.Z - plate.Z) * (me.Z - plate.Z));
        if (d < kFarCm) { why = "the station is not far from the desk"; return false; }
        out0 = Seen(N::CoverReason::OutOfReach);
        return CS::DrillAsk(N::kCoverOpUnscrew, N::kCoverDesk, 1, 1, 0) || (why = "the ask was not sent", false);
    }, [](std::string& why) {
        if (Loose(Read(), 1)) { why = "an unbolt asked from out of reach was applied: plate 1 is unbolted"; return Judge::Fail; }
        return Seen(N::CoverReason::OutOfReach) > out0 ? Judge::Pass : Judge::Pending;
    });
}

void RunClient(bool measure) {
    PS::Mark("client_begin");
    if (!WaitClientReady()) return;
    if (measure) cover_measure::Start();
    if (!ClientJoin()) return;
    if (measure) cover_measure::Census();
    ue_wrap::FVector plateAt{}, stationAt{};
    const bool read = OnGameThreadValue([&] {
        void* const mesh = AD::PlateMesh(Desk(), 0);
        return mesh && E::TryGetComponentLocation(mesh, plateAt) && Station() && E::TryGetActorLocation(Station(), stationAt);
    });
    if (!read) {
        RV::Say(kTag, RV::Outcome::Invalid, "the client's desk or station did not read");
        return;
    }
    if (!WalkNear("the signal desk", plateAt, kDeskStandCm)) return;
    PS::Mark("client_at_desk");
    if (!ArmUnbolt() || !ArmStale() || !ArmTakeOff() || !ArmPutBack() || !ArmCarried()) return;
    if (!WalkNear("the first transformer station", stationAt, kStationStandCm)) return;
    PS::Mark("client_at_station");
    PS::Mark("client_done", ArmDoor() && ArmReach() ? "ok" : "fail");
}

}  // namespace

DWORD WINAPI CoverThread(LPVOID /*arg*/) {
    if (!PS::Begin(kTag)) return 0;
    const bool measure = coop::config::ResolveEnum(coop::config_registry::rows::cover_drill) == "measure";
    if (IsClientRole()) RunClient(measure);
    else RunHost(measure);
    return 0;
}

}  // namespace harness::autotest
