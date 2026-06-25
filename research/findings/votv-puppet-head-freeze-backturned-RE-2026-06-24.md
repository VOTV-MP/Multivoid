# VOTV puppet head-look FREEZE when back-turned — RE (mechanism) 2026-06-24

> **2026-06-25 UPDATE — THE CLAMP HYPOTHESIS IS REFUTED BY THE HANDS-ON PROBE. The
> freeze is NOT the 45° LookAtClamp; the widen-clamp fix below would have been a no-op.**
> A positive-confirm probe (`coop/dev/puppet_head_probe`) was built + deployed (MD5
> `b70f9aec`) and the user ran the back-turn freeze. The 10:40 host log is decisive:
> `DESIRED off=38 -> TWIST head=36.9` (head TRACKS) vs `DESIRED off=43 -> TWIST head=0.5`
> (head FROZEN) — same ~40°, BOTH well INSIDE the ~67.5° clamp reach. When it freezes
> the head SNAPS TO NEUTRAL (TWIST→0), it does NOT pin at ~67°. So the clamp never bites;
> the head-look is GATED OFF when the puppet faces away. `clamp head=45.0` was read live,
> but that confirmed the clamp EXISTS, not that it is what freezes the head. This is the
> probe-don't-guess win: by-elimination + a positive sub-fact ≠ proof of the mechanism;
> the probe killed the wrong fix BEFORE it was built ([[feedback-probe-dont-guess-rule]]).
> **The clamp diagnosis + the "widen the LookAtClamp" fix in the §CONFIRMED + §REFRAME
> sections below are HISTORICAL/WRONG — kept for the audit trail, do NOT build them.**
> **NOW (re-RE in flight):** the probe was EXTENDED (b70f9aec) to log the LookAt node
> ALPHA + `lookingAtPlayer` + `customLookAt` so the next freeze names the actual gate
> (alpha blended to 0? `lookingAtPlayer` state-switch? `customLookAt` reclaimed by BUA?).
> A smoke (standing, DESIRED~0) showed `alpha h=1.00 n=0.50 lookingAtPlayer=0 customLookAt=1`
> — so `lookingAtPlayer=0` alone does NOT drop the alpha; the freeze-moment gate values
> (the user's back-turn repro on b70f9aec) are still pending. **NO head fix until the gate
> is NAMED by the freeze-moment data.**
>
> **STATUS (original): RE ONLY (probe-don't-guess). No fix built.** This pins WHICH of the 3
> candidate freeze-causes is real, WHERE the gate is, whether the puppet+kerfur path
> is shared, and WHERE to branch a puppet-only fix. One behavioral discriminator is
> left for the user to confirm before any code (clamp-saturation vs anim-tick freeze).
> Constraint (user, explicit): a fix touches PUPPETS ONLY; the kerfur NPC native
> head-freeze is left exactly as-is.
>
> **2026-06-24 PM REFRAME (user clarified the mechanism + narrowed the goal) — see
> the dedicated section at the bottom (`## REFRAME`). The puppet head is the
> REPLICATED look-input of the remote client (camera/mouse), not an idle anim and not
> native head-tracking. This re-scopes the 3 causes to: (a) network relevancy stops
> the look-input when back-turned vs (b) input arrives but the pose isn't applied
> (anim-tick) vs (c) both. STATIC RESULT: (a) is RULED OUT — our custom-UDP pose
> stream sends the look-input every tick unconditionally (no relevancy/visibility/
> distance gate anywhere). Leading cause = (b), and specifically the Mesh-slot
> anim-tick gap (the §Latent finding below is promoted to ROOT). The clamp section
> below is retained as the pre-reframe analysis; it is NOT the cause of the
> input-driven freeze.** The GOAL also narrowed: do NOT build a new head system —
> FORCE the existing (working) puppet head past whatever gate silences it, minimally,
> puppet-only.

## The report

Puppet (remote-player puppet, `mainPlayer_C` orphan) head movements "turn off
entirely" (`отключаются напрочь`) when the puppet is turned with its back. User's
diagnostic anchor: **the SAME head-freeze happens on a kerfur NPC** → so it is a
NATIVE engine/AnimBP behavior on a path the puppet and the kerfur SHARE, not a
mod-only bug. Fix may touch the puppet only.

## The three candidate causes (user's framing) and the verdict

| # | hypothesis | verdict | evidence |
|---|---|---|---|
| 1 | Significance/relevance culling lowers head-bone update rate when not significant | **RULED OUT** | no SignificanceManager is wired in VOTV or in our mod (grep: zero `Significance*` in any gameplay path; the only hits are unrelated 3rd-party audio). Nothing reduces head update-rate by significance. |
| 2 | Animation URO / `VisibilityBasedAnimTickOption` throttles/skips pose-tick when not rendered | **NOT the back-turned symptom; but a SEPARATE real gap exists** (see §Latent) | URO freezes a mesh when it is OFF-SCREEN / not recently rendered. "Back-turned" is still on-screen → URO does not explain it. |
| 3 | The head-tracking controller itself gates the aim on visibility/angle (dot-product to camera / not-rendered) | **the GATE form is RULED OUT; the CLAMP form is the ROOT** | AnimBP RE Q3 (below): the `lookingAtPlayer` dot-product gate switches an idle/look STATE only, NOT the head aim. The head LookAt nodes are ALWAYS ON. What actually pins the head is the static **`LookAtClamp = 45°`** per node — a geometric saturation, not an enable/disable gate. |

## The shared path (puppet AND kerfur) — byte-exact

Both the puppet and a kerfur NPC run the **same AnimBP**:
`UAnimBlueprint_kerfurOmega_regular_C`. The visible head twist comes from **two
native `FAnimNode_LookAt` nodes** aiming at the AnimBP member `lookAt` (FVector
@0x2D90, WORLD), fed each tick by the AnimBP FastPath copies:

- head node `AnimGraphNode_LookAt_1` @0x1730, `BoneToModify='head'`, **Alpha 1.0**, `LookAtClamp = 45°` (@node+0x170)
- neck node `AnimGraphNode_LookAt`   @0x18E0, `BoneToModify='neck'`, **Alpha 0.5**, `LookAtClamp = 45°` (@node+0x170)

So the head can swing at most ~**45° (head) + ~22° (neck at Alpha 0.5) ≈ 67°** off
the body-forward before BOTH nodes saturate at their clamp. Past that the head
**pins at the limit and cannot follow further** → reads as "frozen."

**Head-look is ALWAYS ON (RE Q3, `votv-kerfur-headlook-AnimBP-RE-2026-06-07.md`):**
the LookAt node Alphas are static class-defaults (1.0 / 0.5). `lookingAtPlayer`
(@0x2E01) is a dot-product proximity test ("player within ~127° of facing") that is
consumed ONLY as two `TransitionResult.bCanEnterTransition` copies — it selects an
idle/look STATE (a blendspace), **it does not gate the head-bone aim.** So the
head does not turn OFF by angle; it SATURATES at the clamp. This is the precise
reason hypothesis 3's "controller disables the head" is wrong while the clamp is
right.

**Why this is the shared freeze:** both forms use the SAME node, the SAME static
Alphas, the SAME class-default 45° clamp. When the look target goes >67° behind
the body, both heads pin identically. That is exactly the user's "puppet AND
kerfur both freeze = one native path" observation. The body that carries the head
differs (below), but the head-clamp is identical and shared.

## The body half (differs puppet vs kerfur) — why the freeze persists

The clamp is only visible as a "freeze" when the BODY underneath fails to rotate to
absorb the look-lead (a turning body keeps the target within the 67° reach).

- **Puppet:** the body is driven by OUR receiver-side turn-in-place,
  `coop/puppet_body_yaw.h::State::Update` (NOT the wire yaw verbatim). STANDING
  (speed <= 30 cm/s): the body HOLDS until the camera lead `camYaw = wireYaw +
  headYawDelta` exceeds **`kTurnStartDeg = 60°`**, then turns at 360°/s toward the
  camera until within 5°. Because 60° (turn-start) < 67° (clamp reach), STANDING the
  body *should* start turning before the head fully saturates. **The freeze survives
  when the body does NOT absorb the lead**, the two concrete cases being:
  - **MOVING (speed > 30 cm/s):** the moving branch rate-follows the STREAMED actor
    yaw (movement facing), NOT the camera. So walking while looking far to the
    side/back leaves `camYaw` >67° off the body with NO turn-in-place absorption →
    head clamps, body never catches up → frozen head while moving. (Strong candidate
    for "back-turned freeze.")
  - **Lead opened faster than the 360°/s turn can close**, or held just past the
    clamp in the 60–67° band.
- **Kerfur:** NO `UpdateBodyYaw`. Its body facing is the game's NATIVE bodyfacing
  (the actor BP rotates `ACharacter::Mesh` world-yaw toward the LOCAL player,
  `votv-kerfur-bodyfacing-RE-2026-06-07.md`). When you move behind it faster than
  its native body-turn, the head target (you) goes >67° behind → same clamp → frozen
  head until the body catches up. **Different body driver, identical head clamp.**

## Where the puppet-only branch is (clean, by construction)

The puppet and kerfur AnimInstances are reached by **separate code paths on separate
actors** — there is no shared write site:

- **Puppet:** `remote_player.cpp::ApplyToEngine` →
  `ue_wrap::puppet::DriveHeadLookAtWorld(actor_, worldLook)` where `actor_` is the
  `mainPlayer_C` puppet orphan (`GetController() == nullptr` — the definitive
  local-vs-puppet discriminator). It writes the puppet's TWO overlapped AnimInstances
  (`GetSkeletalMeshComponent(actor_)` = mesh_playerVisible AND the `ACharacter::Mesh`
  slot = the head provider).
- **Kerfur:** `ue_wrap::puppet::DriveKerfurLookAt(npcActor)` / `NpcBodyAnimInstance` —
  a different function, the live kerfur NPC actor.

So a clamp-widen (or any per-node anim write) placed **inside `DriveHeadLookAtWorld`
or the `SpawnPuppet` seed** touches ONLY the puppet's 2 AnimInstances. As long as the
write is per-puppet-AnimInstance (NEVER a GUObjectArray / FindObjectByClass walk over
`kerfurOmega_regular` instances), the kerfur NPC AnimInstances are byte-untouched.
**That is the puppet-only guarantee** — it must be enforced at the fix site (no
class-wide write), and the existing drive split makes it natural.

Clamp write surface (NOT yet in `sdk_profile.h`): `LookAtClamp` is at node-base
**+0x170** (RE `votv-puppet-head-look-RE-2026-06-11.md` §Q3). So head-node clamp =
`kKerfurLookAt_1 (0x1730) + 0x170 = 0x18A0`; neck-node clamp = `kKerfurLookAt
(0x18E0) + 0x170 = 0x1A50`. (Would add a named `LookAt_Clamp = 0x170` offset if a fix
ships.) The node bases are already defined in `sdk_profile.h::anim`.

## Fix DIRECTIONS (design only — NOT built; pick after the user confirms §Open)

- **A — widen the clamp (puppet-only):** write a larger `LookAtClamp` (e.g. 80–90°)
  on the puppet's two LookAt nodes in `DriveHeadLookAtWorld` / the spawn seed. Lets
  the head follow further before pinning. Risk: an "owl head" past ~90°; cosmetic
  tuning. Cheapest; does not address the moving-branch body gap.
- **B — make the body absorb the lead in ALL states (RULE-1 root):** extend
  `puppet_body_yaw.h` so the MOVING branch ALSO turns the body toward `camYaw` when
  the lead exceeds the clamp reach (today moving rate-follows movement yaw only). Then
  the head never has to exceed the native 45° clamp — the "head leads, body follows"
  model fully realized, native clamp kept, no owl-head. This treats the freeze as a
  symptom of the body not turning (the deeper cause) rather than papering it with a
  wider clamp. More faithful to the head-then-body intent + MTA's
  presentation-state body rotation.
- A and B are not exclusive; B is the principled fix, A is a knob if B's feel still
  pins at extreme look-back. BOTH are puppet-only (puppet_body_yaw is per-RemotePlayer;
  the clamp write is per-puppet-AnimInstance). Kerfur is untouched in either.

## §Latent (separate, real, NOT this symptom) — Mesh-slot anim-tick gap

`SpawnPuppetMainPlayer` calls `E::SetAnimTickAlways(meshComp)` on **mesh_playerVisible
ONLY** (puppet.cpp:549), NOT on the `ACharacter::Mesh` slot — which is the puppet's
HEAD+arms PROVIDER (removeArms=false). So the head-provider mesh keeps its class-default
`VisibilityBasedAnimTickOption` (`USkinnedMesh_VisibilityBasedAnimTickOption` @0x604,
likely `OnlyTickPoseWhenRendered`). That means the puppet's HEAD pose freezes when the
**Mesh slot is not rendered** (off-screen / occluded) — an OFF-SCREEN freeze, NOT the
back-turned one. It is a genuine gap worth closing (mirror the always-tick onto the
Mesh slot, puppet-only) but it is a DIFFERENT bug from this report. Flagged here so it
is not conflated with the clamp.

## §Open — the one behavioral discriminator the user confirms before code

The clamp (root) and the URO/tick freeze (latent) look different in-hands:
- **Clamp saturation (leading):** the head TRACKS the look direction and PINS at an
  angle (~67° off body-forward), still micro-moving at the limit, unable to follow
  FURTHER. "Stuck at a maximum twist."
- **Anim-tick / URO freeze:** the head goes COMPLETELY inert at whatever pose it last
  held — no micro-movement at all, regardless of where the look points. "Dead-solid."

User to confirm which it looks like, AND (their offered detail) whether the puppet
head TRACKS something (→ clamp/body, fix A/B) vs idle-self-animates and that motion
freezes (→ would re-open the URO/tick angle). The RE points hard at the CLAMP
(tracking + pins-at-angle + shared with kerfur via the class-default 45°); the
confirmation just rules the tick-freeze in or out before committing the fix shape.

## REFRAME (2026-06-24 PM) — the head is REPLICATED look-input; (a) ruled out, (b) is the gate

**User clarification 1 (mechanism):** the puppet head is driven by the remote
CLIENT's camera/mouse INPUT — the replicated look-orientation of the remote player
(where they point their mouse → streamed → applied to the puppet head). NOT an idle
animation, NOT native head-tracking. Confirmed in code: `local_streams.cpp::ReadLocalPose`
reads the controller `pitch` + `headYawDelta = ctlRot.Yaw - actorRot.Yaw` (the
mouse-look), and `remote_player.cpp::ApplyToEngine` reconstructs a world look-point
from `(curYaw_ + curHeadYawDelta_, curPitch_)` and feeds it to `DriveHeadLookAtWorld`
each tick. So the head IS the replicated look-input.

**This re-scopes the freeze to a network-vs-apply question:**
- **(a) network relevancy:** the look-input STOPS being replicated when the puppet is
  back-turned / not visible (UE-style relevancy culling) → head holds its last value =
  "off." Would be PUPPET-ONLY (kerfur has no input replication). Fix on replication.
- **(b) anim-tick:** the input arrives but the POSE is not applied/ticked when
  back-turned (URO / `VisibilityBasedAnimTickOption`). SHARED with kerfur. Fix anim-tick.
- **(c) both.**

**The user's own discriminator (decisive):** the kerfur ALSO freezes, but the kerfur
head is NOT input-replicated (it is autonomous). So the cause that makes BOTH freeze
cannot be input-replication — it is BELOW that, in a layer they share. That layer is
the anim-tick / pose-apply. → points hard at (b).

### (a) is RULED OUT statically — our pose stream has NO relevancy gate

We do NOT use UE replication for gameplay (CLAUDE.md: custom UDP, two channels). The
look-input pose is published **every tick, unconditionally**:
- `local_streams.cpp::Tick` (line 213): `if (ReadLocalPose(...)) session.SetLocalPose(mine);`
  — no visibility / orientation / distance / relevancy gate. Runs every tick whatever
  the player looks at or whoever can see them.
- `net_pump.cpp` receive/drive loop: `session.TryGetRemotePose(slot, ...)` holds only
  the LATEST snapshot per slot; grep for `relevan|visib|rendered|cull|distance` in the
  drive path = only diagnostics (the pose-diag "rendered position" log), zero culling.
- `net/session.cpp` pose handling (`SetLocalPose` @106): zero relevancy/visibility/
  distance terms.

So the replicated head look-input **always reaches the puppet**, back-turned or not.
(a) cannot be the cause. (This is by design — there is no UE relevancy in our layer to
turn off.)

### (b) is the gate — the Mesh-slot anim-tick gap (the §Latent finding, promoted to ROOT)

With the input always arriving, a frozen head means the POSE is not being applied —
the anim-tick layer. The exact puppet gap (and why it presents as a HEAD-ONLY freeze):
the puppet renders TWO overlapped meshes —
- `mesh_playerVisible`: gets `SetAnimTickAlways` (puppet.cpp:549) → always ticks; but
  it is the DE-HEADED underlay (removeArms=true).
- `ACharacter::Mesh` slot: the HEAD+arms PROVIDER (removeArms=false) → does NOT get
  `SetAnimTickAlways` → keeps the class-default `VisibilityBasedAnimTickOption`
  (`USkinnedMesh_VisibilityBasedAnimTickOption` @0x604, the spawn comment at :547 names
  it `OnlyTickPoseWhenRendered`).

→ When the head-provider Mesh slot is not rendered, its pose (and the LookAt eval that
aims the head at our replicated look-input) FREEZES, while the always-ticked body mesh
keeps moving. That is precisely "the HEAD movements turn off" (specifically the head,
not the body) — a HEAD-ONLY freeze the clamp theory does NOT predict. The kerfur shares
the same class-default URO on its single mesh → its whole pose (incl. head) freezes when
not rendered → the shared symptom. The original spawn code already fixed this for
`mesh_playerVisible` (:547-549 comment) but missed the head-provider slot — a latent
half-fix.

### The minimal puppet-only force (design — NOT built)

"Force the existing system past the gate" = apply the SAME always-tick the sister mesh
already gets, to the head-provider Mesh slot, in the PUPPET spawn path only:
`E::SetAnimTickAlways(meshSlotComp)` in `SpawnPuppetMainPlayer` (next to the existing
:549 call, or where the Mesh-slot anim class is set at :585). One line.
- **Puppet-only by construction:** it runs in the puppet spawn path on the puppet's
  OWN `ACharacter::Mesh` component. Kerfur NPCs are never spawned through `SpawnPuppet`;
  their meshes are never touched. NOT a global / class-wide / GUObjectArray write. So
  the kerfur native URO freeze is left exactly as-is (git diff / kerfur behavior = 0).
- **Not a rewrite:** the replicated look-input + the `DriveHeadLookAtWorld` drive are
  unchanged; we only stop the head-provider mesh from skipping its pose tick when not
  rendered. The existing working system then applies the input as it already does when
  facing the camera.
- `SetAnimTickAlways` writes `VisibilityBasedAnimTickOption = 0`
  (`AlwaysTickPoseAndRefreshBones`) — refreshes bones too, correct for an off-screen
  head that must keep following input (engine_component.cpp:208), identical to the
  sister mesh.

### The PROBE (user-requested; confirms (a)-out + (b)-mechanism before the fix)

Because "back-turned" vs "off-screen/not-rendered" is not fully separable by static
reasoning (if the user's repro is back-turned-but-on-screen, the Mesh slot would still
be rendered and URO would NOT fire — then the freeze would be something else and
always-tick would not fix it), a diagnostic probe confirms the mechanism in the real
repro. Design (ini-gated, ~1 Hz, observer-side, per remote puppet slot;
RULE-2-exempt per [[feedback-rule2-exempts-probes-diagnostics-tools]]):
1. Log `curYaw_ / curPitch_ / curHeadYawDelta_` and whether they CHANGED since last
   second → proves the replicated look-input is still ARRIVING + updating when
   back-turned (confirms (a)-out live).
2. Log the Mesh slot's `VisibilityBasedAnimTickOption` value + a `WasRecentlyRendered`
   read on `mesh_playerVisible` AND the `ACharacter::Mesh` slot → proves whether the
   head-provider mesh is considered not-rendered in the repro (URO firing).
3. (Optional) read back the head bone world rotation to show it is frozen while the
   input updates.
**Verdict logic:** input updates + Mesh slot `WasRecentlyRendered==false` + head frozen
→ (b) confirmed, ship the one-line always-tick. Input updates + Mesh slot rendered +
head frozen → NOT URO (revisit; do not ship always-tick as the fix).

### Acceptance mapping (user's 5)
1. Head works always ← always-tick on the head-provider mesh removes the URO skip.
2. Kerfur untouched ← write is in the puppet spawn path on the puppet's own mesh; never
   touches a kerfur mesh; not class-wide.
3. Minimal ← one line; the existing system is forced past the gate, head not rewritten.
4. Puppet-only by identity ← `SpawnPuppetMainPlayer` operates on the `mainPlayer_C`
   puppet orphan's own component; not a global anim/relevancy change.
5. Mechanism diagnosed ← (a) ruled out by code; (b) is it; the probe confirms in-hands.

## CONFIRMED 2026-06-24 — ON-SCREEN ⇒ the cause is the 45° LookAtClamp (URO ruled out)

**User confirmed in-hands: the puppet is ON SCREEN (host sees its BACK) when the head
freezes; it works great when the puppet faces the host.** On-screen ⇒ the head-provider
mesh IS rendered ⇒ `OnlyTickPoseWhenRendered` does NOT fire ⇒ the pose IS ticking and
the look-input IS being applied. So the anim-tick gap (the §REFRAME/§Latent URO theory)
is NOT this bug, and `SetAnimTickAlways` is NOT the fix here. (The Mesh-slot URO gap is
still a real latent OFF-screen bug — keep it as a separate backlog item, do not ship it
for THIS symptom.)

**By elimination + positive fit, the gate is the native `LookAtClamp = 45°` per node:**
- ruled out: significance (no manager), URO (on-screen), input-relevancy (custom UDP
  sends the look-input every tick unconditionally), AnimBP angle-gate (`lookingAtPlayer`
  is a STATE switch, head aim always-on, Q3).
- what remains: the two `FAnimNode_LookAt` nodes can twist the head at most ~45° (head)
  + ~22° (neck @ Alpha 0.5) ≈ **67°** off the body-forward. When the body faces away
  from the look target and the target sits beyond that reach, BOTH nodes pin at the
  clamp ⇒ the head stops following ⇒ "turns off."
- **why it tracks "facing host" and is SHARED with the kerfur:** the kerfur head always
  targets the host's local camera, so when the kerfur body faces away from the host the
  target is behind the body ⇒ clamp ⇒ frozen (works great facing the host). The puppet
  head targets the remote player's look direction; the freeze shows when the body faces
  away while the look points back past the clamp (run-away-look-back, or the MOVING
  branch where `puppet_body_yaw` follows movement-yaw not the camera, so the body never
  turns to absorb the lead). Same class-default 45° clamp on both AnimBPs = the shared
  native behavior the user observed. The kerfur stays as-is (untouched).

### Minimal puppet-only fix (design — NOT built): widen the clamp on the puppet's nodes

"Force the existing head system past the gate" = raise the `LookAtClamp` ceiling on the
PUPPET's two LookAt nodes so the head keeps following the input past 67°. The existing
look-input + `DriveHeadLookAtWorld` drive is unchanged.
- **Write site:** `LookAtClamp` is at node-base **+0x170** (float, degrees). Head node =
  `kKerfurLookAt_1 (0x1730) + 0x170 = 0x18A0`; neck node = `kKerfurLookAt (0x18E0) +
  0x170 = 0x1A50`. Write both, on BOTH puppet AnimInstances (mesh_playerVisible AND the
  `ACharacter::Mesh` slot — the head provider), ONCE in the `SpawnPuppetMainPlayer` seed
  loop (puppet.cpp:626, which already iterates `{meshComp, meshSlotComp}`). The clamp is
  a static node field (not a FastPath/BUA dest), so a one-time spawn write sticks; fall
  back to a per-tick write in `DriveHeadLookAtWorld` only if a reset is observed.
- **Puppet-only by construction:** the write is on the puppet actor's own AnimInstances
  in the puppet spawn path. Kerfur NPC AnimInstances are reached only by
  `DriveKerfurLookAt`/`NpcBodyAnimInstance` (a different path) and are NEVER written here;
  the class CDO is untouched (we write the instance, not the default), so new kerfur
  instances still init at 45°. kerfur git-diff / behavior = 0.
- **Tuning (1 number):** ~85–90° per node ⇒ head+neck reach ~130° (covers realistic
  look-back, no full owl-head); or ~150–179° for near-1:1 follow at any angle (faithful
  but owl-head at extreme reverse). Recommend ~90° and tune by feel.
- **Alternative (keeps the natural 45° clamp, slightly more code):** extend
  `puppet_body_yaw.h` so the MOVING branch also turns the body toward the camera when the
  look-lead exceeds the clamp reach (today moving follows movement-yaw only). Then the
  body absorbs the lead and the head never has to exceed 45° — no owl-head — the
  head-then-body model fully realized. This is the deeper RULE-1 fix; the clamp-widen is
  the more literal "force past the gate." Both puppet-only; kerfur untouched.

### Optional positive-confirm probe (if the user wants proof before the fix)
Read back the frozen head's actual twist angle: log, observer-side ~1 Hz when the head
is frozen, the head-target-vs-body offset and the resolved head-bone deviation. If the
head pins at ~67° while the target offset keeps growing past it ⇒ clamp POSITIVELY
confirmed. (By-elimination is already strong; this is only if positive proof is wanted.)

## Sources
- `research/findings/votv-puppet-head-look-RE-2026-06-11.md` (the puppet head drive +
  the 45° clamp @ node+0x170 + the two overlapped AnimInstances).
- `research/findings/votv-kerfur-headlook-AnimBP-RE-and-coop-sync-2026-06-07.md` (Q3:
  head-look always-on; `lookingAtPlayer` is a STATE switch, not the aim gate).
- `research/findings/votv-kerfur-bodyfacing-RE-2026-06-07.md` (kerfur native bodyfacing).
- Code: `src/votv-coop/src/ue_wrap/puppet.cpp` (`DriveHeadLookAtWorld` @689,
  `SetAnimTickAlways` @549, seeds @626), `src/votv-coop/src/coop/remote_player.cpp`
  (`ApplyToEngine` head drive @786-813), `src/votv-coop/include/coop/puppet_body_yaw.h`
  (turn-in-place), `src/votv-coop/include/ue_wrap/sdk_profile.h::anim` (node offsets).
