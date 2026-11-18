// harness/autotest/autotest_cover.cpp -- TEST-ONLY: the signal desk's back plates and a transformer station's door, as
// each of two peers sees them (cover_drill row, VOTVCOOP_COVER_DRILL; the folder is VOTVCOOP_CAN_DIR).
//
// The host changes a plate and the first station's door before the client's world is up (arm `join`). The client walks to
// the desk with the director and, at each arm, acts as a player does and judges its own copy: `unbolt`, `stale` (the same
// unbolt asked again: Already, the plate stays), `takeoff`, `carried` (a put-back of a prop nobody carried back is refused),
// `putback` (the prop carried to its lid is eaten); then, placed by the game's teleport beside the station, `door` and
// `reach` (a desk ask from out of reach
// is answered OutOfReach and changes nothing). After each arm the client tells the host what its copy shows and the host
// compares it with its own. A failed arm is said FAIL at once and no later arm runs; the host says `rig: VERDICT cover
// PASS` when all held. A verdict is decided at its event (the answer, or the change a red lets through); a time bound
// says INVALID only. Reds are host rows: cover_judge_off (`reach` FAILs), cover_wanted_off (`stale`), cover_hold_judge_off
// (`carried`), cover_announce_off (`unbolt`). cover_drill=measure runs the same arms with cover_measure logging on both peers.
// Not measured: the powertool's sound after a cancelled client unscrew (the arms call unscrewPanel directly, so the unbolt's
// sound timer never runs), and the host judge's hold test (NotHeld), which needs the host's own player to hold the prop.


#include "harness/autotest.h"
#include "harness/autotest/cover_measure.h"
#include "harness/autotest/meet.h"
#include "harness/autotest/peer_signal.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/dev/director/aimed_grab.h"
#include "coop/net/protocol.h"
#include "coop/element/registry.h"
#include "coop/player/players_registry.h"
#include "coop/props/prop_drive_stream.h"
#include "coop/session/teleport_client.h"
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
#include "ue_wrap/engine/engine_nav.h"
#include "ue_wrap/engine/engine_mainplayer.h"
#include "ue_wrap/engine/engine_pawn.h"

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
constexpr int kStepBound = 240;   // the bound on one step of the other peer's (the put-back's grab and carry are one); INVALID only
constexpr int kWalkBound = 240;   // the client's staged walk; INVALID only
constexpr int kStagedWalkBound = 120;  // the station leg after the game's own teleport: a few metres of walk, and its teleport; INVALID only
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
    if (said.rfind("invalid", 0) == 0) {
        RV::Say(kTag, RV::Outcome::Invalid, "%s: %s", name.c_str(), said.c_str());
        return false;
    }
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
enum class Judge { Pending, Pass, Fail, Invalid };

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
            verdict = OnGameThreadValue([&] {
                // At every arm: a client authors no plate prop, so a destroy of one it broadcasts is the desk eating a host
                // prop's mirror on this copy.
                if (CS::ClientPlateDestroys() != 0) {
                    why = "this client broadcast the destroy of a plate prop it did not author";
                    return Judge::Fail;
                }
                return poll(why);
            });
            return verdict != Judge::Pending;
        }))
        return false;
    if (verdict == Judge::Invalid) {
        PS::Mark((name + "_c").c_str(), "invalid " + why);
        RV::Say(kTag, RV::Outcome::Invalid, "%s: %s", name.c_str(), why.c_str());
        return false;
    }
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
        !HostArm("carried", kBits | kProps) || !HostArm("putback", kBits | kProps))
        return;
    if (!PS::Await("client_at_station", kStagedWalkBound)) return;
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

