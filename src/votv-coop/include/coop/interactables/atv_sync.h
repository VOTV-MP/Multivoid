// coop/interactables/atv_sync.h -- ATV/quadbike (AATV_C) rig sync.
//
// Gameplay/network layer (principle 7): owns the wire protocol, the stream and its stamp, the
// per-tick pump, the receiver-side CORRECTOR, the key->actor index, and the connect-snapshot. The
// seat and the syncer are the host's table (atv_authority.h). Talks to the engine ONLY through
// ue_wrap::atv.
//
// A peer that does not sync an ATV does NOT freeze it (MTA CNetAPI::ReadVehiclePuresync +
// CClientVehicle::UpdateTargetPosition + CUnoccupiedVehicleSync). It leaves the rig simulating
// natively and corrects it toward the syncer. AATV_C is a five-body constraint rig whose visible
// output IS suspension travel, so the freeze-and-teleport shape moved the body 29.58 cm where the
// native travel is 2-4 cm, and handed the other peer's copy a 158 cm/s launch on release.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace coop::net {
class Session;
struct AtvStatePayload;
struct AtvAnswerPayload;
struct AtvSpawnPayload;
struct AtvDestroyPayload;
struct AtvSetCompletePayload;
}  // namespace coop::net

namespace coop::atv_sync {

// Resolve AATV_C + build the key->actor index; store the session pointer. Idempotent; retried
// every net-pump tick until the class loads. Game thread.
//
// Identity is the ATV's own game Key on every peer -- a keyed pose stream, not the
// eid/Element/spawn machinery (YAGNI for one always-present keyed actor; the grime and window-dirt
// syncs made the same call). The host announces every ATV its world holds to a joiner and each one its
// index gains (AtvSpawn / AtvDestroy); a joiner adopts the copy its world holds under that Key, else a mirror is
// spawned carrying it.
void Install(coop::net::Session* session);

// Receiver entry: an AtvState packet arrived (payload already memcpy'd + range-checked by
// event_feed). On the HOST it is a client's block: applied to the host's copy only if the sender is the
// ATV's syncer, then re-sent to the other clients stamped with the host's seat and syncer. On a CLIENT it
// is the host's: the stamp is taken as the table, and the pose and condition correct the local simulating
// rig toward the wire (a velocity write plus a bounded corrective term, warping via the game's own
// teleportVehicle only past a speed-scaled threshold) unless this peer syncs the ATV itself or has a
// request for it in flight. Called from event_feed.
void OnReliable(const coop::net::AtvStatePayload& payload, uint8_t senderPeerSlot);

// Receiver entry (CLIENT-only): the host's AtvAnswer to this client's latest AtvSeat or AtvTakeover. The
// latest request's answer writes the table and moves this peer to it (a refused sit dismounts, a refused
// grab drops); an older one is logged and changes nothing. Called from event_feed.
void OnAnswer(const coop::net::AtvAnswerPayload& payload);

// Receiver entry (CLIENT-only): an AtvSpawn arrived. The host announces an ATV of its world under
// its game Key. A key that cannot name an ATV is dropped. The ATV this world already holds under the
// Key -- a row of the index, else the game's own key map (the save's copy) -- is adopted and nothing
// is spawned; a mirror this lane spawned is a duplicate announce and is a no-op. Otherwise
// fresh-spawns a native AATV_C carrying that Key (ue_wrap::atv::SpawnMirror, physics ON = grabbable)
// at the host pose and registers it under it, so the existing AtvState key-stream drives
// it. Waits in the park while this world is not ready to resolve it. From event_feed.
void OnAtvSpawn(const coop::net::AtvSpawnPayload& payload);

// Receiver entry (CLIENT-only): an AtvDestroy arrived. The host's ATV with that Key is gone ->
// K2_DestroyActor the actor this world holds under it (the row's, else the key map's: a mirror this
// lane spawned or the save's copy) + drop the index entry. Waits in the park while this world is not
// ready. From event_feed.
void OnAtvDestroy(const coop::net::AtvDestroyPayload& payload);

// Receiver entry (CLIENT-only): the host's ATV set for this peer is complete (AtvSetComplete). The
// host named every ATV it holds, one AtvSpawn each, ahead of this edge on one lane; once this world is
// ready, every ATV it holds that the set did not name is removed. The edge counts distinct keys. From event_feed.
void OnAtvSetComplete(const coop::net::AtvSetCompletePayload& payload);

// Whether `key` can name an ATV on the wire: not empty, not "None", at most 31 chars (what a WireKey
// carries). No logging; the lane's logging refusals wrap it.
bool AnnounceableAtvKey(const std::wstring& key);

// Every live ATV actor of this world (the witch car included) whose key passes AnnounceableAtvKey, with that key,
// from the object index rather than the lane's map (whose rows lag a birth by a pass). The host's set walk,
// the client's set apply and the spawn drill all read it. The callback must not create or destroy objects.
// Returns how many ATVs the object index holds that are not yet readable (still loading): they are neither
// walked nor counted among the named, so a caller judging a walk complete asks for zero. Game thread.
size_t ForEachAnnounceableAtv(void (*fn)(void* ctx, void* actor, const std::wstring& key), void* ctx);

// HOST-only: snapshot every live ATV's announce, and the pose, VELOCITY and table (stamped with the host's seat
// and syncer) of each the index holds, to a freshly connected client `peerSlot` (adopt=1 -> the joiner warps to
// it). Carrying the velocity is what lets an ATV that is airborne or rolling at the join arrive moving and land,
// instead of hanging where it was (principle 8). The set ends with AtvSetComplete, which the joiner reads as "remove what is not named"; it is
// withheld when the host's walk cannot be taken as complete (an ATV still loading, the index not past this world,
// an announce not sent); a withheld edge is owed to the slot and retried from Tick (from 1 s, doubling to 32 s) while the
// slot is world-ready. Net-pump connect edge, at every connect replay. Game thread.
void QueueConnectBroadcastForSlot(int peerSlot);

// Per-tick pump. The HOST first judges its own player's sit and grab against the table, runs the once-a-second
// syncer pass and sends one stamped block for each ATV whose table changed. A CLIENT reads its own body and
// asks the host for what it did (once its own world is ready), then yields to what the host decided. Then,
// per ATV this peer syncs: stream at ~20 Hz (driving, carrying, or not yet handed back), or, on the host,
// stream at 5 Hz AND only on change (an idle one -- a parked ATV costs zero packets); a mirror does nothing at
// all (a simulating body corrected at packet arrival). Refuses to run if the collision guard did not arm. Call
// every net-pump tick on the game thread.
//
// The two questions are separate on purpose. THE SEAT (`occupantSlot`) is the host's judged claim that
// device_occupancy's E-press deny reads; THE SYNCER (`syncerSlot`) decides who STREAMS and whose machine runs
// the brain: the accumulators, the wheel torque, and the collision-authored damage. A peer merely grabbing an
// ATV must not deny a seat nobody is in.
void Tick();

// Session teardown: disarm the collision guard, give EVERY surviving ATV its brain back
// unconditionally (single-player must own its own vehicles again), destroy every mirror this
// lane spawned, then clear the index.
void OnDisconnect();

// Interaction query: true iff `actor` is indexed as an ATV currently occupied, by the host's stamp, by another
// network peer (occupantSlot != 0xFF && occupantSlot != LocalSlot()). Populates `outOccupantSlot` if non-null.
bool IsOccupiedByOther(void* actor, uint8_t* outOccupantSlot = nullptr);

// The indexed ATV (any ATV_C, the witch car included) whose seat `slot` occupies (occupantSlot == slot), or nullptr.
// The seat is the host's table, as stamped to this peer. A grabber is not seated. The same index guard as
// IsOccupiedByOther; an entry whose actor is no longer live by its index is skipped. Game thread.
void* ActorSeatedBy(uint8_t slot);

// What a drill reads of one ATV's table: the seat, the syncer, the speed of the syncer's last admitted block (the
// host's; a client's stays 0), whether this machine's own body sits in the seat and the actor the entry holds, the
// story event's trap (the host's: its own copy's, as of its last Tick; a client's: the last stamp's bit as applied) and
// the host's blown mark with the slot that blew it (0xFF = none; the host's table only, a client's stays unmarked).
struct TableView {
    uint8_t occupantSlot = 0xFF;
    uint8_t syncerSlot = 0;
    float   lastBlockSpeed = 0.f;
    bool    localSeated = false;
    bool    snapshotReached = false;  // client: this ATV's own connect-snapshot block reached the corrector
    bool    trap = false;
    bool    blown = false;
    uint8_t blownBy = 0xFF;
    void*   actor = nullptr;
};

// The table of the ATV indexed under `key`, as this peer holds it. False, and `out` untouched, unless the index is
// this world's, an entry stands under the key and its actor is live. Game thread.
bool ReadTable(const std::wstring& key, TableView& out);

// Every live entry of this world's index, by key. Nothing when the index is another world's. Game thread.
void ForEachIndexed(void (*fn)(void* ctx, const std::wstring& key, void* actor), void* ctx);

// DEV, for the ATV health drill's silent arm and the hostile drill's atv_ arms: while set, this CLIENT's syncer
// stream sends no block (its body, its request and the host's stamps run on), so the host's silence rule is what
// takes the ATV and the host's counters hold only what the drill sent. Cleared by the drill and by OnDisconnect.
// Game thread.
void SetDrillMute(bool on);

// Diagnostic read: does THIS peer own `actor`'s tick (run its brain) right now? False for an
// unindexed actor, and false whenever the lane is not running. Exposed for coop/dev/atv_probe, so
// an instrumented run can state which side of the mirror each sample came from. Game thread.
bool OwnsTick(void* actor);

// Does the lane hold `actor` as an ATV: a live entry of this world's index (any ATV_C, the witch car included). Game
// thread.
bool IsIndexed(void* actor);

// Is `actor` an indexed ATV whose tick ANOTHER copy owns? True only while the collision guard is active (an inactive
// guard means the lane is not deciding, so every copy runs as it does without the mod), the ATV is indexed and this
// peer does not own its tick. Game thread.
bool NonOwnerCopy(void* actor);

// For the ATV spawn drill, CLIENT, game thread; each counts this session and starts over at a disconnect: the
// announces answered by a copy the world already held, the mirrors this peer fresh-spawned, the actors
// destroyed for the host's AtvDestroy or its set, the host's sets applied, the drill's deliberate parks, the
// arrivals the park holds now, and the keys the host's AtvSpawns named.
uint32_t AdoptedEver();
uint32_t SpawnedEver();
uint32_t DestroysApplied();
uint32_t SetEdgesApplied();
uint32_t DeliberateParks();
size_t ParkPending();
std::vector<std::wstring> ReceivedSpawnKeys();

// For the ATV health drill, CLIENT, game thread; counts this session and starts over at a disconnect: the blocks the
// host re-sent for a client syncer (kAtvFromSyncer) that reached the corrector, counted on the place path before
// ApplyCorrection (which may still refuse them: corrector off, transform unreadable, teleport failed). The connect
// snapshot is read per ATV instead: TableView::snapshotReached.
uint32_t AppliedFromSyncer();

}  // namespace coop::atv_sync
