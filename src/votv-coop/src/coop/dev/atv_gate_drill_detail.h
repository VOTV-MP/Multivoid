// coop/dev/atv_gate_drill_detail.h -- private to the ATV drill: the acts a player does at an ATV (atv_gate_drill_acts.cpp),
// the steps that judge them (atv_gate_drill_steps.cpp, atv_gate_drill_forged.cpp) and the sequencer that runs both
// (atv_gate_drill.cpp). Every act is called from the drill's own thread, hops to the game thread for the engine, and returns
// false with `why` set when it could not be issued; a step whose act could not be issued is INVALID.
#pragma once

#include "coop/interactables/atv_drill_events.h"

#include "harness/autotest/peer_signal.h"

#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/types.h"

#include <atomic>
#include <string>
#include <utility>

namespace coop::dev::atv_gate_drill::detail {

inline constexpr const char* kTag = "atv";

// Runs `fn` on the game thread and returns when it has run. The drill's thread calls this; the game thread never does.
template <class Fn>
void OnGt(Fn&& fn) {
    ue_wrap::game_thread::RunAndWait([&fn](std::atomic<int>& d) { fn(); d.store(1); });
}

// The three peers: the host, the client in slot 1 (the actor of most steps, which leaves and rejoins) and the observer.
enum Role : int { kHost = 0, kClient = 1, kObserver = 2, kRoles = 3 };
const char* RoleName(Role r);

// What a step reads of the ATV on this peer, for an act's precondition.
struct Vitals {
    float fuel = -1.f;
    float health = -1.f;
    int   modules = -1;
    bool  seated = false;   // this peer's player is in the seat
};
Vitals ReadVitals(const std::wstring& key);

// The world's ATV nearest the local player by NavMesh route, its key; empty when none has a route. Host side.
std::wstring ChooseAtv();
bool CopyResolves(const std::wstring& key);                       // this peer's copy under `key` is indexed
bool WalkToAtv(const std::wstring& key, std::string& why);        // the director walks the player beside the ATV

// ---- the acts ----
bool PourCan(const std::wstring& key, std::string& why);          // a gas can in the hand, the fire key held on the ATV
bool UseToolbox(const std::wstring& key, std::string& why);       // a toolbox in the hand, the fire key held on the ATV
bool FitModule(const std::wstring& key, std::string& why);        // a module in the hand, the fire key on the ATV
bool TakeModuleOff(const std::wstring& key, std::string& why);    // the upgrades screen and a click on a module's button
bool ModuleToHand(const std::wstring& key, std::string& why);     // the module in the hand, no act
bool EmptyHand(const std::wstring& key, std::string& why);
bool Sit(const std::wstring& key, std::string& why);              // the game's seat verb
bool Leave(const std::wstring& key, std::string& why);            // the dismount key
bool Lights(const std::wstring& key, std::string& why);           // the lights key
bool Brake(const std::wstring& key, std::string& why);            // the brake key
bool TurboOn(const std::wstring& key, std::string& why);          // the turbo key down, left down
bool TurboOff(const std::wstring& key, std::string& why);         // the turbo key up
bool HornPress(const std::wstring& key, std::string& why);        // the horn key down
bool HornRelease(const std::wstring& key, std::string& why);      // the horn key up
bool HornTap(const std::wstring& key, std::string& why);          // down and up
bool DriveOn(const std::wstring& key, std::string& why);          // the brake off and the throttle key down
void DriveOff(const std::wstring& key);                           // the throttle key up

// The host's own copy's fuel and health set, published through the host's one publish point as any change of the host copy.
bool SeedHost(const std::wstring& key, float fuel, float health, std::string& why);

// A fitted module's value on this peer's copy (the first), or -1.
int FirstModule(const std::wstring& key);
// The hand holds a module prop.
bool HandHoldsModule();

// ---- the sequencer's shared state, and what a step is ----

namespace DE = coop::atv_drill_events;

inline constexpr int kEventS = 40;   // a bound on an event: past it the run is INVALID, never a verdict

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
