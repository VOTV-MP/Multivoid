// coop/interactables/atv_sync_internal.h -- the per-ATV record, shared by the ATV lane's
// translation units. atv_sync.cpp owns the map of these and every structural change to it;
// atv_authority.cpp judges over it (the host's table: the seat and the syncer) and writes those
// fields; atv_corrector.cpp mutates the fields of ONE entry when a packet arrives, and
// atv_runtime_mirror.cpp asks for its two changes through the calls at the end, and atv_index.cpp
// fills the map through the two globals under detail. Nothing outside coop/interactables/ includes this but the
// explosion request lane's ATV family (coop/world/explosion_request_atv.cpp), which judges over an entry's syncer and
// writes its `blown` through FindEntryMutable -- the lane's public surface is coop/interactables/atv_sync.h.

#pragma once

#include "coop/interactables/atv_authority.h"  // AtvRequest
#include "coop/net/intent_bucket.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/devices/atv_condition.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace coop::net { class Session; struct AtvSpawnPayload; struct AtvStatePayload; }

namespace coop::atv_sync {

struct AtvEntry {
    void*   actor = nullptr;
    int32_t idx   = -1;
    uint64_t lastSentMs   = 0;
    uint64_t lastPktMs    = 0;     // arrival of the last correcting packet -- the corrector's dt
    // ---- the host's table: what the host decided, on every peer (the host writes it by its judgement,
    // a client by the host's stamp and answers). Three concepts, never aliased: the seat, the syncer, the
    // local body's own sit and carry below. ----
    uint8_t  occupantSlot = 0xFF;  // the SEATED driver's peer slot (0xFF = seat free). THE SEAT --
                                   // device_occupancy::IsOccupiedByOther reads this and denies an
                                   // E-press from it, so a grabber must never appear here.
    uint8_t  syncerSlot   = 0;     // who WRITES it (0 = the host, k = client slot k): the one whose blocks
                                   // the host applies and re-sends, whose copy owns the tick
    uint8_t  passengerSlot = 0xFF; // the peer slot in the passenger spot behind the driver (0xFF = free). A
                                   // passenger neither syncs the ATV nor denies the driver's seat.
    // Host only: the judgement's clocks. A block, a grant or a pass moves them (atv_authority.cpp).
    uint8_t  prevSyncerSlot = 0xFF; // the client a hand-back just replaced, for the late-block count
    uint64_t lastBlockMs  = 0;     // when the syncer's last block was applied (a grant counts as one)
    uint64_t restSinceMs  = 0;     // since when the syncer's blocks show it at rest (0 = moving)
    uint64_t handBackMs   = 0;     // when the last hand-back to the host happened
    coop::net::IntentBucket streamBucket;  // the syncer's blocks only; reset on every syncer change
    ue_wrap::FVector lastRestPos{};  // the last block's position, for the rest test
    float    lastBlockSpeed = 0.f;   // the last admitted block's linear speed, cm/s (a drill reads it)
    bool     haveRestPos  = false;
    bool     lastGrabbed  = false;   // the last block's grabbed bit: the syncer carries it
    bool     snapshotReached = false; // client: a connect-snapshot block (adopt != 0) reached the corrector for this ATV (a drill reads it)
    bool     stampDue     = false;   // the table changed: one stamped block goes to every ready client
    uint64_t stampRetryMs = 0;       // host: before this a stamp whose payload did not read is not tried again
    // The story event's trap (ATV_C's `trap`), the host's state carried in the stamp's bit 4 (kAtvTrap). Cleared with
    // the table.
    bool     hostTrap     = false;   // host: this copy's trap as of the last Tick; a change owes a stamp
    bool     stampTrap    = false;   // client: the last stamp's bit
    bool     haveStampTrap = false;  // client: a stamp has been seen (the first one writes the level)
    bool     copyTrapSeen = false;   // client: this copy's trap as of the last Tick (the explosion request reads it)
    // Host: this ATV's syncer reported its explosion and the host has accepted the request; no second one is taken
    // until the game's rule re-arms it: the slot that blew it (`blownBy`) reports health at 0 or above; any other
    // syncer, whose copy may be stale, must show a rise from below 0 inside ITS stream (`lastSyncerHealth` is that
    // stream's previous health, and a syncer change clears `haveSyncerHealth`, so its first block never re-arms). The
    // explosion request lane writes `blown` and `blownBy` (0xFF = none), through FindEntryMutable; the host's block
    // apply and Tick re-arm it. Cleared with the table.
    bool     blown        = false;
    uint8_t  blownBy      = 0xFF;
    float    lastSyncerHealth = 0.0f;
    bool     haveSyncerHealth = false;
    // This machine's own body, host and client alike (atv_body.cpp writes them, nothing else does).
    bool     localSeated  = false;   // the local player sits in this ATV
    bool     localCarrying = false;  // the local player's grav hand holds it
    bool     sentSeated   = false;   // what this machine last told the host (the host's own table, for itself)
    bool     takeoverDue  = false;   // a rising carry edge not yet sent
    uint64_t yieldRetryMs = 0;       // before this a seat or carry yield that did not run is not tried again
    uint8_t  pendingSeq   = 0;       // the latest request sent and not yet answered (0 = none)
    coop::atv_authority::AtvRequest pendingAction = coop::atv_authority::AtvRequest::Sit;
    bool     isClientSpawnedMirror = false;  // an ATV mirror WE fresh-spawned (AtvSpawn)
    // idle-syncer change gate
    ue_wrap::FVector  lastSyncPos{};
    ue_wrap::FRotator lastSyncRot{};
    uint64_t lastIdleSendMs = 0;   // last ACTUAL idle send -- the keepalive floor's clock
    float    lastErrCm    = -1.f;  // the previous packet's position error -- the stall detector
    int      stallPackets = 0;     // consecutive packets in which the error refused to shrink
    bool     haveLastSync = false;
    uint64_t lastRestPlaceMs = 0;  // when the last one happened -- the episode's clock, not the
                                   // send's. Counting CONSECUTIVE re-places does not bound
                                   // anything: a teleport lands the rig exactly on the syncer's
                                   // pose, so the very next packet is the one most likely to be
                                   // in band, which zeroes the count. Measured: a give-up
                                   // bounded at three fired three times in 46 seconds.
    // ---- the condition lane (atv_condition_sync.cpp mutates these on ONE entry) ----
    // The LAST-EXPRESSED baseline: the condition state as of the last time each reducer verb ran,
    // or the actor's own state at the first apply. It is seeded from the ACTOR and never from
    // zero, because a zero seed fires updTires' BreakConstraint x8 on a settled correct rig, and
    // a baseline advanced per packet starves updDirt forever on slow drift.
    ue_wrap::atv_condition::Snapshot condExpressed{};
    bool  condExpressedSeeded = false;
    void* condExpressedActor  = nullptr;  // reseed when the entry repoints to a new actor
    // Idle-syncer change gate, condition half: the payload's condition block as of the last
    // ACTUAL send. Pose-only gating would make a parked host eject wait out the 2 s keepalive.
    uint8_t lastSentCond[64] = {};
    bool    haveLastSentCond = false;
    int      restReplaces = 0;     // re-places of a mirror whose syncer is at rest, WITHIN one
                                   // episode (see lastRestPlaceMs).
                                   // A corrector owes a convergence check on EVERY arm it
                                   // has, and this is the at-rest arm's: if putting the rig
                                   // on the syncer's pose and then leaving it alone still
                                   // does not hold, the difference is not something a pose
                                   // lane can close, and the lane must say so instead of
                                   // teleporting for the rest of the session.
    // ---- the index (atv_index.cpp sets these; later fields of that kind append below) ----
    void*    world = nullptr;      // the world the actor was last found in (its UWorld, as an identity, never
                                   // dereferenced): the host sends AtvDestroy for a row it prunes in the
                                   // world it is in now (its actor died, or stands under another key), and
                                   // prunes silently a row whose world was left.
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

// The cadence of a syncer's stream while it drives or carries (about 20 Hz). The host's late-block window
// (atv_authority.cpp) is counted in these.
inline constexpr uint64_t kDriveSendMs = 50;

// A yield or a stamp that did not run is tried again after this, per entry.
inline constexpr uint64_t kRetryMs = 1000;

// atv_body.cpp's surface for atv_sync.cpp's Tick: this machine's own body against the host's table. The host judges
// its own player (leave, then sit, then carry, then its yields, each step over every entry); a client reads its body
// for one entry, asks the host for what it did once `gateOpen` (its own world is ready) and yields to what the host
// decided. ResetRequestSeq restarts the client's request counter for a new session. Game thread.
void JudgeHostBody(void* localPlayer);
void TickClientBody(coop::net::Session& s, AtvEntry& e, const std::wstring& key, void* localPlayer,
                    uint8_t localSlot, bool gateOpen);
void ResetRequestSeq();

// atv_sync.cpp's surface for atv_runtime_mirror.cpp and atv_authority.cpp, a client receiving an announced ATV: the lane's session,
// whether the index belongs to this world, the row under a key, and that half's two structural changes -- a
// fresh-spawned mirror adopted under the host's game Key, a row dropped with its actor's key. FindEntryMutable is the
// explosion request lane's write path to one entry (its `blown`). Game thread.
coop::net::Session* LaneSession();
bool IndexCurrent();
const AtvEntry* FindEntry(const std::wstring& key);
AtvEntry* FindEntryMutable(const std::wstring& key);
void AdoptClientMirror(const std::wstring& key, void* actor);
void EraseEntry(const std::wstring& key);

// atv_runtime_mirror.cpp's surface for the rest of the lane: the arrival state of one session (the keys the host's
// announces named, the host's pending set, the counters), dropped at a disconnect; and the host's set, applied once
// this world is ready -- on its edge's arrival and again from the park's drain. Game thread.
void ResetArrivalState();
void ApplyPendingSet();
// An announced spawn, already counted in the host's set, resolved by key, parked or spawned: OnAtvSpawn's second
// half, and the park's hand-back, which is not a new arrival.
void ResolveSpawn(const std::wstring& key, const coop::net::AtvSpawnPayload& payload);

// Whether the local player's grav hand holds `actor` (its grabbing_actor; holding_actor is the hand item the player
// spawns, never a world ATV): the one carry read the lane's Tick and the host's takeover test share. False for a null
// player or actor. Game thread.
bool IsLocalGrabber(void* actor, void* localPlayer);

// atv_stream_gates.cpp: the stream's per-block gates, each a function of one entry and its actor and none touching the
// lane's map. StampHost sets a block's passenger spot from the host's table and its trap bit from the host's own copy; TakeStampTrap is the client's take of
// that bit; NoteSyncerHealth is the host's re-arm of an accepted explosion request; IdleWorthSending is the idle
// syncer's change gate (it updates the entry's baseline on a yes). Game thread.
void StampHost(const AtvEntry& e, coop::net::AtvStatePayload& p);
void TakeStampTrap(AtvEntry& e, bool stamped, bool owns);
void NoteSyncerHealth(AtvEntry& e, uint8_t syncer, float health);
bool IdleWorthSending(AtvEntry& e);

}  // namespace coop::atv_sync
