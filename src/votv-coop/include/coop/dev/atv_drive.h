// coop/dev/atv_drive.h -- [dev] the acts a drill does to drive an ATV the way a player does: the director's walk to it, the
// ATV's own input-action events (brake, throttle, reverse, turbo, dismount) called as the engine calls them on the possessed ATV, and
// the reads a drill takes of the copy it holds. Shared by the ATV gate drill and the ATV riders drill. Every function is for
// the drill's own thread unless it says game thread; each hops to the game thread for the engine through OnGt.

#pragma once

#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/types.h"

#include <atomic>
#include <string>

namespace coop::dev::atv_drive {

// Runs `fn` on the game thread and returns when it has run. The drill's thread calls this; the game thread never does.
template <class Fn>
void OnGt(Fn&& fn) {
    ue_wrap::game_thread::RunAndWait([&fn](std::atomic<int>& d) { fn(); d.store(1); });
}

// The walk ends this near the ATV's root: its body is wider than that, so the player stands beside it.
inline constexpr float kReachCm = 220.f;

// What a drill reads of the ATV on this peer, for an act's precondition.
struct Vitals {
    float fuel = -1.f;
    float health = -1.f;
    int   modules = -1;
    bool  seated = false;   // this peer's player is in the seat
};
Vitals ReadVitals(const std::wstring& key);

// The local player's pawn, and the ATV this peer holds under `key`. Game thread.
void* Me();
void* Atv(const std::wstring& key);

// A function with no parameters, or one key parameter left zero, called as the engine calls it. Game thread.
bool CallByName(void* obj, const wchar_t* name);

// The float property `name` of `o` read into `v`; false when `o` is null or its class has no such property. Game thread.
bool ReadFloat(void* o, const wchar_t* name, float& v);

// The director walks the player to within `reachCm` of `at`; `carry` keeps what the hand holds. `what` names the goal in the
// log and in `why`; false with `why` set when it did not arrive. `throughKey`, when given, names the prop the walk is about:
// the director's blocker delete leaves it standing, as it leaves a goal's own target.
bool WalkTo(const ue_wrap::FVector& at, float reachCm, bool carry, const char* what, std::string& why,
            const std::wstring& throughKey = std::wstring());

// The director walks the player beside the ATV; false with `why` set when it did not arrive.
bool WalkToAtv(const std::wstring& key, std::string& why);

// One of the ATV's own input-action events, called as the engine calls it on the possessed ATV; the player must be seated.
bool PressAtv(const std::wstring& key, const wchar_t* fn, std::string& why);

bool Brake(const std::wstring& key, std::string& why);       // the brake key
bool TurboOff(const std::wstring& key, std::string& why);    // the turbo key up
bool Leave(const std::wstring& key, std::string& why);       // the dismount key
bool DriveOn(const std::wstring& key, std::string& why);     // the brake off and the throttle key down
void DriveOff(const std::wstring& key);                      // the throttle key up
bool DriveBack(const std::wstring& key, std::string& why);   // the brake off and the reverse key down
void DriveBackOff(const std::wstring& key);                  // the reverse key up

}  // namespace coop::dev::atv_drive
