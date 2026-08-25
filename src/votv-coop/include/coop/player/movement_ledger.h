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
// NOT `PerSlotState<T>`, and this is the deviation stated on purpose. CLAUDE.md 4a-roster says
// per-slot person state should be declared through `PerSlotState<T>`, which registers its own clear.
// That primitive is GAME-THREAD-ONLY (`roster_ledger.h`) and these rows are written from the NET
// thread on every inbound pose, so it does not apply here. What it would have bought -- an automatic
// clear on occupancy change -- is bought instead by stamping the generation on the row and refusing
// a mismatch at BOTH the write and the read.
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
// SELF-CHECKED ON EVERY SESSION START, un-gated. The .cpp ends in a selftest that drives the real
// `ApplyPose` (not a copy of its arithmetic) through the branches a two-peer LAN smoke cannot reach:
// an unstamped sender, a slot recycled to a new occupant, a 24-bit clock wrap, an inflated clock, a
// drained receive queue, the subdivided free jump, the debt floor -- and the honest-motion rows
// (walk / sprint / ATV turbo / noclip diagonal, at both 60 Hz and 10 Hz) whose failure would poison a
// calibration transcript silently. It logs one line: `movement_ledger selftest: ALL PASS (N checks)`,
// or `UE_LOGE` per failing row. It costs microseconds and is deliberately impossible to switch off,
// because a wrong verdict here does not crash -- it merely reads wrong.
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
//
// IT CAPS THE CARRY, NOT THE SAMPLE. The quantity bounded is the credit left STANDING from earlier
// samples -- the part that was not earned during this interval. The interval's own earnings
// (`kMaxTravelSpeedCmS * elapsed`) are spendable in full on top of it. Capping the sum instead makes
// 50 cm the largest step ANY sample may take regardless of elapsed time, which refuses the game's own
// ATV at `speed_turbo` (53 cm between two 60 Hz poses) on its first sample. The first build of this
// module did exactly that; the selftest in the .cpp is what caught it, and rows 4 of that selftest
// exist to keep it caught.
inline constexpr float kUnearnedJumpCm = 50.0f;

// The ceiling on how long ONE unexplained discontinuity withholds trust. THIS IS A POLICY CHOICE,
// NOT A MEASUREMENT, and the doc says so too: the measured world diagonal (~5.53 km, from 75 452
// level vectors) says a genuine cross-world teleport WOULD cost ~55 s at the speed bound, and 30 s is
// a decision about how much loss to accept. It under-charges only hops longer than 3 km, which the
// enumeration attack has no use for because props cluster.
//
// *** USER DECISION 2026-08-25 -- AN UNEXPLAINED DISCONTINUITY NEVER BLOCKS INTERACTION. "Just log." ***
// Asked as a plain product question ("someone teleports and the host can't tell why -- do we punish
// that?"): never block / a few seconds / keep 30 s. The user chose NEVER BLOCK. So this constant is
// now DESCRIPTIVE ONLY -- it governs how long the LOG reports a slot untrusted, and no refusal may
// ever be built on it. Consequences, written here rather than discovered later:
//   - `PositionTrusted` has NO refusal consumer. An intent authorizer may consume it to LOG, never to
//     deny. A future reader wiring it into a deny path is contradicting a user decision, not fixing
//     an oversight.
//   - Therefore an A54 reach gate rests on the sender's last CLAIMED position alone, which the sender
//     writes -- so it stops the NAIVE enumeration (one packet per prop, no movement) and costs a
//     serious attacker exactly one extra pose packet. That is a rate-and-effort mitigation, the same
//     honest label A50 carries. It is NOT a closure, and the only thing that closes it is
//     host-authoritative character movement (see the module header, phase-2 arbiter arc).
//   - The "host-derivable destination set" idea (explain a hop that lands on the save spawn / ATV /
//     bed / treehouse anchor instead of charging it) loses its refusal consumer too, and survives only
//     as LOG-QUALITY work: fewer false discontinuity lines. Sized accordingly, not as a gate.
inline constexpr float kMaxDebtSeconds = 30.0f;

