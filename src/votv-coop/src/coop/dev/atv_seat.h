// coop/dev/atv_seat.h -- seats the local player in an ATV through the game's own seat verb, for the drills and probes
// that need a driver. Private to coop/dev. Dev only; game thread.
//
// The verb is ATV_C::actionName(player, hit, "sit"), gated three deep: the player's fall velocity, empty hands
// (checkEquip) and an unoccupied seat overlap. It needs no proximity: its body attaches the player to the seat and
// possesses the ATV. The seat is refused, not queued, while the player is mid-fall, so a refusal is worth a couple of
// retries.

#pragma once

#include <cstdint>

namespace coop::dev::atv_seat {

inline constexpr int      kSitRetries = 3;
inline constexpr uint64_t kSitRetryMs = 2000;

// One caller's attempts at one ATV: the verb tries made, the time before which the next verb try waits, and whether
// a call left the ATV driven (the verb is not called again after that). The caller owns it and starts a new seating
// with a fresh one.
struct SitTry {
    int      tries = 0;
    uint64_t nextMs = 0;
    bool     called = false;
};

// One step of the seating. True once the attempt is finished: the local peer's seat is `atv` in the ATV lane's index
// (atv_sync::ActorSeatedBy), the seat the host granted, arriving in its stamp or its answer, or it gave up (the verb
// refused kSitRetries times, or the local seat the verb took was lost before the grant came: the host refused and
// the lane yielded it, said in a WARN). False while it is not yet: a verb try pending its gap, or the grant not yet
// arrived for the seat the verb has taken, which is awaited as that edge and not as a count of steps.
bool TrySit(void* player, void* atv, SitTry& t);

// ATV_C::actionName(player, hit, verb) on `atv`, the verb the radial's named actions end in ("sit", "upgrades"); the hit
// stays zeroed. False when the function does not resolve, its frame does not build or the call fails. Game thread.
bool CallActionName(void* player, void* atv, const wchar_t* verb);

}  // namespace coop::dev::atv_seat