// The client's own staging for a place far from the desk: the game's own teleport onto a NavMesh point beside it (never a
// raw SetActorLocation, which can land inside geometry), as the self-teleport lane lands a player; the walk that follows
// is the director's. The landing is read back by an event, the player standing near the stand; the bound says INVALID.
bool StageNear(const char* what, const ue_wrap::FVector& at) {
    constexpr float kRingCm = 250.f;
    ue_wrap::FVector stand{};
    const bool sent = OnGameThreadValue([&] {
        void* const me = coop::players::Registry::Get().Local();
        const float half = me ? E::GetActorCharacterHalfHeight(me) : 0.f;
        if (!me || !(half > 0.f)) return false;
        const ue_wrap::FVector extent{150.f, 150.f, 250.f};
        const ue_wrap::FVector tries[] = {at, {at.X + kRingCm, at.Y, at.Z}, {at.X - kRingCm, at.Y, at.Z},
                                          {at.X, at.Y + kRingCm, at.Z}, {at.X, at.Y - kRingCm, at.Z}};
        ue_wrap::FVector floor{};
        bool found = false;
        for (const ue_wrap::FVector& t : tries) found = found || E::ProjectToNav(me, t, extent, &floor);
        if (!found) return false;
        ue_wrap::FRotator view{};
        if (void* const ctrl = E::GetController(me)) view = E::GetControlRotation(ctrl);
        coop::teleport_client::ApplyArgs args{floor.X, floor.Y, floor.Z + half, view.Pitch, view.Yaw, view.Roll};
        stand = {args.locX, args.locY, args.locZ};
        return coop::teleport_client::ApplyLocally(args);
    });
    if (!sent) {
        RV::Say(kTag, RV::Outcome::Invalid, "no NavMesh stand beside %s, or the game's teleport did not run", what);
        return false;
    }
    UE_LOGI("cover: the client was placed by the game's teleport at (%.0f,%.0f,%.0f) for %s", stand.X, stand.Y, stand.Z, what);
    return PS::WaitFor("the client to stand at its staged place", 20, [&] {
        return OnGameThreadValue([&] {
            ue_wrap::FVector now{};
            if (!E::TryGetActorLocation(coop::players::Registry::Get().Local(), now)) return false;
            return std::sqrt((now.X - stand.X) * (now.X - stand.X) + (now.Y - stand.Y) * (now.Y - stand.Y) +
                             (now.Z - stand.Z) * (now.Z - stand.Z)) < 400.f;
        });
    });
}

bool WalkNear(const char* what, const ue_wrap::FVector& at, float reachCm) {
    if (!meet::WalkToPoint(kTag, at, reachCm, what)) {
        RV::Say(kTag, RV::Outcome::Invalid, "the client could not walk to %s", what);
        return false;
    }
    return true;
}

