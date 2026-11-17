// coop/dev/atv_gate_drill_detail.h -- private to the ATV drill: the acts a player does at an ATV (atv_gate_drill_acts.cpp),
// the steps that judge them (atv_gate_drill_steps.cpp, atv_gate_drill_forged.cpp) and the sequencer that runs both
// (atv_gate_drill.cpp). Every act is called from the drill's own thread, hops to the game thread for the engine, and returns
// false with `why` set when it could not be issued; a step whose act could not be issued is INVALID.
#pragma once

#include "coop/dev/atv_drive.h"
#include "coop/interactables/atv_drill_events.h"

#include "harness/autotest/peer_signal.h"

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/types.h"

#include <atomic>
#include <string>
#include <utility>
#include <vector>

namespace coop::dev::atv_gate_drill::detail {

inline constexpr const char* kTag = "atv";

// The drive closure (the walk, the ATV's input keys, the reads of the held copy) is coop/dev/atv_drive's, shared with the
// riders drill; the drill's own code names these as its own.
using coop::dev::atv_drive::Atv;
using coop::dev::atv_drive::CallByName;
using coop::dev::atv_drive::kReachCm;
using coop::dev::atv_drive::Me;
using coop::dev::atv_drive::OnGt;
using coop::dev::atv_drive::PressAtv;
using coop::dev::atv_drive::ReadFloat;
using coop::dev::atv_drive::ReadVitals;
using coop::dev::atv_drive::Vitals;

// The three peers: the host, the client in slot 1 (the actor of most steps, which leaves and rejoins) and the observer.
enum Role : int { kHost = 0, kClient = 1, kObserver = 2, kRoles = 3 };
const char* RoleName(Role r);

// The world's ATV nearest the local player by NavMesh route, its key; empty when none has a route. Host side.
std::wstring ChooseAtv();
bool CopyResolves(const std::wstring& key);                       // this peer's copy under `key` is indexed
using coop::dev::atv_drive::WalkToAtv;                            // the director walks the player beside the ATV

// ---- the acts ----
bool PourCan(const std::wstring& key, std::string& why);          // a gas can in the hand, the fire key held on the ATV
bool UseToolbox(const std::wstring& key, std::string& why);       // a toolbox in the hand, the fire key held on the ATV
bool FitModule(const std::wstring& key, std::string& why);        // a module in the hand, the fire key on the ATV
bool TakeModuleOff(const std::wstring& key, std::string& why);    // the upgrades screen and a click on a module's button
bool ModuleToHand(const std::wstring& key, std::string& why);     // the module in the hand, no act
bool EmptyHand(const std::wstring& key, std::string& why);
bool Sit(const std::wstring& key, std::string& why);              // the game's seat verb
using coop::dev::atv_drive::Leave;                                // the dismount key
bool Lights(const std::wstring& key, std::string& why);           // the lights key
using coop::dev::atv_drive::Brake;                                // the brake key
bool TurboOn(const std::wstring& key, std::string& why);          // the turbo key down, left down
using coop::dev::atv_drive::TurboOff;                             // the turbo key up
bool HornPress(const std::wstring& key, std::string& why);        // the horn key down
bool HornRelease(const std::wstring& key, std::string& why);      // the horn key up
bool HornTap(const std::wstring& key, std::string& why);          // down and up
using coop::dev::atv_drive::DriveOn;                              // the brake off and the throttle key down
using coop::dev::atv_drive::DriveOff;                             // the throttle key up

// The host's own copy's fuel and health set, published through the host's one publish point as any change of the host copy.
bool SeedHost(const std::wstring& key, float fuel, float health, std::string& why);

// The battery: this peer's copy's charge (-1 none fitted; NaN when it does not read), the copy's charge written with the
// game's updBattery (a local fixture, published by no one), the host copy's charge written and published, a charged battery in
// the hand with the fire key on the ATV, and the panel's unscrew (the game's unscrewPanel on the battery branch).
float ReadBattery(const std::wstring& key);
bool SetLocalBattery(const std::wstring& key, float charge);
bool SeedHostBattery(const std::wstring& key, float charge, std::string& why);
bool FitBattery(const std::wstring& key, float charge, std::string& why);
bool UnscrewBattery(const std::wstring& key, std::string& why);   // the panel's unscrew on the battery branch
// The live battery props of this world as weak references (the hand's and the hand mirrors' excluded), and how many of them a
// snapshot did not hold, with the first new one's charge (-1 when none). Neither counts by place or by element id.
using BatterySet = std::vector<ue_wrap::CachedObjRef>;
BatterySet BatteryProps();
int NewBatteryProps(const BatterySet& before, float* firstCharge);

// A fitted module's value on this peer's copy (the first), or -1.
int FirstModule(const std::wstring& key);
// The hand holds a module prop.
bool HandHoldsModule();

// ---- the sequencer's shared state, and what a step is ----

namespace DE = coop::atv_drill_events;

inline constexpr int kEventS = 40;   // a bound on an event: past it the run is INVALID, never a verdict
inline constexpr float kBatterySame = 2.f;   // the rejoined copy's adopted battery agrees with the host's mark of it within this (it drains)

// A verdict decided anywhere ends every peer's sequence: the deciding peer marks `atv_end` ("F <text>" or "I <text>"), the host
// says the verdict at once, and every wait of the sequencer returns on the mark. Aborted() is true from then on.
void SetRole(Role r);   // this peer's role, set once at the start
bool Aborted();
void End(char kind, const std::string& text);   // decide the run's end here: mark it, and say it when this is the host
// The signal `name`, polled; false when the run was aborted or the bound passed (INVALID is decided by End).
bool AwaitMark(const std::string& name, std::string& out, int boundS);
void SleepStep();

template <class Pred>
bool WaitUntil(const char* what, int boundS, Pred pred) {
    for (int i = 0; i < boundS * 10; ++i) {
        if (Aborted()) return false;
        if (pred()) return true;
        SleepStep();
    }
    if (!Aborted()) End('I', std::string(what) + " not within " + std::to_string(boundS) + " s");
    return false;
}

enum class Out { Pass, Fail, Invalid, Stopped };
struct Result {
    Out out = Out::Stopped;
    std::string text;
};
inline Result Pass(std::string t) { return {Out::Pass, std::move(t)}; }
inline Result Fail(std::string t) { return {Out::Fail, std::move(t)}; }
inline Result Invalid(std::string t) { return {Out::Invalid, std::move(t)}; }
inline Result Stopped() { return {Out::Stopped, "the run was decided elsewhere"}; }

// One step as one peer sees it.
struct Ctx {
    Role me = kHost;
    std::wstring key;
    std::string id;
    uint32_t cur[DE::kEventCount] = {};   // each event's count before the step's setup: a step reads what follows
    uint32_t seq0 = 0;                    // the host's stateSeq at the step's barrier
    uint32_t reqId = 0;                   // the forged request the actor made
    float fuel0 = -1.f, health0 = -1.f;   // this copy's vitals at the barrier
    int modules0 = -1;
    BatterySet bat0;                      // the battery props this peer held before a battery step's act (BatteryProps)
};

// A fact one peer leaves for the others in this step ("<id>_f_<role>"), and the wait for it.
void Fact(Ctx& c, const std::string& text);
bool AwaitFact(Ctx& c, Role from, std::string& text);

// The next record of `e` that `pred` accepts, polled. 1 found, 0 the bound passed (INVALID said), -1 the run ended.
template <class Pred>
int WaitRec(Ctx& c, DE::Event e, Pred pred, DE::Rec& out) {
    for (int i = 0; i < kEventS * 10; ++i) {
        if (Aborted()) return -1;
        while (DE::Next(e, c.cur[e], out))
            if (pred(out)) return 1;
        SleepStep();
    }
    if (!Aborted()) End('I', "no decision event within " + std::to_string(kEventS) + " s");
    return 0;
}

struct Step {
    const char* id;
    Role actor;
    const char* what;
    bool (*prelude)(Ctx&, std::string&);   // host only, before the barrier
    bool (*setup)(Ctx&, std::string&);     // every peer, after the barrier and before the act
    bool (*act)(Ctx&, std::string&);       // the actor
    Result (*decide)(Ctx&);                // every peer, on the events
    void (*teardown)(Ctx&);                // every peer, after its decision
};

// Step 10, a joiner's mirror adopted after the host's state (atv_gate_drill_joiner.cpp).
bool SpawnSecondAtv(Ctx& c, std::string& why);   // HOST prelude: a second ATV spawned, seeded and published; its key a fact
bool ActNone(Ctx& c, std::string& why);          // the step has no act
Result D10(Ctx& c);                              // CLIENT: decided on the adoption and the state kept before it

// The client leaves and rejoins (atv_gate_drill_forged.cpp): step 16 runs with the rejoined client.
bool IsRejoinedLife();
void LeaveForRejoin();             // CLIENT, first life: leave the session for the rig's restart
bool HostRejoin(const std::wstring& key);               // HOST: the rejoined client's steps
void RunRejoinedClient();          // CLIENT, second life

// The steps before the client leaves (atv_gate_drill_steps.cpp).
extern const Step kSteps[];
extern const int kStepCount;

}  // namespace coop::dev::atv_gate_drill::detail
