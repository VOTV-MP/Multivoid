// coop/interactables/atv_stamp.cpp -- the stamp half of the ATV lane (atv_sync.h): the payload a block carries read from
// a live ATV, the host's read of its copies' trap bit, and the host's stamped block to every world-ready client for each
// entry whose table changed. atv_sync.cpp owns the map these read and the stream that calls them.

#include "coop/dev/atv_riders_arm.h"
#include "coop/interactables/atv_drill_events.h"
#include "coop/interactables/atv_sync_internal.h"
#include "coop/interactables/atv_condition_sync.h"
#include "coop/interactables/atv_host_sync.h"
#include "coop/interactables/atv_index_internal.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"  // WireKeyFromString
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/types.h"      // FVector, FRotator
#include "ue_wrap/devices/atv.h"
#include "ue_wrap/engine/engine.h"   // GetActorRootPhysicsVelocity

#include <cstdint>
#include <cstring>
#include <string>

namespace coop::atv_sync {

namespace R = ue_wrap::reflection;
namespace A = ue_wrap::atv;
using ue_wrap::FVector;
using ue_wrap::FRotator;
using coop::net::WireKeyFromString;
using detail::g_session;
using detail::g_atvs;

// An AtvStatePayload from a live read; false if the transform read fails. `grabbed` marks the
// sender's grav hand as holding it (bit 2); `occupantSlot` and `syncerSlot` are the table's values,
// the stamp a host block carries. The control bits are the entry's (BlockControlBits): the host's block of an ATV a client
// syncs carries that client's last horn level.
bool ReadPayload(AtvEntry& e, const std::wstring& key, uint8_t occupantSlot, uint8_t syncerSlot,
                 bool adopt, coop::net::AtvStatePayload& p, bool grabbed) {
    void* const actor = e.actor;
    const auto* session = g_session.load(std::memory_order_acquire);
    const bool host = session && session->role() == coop::net::Role::Host;
    FVector loc; FRotator rot;
    if (!A::GetRootTransform(actor, loc, rot)) return false;
    FVector lin{}, ang{};
    // Best effort: a failed read leaves zeros, the honest value and also a body at rest.
    ue_wrap::engine::GetActorRootPhysicsVelocity(actor, lin, ang);
    std::memset(&p, 0, sizeof(p));
    WireKeyFromString(key, p.key);
    p.x = loc.X; p.y = loc.Y; p.z = loc.Z;
    p.pitch = rot.Pitch; p.yaw = rot.Yaw; p.roll = rot.Roll;
    p.linVelX = lin.X; p.linVelY = lin.Y; p.linVelZ = lin.Z;
    p.angVelX = ang.X; p.angVelY = ang.Y; p.angVelZ = ang.Z;
    p.occupantSlot = occupantSlot;
    p.syncerSlot   = syncerSlot;
    uint8_t sb = BlockControlBits(e, host && syncerSlot != 0);
    if (A::IsDriven(actor)) sb |= 0x1;
    if (grabbed)            sb |= 0x4;
    p.stateBits = sb;
    p.adopt = adopt ? 1 : 0;
    // The riding kerfur is the host's word: every block it sends names it, a client's carries 0.
    static const bool s_redStamp = coop::dev::atv_riders_arm::Is("rider_stamp_off");   // the riders drill's red
    p.riderEid = (host && !s_redStamp) ? e.seatRider : 0;
    // The condition block (tires, spare, dirt, fuel, health), filled at this one site so the
    // syncer, the idle syncer and the adopt seed cannot fork; a failed read leaves it zeroed
    // with tiresValid 0, and receivers touch nothing.
    coop::atv_condition_sync::FillPayload(actor, p);
    // Its vitals ack: the host's own blocks carry its copy's current state sequence, a client's the newest state whose
    // vitals it applied (atv_host_sync.h).
    p.ackSeq = host ? coop::atv_host_sync::CurrentStateSeq(key) : coop::atv_host_sync::AppliedStateSeq(key);
    coop::atv_host_sync::NoteBlockBuilt(key);   // [dev] the drill's held state is applied after this block's ack was read
    if (host && coop::atv_drill_events::On())
        coop::atv_drill_events::Note(coop::atv_drill_events::kStampBuilt, coop::atv_drill_events::KeyTag(key),
                                     static_cast<int32_t>(p.riderEid), 0);
    return true;
}

// HOST: each live entry's own `trap` against the last one seen; a change owes a stamp (bit 4 sits outside the condition's
// change gate, so an idle ATV would otherwise carry it only at the keepalive).
void ReadHostTraps() {
    for (auto& kv : g_atvs) {
        AtvEntry& e = kv.second;
        bool trap = false;
        if (!R::IsLiveByIndex(e.actor, e.idx) || !A::ReadTrap(e.actor, trap) || trap == e.hostTrap) continue;
        e.hostTrap = trap;
        e.stampDue = true;
    }
}

// gets the connect snapshot instead, and none of the stream.
void SendToReadyClients(coop::net::Session& s, const coop::net::AtvStatePayload& p, int skipSlot) {
    for (uint8_t slot = 1; slot < coop::net::kMaxPeers; ++slot)
        if (slot != skipSlot && s.IsSlotWorldReady(slot))
            s.SendReliableToSlot(slot, coop::net::ReliableKind::AtvState, &p, sizeof(p));
}

// HOST: one stamped block to every world-ready client for each entry whose table changed. A payload that does not
// read leaves the stamp due and is tried again once a second per entry, said once. The stamp goes to every client; MTA
// sends StartSync / StopSync to the syncer alone
// (reference/mtasa-blue/Server/mods/deathmatch/logic/CUnoccupiedVehicleSync.cpp:194-222).
void SendStamps(coop::net::Session& s, uint64_t nowMs) {
    for (auto& kv : g_atvs) {
        AtvEntry& e = kv.second;
        if (!e.stampDue || nowMs < e.stampRetryMs || !R::IsLiveByIndex(e.actor, e.idx)) continue;
        coop::net::AtvStatePayload p{};
        if (!ReadPayload(e, kv.first, e.seatOccupant, e.seatSyncer, /*adopt*/false, p)) {
            e.stampRetryMs = nowMs + kRetryMs;
            static bool sSaid = false;
            if (!sSaid) { sSaid = true;
                UE_LOGW("atv: the stamp of '%ls' did not read its transform -- tried again each second (said once)",
                        kv.first.c_str()); }
            continue;
        }
        e.stampRetryMs = 0;
        StampHostTrap(e, p);
        SendToReadyClients(s, p, -1);
        coop::atv_condition_sync::NoteSent(e, p);
        e.stampDue = false;
    }
}

}  // namespace coop::atv_sync
