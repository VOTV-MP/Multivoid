# VOTV player health / vitals / death replication — RE + design (2026-05-30)

7-agent workflow (5 RE finders -> design -> adversarial verify; wf_77d1a49b-4ad).
Closes the design half of audit critical-realization #2 (player vitals/death
UNREPLICATED -> combat/horror loop incoherent across peers). Verdict:
**GO-WITH-FIXES** — the must-fixes below are FOLDED INTO this design as the
corrected truth. Confirmed/Inferred/Unknown markers are evidence-locked; HOW the
damage/death control flow wires is BP bytecode (UNKNOWN) — we drive off the
resulting scalar fields + UFunctions, never the BP graph.

## 1. RE summary (offsets evidence-locked)

**Vitals live in TWO stores (critical split):**
- `UsaveSlot_C` (canonical scalars, one per GameInstance via
  `mainGameInstance_C::save_gameInst`): `health@0x428`, `p_health@0x658`
  (SEMANTICS UNKNOWN -> leave untouched), `maxHealth@0x8B4`, `food@0xE4`,
  `sleep@0xE8` (all float). Proven read/write path already in our codebase:
  `src/votv-coop/src/coop/dev/restore_vitals.cpp` resolves
  GameInstance->save_gameInst->saveSlot offsets via FindPropertyOffset (F3
  RestoreVitals). **CAUTION (verifier D3): there is exactly ONE saveSlot per
  machine — ALL puppets + the local player resolve to the SAME saveSlot. Display
  health for a puppet MUST live on `RemotePlayer::health_`, NEVER written to
  saveSlot, or the local player's persisted health is corrupted.**
- `mainPlayer_C` (runtime actor, per-pawn): `dead@0xA78` (bool), `isRagdoll@0x87F`
  (bool), `ragdollActor@0xC40` (AplayerRagdoll_C*), `ragdollComponent@0xC48`,
  `isBurning@0xE50`, `burningTime@0xAC4`, `irradiation@0xCE8`, `air@0xB24`.

**Damage path (fields/delegates CONFIRMED; control-flow BP-UNKNOWN):** entry
`Add Player Damage(float Damage, FVector damageLocation, FVector fullBody, bool
blood, UObject* Source)`, `addDamage`, `damageByPlayer`, `fireDamage`,
`ImpactDamage`/`impactDamageCPP`/`receivedPhyiscsDamage`. Signals: multicast
`receivedDamage(float Damage, AActor* Actor)@0xD48`, `damaged()@0xC10`. Self-damage
flags (local): `canFallDamage@0xD11`, `isFallDamageActive@0xF0C`, `isBurning`,
`fallVeloc@0x8B0`.

**Death/ragdoll path:** `kill()`; `ragdollMode(bool ragdoll, bool passOut, bool
death)@mainPlayer.hpp:484` — **SHARED by sleep/faint(passOut=true,death=false) AND
death(death=true)**; `fallen(bool death)`; `faint` delegate@0x9C0 (non-lethal KO);
`wakeup(bool passOut)`/`forceWakeup`/`forceGetUp` (ragdoll exit); `intComs_dreamInv`
(dream/sleep system). Death UI `mainGamemode_C::redScreen(Uui_deathscreen_C*)@0x788`.
**Respawn/revive UNKNOWN** — no co-op revive in BP/our enum; SP death = load-last-save
OR ragdoll->getup (`AutoRagdollGetup@0xC6B`, `canGetUp@0xC59`). Gates Inc5.

**Our surface today (CONFIRMED nothing):** protocol v18; `PoseSnapshot` 32B with
`uint8_t _pad[3]` + `stateBits` bit0=isInAir, bits1..7 reserved; `RemotePlayer`
drives pose/anim only; puppet discriminator `GetController()==null`.

## 2. Authority model — PER-PEER-AUTHORITATIVE vitals (MTA-shape)

Each peer computes its OWN health + owns its OWN death; others only DISPLAY.
Resolves the host-auth-enemy "contradiction": host-auth means the host runs the AI
and the HIT (its enemy hits peer N's puppet — a real mainPlayer_C in the host's
world — CONFIRMED `npc_zombie_C::attackInRadius` hits overlapping actors). It does
NOT mean the host owns N's health number. Flow:
1. Host enemy hits peer N's puppet on the host.
2. Host sends a reliable **PlayerDamage** event to N (you took X from source S at L).
3. N applies it to its LOCAL mainPlayer via its own `Add Player Damage` BP (own
   armor/FX/health decrement run — inventory is private per peer, so only N can
   mitigate correctly; principle-6 augmentation, not replace).
