// ue_wrap/actors/wisp.h -- engine access for the Killer Wisp (killerwisp_C). Engine-wrapper
// layer: the reflection and struct-offset details of a killerwisp actor, no network or coop
// state; wisp_attack_sync and the dev probe own those and read the wisp's attack state
// through here. The wisp is a Character, so it already rides the NPC pose-mirror pipeline;
// this adds the read side of its attack machine: Target (the acquired victim, nearest by
// proximity and line of sight over the player, kerfur and hound classes; our client puppets
// are player orphans, so they are valid targets), grab (the lift and tear in progress),
// tryGrab (a grab being attempted), killed (the fatality committed) and harmless (the idle,
// non-lethal mode). The load-bearing single-player fact: the grab, tear and kill steps in the
// wisp's graph operate on player 0, the local player, not on Target, so on the host the wisp
// always grabs and kills the host even when Target is a client puppet. The attack sync polls
// these fields to learn the real victim against who the graph physically grabbed, and drives
// the cross-peer mirror. The grab and kill verbs dispatch blueprint-internally, invisible to
// the ProcessEvent detour, so polling the resolved fields is the only host-side observation.

#pragma once

namespace ue_wrap { struct FVector; }

namespace ue_wrap::wisp {

// Resolve the killerwisp class and the field offsets. Idempotent; true once everything
// resolved (false while the blueprint class is not loaded yet, and the caller retries).
// Offsets come from reflection, with documented fallbacks. Game thread.
bool EnsureResolved();

// True if `obj`'s class is killerwisp_C or a subclass. Cheap, a bounded super walk with no
// allocation. False if not yet resolved, or null.
bool IsKillerWisp(void* obj);

// The mirror-relevant attack state of one wisp. `target` is the acquired victim pawn (null
// when not chasing); whether it is the host or a client puppet is the controller test,
// resolved by the caller, not here.
struct State {
    bool  grab          = false;  // lift or tear in progress
    bool  tryGrab       = false;  // grab being attempted; precedes grab
    bool  killed        = false;  // fatality committed
    bool  playerDamaged = false;  // at least one limb torn; the cumulative damage has started
    bool  harmless      = false;  // non-lethal idle mode
    void* target        = nullptr;  // acquired victim pawn, or null
};

// Read `wisp`'s attack state into `out`. False if the read could not be made (null,
// unresolved, not a killerwisp), leaving `out` untouched. Pure field reads. Game thread.
bool ReadState(void* wisp, State& out);

// True if `target` is within the wisp's grab radius, the graph's 550-unit sphere overlap. The
// host synthesises the grab trigger against the wisp's actual Target with this, because the
// graph's own grab flag arms only on player 0 within that radius, so a client puppet the host
// is far from is chased but never grabbed. A pure distance read. False on null. Game thread.
bool InGrabRange(void* wisp, void* target);

// The distance from the wisp to the target in cm (FLT_MAX on null or a dead read). The
// two-stage close uses it against the contact radius before firing the synthetic grab, so the
// wisp visibly swoops onto its victim first; the native capture fires at move-to acceptance,
// not at the arm radius. Game thread.
float DistanceTo(void* wisp, void* target);

// A raw write of the wisp's Target. The graph has no setter; its scan writes the field inline,
// so this write is the game's own mechanism. The aggro selector re-asserts its
// host-authoritative pick through this every tick, dominating the graph's own nearest-pick
// re-scan. False if unresolved or not a killerwisp. Game thread.
bool WriteTarget(void* wisp, void* pawnOrNull);

// A line-of-sight test with the graph's canReach shape: a line trace from the wisp to `target`
// against the static-and-dynamic object set; reachable means the trace did not hit. Pawns are
// not in the set, so neither body self-blocks. The native test hard-codes player 0 as the
// trace end, so it cannot answer for a puppet; this can. False (blocked) on any resolution
// failure, since the caller treats unreachable as not attackable, the native default. Game
// thread.
bool CanReach(void* wisp, void* target);

// The world location of the wisp body mesh's grab socket, the native victim hold point. The
// cross-peer hold drives the victim puppet to it each tick. False if the mesh or socket is
// unresolvable, `out` untouched. Game thread.
bool GrabSocketWorldLocation(void* wisp, ue_wrap::FVector& out);

// The victim-side grab choreography, the native capture's player template. The native kill
// sequence is hard-bound to player 0: the capture sets the player's movement mode to none,
// attaches it to the wisp mesh at the grab socket, shows the player mesh, sets the held flag
// and decouples the controller rotation flags, so the ride owns the view. A victim client
// replays that template against the local player and the local wisp mirror; the montage and
// the timed death ride the existing tear and grab paths. One divergence: the attach snaps to
// the socket (location and rotation) rather than keeping the world transform, since our
// capture equivalent fires at the close radius rather than at contact, and keeping world from
// 2 m away would leave the victim hanging off the socket for the ride.

// Replay the capture's player-side template: the local possessed player is grabbed by
// `wispActor` (the local killerwisp mirror, or the real wisp on the host). Idempotent enough
// for one call per grab; the caller latches. False if the wisp mesh or the player members are
// unresolvable (logged; the flat death still runs). Game thread.
bool ApplyGrabToLocalPlayer(void* wispActor, void* localPlayer);

// Undo the grab template on the local player: detach, walking movement mode, the held flag
// cleared, the two rotation-follow flags restored. Called right before the scheduled ragdoll
// death fires (the native release detaches first, then ragdolls) and on session teardown, so a
// mid-grab disconnect does not strand the player without movement. A safe no-op if never
// grabbed. Game thread.
bool ReleaseGrabOnLocalPlayer(void* localPlayer);

// The wisp's four limb static-mesh components, the gib weld targets: on the kill the graph
// spawns a blood gib and welds it to one of these, and the tear mirror does the same on the
// mirrored wisp. Returns the component, or null.
enum class Limb { ArmL, LegR, LegL, ArmR };
void* ReadLimbComponent(void* wisp, Limb limb);

// Dispatch the wisp's own releasePlayer verb, its canonical grab cancel: detaches the grabbed
// player, clears the held flag, restores the controller, ragdolls the player (non-lethally if
// no limb has torn yet; the death flag is playerDamaged), then after a second resets grab and
// tryGrab so the wisp can re-acquire. The clean, native way to abort a host false-grab when
// the wisp's real Target is a client puppet. May run latent sub-chains; never assume
// synchronous completion. False on null or unresolved. Game thread.
bool CallReleasePlayer(void* wisp);

// The tear-mirror substrate. On a peer that is not the victim, the mirrored wisp is a
// kinematic puppet (npc_sync parked its actor and movement tick), so it never plays the
// fatality montage itself; the tear mirror drives the visual explicitly through these.

// The wisp's body skeletal-mesh component: the montage AnimInstance host and the parent of the
// grab socket. Null if unresolvable. Game thread.
void* BodyMesh(void* wisp);

// Force the wisp's body mesh to always tick its pose, inverting the park so a played montage
// advances on the parked mirror. Idempotent; false if the mesh is unresolvable. Game thread.
bool ForceMeshTick(void* wisp);

// Play the fatality montage on the wisp's body AnimInstance: play the montage asset (resolved
// once), then jump to its fatality section. Best-effort: false, logged, if the asset, the
// AnimInstance or the UFunctions are unresolved (the caller still has the mesh tick and the
// gibs as the degraded tear). Call ForceMeshTick first, so the montage advances. Game thread.
bool PlayFatalityMontage(void* wisp);

// The plain wisp landing drive. The swarm wisp (wisp_C, a different class from the killer
// wisp, and not the coloured siblings) spawns invisible and fades in at its landing edge: its
// tick reads the movement component's floor hit, then sets landed and fires the dir event (the
// fade timeline forward and the point-light ramp). A network mirror parks the movement tick
// (the pose lane owns position), so the floor stays stale, the native edge can never fire, and
// the mirror would stay invisible forever. This drives that edge explicitly: write landed (a
// plain blueprint bool the graph itself writes inline; no setter exists) and call the dir
// event through the normal dispatcher. Idempotent per landed wisp: re-driving replays an
// already-finished forward timeline, a no-op. Exact-class-gated inside. False until the class
// and its members resolve; the caller retries. Game thread.
bool DriveWispLanding(void* wispActor);

}  // namespace ue_wrap::wisp
