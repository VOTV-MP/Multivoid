// coop/props/container_write_policy.h -- the host's answer to ONE question: may this
// client-authored container slice be applied? The lane beside it (container_contents_sync) owns
// the verb edge, the blob and the apply; this owns the arbitration and the state that arbitration
// needs -- what the host last published per container, when the host itself last changed one, and
// how many writes it has refused.
//
// The decision is split in two on purpose, the shape intent_authority already uses: `Judge` is
// pure arithmetic over what the host knows, so a selftest drives the REAL decision rather than a
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
    Unreachable,         // the author is not where this container is; the element's own outcome
                         // name rides in the log line
    NoBodyYet,           // the author has no body on this machine to measure a reach from, which
                         // is a mid-join window rather than a verdict: the caller HOLDS the slice
                         // instead of refusing it (principle 8 -- a lane owes its late-join answer,
                         // and losing an honest edit for the seconds before a puppet poses is the
                         // worse one). The pen bounds the hold: 8 per author, 30 s
    StaleBase,           // the author edited a world the host has not published
    HostChangeInFlight,  // the author edited the published world, but a host-side change to this
                         // container is younger than the conflict window and the author provably
                         // had not seen it
};

// Where this slice is coming from. A slice the host parked and is now replaying was already
// admitted once: it costs the host no second arrival, and what bounds the cost of holding it is
// the park's own per-author cap, not this window. Its REACH is judged on the replay all the same,
// because that is the first moment it can be judged at all -- an element that did not resolve at
// arrival could not be measured against anybody's reach.
enum class Source : uint8_t { Arrival = 0, Replay };

// What the host knows at the moment of the decision. `lastLocalChangeMs` is 0 when this peer has
// never fired a verb on this container.
struct Inputs {
    uint64_t baseHash          = 0;  // what the author says it edited from
    uint64_t publishedHash     = 0;  // what the host last told the world this container held
    uint64_t lastLocalChangeMs = 0;  // when the host itself last mutated it
    uint64_t nowMs             = 0;
};

// A client write is refused within this window of a host-side change.
inline constexpr uint64_t kConflictWindowMs = 1500;

// The lane's own reach, before intent_authority adds the target's bounds and its pose-staleness
// budget. The game opens and mutates a container through the camera trace mainPlayer::arm, whose
// default length is armLength = 200 uu, so that is the number this lane owns.
inline constexpr float kReachUU = 200.0f;

// How many slices one author may have arbitrated inside a window, and how long the refusal lasts.
// An author's own sweep ships at most one slice per container per 250 ms, so sixteen in a second
// is four times the rate this build can produce and still bounds a sender that ignores its own
// sweep: every accepted change allocates a fresh records buffer on the host and orphans the old
// one, so the rate of accepted change IS the rate the host spends.
inline constexpr int      kRateMax      = 16;
inline constexpr uint64_t kRateWindowMs = 1000;

// The PURE half. An author that never received anything sends base 0 and is refused rather than
// trusted, which is why the match requires a non-zero base.
Decision Judge(const Inputs& in);

// Judge against what this module remembers for that container. The half `Accept` runs after the
// rate and the reach, and the half a selftest can drive without an engine or a session.
Decision JudgeAgainstState(uint32_t eid, uint64_t baseHash, uint64_t nowMs);

// The stateful call the lane makes: the reach question first, then the base. Logs the refusal,
// including which condition failed, and counts it. An eid that resolves to no live prop element is
// NOT a reach refusal -- the lane parks such a slice and replays it, and the park is where the
// birth-skew answer lives.
Decision Accept(uint32_t eid, uint64_t baseHash, uint8_t authorSlot, uint64_t nowMs,
                coop::net::Session& session, Source src);

// The host has told the world what this container holds, by any route -- a fan-out or a targeted
// connect seed. This is the compare-and-swap baseline a later client write is judged against.
void NotePublished(uint32_t eid, uint64_t contentHash);

// This peer's own verb edge fired on this container.
void NoteLocalChange(uint32_t eid, uint64_t nowMs);

void Reset();

// The arithmetic selftest, run once per session start beside the identity ones: the base that is
// refused, the base that is accepted, the host change in flight, and the sequence a THIRD peer
// walks into -- the host publishes, one client's slice is accepted and relayed, and the next
// author is judged against what the relay carried rather than against the older fan-out. Leaves
// the module empty.
bool RunSelftest();

}  // namespace coop::props::container_write_policy
