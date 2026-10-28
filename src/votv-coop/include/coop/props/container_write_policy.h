// coop/props/container_write_policy.h -- the host's answer to ONE question: may this
// client-authored container slice be applied? The lane beside it (container_contents_sync) owns
// the verb edge, the blob and the apply; this owns the arbitration and the state that arbitration
// needs -- the rate it spends per author and how many writes it has refused. A slice is judged
// against what the container holds right now, whoever wrote it: the lane's reader gives the live
// content hash at the arbitration, so a write nothing published (the drone's sale) still
// leaves a stale author refused.
//
// The decision is split in two on purpose, the shape intent_authority already uses: `Judge` is
// pure arithmetic over what the host reads, so a selftest drives the REAL decision rather than a
// copy of it, and `Accept` is the stateful wrapper the lane calls. Host-only and game-thread only:
// a client runs no arbitration at all (it accepts slot 0 and nothing else).

#pragma once

#include <cstdint>

namespace coop::net { class Session; }

namespace coop::props::container_write_policy {

// Why a client-authored slice was accepted or refused. A refusal is never silent and never
// rolled back: the lane answers it by re-publishing the host's truth to the author, so the loser
// converges instead of keeping a divergent view -- all but TooFast, which re-publishes nothing, since
// an author pushing faster must not make the host spend more.
enum class Decision : uint8_t {
    Accept = 0,
    TooFast,             // this author has spent its window's worth of the host's arbitration
    Unreachable,         // the author is neither where this container is nor at an actor that opens
                         // it (coop/props/container_reach); the outcome name rides in the log
    NoBodyYet,           // the author has no body on this machine to measure a reach from, which
                         // is a mid-join window rather than a verdict: the caller HOLDS the slice
                         // instead of refusing it (principle 8 -- a lane owes its late-join answer,
                         // and losing an honest edit for the seconds before a puppet poses is the
                         // worse one). The pen bounds the hold: 8 per author, 30 s
    StaleBase,           // the author edited from contents the container no longer holds
    Unreadable,          // the container's live contents do not read (a transfer into it is in
                         // progress, or its inventory does not resolve), so no base can be judged;
                         // answered like StaleBase
};

// Where this slice is coming from. A slice the host parked and is now replaying was already
// admitted once: it costs the host no second arrival, and what bounds the cost of holding it is
// the park's own per-author cap, not this window. Its REACH is judged on the replay all the same,
// because that is the first moment it can be judged at all -- an element that did not resolve at
// arrival could not be measured against anybody's reach.
enum class Source : uint8_t { Arrival = 0, Replay };

// What the host reads at the moment of the decision.
struct Inputs {
    uint64_t baseHash     = 0;      // what the author says it edited from
    uint64_t liveHash     = 0;      // the content hash of what the container holds now
    bool     liveReadable = false;  // false: the live contents did not read, and liveHash means nothing
};

// How many slices one author may have arbitrated inside a window, and how long the refusal lasts.
// An author's own sweep ships at most one slice per container per 250 ms, so sixteen in a second
// is four times the rate this build can produce and still bounds a sender that ignores its own
// sweep: every accepted change allocates a fresh records buffer on the host and orphans the old
// one, so the rate of accepted change IS the rate the host spends.
inline constexpr int      kRateMax      = 16;
inline constexpr uint64_t kRateWindowMs = 1000;

// The PURE half, and the half a selftest drives without an engine or a session: Unreadable when the
// live contents did not read, then Accept when the base is the live hash. An author that never
// received anything sends base 0 and is refused rather than trusted, which is why the match requires
// a non-zero base.
Decision Judge(const Inputs& in);

// The lane's reader of a container's live content hash: false when it does not read. Accept calls it only when it
// reaches the base, so a slice refused on the rate or the reach, or held for a body, never reads the container.
using LiveHashReader = bool (*)(uint32_t eid, uint64_t& outHash);

// The stateful call the lane makes: the rate (SpendArrival), the reach question, then the base against
// the live hash. Logs the refusal, including which condition failed, and counts it. An eid that
// resolves to no live prop element is NOT a reach refusal -- the lane holds such a slice in bind_wait
// and replays it, and the hold is where the birth-skew answer lives.
Decision Accept(uint32_t eid, uint64_t baseHash, LiveHashReader readLive, uint8_t authorSlot, uint64_t nowMs,
                coop::net::Session& session, Source src);

// The rate half alone, Accept or TooFast: an arrival spends one of its author's window, a replay
// nothing. Accept runs it first; the lane runs it by itself for a slice it takes unjudged (a thrown
// container's birth), which must not be free of the bound either.
// `held` says what the caller does with a TooFast slice, for the log: refuses it, or holds it in the pen.
Decision SpendArrival(uint32_t eid, uint8_t authorSlot, uint64_t nowMs, coop::net::Session& session,
                      Source src, bool held = false);

void Reset();

// The arithmetic selftest, run once per session start beside the identity ones: the base that is
// refused, the base that is accepted, the unreadable contents, and the sequence a THIRD peer walks
// into -- one client's slice is accepted and relayed, so the container holds what the relay wrote,
// and the next author is judged against that rather than against the older fan-out. Leaves the
// module empty.
bool RunSelftest();

}  // namespace coop::props::container_write_policy
