// coop/interactables/atv_sync_internal.h -- the per-ATV record, shared by the ATV lane's TUs.
//
// Born with the 2026-08-30 corrector extraction. atv_sync.cpp owns the map of these and every
// structural change to it; atv_corrector.cpp mutates the fields of ONE entry when a packet
// arrives. Nothing outside coop/interactables/ includes this -- the lane's public surface is
// coop/interactables/atv_sync.h.

#pragma once

#include "ue_wrap/core/types.h"

#include <chrono>
#include <cmath>
#include <cstdint>

namespace coop::atv_sync {

struct AtvEntry {
    void*   actor = nullptr;
    int32_t idx   = -1;
    uint64_t lastSentMs   = 0;
    uint64_t lastPktMs    = 0;     // arrival of the last correcting packet -- the corrector's dt
    uint8_t  occupantSlot = 0xFF;  // the SEATED driver's peer slot (0xFF = seat free). THE SEAT --
                                   // device_occupancy::IsOccupiedByOther reads this and denies an
                                   // E-press from it, so a grabber must never appear here.
    uint8_t  authorSlot   = 0xFF;  // who STREAMS it (driver or grabber; 0xFF = nobody -> host syncs)
    bool     wasPoseAuthor = false;// POSE authority last tick -- the release-edge detect, and it is
                                   // deliberately not the same variable as ownsTick (PR #9)
    bool     isClientSpawnedMirror = false;  // v77: a runtime ATV WE fresh-spawned (AtvSpawn) -> K2 on destroy
    // idle-syncer change gate
    ue_wrap::FVector  lastSyncPos{};
    ue_wrap::FRotator lastSyncRot{};
    uint64_t lastIdleSendMs = 0;   // last ACTUAL idle send -- the keepalive floor's clock
    float    lastErrCm    = -1.f;  // the previous packet's position error -- the stall detector
    int      stallPackets = 0;     // consecutive packets in which the error refused to shrink
    bool     haveLastSync = false;
    int      restReplaces = 0;     // consecutive re-places of a mirror whose author is at rest.
                                   // A corrector owes a convergence check on EVERY arm it has
                                   // (docs/LESSONS.md); this is the at-rest arm's. If putting the
                                   // rig on the author's pose and then leaving it alone still does
                                   // not hold, the difference is not something a pose lane can
                                   // close and the lane must say so instead of teleporting for
                                   // the rest of the session.
};

// Vector length. Shared rather than duplicated: the corrector measures the position error with
// it and the idle syncer's change gate measures movement with it, and two copies of a distance
// is how two halves of one lane end up disagreeing about how far apart something is.
inline float Len(const ue_wrap::FVector& v) { return std::sqrt(v.X * v.X + v.Y * v.Y + v.Z * v.Z); }

// One clock for the whole lane. Inline rather than duplicated: both TUs time packet arrival, and
// two copies of a clock read is how two lanes end up disagreeing about what "now" is.
inline uint64_t NowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

}  // namespace coop::atv_sync
