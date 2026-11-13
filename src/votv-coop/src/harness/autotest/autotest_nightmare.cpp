// harness/autotest/autotest_nightmare.cpp -- TEST-ONLY: a nightmare is one player's (the nightmare_drill row).
// Phase A: both peers go to bed through the game's own sleep and are held asleep (sleep need 30). The host's world is
// put back to dilation 1 and its own roll held off [dev writes]; the client's bed is given dreamProb 1, so
// coop/player/nightmare_roll's one roll, 25 real seconds into the rest, wins. While the client dreams the host judges
// the client's state dreaming, its copy hidden, itself still asleep; the client then leaves the dream and its copy is
// shown again. Phase B: the host runs the game's own wakeup and createDream and the client, awake, judges the same.
// Every step ends on the other peer's signal (peer_signal, VOTVCOOP_CAN_DIR) or on the state it waits for, never a
// clock. Each peer says `nightmare: ARM <phase> <role> PASS|FAIL <why>` for its own check, and the host says the one
// `rig: VERDICT nightmare PASS|FAIL|INVALID`. The save's nightmare rule is written on at the start [dev write]. Needs are
// left alone: a refill that writes sleep to 100 is the game's own wake.
// Reds: nightmare_roll_off (no dream comes) and nightmare_hide_off (the dreamer's copy stays shown).

#include "harness/autotest.h"
#include "disc_drills.h"   // co-located private header (src tree, not include/)
#include "harness/autotest/peer_signal.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/net/protocol.h"
#include "coop/player/nightmare_roll.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/player/sleep_rest.h"
#include "coop/player/sleep_sync.h"
#include "coop/session/rig_verdict.h"
#include "ue_wrap/actors/sleep.h"
#include "ue_wrap/actors/vitals.h"
#include "ue_wrap/world/active_events.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/world/world_singleton.h"

#include <windows.h>

#include <cmath>
#include <string>
#include <vector>

