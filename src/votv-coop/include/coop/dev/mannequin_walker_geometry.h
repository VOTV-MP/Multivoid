// coop/dev/mannequin_walker_geometry.h -- the walker drill's places, views and walks (coop/dev/mannequin_walker_drill).
//
// Where a player stands against a walker, where it looks, and where the bot director walks it: the drill's measurements are
// judged in the drill, and everything it asks of the world's geometry is here, with what the drill's two files (the first and the
// second increment, coop/dev/mannequin_walker_steps_b) share: the step line and verdict helpers and the walkers by key. Every
// function but the templates reads or drives the engine and must run on the game thread unless it says otherwise (WalkTo and
// BedAnchor are called from the drill's own thread and hop onto it themselves).

#pragma once

#include "harness/autotest/peer_signal.h"

#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/types.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace coop::dev::mannequin_walker_drill {

// The drill's name on its `rig: VERDICT` line, and the client's slot on the host.
inline constexpr const char* kTag = "mannequin";
inline constexpr uint8_t     kClientSlot = 1;

// The other peer's puppet, from its ready edge to a pose.
inline constexpr int kPuppetBoundS = 60;

// A director walk's own deadline (after the stage), and how long the drill waits for one to end.
inline constexpr int kWalkDeadlineS = 400;
inline constexpr int kWalkWaitS = kWalkDeadlineS + 90;

// The walk's reach of a point it is placed at: within it the director's stage places the walk at once (a bed anchor, the KPP gate).
inline constexpr float kAnchorReachCm = 500.f;
// Where a walk toward a walker gives up: inside the walker's own hunt range with a margin (it normally ends sooner, on the hunt).
inline constexpr float kInRangeCm   = 19000.f;   // 3D, inside the Blueprint's 20000 GetDistanceTo range with margin
inline constexpr float kStepReachCm = 17000.f;   // the step-in walk's own end (flat), if the in-range hunt never comes
// How long the walker gets, after that walk's end, to show that it took the player (it reads its range every half second).
inline constexpr int kHuntGraceS = 5;
// A walker within this of its target stands at its shoulder, where the game's own frustum test of its bounds holds whatever the
// yaw: no measurement is made there, and no walk closes in past it.
inline constexpr float kApproachEndCm = 3000.f;
// The close-in to a walker that is not seen: how far its walk stops from the copy (flat), how long one leg of walk runs before the
// view is aimed and read again, how long that read lasts (a render needs a frame after the aim), and the most the whole close-in
// takes.
inline constexpr float kCloseReachCm = 2.f * kApproachEndCm;
inline constexpr int   kCloseLegS = 20;
inline constexpr int   kAimPollMs = 1500;
inline constexpr int   kCloseInS = 150;

template <class Fn>
void OnGameThread(Fn&& fn) {
    ue_wrap::game_thread::RunAndWait([&fn](std::atomic<int>& d) { fn(); d.store(1); });
}

// A readiness wait on a state the game thread reads. A judged wait (`abort` false) ends in the caller's check.
template <class Pred>
bool WaitGT(const char* what, int seconds, Pred pred, bool abort = true) {
    return harness::autotest::peer_signal::WaitFor(what, seconds, [&] {
        bool ok = false;
        OnGameThread([&] { ok = pred(); });
        return ok;
    }, abort);
}

// Says `rig: VERDICT mannequin INVALID -- <why>` and returns false.
bool Invalid(const char* why);

// The step lines and the verdict of a measured failure, shared by the drill's halves. Check logs `mannequin-drill: step <step>
// PASS|FAIL <what>` and counts a failure (FailCount); Fail says the measured FAIL as the verdict and returns false for the caller to
// return: a measured failure ends the run at its step, so that no later precondition can turn it into an INVALID. The host says it;
// a client's step marks a word the host turns into this: AwaitClient waits for the mark and reads the client's word, "ok" or "fail
// <step> <what>" (a step the client measured to fail, said as the verdict); false when the run must end, the failure said or no
// word within the bound (an INVALID said by the wait). `text` receives the word when given. FailedToHost is the client's side: it
// marks "fail step <step> <what>" at `mark` and returns false.
int  FailCount();
bool Check(bool ok, const char* step, const char* what);
bool Fail(const char* step, const char* what);
bool AwaitClient(const char* mark, int seconds, std::string* text = nullptr);
bool FailedToHost(const char* mark, const char* step, const char* what);

