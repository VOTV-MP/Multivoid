// coop/interactables/atv_authority.h -- the host's table of who sits in and who syncs each ATV, and its judgements.
//
// Three concepts, never aliased: the birth owner (the host's index row), the
// OCCUPANT (a seat, a claim the host judges) and the SYNCER (write authority the host assigns). The table is two
// fields of the ATV's AtvEntry, `occupantSlot` and `syncerSlot`, written here on the host by judgement and
// everywhere else by the host's stamp. A client asks (AtvSeat, AtvTakeover) and meets the answer (AtvAnswer); the
// host's own player is judged by the same table without the reach test. MTA is the precedent: seats are
// server-judged requests (reference/mtasa-blue/Server/mods/deathmatch/logic/CGame.cpp:3079-3365); an unoccupied
// vehicle keeps its syncer while that player stays in range, the server finding another when it leaves and a push
// by another player being a rate-limited, range-checked takeover
// (reference/mtasa-blue/Server/mods/deathmatch/logic/CUnoccupiedVehicleSync.cpp:96-158, :171-205, :480-500).
// Divergences, each named where it is made: a parked ATV returns to the host, which always holds the world; a
// takeover is a grab, not a push by contact; an occupied seat is refused, VOTV has no jack.
//
// The stream and the stamp live in atv_sync.cpp, which asks this module for the verdicts. Game thread only.

#pragma once

#include <cstdint>

namespace coop::net {
struct AtvSeatPayload;
struct AtvTakeoverPayload;
struct AtvStatePayload;
}  // namespace coop::net
namespace coop::atv_sync { struct AtvEntry; }

namespace coop::atv_authority {

// What an ATV's machine asks of the table: the host's own player (JudgeLocal) and a client's (its pendingAction).
enum class AtvRequest : uint8_t { Sit, Leave, Takeover };

// HOST: a client's AtvSeat / AtvTakeover, from sender `slot` (1..kMaxPeers-1, checked by the dispatch). Judged in
// the design's order; every judged request is answered to its sender with AtvAnswer, a request from a slot whose
// world is not ready alone is counted and not answered.
void OnSeat(const coop::net::AtvSeatPayload& p, uint8_t slot);
void OnTakeover(const coop::net::AtvTakeoverPayload& p, uint8_t slot);

// HOST: the host's own player's sit, leave or takeover on `e`, judged by the table's tests only: a sit is refused by
// `already_seated` and `occupied`, a takeover by `seated` and `synced`. No reach, no bucket, no answer (the host
// may cheat; it is its own authority). True when the table granted it. Called from atv_sync's Tick.
bool JudgeLocal(coop::atv_sync::AtvEntry& e, AtvRequest req);

// HOST: the host's own player's passenger sit (`sit`) or leave on `e`, judged by the table's tests only: a sit is
// refused by `already_seated`, `occupied` and `no_driver`, a leave when the host is not the passenger. True when the
// table took it. Called from atv_passenger's Tick.
bool JudgeLocalPassenger(coop::atv_sync::AtvEntry& e, bool sit);

// HOST: whether `slot` is the syncer a hand-back just replaced, inside the window its late block is still honest: a
// round trip and two sends of it. AdmitBlock counts such a block `late_syncer` and the explosion request's judge
// admits the death report of that syncer; one window for both.
bool IsLateSyncer(const coop::atv_sync::AtvEntry& e, uint8_t slot, uint64_t nowMs);

// HOST: whether a client's AtvState block is admitted. Only the entry's syncer's blocks are; a block from another
// slot is dropped and counted (`late_syncer` when it is the syncer a hand-back just replaced, within a round trip
// and two sends of it, else `not_syncer`), one past the entry's stream bucket is dropped and counted
// (`stream_paced`). An admitted block stamps the entry's last-block time and updates its rest state. The caller
// applies the block and re-sends it stamped.
bool AdmitBlock(coop::atv_sync::AtvEntry& e, const coop::net::AtvStatePayload& p, uint8_t senderSlot,
                uint64_t nowMs);

// HOST, once a second: per entry whose syncer is a client, the first rule that holds ends the entry's pass: the
// syncer is silent (the host takes it, and the seat if that client held it), it is seated (no change), it is out
// of range or the ATV is at rest and uncarried (the host takes it). Every change marks the entry for a stamp.
void RunPass(uint64_t nowMs);

// The table reset: the seat empty, the syncer the host, the clocks, the stream bucket, the trap and blown marks cleared, a stamp due.
// A departure runs this alone (a neighbour leaving never cancels this machine's own request); the successor reset
// of a repointed entry runs it with ResetRequests (the actor changed).
void ClearTable(coop::atv_sync::AtvEntry& e);
// This machine's own request fields: nothing pending, no takeover due. `sentSeated` is kept: it is what the host
// believes of this machine's seat.
void ResetRequests(coop::atv_sync::AtvEntry& e);

// The session's judgements, for the teardown line and the drills: every refusal by reason (indexed by AtvRefusal),
// the grants, and the stream's counts.
struct Counters {
    unsigned long long refused[32] = {};   // by AtvRefusal; [0] unused
    unsigned long long admitted   = 0;     // any request granted (sit, leave, takeover)
    unsigned long long notSyncer  = 0;     // a block from a slot that does not sync the ATV (also an honest client's
                                           // optimistic blocks for a request the host refused)
    unsigned long long lateSyncer = 0;     // a block from the syncer a hand-back just replaced
    unsigned long long streamPaced = 0;    // a block past the syncer's stream bucket
    unsigned long long handedBack = 0;     // the host took an ATV from a client syncer (range or rest)
    unsigned long long silenced   = 0;     // the host took an ATV from a silent syncer
    unsigned long long idleStreamBlocks = 0;  // blocks admitted from a client syncer with the seat free and no carry
                                              // (the release stream before a hand-back)
    unsigned long long carriedStillBlocks = 0;  // blocks admitted with the grabbed bit whose speed and moved distance
                                                // pass the rest test: a carry standing still, never handed back
};
Counters ReadCounters();

// The teardown line (nothing when nothing was judged), then the table's counters, buckets and refusal log clear.
void LogSummary();
void Reset();

}  // namespace coop::atv_authority