// The unbolt, then a fence ask sent behind it on the same ordered lane: an unbolt of the desk's fixed plate, which the host
// always refuses and changes nothing for (so no red can toggle it back). The host's plates reach this client before its
// refusal, so the refusal is the point after which the plates cannot still be coming: plate 0 unbolted here by then holds, and
// plate 0 still bolted is the host's unannounced change (cover_announce_off) or an unbolt it refused.
bool ArmUnbolt() {
    static uint32_t refused0 = 0;
    return ClientArm("unbolt", [](std::string& why) {
        const Snap s = Read();
        if (!On(s, 0) || Loose(s, 0)) { why = "plate 0 is not on and bolted at the start"; return false; }
        refused0 = Refusals();
        SetLooking(Desk(), 0);
        if (!Verb(Desk(), L"unscrewPanel")) { why = "unscrewPanel did not run"; return false; }
        return CS::DrillAsk(N::kCoverOpUnscrew, N::kCoverDesk, AD::kFixedPlate, 1, 0) || (why = "the fence ask was not sent", false);
    }, [](std::string& why) {
        if (Refusals() <= refused0) return Judge::Pending;
        if (!Loose(Read(), 0)) { why = "the host answered the fence but plate 0 is not unbolted here: its plates never reached this client"; return Judge::Fail; }
        return Judge::Pass;
    });
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

// The plate prop's copy here, once the host's drive stream is done with it: the End of its stream was applied, or the copy
// is no longer parked and simulates. A grab before that is taken from a parked copy and does not take.
void* FreeCopy(uint32_t eid) {
    void* const prop = coop::element::LiveActorOfType(static_cast<coop::element::ElementId>(eid), coop::element::ElementType::Prop);
    if (!prop) return nullptr;
    coop::prop_drive_stream::AppliedEnd ends[coop::prop_drive_stream::kRecentEnds];
    const size_t n = coop::prop_drive_stream::RecentEnds(ends, coop::prop_drive_stream::kRecentEnds);
    for (size_t i = 0; i < n; ++i)
        if (ends[i].eid == eid) return prop;
    return !coop::prop_drive_stream::IsParked(prop) && E::IsActorRootSimulatingPhysics(prop) ? prop : nullptr;
}

// A put-back asked for a plate prop nobody has carried back: refused, and the plate stays off. Asked of the take-off's own
// prop, which the arm before left on its lid, before anything carries it. The host refuses it as a prop still fresh on its lid
// (Busy) or, once it has fallen off, as not on the lid (OutOfReach); any other answer, NotHeld included, is a wrong refusal.
// The hold test (NotHeld) is not judged by any arm: it needs the host's own player to hold the prop.
bool ArmCarried() {
    static uint32_t busy0 = 0, reach0 = 0, other0 = 0;
    return ClientArm("carried", [](std::string& why) {
        busy0 = Seen(N::CoverReason::Busy);
        reach0 = Seen(N::CoverReason::OutOfReach);
        other0 = Seen(N::CoverReason::NotHeld) + Seen(N::CoverReason::NoStation);
        const uint32_t eid = CS::LastTakeOffEid();
        if (eid == 0 || On(Read(), 0) || !PlateProp()) { why = "no plate prop of the take-off, or plate 0 is on"; return false; }
        return CS::DrillAsk(N::kCoverOpPutBack, N::kCoverDesk, 0, 0, eid) || (why = "the ask was not sent", false);
    }, [](std::string& why) {
        if (On(Read(), 0)) { why = "a put-back nobody carried was applied: plate 0 is on"; return Judge::Fail; }
        if (Seen(N::CoverReason::NotHeld) + Seen(N::CoverReason::NoStation) > other0) {
            why = "the put-back of a prop nobody holds was refused for the wrong reason (not Busy or OutOfReach)";
            return Judge::Fail;
        }
        return Seen(N::CoverReason::Busy) > busy0 || Seen(N::CoverReason::OutOfReach) > reach0 ? Judge::Pass : Judge::Pending;
    });
}

bool HeldHere(void* prop) {
    ue_wrap::engine::MainPlayerGrabState gs{};
    return prop && ue_wrap::engine::ReadMainPlayerGrabState(coop::players::Registry::Get().Local(), gs) &&
           (gs.grabbingActor == prop || gs.holdingActor == prop);
}

bool LaneArmed() { return OnGameThreadValue([] { return CS::PlateArmed(0); }); }

// Before each grab try: the director walks the client to a NavMesh point about 120 cm from the prop copy's current pose, on
// the side toward the client, and the player turns to face it, so the aim fan starts within arm's reach and in view. A prop no
// NavMesh point reaches ends INVALID with its pose logged.
bool StandBeforeProp(void* prop) {
    constexpr float kStandCm = 120.f;
    ue_wrap::FVector at{}, stand{};
    bool known = false, projected = false;
    OnGameThread([&] {
        ue_wrap::FVector me{};
        void* const local = coop::players::Registry::Get().Local();
        if (!local || !E::TryGetActorLocation(prop, at) || !E::TryGetActorLocation(local, me)) return;
        known = true;
        float dx = me.X - at.X, dy = me.Y - at.Y;
        const float len = std::sqrt(dx * dx + dy * dy);
        if (len > 1.f) { dx /= len; dy /= len; } else { dx = 1.f; dy = 0.f; }
        const ue_wrap::FVector want{at.X + dx * kStandCm, at.Y + dy * kStandCm, at.Z};
        projected = E::ProjectToNav(local, want, ue_wrap::FVector{150.f, 150.f, 250.f}, &stand);
    });
    if (!known || !projected) {
        RV::Say(kTag, RV::Outcome::Invalid, "putback: no NavMesh stand beside the plate prop at (%.0f,%.0f,%.0f)", at.X, at.Y,
                at.Z);
        return false;
    }
    if (!WalkNear("the plate prop", stand, 60.f)) return false;
    OnGameThread([&] {
        void* const local = coop::players::Registry::Get().Local();
        void* const ctrl = local ? E::GetController(local) : nullptr;
        ue_wrap::FVector me{};
        if (!ctrl || !E::TryGetActorLocation(local, me) || !E::TryGetActorLocation(prop, at)) return;
        const float dx = at.X - me.X, dy = at.Y - me.Y, dz = at.Z - me.Z;
        ue_wrap::FRotator face{};
        face.Yaw = std::atan2(dy, dx) * 57.2957795f;
        face.Pitch = std::atan2(dz, std::sqrt(dx * dx + dy * dy)) * 57.2957795f;
        E::SetControlRotation(ctrl, face);
    });
    return true;
}

// The plate prop taken in hand as the use key grabs it. The grab waits for the copy to be free of the host's drive stream and
// is aimed afresh at the copy's current pose on each try (a new AimedGrab, again when its chain fails).
bool GrabPlateProp(uint32_t eid) {
    constexpr int kGrabTries = 5;  // a failed chain is tried again; the aim fan ends itself
    if (!PS::WaitFor("the plate prop's copy to leave the host's drive stream", kStepBound, [&] {
            return OnGameThreadValue([&] { return FreeCopy(eid) != nullptr; });
        }))
        return false;
    std::string why = "no try";
    for (int tries = 0; tries < kGrabTries; ++tries) {
        void* const me = OnGameThreadValue([] { return coop::players::Registry::Get().Local(); });
        void* const prop = OnGameThreadValue([&] { return FreeCopy(eid); });
        if (!me || !prop) { why = "the player or the plate prop copy is gone"; break; }
        if (!StandBeforeProp(prop)) return false;
        coop::director::AimedGrab grab(me, prop);
        coop::director::GrabState state = coop::director::GrabState::Working;
        while (state == coop::director::GrabState::Working) {  // the fan ends itself, and its own warning names the blocker
            state = OnGameThreadValue([&] { return grab.Tick(); });
            if (state == coop::director::GrabState::Working) ::Sleep(100);
        }
        if (state == coop::director::GrabState::Grabbed) return true;
        why = grab.Why();
        if (grab.NoAim()) {  // a precondition of the grab, never reached: INVALID, as the grab's header says
            RV::Say(kTag, RV::Outcome::Invalid, "putback: no aim in the fan took the plate prop: %s", why.c_str());
            return false;
        }
        UE_LOGI("cover: the grab try %d did not take (%s) -- aimed again at the copy's pose", tries + 1, why.c_str());
    }
    RV::Say(kTag, RV::Outcome::Invalid, "putback: the plate prop was not grabbed in %d tries: %s", kGrabTries, why.c_str());
    return false;
}

// The held prop carried beyond the lane's arming distance from its plate: the lane's own line (the prop in this player's hand,
// off its lid) is the readiness event. A grab that already holds it there needs no walk; otherwise the director walks the
// player back from the desk, still holding it.
bool CarryPlateAway() {
    if (PS::WaitFor("the lane to arm the plate prop", 3, [&] { return LaneArmed(); }, false)) return true;
    ue_wrap::FVector me{}, plate{}, away{};
    const bool read = OnGameThreadValue([&] {
        void* const mesh = AD::PlateMesh(Desk(), 0);
        if (!mesh || !E::TryGetComponentLocation(mesh, plate) ||
            !E::TryGetActorLocation(coop::players::Registry::Get().Local(), me))
            return false;
        float dx = me.X - plate.X, dy = me.Y - plate.Y;
        const float len = std::sqrt(dx * dx + dy * dy);
        if (len > 1.f) { dx /= len; dy /= len; } else { dx = 1.f; dy = 0.f; }
        away = {plate.X + dx * (len + 250.f), plate.Y + dy * (len + 250.f), me.Z};
        return true;
    });
    if (!read) {
        RV::Say(kTag, RV::Outcome::Invalid, "putback: the player or plate 0 did not read");
        return false;
    }
    if (!WalkNear("a place back from the signal desk", away, 150.f)) return false;
    return PS::WaitFor("the lane to arm the plate prop", 20, [&] { return LaneArmed(); });
}

// The prop carried back to its lid, still held: the plate is on and the prop gone, from a CoverState applied after the
// prop's destroy (its destroy and the state ride different lanes, so never judged in the gap). A hold that drops on the way
// is INVALID: the arm measures the put-back, not the hold.
bool ArmPutBack() {
    if (!GrabPlateProp(CS::LastTakeOffEid()) || !CarryPlateAway()) return false;
    static bool s_gone = false;
    static uint32_t s_statesAtGone = 0;
    return ClientArm("putback", [](std::string& why) {
        s_gone = false;
        if (!HeldHere(PlateProp())) { why = "the plate prop is not held after the carry"; return false; }
        return true;
    }, [](std::string& why) {
        if (On(Read(), 0)) return Judge::Pass;
        void* const prop = PlateProp();
        if (!prop) {
            if (!s_gone) {
                s_gone = true;
                s_statesAtGone = CS::DeskStatesApplied();
            }
            if (CS::DeskStatesApplied() > s_statesAtGone) {
                why = "the plate prop is gone and the state that followed left plate 0 off";
                return Judge::Fail;
            }
            return Judge::Pending;
        }
        if (!HeldHere(prop)) { why = "the hold dropped before the prop reached its lid"; return Judge::Invalid; }
        ue_wrap::FVector lidAt{};
        void* const lid = AD::Lid(Desk(), 0);
        if (lid && E::TryGetComponentLocation(lid, lidAt)) E::SetActorLocation(prop, lidAt);
        return Judge::Pending;
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
    if (!ArmUnbolt() || !ArmStale() || !ArmTakeOff() || !ArmCarried() || !ArmPutBack()) return;
    if (!StageNear("the first transformer station", stationAt) || !WalkNear("the first transformer station", stationAt, kStationStandCm)) return;
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
