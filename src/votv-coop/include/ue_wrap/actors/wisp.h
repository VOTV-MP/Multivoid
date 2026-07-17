// ue_wrap/wisp.h -- standalone engine access for the VOTV Killer Wisp
// (Akillerwisp_C / FName "killerwisp_C"). Principle-7 engine-wrapper layer: it wraps
// the reflection / struct-offset details of a killerwisp actor. NO network logic, NO
// gameplay/coop state -- coop::wisp_attack_sync (and the dev probe) own those and read
// the wisp's resolved FSM state through here.
//
// THE WISP IS AN ACharacter (killerwisp.hpp:4) so it already rides the NPC pose-mirror
// pipeline (coop::npc_sync). What this wrapper adds is the read side of its attack FSM:
//   * Target  @0x0610 (APawn*)  -- who it ACQUIRED (proximity+LOS+nearest over the
//     {mainPlayer_C, kerfurOmega_C, fossilhound_C} class allow-set; our client puppets
//     are mainPlayer_C orphans so they're already valid targets).
//   * grab    @0x0600 (bool)    -- the lift/tear is in progress.
//   * tryGrab @0x05D8 (bool)    -- a grab is being attempted (precedes grab).
//   * killed  @0x0618 (bool)    -- the fatality committed.
//   * harmless@0x0658 (bool)    -- the wisp is in its non-lethal/idle mode.
// THE LOAD-BEARING SP FACT (RE, votv-event-system / wisp findings): the grab/tear/KILL
// steps in the wisp's ubergraph operate on GetPlayerPawn(0)/getMainPlayer() -- the LOCAL
// player -- NOT on `Target`. So on the host the wisp always grabs+kills the HOST even
// when `Target` is a client puppet. coop::wisp_attack_sync polls these fields to learn
// the REAL victim (Target) vs who the BP physically grabbed (always the host), and to
// drive the cross-peer mirror. The grab/kill verbs themselves dispatch BP-internally
// (EX_LocalVirtualFunction / EX_CallMath), so they are INVISIBLE to our ProcessEvent
// detour -- polling the resolved fields is the only host-side observation path (the same
// reason keypad_sync / door_sync POLL rather than observe).
//
// RE: research/findings/npc-creatures/votv-wisp-and-client-inventory-RE-2026-06-12.md +
//     research/findings/events/votv-event-system-RE-2026-06-13.md (sec 10).

#pragma once

namespace ue_wrap { struct FVector; }