4. N streams its resulting health (display) for all puppets-of-N.
5. When N's own health crosses lethal, **N** sends the reliable **PlayerDeath**
   (only N authoritatively knows it died). Self-damage (fall/fire/rad) is local —
   no event, just shows in the stream.
Uniform rule: *a peer's health is always computed by that peer.* MTA citations:
`CPlayerPuresyncPacket` (client-reported health stream, server doesn't validate) +
reliable `CPlayerWastedPacket` (death) + `CPlayerSpawnPacket` (respawn). RULE-3:
NO anti-cheat — **documented accepted limitation: a dishonest peer can refuse to
die / under-report health** (4 trusted LAN peers; MTA trusts the same at C++).

## 3. Wire protocol (v18 -> v19)

**3a. Continuous vitals — piggyback PoseSnapshot (unreliable, ZERO size change):**
spend the existing `_pad[3]`: `health`,`food`,`sleep` as uint8. **CORRECTION
(verifier D2, must-fix #5): stream `health` as the FRACTION `health/maxHealth ->
0..255`, NOT absolute** — per-peer saves mean different `maxHealth` (upgrades/story);
a fraction is what a bar needs and needs no shared maxHealth. `stateBits` bit1=
isRagdoll, bit2=isBurning (DISPLAY-ONLY; see 3c gating). `static_assert(sizeof==32)`
stays green. Stale-drop: inherits the pose `seq`+`senderEpoch`.

**3b. Reliable PlayerDamage (host->owner, enemy hits ONLY):** `{senderElementId,
targetElementId, damage, loc[3], impulse[3], blood, sourceKind}`. Receiver routes
by `targetElementId==own element`, invokes LOCAL `Add Player Damage` via reflection
(SP BP runs). **Trust (must-fix #2): host-only SEND, gate `senderPeerSlot==0` on
receive, NOT relayable; host observer gated `GetController()==null && remote-slot`
(never send PlayerDamage for the host's own slot-0 pawn — that hit is purely local,
must-fix #3/A3).** Reliable (not MTA's in-band) because a dropped lethal hit must
not be lost on the unreliable relay path; we do NOT derive damage from health-delta.

**3c. Reliable PlayerRagdollState/Death (the ragdoll authority — CORRECTED):**
`{senderElementId, killerElementId, cause, ragdoll, passOut, death}`. **MUST-FIX #4
(biggest): `ragdollMode`/`isRagdoll` are SHARED with sleep/faint — driving the
puppet from the unreliable `isRagdoll` bit alone would ragdoll a SLEEPING host as
"dead" on every client (sleep is a core nightly mechanic).** So:
- Drive the reliable event on the rising edge of `isRagdoll@0x87F` (NOT just
  `dead`), carrying `death`+`passOut` read at that instant.
- Receiver calls `ragdollMode(ragdoll, passOut, death)` on the puppet with the
  TRANSMITTED flags -> sleeping host shows a sleeping/fainted puppet, dead host
  shows a dead puppet. Non-lethal exit driven by `wakeup(passOut)`/`forceGetUp`
  from the state-change event.
- **MUST-FIX #3 (cross-lane authority): the reliable event is the SOLE authority
  for DEAD/RAGDOLL<->ALIVE transitions. The unreliable `isRagdoll`/health bits are
  DISPLAY-ONLY, gated by the authoritative state — they never trigger a transition**
  (else a late unreliable `isRagdoll=1,health=0` re-ragdolls a revived puppet).
  This gating IS our substitute for MTA's per-respawn `m_ucTimeContext`.
Self-attested (only the owner sends its own). Relay: **must-fix #1 — add this kind
to `IsClientRelayableReliableKind` (session_lanes.h) + `LaneForKind->High`**, else
client B's death never reaches client C (breaks at 3+ peers, invisible in 2-peer
smoke).

**3d. PlayerRevive — DEFERRED to Inc5** (gated on respawn-mechanism RE; cut the wire
per RULE 2 if VOTV death is load-last-save).

**3e. Bump `kProtocolVersion 18->19`; new ReliableKind `PlayerDamage`,
`PlayerRagdollState` (fresh IDs, don't reuse retired 16/17).** v19 bump invalidates
v18 peers -> full `deploy-all.ps1` redeploy required (else silent ParseHeader
mismatch).

## 4. Puppet display (reuse infra)
- Health bar on the existing nameplate from `RemotePlayer::health_` (fraction);
  green->yellow->red. food/sleep streamed (free) but NOT shown by default (private).
- Death: `ragdollMode(...)` on the puppet -> its own BP spawns `ragdollActor` natively
  (**must-verify #8: confirm `ragdollMode` works on an UNPOSSESSED puppet — the BP
  may early-out on GetController(); single most likely Inc2 surprise**).
  `RemotePlayer` ALIVE -> DEAD_RAGDOLL (interp frozen; re-entry no-op, must-fix #9).
- Burning: stateBit2 -> puppet `startBurning`/`extinguishFire`. Inc4.
- Echo/role gate: sender observer fires only when `GetController()!=null` (local);
  self-echo guarded `senderElementId==own`.

## 5. Scope text -> docs/COOP_SCOPE.md (added below; see that doc).

## 6. Increment plan (smallest-first; each smoke-verified per the 6-item checklist)
- **Inc0 (P7, RULE-2 separate commit BEFORE Inc1):** extract the
  GameInstance->save_gameInst->saveSlot offset resolver out of `coop/dev/
  restore_vitals.cpp` into a new `ue_wrap::vitals` accessor (engine substrate;
  ZERO network/quantization logic — quantize stays coop-side).
- **Inc1 — health->nameplate bar (display only):** bump v19; repurpose `_pad[3]`;
  sender packs `health/maxHealth` fraction + food + sleep in ReadLocalPose;
  receiver `RemotePlayer::SetHealth`; nameplate bar. No damage/death. Smoke: F3
  refill -> remote bar fills; hunger/fall drop -> remote bar drops; numeric assert
  puppet health_ tracks source fraction.
- **Inc2 — reliable PlayerRagdollState + puppet ragdoll:** rising-edge poll of
  `isRagdoll@0x87F` carrying death/passOut; receiver `ragdollMode` on puppet +
  RemotePlayer DEAD/KO state (interp frozen); stateBit1 display-only (gated).
  Add to relay whitelist (#1). `vitals_sync::OnDisconnectForSlot` wired at
  net_pump disconnect (#7, clears DEAD latch so reconnect starts ALIVE). RE
  pre-req #8 (ragdoll on unpossessed puppet). Smoke: `DebugForceKill`/sleep ->
  puppet ragdoll vs faint distinct.
- **Inc3 — reliable PlayerDamage (host enemy hit -> owner):** host observer on
  damage UFunction on a CLIENT puppet (gated GetController()==null && remote)
  -> send to owner -> owner runs LOCAL Add Player Damage. **MUST-VERIFY #6
  BEFORE Inc3: does `addDamage`/`Add Player Damage` read health from the ACTOR or
  the shared saveSlot? If shared, invoking it on a host-side puppet corrupts the
  LOCAL player's saveSlot.** Smoke via `DebugForceHitPuppet`.
- **Inc4 — flinch/burning display (polish).**
- **Inc5 — respawn/revive RE + conditional PlayerRevive (or cut per RULE 2).**

## Must-fix / must-verify ledger (from adversarial verify)
MUST (in design): #1 relay-whitelist PlayerRagdollState; #2 PlayerDamage host-only/
not-relayable trust; #3 reliable-event-is-DEAD/ALIVE-authority, unreliable bits
display-only; #4 sleep/faint vs death distinction via passOut; #5 stream health
FRACTION not absolute. MUST-VERIFY (gate increments): #6 puppet-damage-vs-shared-
saveSlot (pre-Inc3); #8 ragdollMode on unpossessed puppet (pre-Inc2). SHOULD: #7
OnDisconnectForSlot; #9 re-death idempotent no-op; #10 doc the refuse-to-die trust
limit; #11 P7 extraction separate commit; #12 full redeploy on v19.

UNKNOWNs feeding later RE: `p_health@0x658` semantics; the BP setter of `dead`
(we poll, don't need it); VOTV respawn mechanism (gates Inc5); whether `maxHealth`
changes mid-session (moot — we stream the fraction).
