// coop/player/movement_ledger.h -- how fast a peer CLAIMS to have moved, and whether the host
// believes it. Security A52; A50's residual.
//
// THE PROBLEM. The pose receive boundary validates nothing about MOTION. `coop::net::ValidatePose`
// is a static garbage filter -- finite, |xyz| <= kMaxCoord (1.0e6 cm = 10 km), angle ranges, and a
// SELF-REPORTED `speed` bound -- with no delta-vs-time check anywhere. So a peer may place its
// claimed body anywhere in the world in a single packet, and every "was this peer near it" gate in
// the mod rests on a value that peer writes. That is why `docs/security/TRACKER.md` carries A50 as
// MITIGATED and not CLOSED, and it is the shape of
// `[[lesson-a-gate-anchored-on-a-client-authored-value]]`.
//
// WHAT THIS DOES, AND WHAT IT DELIBERATELY DOES NOT. The pose is ALWAYS stored and ALWAYS relayed.
// A discontinuity costs TRUST, never display. Refusing a pose is not an option: `[V]` the game's own
// blueprints teleport the player from ten measured sites -- the save-load spawn
// (`mainGamemode` @62203), `trigger_teleporter` @115, `teleportWObackrooms` (14 call sites), the ATV
// seat, four panel snaps -- and EVERY ONE of them runs in the CLIENT's process where the host cannot
// observe the trigger. A validator that refuses a discontinuity refuses a legitimate game event it
// cannot recognise, which manufactures exactly the LOSS defect
// `[[lesson-a-protection-against-cheat-manufactures-a-loss]]` names.
//
// This is also MTA's shape at the identical seam: `CPlayerPuresyncPacket.cpp:148` measures
// `DistanceBetweenPoints3D` against `playerTeleportAlert`, fires `onPlayerTeleport`, and then calls
// `SetPosition(reported)` ANYWAY. An alert, never a refusal. We port that and not its resource-layer
// half (RULE 3 / the "skip anti-cheat" clause) -- our consumer is the arbiter instead.
//
// THE HONEST BOUND, STATED SO NOBODY OVERSTATES IT LATER. This RATE-LIMITS the A50 sweep; it does
// not refuse it. At `kMaxTravelSpeed` a trusted client crosses the measured ~5.53 km world in ~55 s
// with every prop on the path inside the coin gun's 1000 uu reach. What it removes is the FREE
// TELEPORT enumeration -- one packet per prop, no gun, no shot, no destroy -- leaving a continuous
// traversal at the game's own maximum, paying for every prop destroyed. The option that would
// actually close it is HOST-AUTHORITATIVE CHARACTER MOVEMENT (the host simulates the client's
// character instead of believing its poses), which dissolves this module, the reach anchors and the
// trust question together. It belongs to the phase-2 ARBITER arc (`docs/ROADMAP.md`) -- and note the
// citation, because an earlier draft of this comment said "ROADMAP phase 8": the authority work was
// pulled FORWARD to phase 2 on 2026-07-20 and the old "native standalone server" phase 8 was retired
// outright. It is named here rather than buried so the next reader does not mistake this for closure.
//
// CLIENT-SCOPED, AND THAT IS NOT NEGOTIABLE (USER 2026-08-24, `docs/security/THREAT_MODEL.md`): the
// host may cheat and we relay it. This runs on the HOST ONLY, over poses arriving on a
// GNS-authenticated client connection. A client validates nothing. A symmetric validator is a BUG.
//
// THREADING. `OnClientPose` is the NET thread. `PositionTrusted` / `TryGetAcceptedPosition` / `Tick`
// are the GAME thread. Rows are guarded by this module's own mutex because a row holds correlated
// fields (position, path length, both clocks, the verdict) that cannot be read a field at a time.
// The occupancy generation is read with `Session::peerGenerationForSlot`, an acquire load documented
// "Any thread", ON THE WRITER'S OWN THREAD -- routing it through a game-thread reconcile would leave
// a window in which a RECYCLED slot's first pose is judged against the PREVIOUS occupant's anchor
// and lands untrusted, which is principle 8 broken exactly on the recycle path, where slots go X->Y
// with no absence between and no boolean can see it.
//
// STATUS ON THIS BUILD: MEASURE-ONLY. It computes, it logs its inputs, and NOTHING REFUSES. The
// constants below are derived from bytecode, but the only field distribution that exists came from a
// session that never left a 35 m patch of the base -- no ATV, no noclip, no teleporter, no
// save-load. Enforcing on that would make the user's first working build its own calibration run.
//
// Design of record: `research/findings/inventory-items/votv-v137-field-defects-DESIGN-2026-08-24.md`
// section 13 (converged over a 14-round `/qf`; read that section and no earlier copy of it).

#pragma once

#include "coop/player/players_registry.h"  // kMaxPeers
#include "ue_wrap/core/types.h"            // FVector

#include <cstdint>

namespace coop::net { class Session; }

