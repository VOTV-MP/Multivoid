// coop/interactables/atv_stream_gates.cpp -- the ATV stream's per-block gates (declared in atv_sync_internal.h): what a
// block carries or skips, read off ONE entry and the actor behind it. The host's trap bit stamped into a block, the
// client's take of that bit, the take of the brake, lights, turbo and horn bits and the syncer's side of them (the bits a
// block carries, when the next one is due, the horn latch), the re-arm of an accepted explosion request, and the idle
// syncer's change gate. None of them touches the lane's map or its state but NoteHorn, which finds its entry by key;
// atv_sync.cpp calls them where it builds, applies and paces the stream.

#include "coop/interactables/atv_sync_internal.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/interactables/atv_drill_events.h"
#include "coop/interactables/atv_host_sync.h"
#include "coop/interactables/atv_sync.h"

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

// HOST: the bit-4 mark of a block it sends is this copy's own `trap` (the host's state, which every client takes from the
// stamp; a client's bit is never read). Set on the whole byte by mask; `e.actor` is live at every call. A trap that cannot
// be read stays host-only. MTA keeps the cause of a blow server-side too: the server marks the vehicle awaiting its
// explosion (reference/mtasa-blue/Server/mods/deathmatch/logic/CStaticFunctionDefinitions.cpp:5617) and the client
// reports the blow bit back (reference/mtasa-blue/Client/mods/deathmatch/logic/CClientGame.cpp:5380); here the trap
// is the host's state.
void StampHostTrap(const AtvEntry& e, coop::net::AtvStatePayload& p) {
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

// The brake, lights, turbo and horn bits of a block this peer took (the host: an admitted client syncer's; a client: the
// host's, its own copy's or the re-sent syncer's). A bit is applied when it differs from the last block's -- the first
// block applies every bit that differs from the copy -- so a copy whose own battery put the lights out is not forced on
// again by every block. The horn has no state on the copy to compare with, so it is compared with the last level replayed
// (the memory in controlBits, which the first block of an entry shares): a change replays the ATV's own press or release,
// which sounds it and leaves the copy's own controls alone (ue_wrap::atv::ReplayHorn). Never called for an ATV this peer
// syncs: its own copy is where the bits are written from.
void TakeControls(AtvEntry& e, uint8_t stateBits) {
    constexpr uint8_t kHorn = coop::net::kAtvHorn;
    constexpr uint8_t kCopyBits = static_cast<uint8_t>(coop::net::kAtvControlBits & ~kHorn);
    const uint8_t bits = stateBits & coop::net::kAtvControlBits;
    const uint8_t hornChanged = static_cast<uint8_t>((bits ^ e.controlBits) & kHorn);
    const uint8_t changed = static_cast<uint8_t>(
        (e.haveControlBits ? static_cast<uint8_t>((bits ^ e.controlBits) & kCopyBits) : kCopyBits) | hornChanged);
    e.controlBits = bits;
    e.haveControlBits = true;
    const bool lights = (bits & coop::net::kAtvLights) != 0;
    const bool brake = (bits & coop::net::kAtvBrake) != 0;
    const bool turbo = (bits & coop::net::kAtvTurbo) != 0;
    // Red atv_bits_off: the block's control bits are ignored.
    static const bool s_bitsOff = coop::config::ResolveEnum(::coop::config_registry::rows::atv_gate_drill) == "atv_bits_off";
    if (s_bitsOff) {
        if (changed) coop::atv_drill_events::Note(coop::atv_drill_events::kControls, changed, 0, 0, bits);
        return;
    }
    uint8_t written = 0;   // the bits this block made the copy change: a block the copy already agreed with writes none
    if ((changed & coop::net::kAtvLights) && lights != A::GetLights(e.actor)) {
        A::SetLights(e.actor, lights);
        written |= coop::net::kAtvLights;
    }
    if ((changed & coop::net::kAtvBrake) && brake != A::GetBrake(e.actor)) {
        A::SetBrake(e.actor, brake);
        written |= coop::net::kAtvBrake;
    }
    if ((changed & coop::net::kAtvTurbo) && turbo != A::GetTurbo(e.actor)) {
        A::SetTurbo(e.actor, turbo);
        written |= coop::net::kAtvTurbo;
    }
    uint8_t hornSounded = 0;
    if (hornChanged) {
        coop::atv_host_sync::ApplyScope applying;   // our own replay is no key press
        if (A::ReplayHorn(e.actor, (bits & kHorn) != 0)) hornSounded = kHorn;
    }
    if (changed) {
        // [dev] the drill's event: which of the changed bits the copy now shows as the block says, and which it was written;
        // the horn is shown when its replay ran, and the block's bits ride f0 for the level.
        uint8_t equal = hornSounded;
        if (lights == A::GetLights(e.actor)) equal |= coop::net::kAtvLights;
        if (brake == A::GetBrake(e.actor)) equal |= coop::net::kAtvBrake;
        if (turbo == A::GetTurbo(e.actor)) equal |= coop::net::kAtvTurbo;
        coop::atv_drill_events::Note(coop::atv_drill_events::kControls, changed, changed & equal, written | hornSounded, bits);
    }
}

// The control bits a block built from `e`'s copy carries. The copy's own brake, lights and turbo; the horn is the key's
// level, which the copy cannot say: the BP writes `input_alt` only in the honk key's two events and never clears it when the
// seat ends, so a key held at the dismount would read down for ever. The level is the honk rows' (NoteHorn) on a seated
// copy, or a press no block has carried yet; the host's copy of an ATV a client syncs carries that client's last admitted
// level (`ofClient`), which a joiner's snapshot needs.
uint8_t BlockControlBits(AtvEntry& e, bool ofClient) {
    uint8_t bits = 0;
    if (A::GetBrake(e.actor)) bits |= coop::net::kAtvBrake;
    if (A::GetLights(e.actor)) bits |= coop::net::kAtvLights;
    if (A::GetTurbo(e.actor)) bits |= coop::net::kAtvTurbo;
    if (ofClient) return static_cast<uint8_t>(bits | (e.controlBits & coop::net::kAtvHorn));
    if (!e.localSeated) e.hornHeld = false;   // the seat ended: a key held then is up
    if (e.hornHeld || e.hornLatch) bits |= coop::net::kAtvHorn;
    return bits;
}

// The syncer's next block is due at once when its control bits differ from the last block's: once per change, since the
// block that goes records them (NoteControlsSent), so a key held down sends nothing more than the stream's own cadence.
bool ControlsDue(AtvEntry& e) { return BlockControlBits(e, false) != e.sentControlBits; }

// A block of this syncer went out carrying `stateBits`: its control bits are the new baseline, and a press it carried is
// delivered, so the latch clears with that block and with no timer.
void NoteControlsSent(AtvEntry& e, uint8_t stateBits) {
    e.sentControlBits = stateBits & coop::net::kAtvControlBits;
    if (stateBits & coop::net::kAtvHorn) e.hornLatch = false;
}

void ForgetHorn(AtvEntry& e) { e.controlBits &= static_cast<uint8_t>(~coop::net::kAtvHorn); }

// The honk key's press or release on a copy a player sits in (atv_gate's rows): the press is held and latched, the release
// lets the key go. A copy nobody sits in has no such key.
void NoteHorn(const std::wstring& key, bool down) {
    AtvEntry* e = FindEntryMutable(key);
    if (!e || !e->localSeated) return;
    e->hornHeld = down;
    if (down) e->hornLatch = true;
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
