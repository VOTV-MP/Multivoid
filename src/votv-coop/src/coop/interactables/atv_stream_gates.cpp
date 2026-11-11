// coop/interactables/atv_stream_gates.cpp -- the ATV stream's per-block gates (declared in atv_sync_internal.h): what a
// block carries or skips, read off ONE entry and the actor behind it. The host's trap bit stamped into a block, the
// client's take of that bit, the re-arm of an accepted explosion request, and the idle syncer's change gate. None of
// them touches the lane's map or its state; atv_sync.cpp calls them where it builds, applies and paces the stream.

#include "coop/interactables/atv_sync_internal.h"

#include "coop/net/protocol.h"

#include "ue_wrap/devices/atv.h"
#include "ue_wrap/engine/engine.h"          // GetActorRootPhysicsVelocity
#include "ue_wrap/core/types.h"           // FVector, FRotator, NormalizeAxis

#include <cmath>
#include <cstdint>

namespace coop::atv_sync {

namespace A = ue_wrap::atv;
using ue_wrap::FVector;
using ue_wrap::FRotator;

namespace {

// The idle syncer's change gate (MTA's WriteVehicleInformation in our units): a parked ATV sends
// nothing.
constexpr float kIdleMovedCm   = 1.0f;
constexpr float kIdleTurnedDeg = 0.5f;
constexpr float kIdleMovingCmS = 1.0f;

}  // namespace

// HOST: the stamp's passenger spot, and the bit-4 mark of a block it sends is this copy's own `trap` (the host's state, which every client takes from the
// stamp; a client's bit is never read). Set on the whole byte by mask; `e.actor` is live at every call. A trap that cannot
// be read stays host-only. MTA keeps the cause of a blow server-side too: the server marks the vehicle awaiting its
// explosion (reference/mtasa-blue/Server/mods/deathmatch/logic/CStaticFunctionDefinitions.cpp:5617) and the client
// reports the blow bit back (reference/mtasa-blue/Client/mods/deathmatch/logic/CClientGame.cpp:5380); here the trap
// is the host's state.
void StampHost(const AtvEntry& e, coop::net::AtvStatePayload& p) {
    p.passengerSlot = e.passengerSlot;   // the passenger spot is the host's table too
    bool trap = false;
    A::ReadTrap(e.actor, trap);
    p.stateBits = static_cast<uint8_t>((p.stateBits & ~coop::net::kAtvTrap) | (trap ? coop::net::kAtvTrap : 0));
}

// CLIENT: the stamp's trap bit. A copy that does not own the tick takes the stamp's level on every stamp. One that owns
// it takes only a change, so the copy that just fired is not re-armed by a stamp sent before the host learned it fired,
// and the first stamp of an entry writes the level either way.
void TakeStampTrap(AtvEntry& e, bool stamped, bool owns) {
    const bool first = !e.haveStampTrap;
    const bool changed = !first && e.stampTrap != stamped;
    e.stampTrap = stamped;
    e.haveStampTrap = true;
    if (!owns || first || changed) A::WriteTrap(e.actor, stamped);
}

// HOST: the re-arm of an accepted explosion request. The game re-arms at health >= 0 (its explode test is the old value
// >= 0), so the slot that blew the ATV re-arms it at once on a report at 0 or above, which a trap death at exactly 0
// needs. Any other syncer needs a RISING EDGE inside its own stream (the health read before this one below 0, this one
// not), since its copy may be stale from before the death; its first read after a syncer change (haveSyncerHealth
// false) only records. The host is never `blownBy` (its own death needs no request). A NaN stays blown. MTA bumps a
// sync time context on its re-arm
// (reference/mtasa-blue/Server/mods/deathmatch/logic/CStaticFunctionDefinitions.cpp:5585-5589, so a report in flight
// from before it is dropped, CUnoccupiedVehicleSync.cpp:285); here the edge for a stale copy serves.
void NoteSyncerHealth(AtvEntry& e, uint8_t syncer, float health) {
    if (e.blown && health >= 0.0f &&
        (syncer == e.blownBy || (e.haveSyncerHealth && e.lastSyncerHealth < 0.0f))) {
        e.blown = false;
        e.blownBy = 0xFF;
    }
    e.lastSyncerHealth = health;
    e.haveSyncerHealth = true;
}

// The idle syncer's change gate: true if the ATV moved, turned or is moving; the baseline updates
// on a yes, so a slow drift accumulates into a send instead of being rounded away.
bool IdleWorthSending(AtvEntry& e) {
    FVector loc; FRotator rot;
    if (!A::GetRootTransform(e.actor, loc, rot)) return false;
    FVector lin{}, ang{};
    ue_wrap::engine::GetActorRootPhysicsVelocity(e.actor, lin, ang);
    bool send = !e.haveLastSync || Len(lin) > kIdleMovingCmS;
    if (!send) {
        const FVector d{ loc.X - e.lastSyncPos.X, loc.Y - e.lastSyncPos.Y, loc.Z - e.lastSyncPos.Z };
        send = Len(d) > kIdleMovedCm ||
               std::fabs(ue_wrap::NormalizeAxis(rot.Pitch - e.lastSyncRot.Pitch)) > kIdleTurnedDeg ||
               std::fabs(ue_wrap::NormalizeAxis(rot.Yaw   - e.lastSyncRot.Yaw))   > kIdleTurnedDeg ||
               std::fabs(ue_wrap::NormalizeAxis(rot.Roll  - e.lastSyncRot.Roll))  > kIdleTurnedDeg;
    }
    if (send) { e.lastSyncPos = loc; e.lastSyncRot = rot; e.haveLastSync = true; }
    return send;
}

}  // namespace coop::atv_sync
