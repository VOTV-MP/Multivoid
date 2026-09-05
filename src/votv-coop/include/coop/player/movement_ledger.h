// coop/player/movement_ledger.h -- how fast a peer claims to have moved, and whether the host
// believes it. The pose receive boundary validates nothing about motion: ValidatePose is a
// static filter (finite, |xyz| within kMaxCoord, a self-reported speed), so a peer may place its
// claimed body anywhere in one packet, and every proximity gate rests on a value the peer
// writes. This ledger charges each accepted pose against the real time elapsed at the game's
// own top speed and records the verdict. The pose is always stored and always relayed: a
// discontinuity costs trust, never display, since the game's own blueprints teleport the player
// from many sites, all in the client's process where the host cannot see the trigger; MTA's
// pure-sync packet has the same shape, an alert and then SetPosition anyway. What this buys is a
// rate limit, not a closure: a trusted client still crosses the world at the speed bound. Host
// only, over poses on an admitted client connection; a client validates nothing. Rows are
// written from the net thread on every inbound pose, so they carry their own mutex and the
// occupancy generation instead of the game-thread-only per-slot state primitive, and a stale
// generation is refused at both the write and the read. Measure-only on this build: it
// computes, logs, and nothing refuses. The selftest runs on every session start, un-gated.

#pragma once

#include "coop/player/players_registry.h"  // kMaxPeers
#include "ue_wrap/core/types.h"            // FVector

#include <cstdint>

namespace coop::net { class Session; }

namespace coop::movement_ledger {

// The fastest the game itself moves a player continuously, plus headroom, from the blueprints:
// walk defSpeed 400; sprint defSpeed x 2 x Lerp(1, 1.25, agility/100), at most 1000
// (mainPlayer's updateSpeed); the ATV's speed_turbo 3200; and noclip at 5000 cm/s per axis
// while sprinting, three summed axis terms, so 8660 on the diagonal. Noclip is reachable in
// ordinary play: its gate, the library's isBuoyant, returns the gamemode's hasWeapon when
// isFlying is false. 10 000 covers all of it with headroom, ten times tighter than ValidatePose's
// kMaxSpeed, which bounds a self-reported field.
inline constexpr float kMaxTravelSpeedCmS = 10000.0f;

// The un-earned jump: a standing budget that refills on every sample under the speed bound,
// which an attacker may spend at will, so it is sized against the smallest reach it can defeat
// (mainPlayer's armLength of 200, behind coin collection and clump grabbing), not merely to
// swallow jitter: 50 cm is four times under that reach and five times above the analytic floor
// (the 1 ms wire quantisation is 10 cm at the bound). It caps the carry, the credit left
// standing from earlier samples, while the interval's own earnings are spendable in full on
// top; capping the sum would make 50 cm the largest step any sample may take, which refuses the
// ATV at speed_turbo (53 cm between two 60 Hz poses). The selftest's ordinary-motion rows keep
// that caught.
inline constexpr float kUnearnedJumpCm = 50.0f;

// The ceiling on how long one unexplained discontinuity withholds trust: a policy choice, not a
// measurement. A genuine cross-world teleport would cost about 55 s at the bound; 30 s
// under-charges only hops over 3 km, which the enumeration attack has no use for, since props
// cluster. An unexplained discontinuity never blocks interaction: this constant governs how
// long the log reports a slot untrusted, and no refusal may be built on it. PositionTrusted
// therefore has no refusal consumer (an authorizer may log on it, never deny), so a reach gate
// rests on the sender's last claimed position, which stops the naive enumeration (one packet
// per prop, no movement) and costs a serious attacker one extra pose packet. A mitigation, not
// a closure; host-authoritative character movement is what closes it.
inline constexpr float kMaxDebtSeconds = 30.0f;

// How much real time the sender may bank and spend as claimed elapsed. This absorbs a network
// clump: the net thread polls at about 200 Hz but drains its queue in an unbounded loop, so a
// stalled receiver delivers packets whose arrival intervals are near zero while their
// production intervals were real, and without a bank every packet after the first in a clump
// would be credited almost no time. What one packet may spend is the carry plus the credited
// interval at the bound, so a peer silent for 7 s (under kClockRebaseGapMs) banks the full 5 s
// and may cover 500 m in one packet: inside the sustained rate, so the cap tightens the bound,
// but silent, since trust never falls and no discontinuity is logged. Capping the per-packet
// credited interval would make that jump visible at the price of refusing an honest peer whose
// link stalled that long; which cost is real depends on the receive-gap distribution, so the
// per-slot summary measures it (dt and use), and selftest row 10 pins today's behaviour.
inline constexpr uint32_t kSkewBankMaxMs = 5000;

// A gap longer than this re-bases the sender clock without touching the position anchor or the
// credit: the wire stamp is 24 bits (16 777 216 ms), so a longer gap makes the elapsed
// ambiguous, and re-anchoring the position on a gap would hand a free teleport to anyone who
// mutes his own pose stream. Silence earns nothing.
inline constexpr uint32_t kClockRebaseGapMs = 8000;

// Net thread.

// One accepted, fresh, client-originated pose; call after the per-slot sequence freshness
// check, or a reordered datagram would walk the anchor backwards and forwards and bill one
// metre twice. `stateTimeMs24` is the wire field; 0 means the sender did not stamp, which marks
// the slot untrusted with a named reason, reported once per slot.
void OnClientPose(coop::net::Session& session, int slot,
                  const ue_wrap::FVector& pos, uint32_t stateTimeMs24);

// Game thread.

// Is this slot's claimed path achievable at the game's maximum speed? Fail-closed on every
// axis: no accepted sample, a departed slot, or a row anchored under a different occupancy
// generation all answer false, since a proximity decision needs a body and a body needs a pose
// from the person asked about. The session argument matters: the generation drops to 0 the
// moment a peer disconnects, and the row keeps the departed peer's verdict until the next
// occupant's first pose, while a joining peer can send a reliable before a pose. Nothing
// consumes this yet; a consumer may log on it, never deny.
bool PositionTrusted(coop::net::Session& session, int slot);

// The last position the ledger accepted for `slot`; this, not the puppet's transform, is what
// authorization consumes: the puppet is written by the interpolator and snaps on a teleport, so
// reading it would make the validated and the consumed value two quantities, and it goes stale
// observably (the sample count stops), where a puppet transform just keeps standing there.
bool TryGetAcceptedPosition(coop::net::Session& session, int slot, ue_wrap::FVector& out);

// Per tick, host only: the throttled per-slot summary and the wire-versus-actor divergence
// sample, which lives here rather than in the net-thread write because reading a puppet's
// transform is an engine read.
void Tick(coop::net::Session& session);

// Lifecycle.

// Wipes every row and runs the selftest. No OnSessionStop counterpart: a teardown sweep is the
// wrong layer for staleness, since a slot recycles with no absence between and there is no
// teardown edge in the case that matters. Both readers refuse a stale generation instead, which
// covers the recycle and the session boundary with one rule.
void OnSessionStart();

}  // namespace coop::movement_ledger