// How much REAL time the sender may bank and spend as claimed elapsed. This is what absorbs a
// network clump: the net thread polls at ~200 Hz but drains its queue in an unbounded inner loop, so
// a stalled receiver delivers N packets whose arrival intervals are ~0 while their PRODUCTION
// intervals were real. Without a bank, every packet after the first in a clump would be credited
// almost no time and an ordinary hiccup would read as motion.
//
// WHAT IT BOUNDS, STATED PROPERLY -- an earlier version of this comment said "it cannot be abused,
// credit is capped at `kUnearnedJumpCm` regardless of how much time is claimed", and that is wrong
// about the quantity that matters. `kUnearnedJumpCm` bounds the STANDING budget; what ONE packet may
// spend is `carry + useMs * kMaxTravelSpeedCmS`, so a peer silent for 7 s (under `kClockRebaseGapMs`,
// so no re-base) banks the full 5 s and may cover 500 m in a single packet. That is INSIDE the
// sustained rate -- 7 s of real time legitimately buys 700 m, so this cap TIGHTENS the bound rather
// than loosening it -- and it is not a rate hole. What it IS: silent. Trust never falls, so no
// DISCONTINUITY line is logged, and the discontinuity record is this build's entire product.
//
// THE OPEN QUESTION, deliberately left open. Capping the per-packet credited interval (say 250 ms)
// would make that jump visible, at the price of refusing an honest peer whose link stalled that long.
// Which cost is real depends on the receive-gap distribution, which nobody has measured -- so this
// build MEASURES it (`dt=[min..max]` and `use<=` in the per-slot summary) instead of guessing a
// constant, and selftest row 10 pins today's behaviour so a later change is deliberate.
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

// Is this slot's claimed path achievable at the game's own maximum speed? Fail-CLOSED on every
// axis: no accepted sample, a departed slot, or a row anchored under a DIFFERENT occupancy
// generation all answer false, because a proximity decision needs a body and a body needs a pose
// from the person being asked about.
//
// The session argument is not decoration. `peerGenBySlot_` drops to 0 the moment a peer
// disconnects, and the row keeps the DEPARTED peer's verdict at the DEPARTED peer's position until
// the next occupant's first pose reaches the net thread -- and a joining peer can send a reliable
// before it sends a pose. The write path already reads the generation for exactly this reason; a
// read path that did not would answer a question about Y with a fact about X, which is the
// attribution failure this module's threading note is about, one API away.
//
// NOTHING CONSUMES THIS YET -- the enforcing build wires it into the intent authorizer.
bool PositionTrusted(coop::net::Session& session, int slot);

// The last position the ledger ACCEPTED for `slot`. This -- not the puppet actor's transform -- is
// what authorization must consume once it is wired: the puppet is written by the interpolator and
// SNAPS on a teleport, so reading it would make the validated value and the consumed value two
// different quantities and would rest authority on a display pipeline's output. It also goes stale
// OBSERVABLY (the sample count stops advancing), where a puppet transform just keeps standing there.
bool TryGetAcceptedPosition(coop::net::Session& session, int slot, ue_wrap::FVector& out);

// Per-tick, host-only. Emits the throttled per-slot summary and samples the wire-vs-actor divergence
// -- which lives HERE and not in the net-thread write because reading a puppet's transform is an
// ENGINE read and engine reads are game-thread only.
void Tick(coop::net::Session& session);

// --- lifecycle ---------------------------------------------------------------

// Wipes every row and runs the selftest. There is deliberately no OnSessionStop counterpart, and
// the reason is not merely that no subsystem in this tree has one (the project resets on START --
// `session_runtime.cpp` says so in its own comment beside this call) and that a stop hook with no
// caller is a stub RULE 2 forbids. It is that a teardown sweep is the WRONG LAYER for staleness: a
// slot recycles X->Y with no absence in between, so there is no teardown edge to hook in the case
// that actually matters. Both readers refuse a stale generation instead, which covers the recycle
// AND the session boundary with one rule.
void OnSessionStart();

}  // namespace coop::movement_ledger