namespace harness::autotest {
namespace {

namespace PS  = harness::autotest::peer_signal;
namespace R   = ue_wrap::reflection;
namespace E   = ue_wrap::engine;
namespace SLP = ue_wrap::sleep;
namespace V   = ue_wrap::vitals;
using disc_drill::OnGameThread;
using PS::Await;
using PS::AwaitText;
using PS::Mark;

constexpr const char* kTag = "nightmare";
constexpr int kStepSec = 120;    // the bound on one step of the other peer's
constexpr int kDreamSec = 120;   // the bound on the roll point (25 s into the rest) or a dream: past it the verdict is Invalid
constexpr int kStateSec = 30;    // the bound on a state or a copy following a dream's edge across the wire
constexpr float kBackCm = 1500.f;

const char* Who() { return IsClientRole() ? "client" : "host"; }
void* Me() { return coop::players::Registry::Get().Local(); }
int OtherSlot() { return IsClientRole() ? 0 : 1; }

struct Now {
    bool asleep = false, dreaming = false;
    ue_wrap::FVector at{};
    float health = 0.f, need = 0.f;
};

Now Read() {
    Now n;
    OnGameThread([&n] {
        n.asleep = SLP::IsSleeping();
        n.dreaming = SLP::IsDreaming();
        if (void* body = SLP::OrigPawn()) E::TryGetActorLocation(body, n.at);
        else if (Me()) E::TryGetActorLocation(Me(), n.at);
        V::Read(V::Field::Health, &n.health);
        V::Read(V::Field::Sleep, &n.need);
    });
    return n;
}

int StateOf(int slot) {
    int st = 0;
    OnGameThread([&st, slot] { st = static_cast<int>(coop::sleep_sync::PeerState(slot)); });
    return st;
}

bool Dreams(int slot) { return StateOf(slot) == static_cast<int>(coop::net::SleepPeerState::Dreaming); }

void Log(const char* phase, const Now& n) {
    int st[3] = {};
    for (int i = 0; i < 3; ++i) st[i] = StateOf(i);
    UE_LOGI("nightmare: STATE %hs %hs asleep=%d dreaming=%d at=(%.0f,%.0f,%.0f) health=%.0f need=%.0f peers=[%d %d %d]",
            phase, Who(), n.asleep ? 1 : 0, n.dreaming ? 1 : 0, n.at.X, n.at.Y, n.at.Z, n.health, n.need, st[0], st[1],
            st[2]);
}

// AActor::bHidden of the slot's copy here (SetActorHiddenInGame's flag): 1, 0, or -1 with no copy or an unread flag.
int CopyHidden(int slot) {
    int out = -1;
    OnGameThread([&out, slot] {
        coop::RemotePlayer* p = coop::players::Registry::Get().Puppet(static_cast<uint8_t>(slot));
        void* actor = p && p->valid() ? p->actor() : nullptr;
        int32_t off = -1;
        uint8_t mask = 0;
        if (actor && R::FindBoolProperty(R::ClassOf(actor), L"bHidden", off, mask))
            out = (static_cast<const uint8_t*>(actor)[off] & mask) ? 1 : 0;
    });
    return out;
}

// One peer's own check, in the lane's own line; the host's verdict is built from these.
bool Check(const char* phase, bool ok, const std::string& why) {
    if (ok) UE_LOGI("nightmare: ARM %hs %hs PASS %hs", phase, Who(), why.c_str());
    else UE_LOGW("nightmare: ARM %hs %hs FAIL %hs", phase, Who(), why.c_str());
    return ok;
}

// A check's result as a signal's text, and its reading back. A verdict is decided by an event: a result is Fail only
// when the state it judges can no longer change, and Invalid when a bound ran out before the event came.
enum class Res { Fail = 0, Pass = 1, Invalid = 2 };
std::string Result(Res r, const std::string& why) { return std::to_string(static_cast<int>(r)) + " " + why; }
std::string Result(bool ok, const std::string& why) { return Result(ok ? Res::Pass : Res::Fail, why); }
Res ResultOf(const std::string& t) {
    return t.empty() ? Res::Invalid : t[0] == '1' ? Res::Pass : t[0] == '0' ? Res::Fail : Res::Invalid;
}
bool ResultOk(const std::string& t) { return ResultOf(t) == Res::Pass; }
std::string ResultWhy(const std::string& t) { return t.size() > 2 ? t.substr(2) : std::string("no reason given"); }

void HostFail(const std::string& why) {
    coop::rig_verdict::Say(kTag, coop::rig_verdict::Outcome::Fail, "%s", why.c_str());
}
void HostInvalid(const std::string& why) {
    coop::rig_verdict::Say(kTag, coop::rig_verdict::Outcome::Invalid, "%s", why.c_str());
}

// The host's word on the client's result: true when it passed, else the verdict is said and the drill ends.
bool Judge(const std::string& t, const std::string& what) {
    const Res r = ResultOf(t);
    if (r == Res::Pass) return true;
    if (r == Res::Fail) HostFail(what + ": " + ResultWhy(t));
    else HostInvalid(what + ": " + ResultWhy(t));
    return false;
}

void HoldNeed() { OnGameThread([] { V::Write(V::Field::Sleep, 30.f); }); }

// To bed, the nearest, through the game's own sleep; the need first, so the game's wake at 100 is far off.
bool GoToBed() {
    bool ok = false;
    OnGameThread([&ok] {
        V::Write(V::Field::Food, 100.f);
        V::Write(V::Field::Sleep, 30.f);
        ue_wrap::FVector at{};
        if (Me()) E::TryGetActorLocation(Me(), at);
        void* bed = SLP::NearestBed(at);
        ok = bed && SLP::CallSleep(bed);
        // The host's world back to 1 in the same frame [dev write]: at the game's 20 the night's scheduled
        // events arrive within seconds, and every sleeper wakes for them.
        if (ok && !IsClientRole()) SLP::SetGlobalTimeDilation(1.f);
    });
    return ok && PS::WaitFor("the sleeping flag after the game's sleep", 15, [] { return Read().asleep; }, false);
}

// This peer's own copy of its bed (the gamemode's bed while it sleeps) given a dream chance of `p`.
void BedChance(float p) {
    OnGameThread([p] {
        void* gm = ue_wrap::world_singleton::Gamemode();
        const int32_t offBed = gm ? R::FindPropertyOffset(R::ClassOf(gm), L"bed") : -1;
        void* bed = offBed >= 0 ? *reinterpret_cast<void**>(static_cast<char*>(gm) + offBed) : nullptr;
        const int32_t off = bed ? R::FindPropertyOffset(R::ClassOf(bed), L"dreamProb") : -1;
        if (off >= 0) *reinterpret_cast<float*>(static_cast<char*>(bed) + off) = p;
        UE_LOGI("nightmare: %hs bed %ls dreamProb set to %.0f (%d)", Who(), bed ? R::ClassNameOf(bed).c_str() : L"none",
                static_cast<double>(p), off >= 0 ? 1 : 0);
    });
}

void LeaveDream() {
    OnGameThread([] {
        void* gm = ue_wrap::world_singleton::Gamemode();
        void* fn = gm ? R::FindFunction(R::ClassOf(gm), L"leaveDream") : nullptr;
        ue_wrap::ParamFrame pf(fn);
        const bool ok = fn && pf.valid() && ue_wrap::Call(gm, pf);
        UE_LOGI("nightmare: %hs leaveDream ran=%d", Who(), ok ? 1 : 0);
    });
}

void RuleOn() {
    OnGameThread([] {
        void* gm = ue_wrap::world_singleton::Gamemode();
        const int32_t off = gm ? R::FindPropertyOffset(R::ClassOf(gm), L"nightmaresEnabled") : -1;
        UE_LOGI("nightmare: %hs nightmaresEnabled=%d -- set to 1 for the drill [dev write]", Who(),
                off >= 0 ? static_cast<int>(*(static_cast<uint8_t*>(gm) + off)) : -1);
        if (off >= 0) *(static_cast<uint8_t*>(gm) + off) = 1;   // the rig's save has the rule off
    });
}

float Dist(const ue_wrap::FVector& a, const ue_wrap::FVector& b) {
    const float dx = a.X - b.X, dy = a.Y - b.Y, dz = a.Z - b.Z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// The dreamer leaves its dream through the game's own leaveDream and is judged at once: leaveDream clears the flag and
// moves the player inside the call, so what is read after it is final. Out of the dream and beside the place it dreamt
// from.
bool LeaveAndReturn(const char* phase, const Now& before, std::string& why) {
    LeaveDream();
    const Now n = Read();
    Log(phase, n);
    const bool back = !n.dreaming && Dist(n.at, before.at) < kBackCm;
    why = back ? "dreamt and came back beside its bed" : "not back from the dream";
    return back;
}

// Whether the world's events have all ended (an unreadable count is not "gone").
bool WorldEventsGone() {
    int32_t n = -1;
    OnGameThread([&n] { ue_wrap::active_events::ReadCount(n); });
    return n == 0;
}

// Whether an event is running now (an unreadable count is not "running"): the sample a wake is attributed by.
bool WorldEventRunning() {
    int32_t n = -1;
    OnGameThread([&n] { ue_wrap::active_events::ReadCount(n); });
    return n > 0;
}

// The roll point of this client's rest, or a dream, or the end of the rest: the events after which no dream can come
// (or one has). RollPoints counts the point whether the roll was made or held back by the red arm. A rest that ends
// before its roll point was ended by something else (the game wakes every sleeper for an event), which is no verdict on
// the roll.
struct RollWatch {
    uint32_t pointsBefore = 0;
    bool PointReached() const {
        bool point = false;
        OnGameThread([&] { point = coop::nightmare_roll::RollPoints() > pointsBefore; });
        return point;
    }
    bool Decided() const {
        HoldNeed();
        bool restOver = false;
        OnGameThread([&] { restOver = !coop::sleep_rest::Resting(); });
        return Read().dreaming || PointReached() || restOver;
    }
};

// ---- the two-peer drill ---------------------------------------------------------------------------------------

void RunHost() {
    if (!disc_drill::HostWorldUp()) return;
    if (!Await("client_ready", 480)) return;
    if (!PS::WaitFor("the client's copy standing here", 60, [] { return CopyHidden(1) >= 0; })) return;
    RuleOn();
    OnGameThread([] { SLP::SetDreamProbability(0.f); });   // phase A: the host's own roll held off

    // Phase A: both asleep; the client dreams.
    const bool slept = GoToBed();
    UE_LOGI("nightmare: A host in bed=%d", slept ? 1 : 0);
    if (!slept) return HostInvalid("the game's sleep did not take the host to bed");
    Mark("A_bed_host");
    if (!Await("A_bed_client")) return;
    std::string dream;
    if (!AwaitText("A_dream", dream, kStepSec)) return;
    if (!Judge(dream, "the client's dream")) {
        Check("A", false, "the client reports: " + ResultWhy(dream));
        return;
    }
    // The state is an event (the client's report reaching this host); a bound that runs out first is Invalid. The hide
    // is applied inside the call that takes the state, so it is read once, at once.
    // An event running at any sample while the host rests is latched: the game wakes every sleeper for one, and it may
    // have ended again by the time the verdict is read.
    bool eventSeen = false;
    if (!PS::WaitFor("the client's state dreaming here", kStateSec, [&eventSeen] {
            HoldNeed();
            eventSeen = eventSeen || WorldEventRunning();
            return Dreams(1);
        }, false)) {
        Mark("A_seen", "0");
        return HostInvalid("the client's rest state dreaming did not reach the host");
    }
    const bool hidden = CopyHidden(1) == 1;
    const Now n = Read();
    eventSeen = eventSeen || WorldEventRunning();
    Log("A", n);
    const std::string why = !hidden     ? "the client's copy is not hidden here while the client dreams"
                            : !n.asleep ? "the host woke while the client dreamt"
                                        : "";
    // A host that woke while an event ran was woken by it (the game wakes every sleeper for one): no verdict on the lane.
    if (hidden && !n.asleep && eventSeen) {
        UE_LOGW("nightmare: ARM A host INVALID the host was woken by a world event while the client dreamt");
        Mark("A_seen", "0");
        return HostInvalid("the host was woken by a world event while the client dreamt");
    }
    const bool seen = Check("A", why.empty(), why.empty() ? "slept through it, saw the client dream, its copy hidden" : why);
    Mark("A_seen", seen ? "1" : "0");
    if (!seen) return HostFail(why);
    std::string woke;
    if (!AwaitText("A_woke", woke, kStepSec)) return;
    if (!Judge(woke, "the client's return")) return;
    if (!PS::WaitFor("the client's state back here", kStateSec, [] { return !Dreams(1); }, false))
        return HostInvalid("the client's rest state after its dream did not reach the host");
    const bool shown = CopyHidden(1) == 0;
    if (!Check("A", shown, shown ? "the client's copy is shown again" : "the client's copy stays hidden after its dream"))
        return HostFail("the client's copy stays hidden here after its dream");
    OnGameThread([] { V::Write(V::Field::Sleep, 100.f); });   // the game's own wake
    PS::WaitFor("the host's wake", kStateSec, [] { return !Read().asleep; }, false);

    // Phase B: the host dreams; the client is awake. An event running now refuses the bed (the night's own, often begun
    // as phase A ends); the dream does not need it, so the host then dreams from where it stands.
    if (!GoToBed()) UE_LOGI("nightmare: B host not in bed (an event refused it) -- dreaming from where it stands");
    const Now inBed = Read();
    bool ran = false;
    OnGameThread([&ran] { ran = SLP::BeginNightmare(); UE_LOGI("nightmare: B host BeginNightmare ran=%d", ran ? 1 : 0); });
    const Now dreaming = Read();   // createDream sets the flag inside the call
    Log("B", dreaming);
    const bool dreamed = ran && dreaming.dreaming;
    Mark("B_dreaming", Result(dreamed, dreamed ? "the host dreams" : "the host's dream did not begin"));
    if (!dreamed) return HostFail("the game's wakeup and createDream did not put the host in a dream");
    std::string seenB;
    if (!AwaitText("B_seen", seenB, kStepSec)) return;
    if (!Judge(seenB, "the client beside the host's dream")) return;
    std::string backWhy;
    const bool back = LeaveAndReturn("B", inBed, backWhy);
    Check("B", back, backWhy);
    Mark("B_woke", Result(back, backWhy));
    if (!back) return HostFail("the host: " + backWhy);
    std::string end;
    if (!AwaitText("B_end", end, kStepSec)) return;
    if (!Judge(end, "the client after the host's dream")) return;
    OnGameThread([] { SLP::SetDreamProbability(-1.f); });
    UE_LOGI("nightmare: DONE host");
    coop::rig_verdict::Say(kTag, coop::rig_verdict::Outcome::Pass,
                           "the client dreamt beside a sleeping host, the host dreamt beside an awake client, each copy hidden and shown again");
}

void RunClient() {
    if (!disc_drill::HostPuppetUp()) return;
    Mark("client_ready");
    RuleOn();

    // Phase A: both asleep; this client dreams. Its bed is given the certain dream the moment it is in bed, before the
    // rest's roll point (25 s in) can pass.
    RollWatch watch;
    OnGameThread([&watch] { watch.pointsBefore = coop::nightmare_roll::RollPoints(); });
    const bool slept = GoToBed();
    UE_LOGI("nightmare: A client in bed=%d", slept ? 1 : 0);
    if (!slept) return coop::rig_verdict::Say(kTag, coop::rig_verdict::Outcome::Invalid, "the game's sleep did not take the client to bed");
    BedChance(1.f);
    const Now before = Read();
    Mark("A_bed_client");
    if (!Await("A_bed_host")) return;
    // No dream can come in this rest once its roll point has passed (the roll made, or held back by the red arm) or the
    // rest has ended: that is the event this verdict is decided at. A bound that runs out first is Invalid.
    if (!PS::WaitFor("this client's roll point, a dream or the rest's end", kDreamSec, [&watch] { return watch.Decided(); }, false)) {
        Mark("A_dream", Result(Res::Invalid, "neither the roll point nor a dream came"));
        return;
    }
    const Now after = Read();
    Log("A", after);
    const bool dreamed = after.dreaming;
    if (!dreamed && !watch.PointReached()) {
        // The rest ended before its roll point: an event woke the sleeper, so the roll never came to be judged.
        Mark("A_dream", Result(Res::Invalid, "the rest ended before its roll point (an event woke the client)"));
        return;
    }
    const std::string dWhy = dreamed ? "the client's own roll won and it dreams"
                                     : "the rest's roll point passed with no dream: the client never dreamt";
    Check("A", dreamed, dWhy);
    Mark("A_dream", Result(dreamed, dWhy));
    if (!dreamed) return;
    std::string seen;
    if (!AwaitText("A_seen", seen, kStepSec)) return;
    std::string backWhy;
    const bool back = LeaveAndReturn("A", before, backWhy);
    Check("A", back, backWhy);
    Mark("A_woke", Result(back, backWhy));
    if (!back) return;

    // Phase B: the host dreams; this client is awake.
    std::string hostDream;
    if (!AwaitText("B_dreaming", hostDream, 480)) return;
    if (!ResultOk(hostDream)) return;
    if (!PS::WaitFor("the host's state dreaming here", kStateSec, [] { return Dreams(0); }, false)) {
        Mark("B_seen", Result(Res::Invalid, "the host's rest state dreaming did not reach this client"));
        return;
    }
    const bool hidden = CopyHidden(0) == 1;   // applied inside the call that took the state
    const Now n = Read();
    Log("B", n);
    const std::string why = !hidden ? "the host's copy is not hidden here while the host dreams"
                            : (n.asleep || n.dreaming) ? "this client was not awake beside the host's dream"
                                                       : "";
    const bool ok = Check("B", why.empty(), why.empty() ? "awake, saw the host dream, its copy hidden" : why);
    Mark("B_seen", Result(ok, ok ? "seen" : why));
    if (!ok) return;
    std::string woke;
    if (!AwaitText("B_woke", woke, kStepSec)) return;
    if (!PS::WaitFor("the host's state back here", kStateSec, [] { return !Dreams(0); }, false)) {
        Mark("B_end", Result(Res::Invalid, "the host's rest state after its dream did not reach this client"));
        return;
    }
    const bool shown = CopyHidden(0) == 0;
    Check("B", shown, shown ? "the host's copy is shown again" : "the host's copy stays hidden after its dream");
    Mark("B_end", Result(shown, shown ? "shown again" : "the host's copy stays hidden here after its dream"));
    UE_LOGI("nightmare: DONE client");
}

// ---- staging, for a person's test (nightmare_drill = sleeper | dreamer) ------------------------------------------
// Each peer alone, with no handshake. `sleeper` turns the rule on, goes to bed and is held there for good, back to bed
// whenever an event wakes it; `dreamer` turns the rule on, waits for the world's events to pass (an event ends a
// dream), and goes to bed with its bed's dreamProb at 1; with nightmare_dream=<class> it starts that dream itself once
// it is in bed instead. The rest is the person's.
//
// [dev, staging] The host ends every running ventCrawler event as the game ends one: lib_C::setEvent(false) with the
// creature as its sender, then the creature destroyed. The save's night brings one within minutes of the load, and it
// holds the event (which refuses every bed and ends every dream) for as long as the creature lives.
void EndCrawlerEvents() {
    OnGameThread([] {
        ue_wrap::active_events::Senders snd{};
        if (!ue_wrap::active_events::ReadSenders(snd) || snd.num <= 0) return;
        void* lib = R::FindClassDefaultObject(L"lib_C");
        void* fn = lib ? R::FindFunction(R::ClassOf(lib), L"setEvent") : nullptr;
        std::vector<void*> crawlers;
        for (int32_t i = 0; i < snd.num; ++i)
            if (snd.data[i] && R::IsLive(snd.data[i]) && R::ClassNameOf(snd.data[i]) == L"ventCrawler_C")
                crawlers.push_back(snd.data[i]);
        for (void* c : crawlers) {
            ue_wrap::ParamFrame pf(fn);
            const bool ended = fn && pf.valid() && pf.Set<bool>(L"isEventActive", false) &&
                               pf.Set<bool>(L"deactivateAmbientTrack", false) && pf.Set<void*>(L"__WorldContext", c) &&
                               ue_wrap::Call(lib, pf);
            void* destroy = R::FindDispatchFunctionCached(R::ClassOf(c), L"K2_DestroyActor");
            ue_wrap::ParamFrame d(destroy);
            const bool gone = destroy && d.valid() && ue_wrap::Call(c, d);
            int32_t n = -1;
            ue_wrap::active_events::ReadCount(n);
            UE_LOGI("nightmare: host ended a ventCrawler's event (setEvent=%d destroyed=%d) -- %d event(s) left",
                    ended ? 1 : 0, gone ? 1 : 0, n);
        }
    });
}

// The pace of a hold that lasts as long as the process does: a person's test, with no end to wait for.
void HoldPace() { ::Sleep(1000); }

void StageDreamer() {
    // Any running event ends a dream (dreamBase_C's tick asks getEvent), and a client counts the host world's events
    // too: the save's night brings one within minutes of the load, so the dream waits for it to pass.
    const bool passed = PS::WaitFor("the world's event to pass", 360, [] { return WorldEventsGone(); }, false);
    UE_LOGI("nightmare: %hs dreamer -- %hs", Who(), passed ? "the world's event has passed" : "no event passed in 6 min");
    const bool inBed = GoToBed();
    const std::string cls = coop::config::ResolveString(coop::config_registry::rows::nightmare_dream);
    if (!cls.empty()) {
        // A named dream: the game's own calls with that class once in bed, the roll held off.
        BedChance(0.f);
        OnGameThread([&cls] {
            const std::wstring w(cls.begin(), cls.end());
            void* c = ue_wrap::object_index::ClassByName(w.c_str());
            UE_LOGI("nightmare: %hs dreamer -- %ls: BeginNightmare ran=%d", Who(), w.c_str(),
                    c && SLP::BeginNightmare(c) ? 1 : 0);
        });
        UE_LOGI("nightmare: STAGED %hs dreamer -- in a %hs nightmare (in bed %d)", Who(), cls.c_str(), inBed ? 1 : 0);
        return;
    }
    BedChance(1.f);
    UE_LOGI("nightmare: STAGED %hs dreamer -- in bed with a certain nightmare (in bed %d)", Who(), inBed ? 1 : 0);
}

void StageSleeper() {
    UE_LOGI("nightmare: STAGED %hs sleeper -- held asleep, with no nightmare of its own", Who());
    OnGameThread([] { SLP::SetDreamProbability(0.f); });   // the host's own roll; a client's is held at 0 already
    for (int t = 0;; ++t) {
        if (!IsClientRole()) EndCrawlerEvents();
        if (!Read().asleep && t % 5 == 0 && GoToBed()) BedChance(0.f);
        HoldNeed();
        HoldPace();
    }
}

void Stage(const std::string& as) {
    if (IsClientRole()) {
        if (!disc_drill::HostPuppetUp()) return;
    } else if (!disc_drill::HostWorldUp()) {
        return;
    }
    RuleOn();
    if (as == "dreamer") StageDreamer();
    else StageSleeper();
}

}  // namespace

DWORD WINAPI NightmareThread(LPVOID /*arg*/) {
    const std::string arm = coop::config::ResolveEnum(coop::config_registry::rows::nightmare_drill);
    if (arm == "sleeper" || arm == "dreamer") {
        Stage(arm);
        return 0;
    }
    if (!PS::Begin(kTag)) return 0;
    if (IsClientRole()) RunClient();
    else RunHost();
    return 0;
}

}  // namespace harness::autotest
