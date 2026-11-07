// coop/dev/atv_gate_drill_detail.h -- private to the ATV gate drill: the acts a player does at an ATV
// (atv_gate_drill_acts.cpp) as the sequencer (atv_gate_drill.cpp) runs them. Every act is called from the drill's own
// thread, hops to the game thread for the engine, and returns false with `why` set when the act could not be issued.
// An act that was issued and drew no answer from the game is a result to measure, not a failure.
#pragma once

#include "ue_wrap/core/game_thread.h"

#include <atomic>
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
bool TakeModuleOff(const std::wstring& key, std::string& why);  // the upgrades screen and a click on a module's button
bool Sit(const std::wstring& key, std::string& why);            // the game's seat verb
bool Leave(const std::wstring& key, std::string& why);          // the brake key, then the dismount key
bool Lights(const std::wstring& key, std::string& why);         // the lights key
bool Brake(const std::wstring& key, std::string& why);          // the brake key
bool Turbo(const std::wstring& key, std::string& why);          // the turbo key down and up
bool Honk(const std::wstring& key, std::string& why);           // the horn key down and up

}  // namespace coop::dev::atv_gate_drill::detail
