// coop/dev/mannequin_walker_geometry.h -- the walker drill's places, views and walks (coop/dev/mannequin_walker_drill).
//
// Where a player stands against a walker, where it looks, and where the bot director walks it: the drill's measurements are
// judged in the drill, and everything it asks of the world's geometry is here. Every function but the templates reads or drives
// the engine and must run on the game thread unless it says otherwise (WalkTo and BedAnchor are called from the drill's own
// thread and hop onto it themselves).

#pragma once

#include "harness/autotest/peer_signal.h"

#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/types.h"

#include <atomic>
#include <cstdint>

namespace coop::dev::mannequin_walker_drill {

// The drill's name on its `rig: VERDICT` line, and the client's slot on the host.
inline constexpr const char* kTag = "mannequin";
inline constexpr uint8_t     kClientSlot = 1;

// A director walk's own deadline (after the stage), and how long the drill waits for one to end.
inline constexpr int kWalkDeadlineS = 400;
inline constexpr int kWalkWaitS = kWalkDeadlineS + 90;

// The walk's reach of a point it is placed at: within it the director's stage places the walk at once (a bed anchor, the KPP gate).
inline constexpr float kAnchorReachCm = 500.f;
// Where a walk toward a walker gives up: inside the walker's own hunt range with a margin (it normally ends sooner, on the hunt).
inline constexpr float kInRangeCm = 17000.f;
// How long the walker gets, after that walk's end, to show that it took the player (it reads its range every half second).
inline constexpr int kHuntGraceS = 5;

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

// After a bed-anchor walk: brings this player to the walker until the walker TAKES it as its target, which a bed anchor can leave
// outside the walker's range. `hunted` is read on the game thread: "this walker hunts this player". Already hunted, no walk.
// Otherwise the director walks it toward the walker along the NavMesh and the walk ends on the event, not on a distance: the
// walker closes on its target whatever the route, so a distance chosen beforehand does not hold. Its reach (kInRangeCm, flat) is
// only where the walk gives up; the hunt then gets kHuntGraceS to show. False, with an INVALID naming the distance, when a place
// does not read or the walker never takes this player. `who` names the player in that text. Called from the drill's own thread.
bool EnterRange(void* walker, const char* who, bool (*hunted)(void* walker));

// The bot director walks this player, staged at a known-good anchor when the route is long (the director's stage); true when it
// arrived within `reachCm`.
bool WalkTo(const ue_wrap::FVector& to, float reachCm, const char* what);

}  // namespace coop::dev::mannequin_walker_drill
