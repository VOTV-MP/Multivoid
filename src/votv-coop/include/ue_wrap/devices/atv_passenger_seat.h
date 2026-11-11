// ue_wrap/devices/atv_passenger_seat.h -- the ATV's passenger spot, behind the driver where a riding kerfur sits. The
// game has no passenger seat; a player takes the spot through the game's own chair sit (mainPlayer_C::sit), on a
// sitBox_C attached to the ATV. The sit attaches the player to the ATV, takes its movement and turns the mouse into a
// head turn; the player's own dismount key runs its `unsit`, which sets the player down at the box's get-off point.
// Engine-wrapper layer: each call marshals the game's own functions and fields, with no coop state. Game thread.

#pragma once

#include "ue_wrap/core/types.h"

namespace ue_wrap::atv_passenger_seat {

// A seated passenger's eye over the ATV's root: the driver's own camera stands 135 cm over it.
inline constexpr float kEyeAboveRootCm = 140.f;

// Where the game's sit puts a passenger's capsule centre: 60 cm behind `sitarrow` along the ATV's facing, on the back
// half of the seat, and 170 cm above it; turned as the ATV is (yaw only). False on null or an unreadable ATV.
bool SitBoxTransform(void* atv, FVector& loc, FRotator& rot);

// Where a remote passenger's body goes in the AnimBP's ATV seat pose: under the sit box, 25 cm above `sitarrow`, the
// height the driver's body rides at. False as SitBoxTransform.
bool BodyTransform(void* atv, FVector& loc, FRotator& rot);

// A drill's sweep of the seat's place: the sit box `aheadCm` ahead of `sitarrow` and `upCm` above it. Game thread.
void SetSeatOffsetsForDrill(float aheadCm, float upCm);

// A sitBox_C at SitBoxTransform, attached to the ATV where it stands, whose get-off point is 120 cm to the ATV's right.
// Null when the class is not loaded, the ATV does not read or the spawn fails.
void* SpawnSitBox(void* atv);

// The player's own sit on `atv` at `box` (mainPlayer_C::sit with no sitting component). The sit refuses itself, with
// the game's hint, when the ATV moves faster than 2 m/s, lies tipped, or a trace from the player to the box is
// blocked. True when the call ran; ReadMainPlayerSittingOn says whether it took.
bool Sit(void* player, void* atv, void* box);

// The player's own `unsit`: detached, set down at the box's get-off point, walking again. True when the call ran.
bool Unsit(void* player);

}  // namespace ue_wrap::atv_passenger_seat