namespace ue_wrap::wisp {

// Resolve killerwisp_C UClass + the field offsets (Target / grab / tryGrab / killed /
// harmless). Idempotent; true once everything resolved (false while the BP class is not
// yet loaded -- the caller retries on a later tick). Offsets come from FindPropertyOffset
// (reflection-first), with documented Alpha 0.9.0-n fallbacks. Game thread.
bool EnsureResolved();

// True iff `obj`'s class is killerwisp_C or a subclass. Cheap (bounded super walk; no
// allocation). False if not yet resolved / null.
bool IsKillerWisp(void* obj);

// The mirror-relevant attack-FSM state of one wisp. `target` is the acquired victim
// APawn* (null when not chasing) -- the discriminator between host and a client puppet
// is GetController(target) (possessed == the host's own player; null == a puppet) and is
// resolved by the caller (coop domain), NOT here (principle 7).
struct State {
    bool  grab          = false;  // @0x0600 -- lift/tear in progress
    bool  tryGrab       = false;  // @0x05D8 -- grab being attempted (precedes grab)
    bool  killed        = false;  // @0x0618 -- fatality committed
    bool  playerDamaged = false;  // @0x0635 -- at least one limb torn (the 4x24 cumulative dmg has started)
    bool  harmless      = false;  // @0x0658 -- non-lethal/idle mode
    void* target        = nullptr; // @0x0610 -- acquired victim APawn* (or null)
};

// Read `wisp`'s attack-FSM state into `out`. False if the read could not be made
// (null / not resolved / not a killerwisp); leaves `out` untouched on failure. Pure
// field reads (no UFunction dispatch). Game thread.
bool ReadState(void* wisp, State& out);

// True iff `target` is within the wisp's grab/kill radius -- the BP's SphereOverlapActors
// 550u grab-arm radius (killerwisp.json scanForActors @5702). The HOST synthesizes the
// grab trigger against the wisp's ACTUAL Target with this, because the BP's own `grab`
// flag arms only on GetPlayerPawn(0) (the host's local pawn within 550u) -- so a client
// puppet (or an NPC) the host happens to be far from is chased but never grabbed/killed.
// Pure 3D distance read (actor locations), no UFunction. False on null. Game thread.
bool InGrabRange(void* wisp, void* target);

// 3D distance wisp->target in cm (FLT_MAX on null / dead read) -- the v2 two-stage
// close uses it against the contact radius before firing the synthetic grab, so the
// wisp visibly swoops onto its victim first (native Capture fires at MoveTo acceptance
// ~5u, not at the 550u arm radius). Game thread.
float DistanceTo(void* wisp, void* target);

// Raw write of the wisp's `Target` @0x0610 (APawn* or null). The BP has NO setter --
// scanForActors writes the field inline via EX_LetObj, so this masked write IS the
// game's own write mechanism (same legitimacy class as wisp_C `landed`). The v2 aggro
// selector re-asserts its host-authoritative pick through this every Tick, dominating
// the BP's own nearest-pick re-scan. False if unresolved / not a killerwisp. Game thread.
bool WriteTarget(void* wisp, void* pawnOrNull);

// Native-parity LOS test -- the BP's canReach() shape (killerwisp.json 1b): a
// LineTraceSingleForObjects from the wisp to `target` on the {WorldStatic, WorldDynamic}
// object set (lib_obj.obj_statDyn); reachable == the trace did NOT hit. Pawns are not in
// the object set, so neither body self-blocks. The native canReach player-arm hard-codes
// GetPlayerPawn(0) as the trace end, so it cannot answer "can the wisp reach THIS
// puppet" -- this wrapper is that answer. False (blocked) on any resolution failure --
// the caller treats unreachable as not-attackable, the native default. Game thread.
bool CanReach(void* wisp, void* target);

// World location of the wisp body mesh's 'playerGrab' socket -- the native victim hold
// point (Capture attaches the grabbed player there). The cross-peer hold drives the
// victim PUPPET to this point each tick. False if the mesh/socket is unresolvable
// (out untouched). Game thread.
bool GrabSocketWorldLocation(void* wisp, ue_wrap::FVector& out);

// ---- v2 victim-side grab choreography (the native Capture player template) ------------
// The native kill sequence is hard-bound to player 0: Capture does (bytecode, killerwisp
// .json @12313 region): player.CMC.SetMovementMode(MOVE_None) -> player.K2_AttachTo
// Component(wisp.Mesh, 'playerGrab', KeepWorld x3, weld) -> mainPlayer.Mesh.SetVisibility
// (true) -> mainPlayer.held := true -> pawn.bUseControllerRotationYaw := false ->
// mainPlayer.lag.bUsePawnControlRotation := false (camera decoupled so the ride owns the
// view). On a VICTIM CLIENT we replay exactly that template against the LOCAL player and
// the LOCAL wisp MIRROR -- the montage + the timed death ride the existing WispTear/
// WispGrab paths. Divergence note: we attach SnapToTarget (loc/rot) instead of the
// native KeepWorld because our Capture-equivalent fires at the <=200u close radius, not
// at MoveTo-acceptance contact -- KeepWorld from 2 m away would leave the victim hanging
// off-socket for the whole ride.

// Replay the Capture player-side template: `localPlayer` (the LOCAL possessed
// mainPlayer_C) is grabbed by `wispActor` (the local killerwisp mirror -- or the real
// wisp on the host). Idempotent enough for a one-shot call per grab; the caller latches.
// False if the wisp mesh / player members are unresolvable (logged; the flat v1 death
// still runs). Game thread.
bool ApplyGrabToLocalPlayer(void* wispActor, void* localPlayer);

// Undo the grab template on the local player: K2_DetachFromActor (KeepWorld) +
// SetMovementMode(MOVE_Walking) + held := false + restore the two rotation-follow
// flags. Called right before the scheduled ragdoll death fires (native releasePlayer
// shape: detach FIRST, then ragdoll) and on session teardown (a mid-grab disconnect
// must not strand the player in MOVE_None). Safe no-op if never grabbed. Game thread.
bool ReleaseGrabOnLocalPlayer(void* localPlayer);

// The wisp's four limb static-mesh components -- the gib weld targets (killerwisp.hpp:
// leg_L@0x0550, arm_L@0x0558, LEG_R@0x0560, arm_R@0x0568). On the kill, the BP spawns a
// prop_bloodGib and welds it to one of these; the coop tear-mirror does the same on the
// mirrored wisp. ReadLimbComponent returns the UStaticMeshComponent* (or null).
enum class Limb { ArmL, LegR, LegL, ArmR };
void* ReadLimbComponent(void* wisp, Limb limb);

// Dispatch the wisp's own `releasePlayer()` verb -- its canonical grab CANCEL: detaches
// the grabbed player, clears held@mainPlayer, restores controller/sit, ragdolls the player
// (NON-LETHALLY if no limb has torn yet -- the death flag is the wisp's `playerDamaged`),
// then after 1 s resets grab/tryGrab so the wisp can re-acquire. This is the clean,
// BP-native way to abort a host false-grab when the wisp's real Target is a client puppet.
// May run latent sub-chains; never assume synchronous completion. False on null /
// unresolved. Game thread.
bool CallReleasePlayer(void* wisp);

// ---- Inc1b: tear-mirror substrate (engine API; no game-BP RE) -------------------------
// On a peer that is NOT the victim, the mirrored wisp is a KINEMATIC puppet (npc_sync
// parked its actor + CMC tick), so it never plays the BP `fatality` montage itself. The
// coop tear-mirror drives the tear visual explicitly through these.

// The wisp's body skeletal-mesh component (ACharacter::Mesh) -- the montage AnimInstance
// host AND the parent for the victim grab-hold ('playerGrab' socket). Null if not
// resolvable. Game thread.
void* BodyMesh(void* wisp);

// Force the wisp's body skeletal mesh to ALWAYS tick its pose (engine SetAnimTickAlways),
// inverting the npc_sync park so a played montage actually advances on the parked mirror.
// Idempotent; false if the mesh is unresolvable. Game thread.
bool ForceMeshTick(void* wisp);

// Play the `fatality` montage on the wisp's body AnimInstance: Montage_Play the
// killerWispAnim1 montage asset (resolved once via FindObject) then Montage_JumpToSection
// 'fatality'. Best-effort -- false (logged) if the asset / AnimInstance / UFunctions are
// unresolved (the caller still has ForceMeshTick + the gibs as the degraded tear). Call
// ForceMeshTick FIRST so the played montage advances. Game thread.
bool PlayFatalityMontage(void* wisp);

// ---- plain wisp_C landing drive (2026-07-03 wisp mirror lane) --------------------------
// The plain wispSwarm-event wisp (Awisp_C -- a DIFFERENT class from killerwisp_C above,
// and NOT the colored wisp_o/b/g siblings) spawns INVISIBLE and fades in only at its
// landing edge: its ReceiveTick reads CMC CurrentFloor.bBlockingHit -> landed := true +
// dir(true) (fade timeline forward + PointLight ramp). A network mirror parks the CMC
// tick (the pose lane owns position), so CurrentFloor stays stale and the native edge can
// NEVER fire -- the mirror would stay invisible forever. This drives that edge explicitly:
// write `landed` (a plain BP bool the BP itself writes inline -- no setter exists) + call
// the `dir` event through the normal dispatcher. Idempotent per landed wisp (re-driving a
// landed wisp re-plays an already-finished forward timeline: no-op). Exact-class-gated
// internally. False until wisp_C + its members resolve (caller retries). Game thread.
bool DriveWispLanding(void* wispActor);

}  // namespace ue_wrap::wisp
