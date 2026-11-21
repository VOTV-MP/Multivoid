// coop/player/pose_bases.h -- the movers a player's pose may name as the thing it stands on.
//
// A player's own engine always knows what it stands on (ue_wrap::character_base, ACharacter::BasedMovement.MovementBase). A
// pose that carries only the absolute position lags a body that moves (the cargo lift's platform rises 42 cm/s while the
// puppet is placed at the rider's last absolute pose, so its capsule presses the platform and the lift's own block test
// reverses it: the based-movement drill measured it). So a pose may name its base and the player's offset from it, and a
// receiver rides its puppet on the base's CURRENT place. MTA's shape: the contact entity and the position minus its position
// (CNetAPI.cpp:1117-1168 send, :859-991 receive), no rotation; contact there is limited to vehicles and objects
// (CClientPed.cpp:5183), as the table below is limited to what it lists.

#pragma once

#include "coop/net/protocol.h"
#include "ue_wrap/core/types.h"

#include <cstdint>

// A base is a registered MOVER, not any actor: this table names the classes a pose may carry and how each resolves. A world
// offset slides on a base that rotates (a door, a UFO), so a mover that rotates is a row of its own, with its rotation. Row 1:
// the cargo lift. The engine reports the platform, the lift's child actor `prop_cargolift_C` (the `lift` ChildActorComponent's
// child, cargoLift.cpp:23,202; the parent link is engine::ParentActorOf, engine.cpp:380); the pose names the PARENT
// `cargoLift_C` by FNV-1a 32 of the key the lift lane's interactable index files it under (interactable_sync's CargoLiftKey:
// the portable `mv_` key where one exists, else the level-export FName), and the reference point is the platform actor's
// location (the part that moves; the parent never does). Every other base is sent as none, said once per class.
namespace coop::pose_bases {

// Each peer files, from its own index, a map hash -> lift, rebuilt when the lane's index completes a pass or the world
// changes; two lifts on one hash make the hash unusable (both sent and resolved as none, said once). Game thread.

// The sender: the local player's base fields of `pose` (whose x, y, z are set), and the pad bytes zero. A player standing on a
// registered mover that resolves uniquely on this peer sends its id and the offset from the platform's location; everything
// else is none.
void FillBase(void* localPlayer, coop::net::PoseSnapshot& pose);

// A mover as this peer resolves it, for one call.
struct Resolved {
    void* lift = nullptr;        // the keyed lift actor (it never moves), live now
    void* actor = nullptr;       // the platform actor (the part that moves), live now
    ue_wrap::FVector origin{};   // its location now: the offset's reference point
};

// The receiver: the mover `baseId` names on this peer, resolved this call and never held across a tick. False when this peer's
// map holds none under that hash, the hash is unusable (two actors), or the mover is not live.
bool Resolve(uint32_t baseId, Resolved& out);

// The receiver's judgment of a pose's base claim from peer `slot`: true with the mover when the pose names one this peer
// resolves AND the claim is consistent with the pose's absolute position, false when the pose is to be placed by its absolute
// position (it names none, names one that does not resolve, or fails a check below, which is then counted).
//
// The checks are per axis and independent of the link: the lift moves on Z only, so the XY gap between the absolute position
// and (the platform's location now + the offset) must stay within kMaxXyGapCm whatever the link's lag; the absolute Z must lie
// within the lift's travel (its two stops, minus kBelowStopCm and plus kAboveStopCm for a standing player); the offset's Z
// must put the player's centre over the platform's origin, 0..kMaxOffsetZCm; the Z gap between the absolute pose and (platform +
// offset) must stay within the lift's peak speed times kMaxZGapSeconds, and the offset's XY inside the platform's own bounds plus
// kBoundsMarginCm. The same call judges a client's pose on the host
// and a relayed pose on every other client.
bool Claim(const coop::net::PoseSnapshot& pose, uint8_t slot, Resolved& out);

inline constexpr float kMaxXyGapCm = 30.f;
inline constexpr float kBelowStopCm = 50.f;
inline constexpr float kAboveStopCm = 250.f;
inline constexpr float kMaxOffsetZCm = 250.f;
inline constexpr float kMaxZGapSeconds = 1.f;    // the Z gap between the absolute pose and (platform + offset): the lift's peak speed times this
inline constexpr float kBoundsMarginCm = 50.f;   // the offset's XY lies inside the platform's own bounds plus this

// Why a claim was set aside, each counted per slot (the log says the first of each minute).
enum class Reject : uint8_t { XyGap, OffsetZ, Volume, ZGap, OffsetBounds };
// How many claims of `slot` were set aside for `why`, and how many passed, since the process began. A drill reads them.
uint32_t Rejected(uint8_t slot, Reject why);
uint32_t Accepted(uint8_t slot);

// [dev] A hostile client's claim: when set, every pose that names a mover is handed to `fn` after it is filled, which may
// rewrite the claim (the drill's bad_base arms). Null clears it.
using Forge = void (*)(coop::net::PoseSnapshot& pose);
void SetOutboundForge(Forge fn);

}  // namespace coop::pose_bases
