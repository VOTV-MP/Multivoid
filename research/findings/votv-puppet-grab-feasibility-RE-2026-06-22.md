> **UPDATE 2026-06-23: the RE below is DURABLE + still true (the puppet grab ENGAGES with no controller dep;
> its tick won't drive the PHC → the host drives the hold pose). The "Consequence for Increment 2" steps at
> the bottom are now AS-BUILT** (the full chain, `votv-increment2-clientgrab-FULL-CHAIN-AS-BUILT-2026-06-23.md`)
> — with ONE correction: step 2's "client suppress-native" is RETIRED; recognition is a camera-ray cone (a
> bare proxy can't be `lookAtActor`; the pile mesh has no collision body). The probe itself is unchanged truth.

# Puppet-grab feasibility — can the host execute a chipPile grab ON a puppet? — RE + runtime probe

**Date:** 2026-06-22. **Status:** the open `[?]` in `docs/piles/08-HOST-AUTH-TRASH-CHANNEL.md`
("Increment 2 — the CLIENT-grab direction") is now **RESOLVED** — by static RE (bytecode, agent-verified
+ cited) AND a runtime probe asserted against a real 2-peer smoke log. Durable RE fact (an engine/puppet
behaviour that won't change unless the game recooks the grab graph).

**Deployed for the probe:** `votv-coop.dll` SHA `CDBDCFB2996A68DC` ×4, proto v83 (NO wire change — the
probe is a read-only harness scenario, env `VOTVCOOP_RUN_PUPPET_GRAB_PROBE=1`). HEAD at probe time
`a5282f57`.

---

## The question (Increment-2 gate)

The client-grab direction (`docs/piles/08`) has the host execute the REAL grab verb on **puppet-N** (an
unpossessed `mainPlayer_C`, `GetController()==null`) via `reflection::CallFunction(pile, playerGrabbed,
{puppetN, hit})`, so every peer mirrors one host-authoritative convert. The open question: **does a grab
actually work when driven on a puppet**, or does it depend on player-state a puppet lacks (possession, a
PlayerController, a camera/view-target, the physics-handle grab path)?

## Static RE verdict (B): the grab path has NO controller dependency

Full trace cited in the agent RE (this session). Key facts:

- `actorChipPile_C::playerGrabbed` calls `pickupObjectDirect` on the **`player` PARAM** passed in
  (`actorChipPile.json:8724-8769`, `EX_Context` ObjectExpression = `K2Node_Event_player`), NOT a hardcoded
  `GetPlayerCharacter`/`GetOwningPlayer`. So whoever you pass as `player` carries it — pass the puppet, the
  puppet carries it. The SAME value is written to the clump's `holdPlayer` (`:8508-8524`).
- The player-side grab chain `pickupObjectDirect` → `pickupObject` → ubergraph → sets `grabbing_actor` /
  `grabbing_component` → **`grabHandle.GrabComponentAtLocationWithRotation`** (a `UPhysicsHandleComponent`)
  → per-tick `grabHandle.SetTargetLocationAndRotation`. EVERY field/component it touches
  (`grabbing_actor`, `grabbing_component`, `grabHandle`, `grabrot`, `grabLen`, `Camera` as a component) is
  a plain member of `mainPlayer_C` — **a puppet has all of them.**
- A whole-file grep of the grab + grab-maintenance path found **NO** `GetController`, `IsLocallyControlled`,
  `HasAuthority`, `cast<PlayerController>`, `GetPlayerController`, or `ViewTarget`. The hold is computed
  **component-relative** (`Camera.GetForwardVector()*grabLen + Camera.K2_GetComponentLocation()`), never via
  the PlayerController/camera-manager.
- The ONE thing static analysis could not prove: whether an **unpossessed puppet's `ReceiveTick`
  dispatches**, i.e. whether the per-tick PHC maintenance RUNS so the clump tracks to the puppet's hand, vs
  floats at the spawn spot.

## Runtime probe — the decisive result (asserted against the real smoke log)

`harness/autotest_chippile.cpp::RunPuppetGrabProbe` (host-only): wait for the slot-1 puppet to go live,
confirm `GetController()==null`, find the nearest chipPile, call `pile.playerGrabbed(Player=puppet)`, then
poll the grab state + geometry for ~4 s. Why the geometry is the right signal: in the trash channel the
host **streams the clump's HOST-side world pose** (PropPose, eid-keyed) to all peers, so "the clump ends up
in the puppet's hand ON THE HOST" is exactly what every peer renders.

**Host log (`Game_0.9.0n/.../votv-coop.log`, 2026-06-22 21:29, SHA `CDBDCFB2996A68DC`):**

```
slot-1 puppet LIVE actor=...ACB9070, GetController()==null CONFIRMED (a true puppet)
puppet@(1879,806,6186) yaw=-164 -- nearest chipPile=...3994CB80 @(1214,940,6099) dist=684cm
playerGrabbed(puppet) fn=... paramSet=1 call=1
poll 0  -- grabbing_actor=...D69DFD00[clump=1] holding_actor=0 dist=678cm dZ=-67cm grabLen=150.0
poll 8  -- grabbing_actor=...D69DFD00[clump=1] ...               dist=678cm dZ=-68cm grabLen=150.0
poll 32 -- grabbing_actor=...D69DFD00[clump=1] ...               dist=678cm dZ=-68cm grabLen=150.0
VERDICT -- ENGAGED=1 HELD=1 TRACKED=0 | engagedPolls=40/40 | dist first=678 last=678 min=678 |
           dZ first=-67 last=-68 | grabLen=150.0 | pulledIn=0
RESULT = FLOATING (held, tick likely DEAD)
```

Reading:
- **ENGAGED=1** — the grab runs on a puppet; `grabbing_actor := clump` at poll 0. The static verdict is
  runtime-confirmed: no possession/controller needed to ENGAGE the grab. **[V harness]**
- **HELD=1** — the binding persisted all 40/40 polls (~4 s); the clump did not drop or self-free. **[V harness]**
- **TRACKED=0 (FLOATING)** — the clump stayed at the pile's spawn location (`dist=678cm` constant = the
  spawn distance), `dZ=-68cm` constant (ground level, not lifted to hand height, not falling), `grabLen`
  frozen at 150.0. **The per-tick PHC maintenance does NOT run on the unpossessed puppet — the puppet's
  `ReceiveTick` is effectively not driving the hold.** **[V harness]**

(`dZ` constant rather than decreasing ⇒ the clump is not gravity-falling; the grab held it static at the
grab spot. Whether the PHC engaged-but-untracked or never moved the target is moot — the actionable truth
is identical: the clump does not reach the puppet's hand on its own.)

## Consequence for Increment 2 (the refined design — no longer a blocker)

The host-side puppet grab is **feasible**: it ENGAGES and the identity binding (`grabbing_actor`/`holdPlayer`
= puppet, the eid hook the trash channel keys on) works. The ONLY gap is the **visible hold position**: the
host must drive the puppet-held clump's pose itself rather than relying on the puppet's tick. Because the
trash channel already streams the clump's host-side pose to all peers, this folds in cleanly:

- **For a puppet-held trash clump, the host kinematically positions the clump at the puppet's hand each tick**
  — `puppetCamLoc + syncedForward * grabLen` (or, simplest, set the clump's actor location/rotation directly
  to the puppet's hand transform), exactly the kinematic drive `local_streams` already applies to host-held
  props. The data (the puppet's synced aim) is already streamed. No reliance on the puppet's `ReceiveTick`,
  no `grabHandle.SetTargetLocationAndRotation` fight.

So Increment 2 is: (1) wire `GrabIntent`/`ThrowIntent`/`PileResyncRequest` (v83→v84, the 3-place ReliableKind
router); (2) client suppress-native + send `GrabIntent`; (3) host `OnGrabIntent` → validate → `playerGrabbed`
on puppet-N + the eid convert broadcast; **(4) NEW, from this probe — the host drives the puppet-held clump's
hold pose** (the verdict-B fallback). Plus phase-2 collision (the client cannot aim a NoCollision proxy to
initiate the grab — a separate prerequisite). All UNBUILT, greenlight-gated.

## Reproduce

```
# build + deploy, then:
VOTVCOOP_RUN_PUPPET_GRAB_PROBE=1 python tools/mp.py smoke --duration 200 --join-grace 120
# read Game_0.9.0n/.../votv-coop.log for `puppet_grab_probe: VERDICT`
```
The probe is read-only beyond the one pile the real grab verb itself consumes; host-only; the client just
stands so the puppet exists. No wire change (proto v83).
