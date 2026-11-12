// coop/dev/atv_gate_drill_detail.h -- private to the ATV gate drill: the acts a player does at an ATV
// (atv_gate_drill_acts.cpp) as the sequencer (atv_gate_drill.cpp) runs them. Every act is called from the drill's own
// thread, hops to the game thread for the engine, and returns false with `why` set when the act could not be issued.
// An act that was issued and drew no answer from the game is a result to measure, not a failure.
#pragma once

#include "coop/dev/atv_gate_probe.h"

#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/types.h"

#include <atomic>
#include <initializer_list>
#include <string>

namespace coop::dev::atv_gate_drill::detail {

inline constexpr const char* kTag = "atvprobe";

// Runs `fn` on the game thread and returns when it has run. The drill's thread calls this; the game thread never does.
template <class Fn>
void OnGt(Fn&& fn) {
    ue_wrap::game_thread::RunAndWait([&fn](std::atomic<int>& d) { fn(); d.store(1); });
}

// One act: `key` names the ATV (its game Key, the same on both peers).
using ActFn = bool (*)(const std::wstring& key, std::string& why);

// ---- the ATV and what the sequencer reads of it ----

// What a step reads of the ATV on this peer, before and after.
struct Vitals {
    float fuel = -1.f;
    float health = -1.f;
    int   modules = -1;
    bool  seated = false;   // this peer's player is in the seat
    int   alt = -1;         // input_alt, which the horn writes and the Fly module shares; -1 unreadable
    int   fly = -1;         // the Fly module flag the horn sets
    int   hasFly = -1;      // the Fly module is fitted
};
Vitals ReadVitals(const std::wstring& key);

// The world's ATV nearest the local player by NavMesh route, its key; empty when none has a route. Host side.
std::wstring ChooseAtv();

// The copy this peer holds under `key` is indexed. Client side.
bool CopyResolves(const std::wstring& key);

// The director walks the local player to within reach of the ATV; a deadline bounds it.
bool WalkToAtv(const std::wstring& key, std::string& why);

// ---- the acts ----

bool PourCan(const std::wstring& key, std::string& why);        // a gas can in the hand, the fire key held on the ATV
bool UseToolbox(const std::wstring& key, std::string& why);     // a toolbox in the hand, the fire key held on the ATV
bool FitModule(const std::wstring& key, std::string& why);      // a module in the hand, the fire key on the ATV
bool FitFly(const std::wstring& key, std::string& why);         // the Fly module in the hand, the fire key on the ATV
bool TakeModuleOff(const std::wstring& key, std::string& why);  // the upgrades screen and a click on a module's button
bool Sit(const std::wstring& key, std::string& why);            // the game's seat verb
bool Leave(const std::wstring& key, std::string& why);          // the brake key, then the dismount key
bool Lights(const std::wstring& key, std::string& why);         // the lights key
bool Brake(const std::wstring& key, std::string& why);          // the brake key
bool Turbo(const std::wstring& key, std::string& why);          // the turbo key down and up
bool Honk(const std::wstring& key, std::string& why);           // the horn key down and up

// The ATV's fuel on this peer's copy set to the low value a refuel measures a gain from. A setup write, not an act.
bool Drain(const std::wstring& key, std::string& why);

// The seated player's throttle key held for kBurnS seconds with the brake on: the window in which a copy nobody drives
// is read for burn (the step's actor drives; the other peer's copy is the idle one).
bool IdleBurn(const std::wstring& key, std::string& why);

// The horn's two events fired on this peer's own copy while nobody sits, each logging the horn's fields before and after.
bool HonkReplayPress(const std::wstring& key, std::string& why);
bool HonkReplayRelease(const std::wstring& key, std::string& why);

// A gas can released from the hand over the ATV's fuelbox with the drop key.
bool DropCanInFuelbox(const std::wstring& key, std::string& why);

// A note the last act left for the other peer (the can's key), taken once. The sequencer sends it with the act's signal;
// the observer hands it to ObserveNote with the step's name.
std::string TakeNote();
void ObserveNote(const char* step, const std::string& note);

// ---- the helpers the act files share (atv_gate_drill_acts.cpp) ----

inline constexpr int   kAnswerS = 6;          // the game answers a fire press within this, or it did not
inline constexpr float kLowFuel = 20.f;       // the tank a refuel's gain is measured from
inline constexpr float kCanFuel = 100.f;      // a gas can's hold record loads empty without it
inline constexpr const wchar_t* kCanClass = L"prop_gascan_C";

void* Me();                                                                   // the local player; game thread
void* Atv(const std::wstring& key);                                           // this peer's copy under `key`; game thread
void* ObjProp(void* o, const wchar_t* name);                                  // a pointer property by name, or null
bool ReadFloat(void* o, const wchar_t* name, float& v);
bool WriteFloat(void* o, const wchar_t* name, float v);
bool CallByName(void* obj, const wchar_t* name);                              // a no-parameter function, called as the engine does
bool HandIs(const wchar_t* cls);                                              // drill thread
bool HandEmpty();                                                             // drill thread
bool TakeHand(const wchar_t* row, const wchar_t* cls, const float* fuel = nullptr);   // drill thread
bool Rose(const atv_gate_probe::Counts& base, std::initializer_list<atv_gate_probe::Fn> fns);   // drill thread
atv_gate_probe::Counts ProbeCounts();                                         // drill thread
bool WalkTo(const ue_wrap::FVector& at, float reachCm, bool carry, const char* what, std::string& why);   // drill thread
bool PressAtv(const std::wstring& key, const wchar_t* fn, std::string& why);  // one of the ATV's key events, seated; drill thread
const char* RoleName();

// A bool property of `o` (-1 when the class has none); game thread.
int ReadBoolField(void* o, const wchar_t* name);

// This copy's own isDrive read every half second, on a helper thread, from Start to Stop; Stop says the count of true
// readings. For the two idle-burn windows, on both peers: a copy nobody drives with isDrive false burns nothing of its own.
void StartDriveSampling(const std::wstring& key);
void StopDriveSampling(const char* drivenBy);

}  // namespace coop::dev::atv_gate_drill::detail