namespace coop::movement_ledger {

// The fastest the GAME ITSELF can move a player continuously, plus headroom. `[V]` measured from the
// blueprints: walk `defSpeed` = 400; sprint = `defSpeed * 2.0 * Lerp(1.0, 1.25, agility/100)` <= 1000
// (`mainPlayer.updateSpeed`); ATV `speed_turbo` = 3200; UE4's default TerminalVelocity ~4000 with no
// override in any of the 292 asset dumps; and the real ceiling, NOCLIP, at 5000 cm/s per axis while
// sprinting (`mainPlayer` ubergraph @64501) applied as three summed `dir * dt * 5000` terms, so
// sqrt(3) * 5000 = 8660 worst case. Noclip is reachable in ordinary play: its gate `lib_C::isBuoyant`
// returns `gamemode.hasWeapon` when `isFlying` is false.
//
// 10 000 covers all of it with ~15% headroom, and is 10x TIGHTER than the `kMaxSpeed = 1.0e5` that
// `ValidatePose` already carries -- which is a self-reported field and bounds nothing at all.
inline constexpr float kMaxTravelSpeedCmS = 10000.0f;

// THE UN-EARNED JUMP. Say it that way and the sizing rule follows: this budget refills on every
// sample below `kMaxTravelSpeedCmS`, so it is STANDING, not one-shot, and an attacker may spend it at
// will. It must therefore be small relative to THE SMALLEST REACH IT CAN DEFEAT -- `[V]`
// `mainPlayer.armLength = 200.0`, the reach behind coin collection and clump grabbing -- not merely
// large enough to swallow jitter. 50 cm is 4x under that reach and 5x above the analytic floor
// (the 1 ms wire quantization is 10 cm at the speed bound above).
//
// Earlier drafts had 20 000, then 1500, then 300. 300 already EXCEEDED the 200 uu reach it was meant
// to protect, which is what happens when a bound is sized against the noise instead of against the
// thing it guards.
inline constexpr float kUnearnedJumpCm = 50.0f;

// The ceiling on how long ONE unexplained discontinuity withholds trust. THIS IS A POLICY CHOICE,
// NOT A MEASUREMENT, and the doc says so too: the measured world diagonal (~5.53 km, from 75 452
// level vectors) says a genuine cross-world teleport WOULD cost ~55 s at the speed bound, and 30 s is
// a decision about how much loss to accept. It under-charges only hops longer than 3 km, which the
// enumeration attack has no use for because props cluster.
inline constexpr float kMaxDebtSeconds = 30.0f;

// How much REAL time the sender may bank and spend as claimed elapsed. This is what absorbs a
// network clump: the net thread polls at ~200 Hz but drains its queue in an unbounded inner loop, so
// a stalled receiver delivers N packets whose arrival intervals are ~0 while their PRODUCTION
// intervals were real. Without a bank, every packet after the first in a clump would be credited
// almost no time and an ordinary hiccup would read as motion.
//
// It cannot be abused: banking time only ever produces CREDIT, and credit is capped at
// `kUnearnedJumpCm` regardless of how much time is claimed. The two caps are independent and each
// does exactly one job.
inline constexpr uint32_t kSkewBankMaxMs = 5000;

// A gap longer than this re-bases the sender clock WITHOUT touching the position anchor, the path
// length or the credit. Two reasons, and only one of them is arithmetic: the wire stamp is 24 bits
// (16 777 216 ms), so a longer gap makes the elapsed ambiguous; and re-anchoring the POSITION on a
// gap would hand an attacker a free teleport for the price of muting his own pose stream, which is
// cheaper than the debt for any hop over ~80 m. Silence earns nothing here.
inline constexpr uint32_t kClockRebaseGapMs = 8000;

// --- net thread --------------------------------------------------------------

// One accepted, FRESH, client-originated pose. Call AFTER the existing per-slot sequence freshness
// check: a reordered datagram would otherwise walk the anchor backwards and then forwards, and the
// ledger would bill the peer twice for one metre. `stateTimeMs24` is the wire field; 0 means the
// sender did not stamp, which marks the slot untrusted with a named reason (fail-CLOSED) and is
// reported once per slot rather than per packet.
void OnClientPose(coop::net::Session& session, int slot,
                  const ue_wrap::FVector& pos, uint32_t stateTimeMs24);

// --- game thread -------------------------------------------------------------

// Is this slot's claimed path achievable at the game's own maximum speed? Fail-CLOSED: a slot with
// no accepted sample is NOT trusted, because a proximity decision needs a body and a body needs a
// pose. NOTHING CONSUMES THIS YET -- the enforcing build wires it into the intent authorizer.
bool PositionTrusted(int slot);

// The last position the ledger ACCEPTED for `slot`. This -- not the puppet actor's transform -- is
// what authorization must consume once it is wired: the puppet is written by the interpolator and
// SNAPS on a teleport, so reading it would make the validated value and the consumed value two
// different quantities and would rest authority on a display pipeline's output. It also goes stale
// OBSERVABLY (the sample count stops advancing), where a puppet transform just keeps standing there.
bool TryGetAcceptedPosition(int slot, ue_wrap::FVector& out);

// Per-tick, host-only. Emits the throttled per-slot summary and samples the wire-vs-actor divergence
// -- which lives HERE and not in the net-thread write because reading a puppet's transform is an
// ENGINE read and engine reads are game-thread only.
void Tick(coop::net::Session& session);

// --- lifecycle ---------------------------------------------------------------

void OnSessionStart();
void OnSessionStop();

}  // namespace coop::movement_ledger