// The walkers this peer holds, and one of them by its key (a GUID's text); `count` receives how many carry it. Game thread.
std::vector<void*> Walkers();
std::string        KeyOf(void* walker);
void*              ByKey(const std::string& key, int* count = nullptr);
// A key as narrow text from its wide form, and back (a key is a GUID's text, so each character is one ASCII one).
std::string  Narrow(const std::wstring& key);
std::wstring Widen(const std::string& key);

// This client's world is up and the host's puppet stands with a pose, within the bounds the drills use. A wait that runs out is an
// INVALID (peer_signal's); false then.
bool HostPuppetUp();

// The walker's own anger route, `fireDamage(1.0)`: it sets the walker's `angry` flag, and only an angry walker hits (its tick
// reaches the overlap and the damage only when `angry` is set and `looking` is not). False when the call did not dispatch.
// ReadAngry reads that flag; false when the walker or the field does not read. Game thread.
bool AngerWalker(void* walker);
bool ReadAngry(void* walker, bool& angry);

float Dist3(const ue_wrap::FVector& a, const ue_wrap::FVector& b);

// This player's view on `walker`: toward it, or turned the other way, so that it is not in this view (the camera behind it).
// Keeps the player fed (a ragdoll turns no view). Game thread.
void Aim(void* walker, bool toward);

// This player's distance to `actor`, negative when either place does not read. Game thread.
float PlayerTo(void* actor);

// On the host: the client's puppet to `actor`, negative when either place does not read. The puppet counts once it is spawned and
// has a pose, the test walker_target's PuppetBody makes. Game thread.
float ClientTo(void* actor);

// This player's bed anchor, as the director's stage reads it; false when the base has no bed (the KPP fallback is not one).
bool BedAnchor(ue_wrap::FVector& out);

// After a bed-anchor walk: brings this player to the walker until the walker hunts it from inside its own range, which a bed
// anchor can leave outside. `hunted` is read on the game thread: "this walker hunts this player"; the hunt alone is not enough (a
// walker with nobody in range hunts the host), so the 3D distance must also be within kInRangeCm. Already so, no walk. Otherwise
// the director walks it toward the walker along the NavMesh and the walk ends on that state, read while it walks: the walker
// closes on its target whatever the route, so a distance chosen beforehand does not hold. Its reach (kStepReachCm, flat) is only
// where the walk gives up; the state then gets kHuntGraceS to show. False, with an INVALID naming the distance, when a place
// does not read or the walker never takes this player. `who` names the player in that text. Called from the drill's own thread.
bool EnterRange(void* walker, const char* who, bool (*hunted)(void* walker));

// The walker's view test with each part told apart (ue_wrap::ambient_creature::WalkerViewParts): `seen` for CloseUntilSeen that,
// while the copy is not in view, logs `mannequin-drill: view parts: ...` at the first read and then every kPartsLogEveryS; and
// the same line's text for a verdict. A part not read says -1. Game thread.
inline constexpr int kPartsLogEveryS = 5;
bool InViewReportingParts(void* copy);
std::string ViewPartsOf(void* copy);

// A walker behind a wall is not seen (the game's own rule: it is not rendered), and a bed anchor inside the house puts the target
// behind one. Aims this player's view at the copy `copyOf` names (read on the game thread) and reads `seen` of it; while it is not
// seen, walks toward it along the NavMesh without stopping (a leg of at most kCloseLegS, reading the view as it goes with the walk's
// own facing, ending at once on a copy in view or within kCloseReachCm; a mesh past its draw distance is drawn only once the walk has
// closed in), lets go, and aims and reads again for kAimPollMs (a render needs a frame after the aim), for at most boundS in all.
// The director's walk faces its own way, so aim and walk alternate by leg. Never walks to within kApproachEndCm. True when seen; false with `lastCm` (distance to the copy at the last read) and `lastSeen` otherwise.
// Logs the two places at the first read that is not seen. Called from the drill's own thread.
bool CloseUntilSeen(const std::function<void*()>& copyOf, bool (*seen)(void* copy), int boundS, float& lastCm, bool& lastSeen);

// The bot director walks this player, staged at a known-good anchor when the route is long (the director's stage); true when it
// arrived within `reachCm`.
bool WalkTo(const ue_wrap::FVector& to, float reachCm, const char* what);

}  // namespace coop::dev::mannequin_walker_drill
