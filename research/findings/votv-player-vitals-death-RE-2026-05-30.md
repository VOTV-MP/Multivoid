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

**Faint TRIGGER (user 2026-05-31 + dump-grounded):** faint fires on EXHAUSTION
(energy/stamina near 0). Dump confirms an exhaustion system: `isExhausted_bool@0x0F04`
(bool), UFunction `isExhausted(bool Message, bool& exhausted)@490`, `skipFatigue@0x0B49`.
The numeric meter is most likely `saveSlot.sleep@0xE8` (VOTV's tiredness/stamina bar):
sleep->0 => isExhausted => `faint()@159` => `ragdollMode(true, passOut=true, false)`.
INC2b CONSEQUENCE: the forced-faint verification autotest can call `faint()` DIRECTLY
on the local possessed player to drive the rising edge (no need to deplete stamina),
then assert the OTHER peer's puppet ragdolls end-to-end. Exact trigger chain
(sleep-threshold vs isExhausted vs faint) is BP-internal -> confirm at Inc2b runtime.

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

**3d. PlayerRevive — CUT (user decision 2026-05-30, see 4.5).** Death is permadeath-
rejoinable via the native SP death->menu flow; there is no respawn/revive, so no
PlayerRevive/PlayerSpawn wire is built (RULE 2 -- not deferred, removed).

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

## 4.5. Death lifecycle policy (USER DECISION 2026-05-30)

Question raised: VOTV SP death KILLS the session and throws the player to the main
menu -- what happens in coop when the host or a peer dies? User decision:
**permadeath, rejoinable; host death ends the session.** Both host and peer death
use VOTV's NATIVE SP death->menu flow -- we do NOT intercept the death->menu
transition (principle 6: the native behavior is acceptable for coop here; we
augment only what's actually broken, and here nothing is).

- **Host death = session ends for ALL.** Native SP flow runs: host saves (host-
  authoritative canonical world -> persists, desired) then returns to menu; all
  peers disconnect. This == host exit and is already the model
  ([[project-coop-no-host-migration]] -- no client is promoted). NO new code.
- **Peer (client) death = that peer disconnects to menu** (native SP flow). The
  host observes a normal disconnect and runs the EXISTING teardown (puppet despawn
  + D1-7 per-slot prop-mirror drain). The dying client's death-save is already
  BLOCKED by the client save-block ([[project-foundation2-save-safety]]), so it
  can't write. The peer then REJOINS the host's ongoing world via the EXISTING
  late-join snapshot path. NO new intercept code.
- **Rejoinable:** a kicked-to-menu peer reconnects into the still-running host
  world (existing connect + snapshot). Permadeath = on death they lose their
  in-progress run / private inventory (SP semantics) and return fresh into the
  shared world. Optional polish: an event-feed "X died" / "session ended" line.

CONSEQUENCES for the increment plan (this SHRINKS the pillar):
- **Respawn/revive (old Inc5) is CUT entirely (RULE 2).** No respawn-in-place, no
  revive interaction, no PlayerRevive/PlayerSpawn wire. Death = leave + rejoin.
- **Inc2 (ragdoll sync) NARROWS to the NON-death states** -- sleep, faint/passOut,
  knockdown -- where the player STAYS in the world. The death-vs-faint distinction
  (must-fix #4, via passOut) keeps a fainted/sleeping peer from looking like it
  left, vs a peer that actually died (and is about to disconnect). A truly DEAD
  peer just despawns on disconnect, so a lingering death-ragdoll on its puppet is
  optional (the puppet is torn down anyway).
- **Inc3 (PlayerDamage) UNCHANGED and still needed** -- the combat loop: host-auth
  enemy hits a peer -> reliable PlayerDamage to owner -> owner health drops
  (visible via the Inc1 bar) -> at 0 native death->menu fires -> rejoin. This is
  what makes "enemies target both" + the zombie-kerfur pursuit
  ([[project-coop-enemies-target-both]]) actually threatening to peers.
- **Inc1 (health bar) UNCHANGED** -- gives the pre-death warning. SHIPPED 6f5949c.

RE still needed (LIGHTER now -- the death->menu intercept is NO LONGER needed):
confirm the client death->menu path cleanly DISCONNECTS (not a hard crash mid-
teardown) and that rejoin-after-death works through the existing snapshot. The
exact death->menu call site no longer needs hooking.

## 5. Scope text -> docs/COOP_SCOPE.md (added below; see that doc).

## 6. Increment plan (smallest-first; each smoke-verified per the 6-item checklist)
- **Inc0 (P7, RULE-2 separate commit BEFORE Inc1):** extract the
  GameInstance->save_gameInst->saveSlot offset resolver out of `coop/dev/
  restore_vitals.cpp` into a new `ue_wrap::vitals` accessor (engine substrate;
  ZERO network/quantization logic — quantize stays coop-side).
- **Inc1 — health->nameplate bar (display only): SHIPPED 6f5949c (Pelmentor).**
  v19; `_pad[3]` -> `healthFrac`/`foodFrac`/`sleepFrac` (quantized [0,1] via shared
  protocol.h Quantize/DequantizeUnitFraction); sender packs in net_pump::ReadLocalPose
  off ue_wrap::vitals; receiver `RemotePlayer::SetVitals` (display-only health_/food_/
  sleep_); nameplate draws a 2nd line "HP [|||...] N%" (ASCII text bar, font-safe,
  change-gated on integer percent). As-built NOTES: (a) text bar not a graphical
  UProgressBar -- no UProgressBar in the codebase + its UFunction names unverified;
  text reuses SetWidgetText (zero new unverified reflection), upgradeable later;
  (b) food/sleep streamed (free) but not shown by default. Pre-deploy checklist PASS;
  a latched one-shot sender log PROVED the chain resolves real values on both peers
  (health=100/100 food=96.5 sleep=97.6 -> wire h=255 f=246 s=249). RESIDUAL (hands-on):
  bar-drops-on-damage / F3-refill dynamic check (autonomous peers take no damage).
- **Inc2a SHIPPED 4d52d40 — MUST-VERIFY #8 = PASS** (env-gated host probe): an
  UNPOSSESSED puppet CAN ragdoll via `ragdollMode(1,1,0)` (isRagdoll 0->1 +
  ragdollActor spawned, dead stayed 0); recover = `forceGetUp()` (NOT
  `ragdollMode(0,0,0)`, which does NOT clear the AnimBP gate on an unpossessed
  puppet). Offsets matched the dump (isRagdoll@0x87F dead@0xA78 ragdollActor@0xC40).
- **Inc2b SHIPPED 2026-05-31 — DESIGN PIVOT: continuous per-peer DISPLAY bit, NOT a
  reliable event.** v19->v20; `PoseSnapshot.stateBits` bit 1 `kStateBitRagdoll`
  (sibling of bit0 isInAir + the v19 vitals piggyback). Sender (net_pump::
  ReadLocalPose) sets the bit = OWN `isRagdoll && !dead` via
  `ue_wrap::engine::ReadMainPlayerRagdollState`. Receiver (RemotePlayer::
  SetTargetPose) EDGE-DETECTS the wire bit (`ragdollDispatched_` latch, reset in
  Destroy) + dispatches ONCE per transition; while ragdolled, `ApplyToEngine`
  early-returns (pose-drive paused, physics owns the body).
  **RECEIVER VISUAL (the hard part): the puppet is a tickless, controllerless
  orphan, and VOTV's real `ragdollMode` spawns a SEPARATE `playerRagdoll_C` physics
  actor whose lifecycle (get-up, GC, PhysX teardown) is tick/timeline-coupled -> on
  the orphan it goes PendingKill but never GC-reaps + keeps simulating -> +5 GB RSS
  leak (root-caused; 6 teardown attempts + 2-agent IDA/SDK RE failed to kill it).
  USER DECISION 2026-05-31: ragdoll the puppet's OWN mesh (we own it -> clean
  enable/disable, no zombie).** `StartPuppetMeshRagdoll`: the skin
  (`kerfurOmega_KelSkin`) has no physics asset, so BORROW the player ragdoll asset
  (`kerfurOmegaV1_PhysicsAsset`, resolved by name) -> `SetPhysicsAsset` on
  `mesh_playerVisible` -> `SetCollisionEnabled(QueryAndPhysics)` (render-only mesh
  needs collision or the bodies do not simulate) -> `SetAllBodiesSimulatePhysics(true)`.
  `StopPuppetMeshRagdoll` reverses it. Probe-CONFIRMED: puppet mesh
  `IsAnyRigidBodyAwake=1` during / 0 after, world `playerRagdoll_C=0`, host RSS
  stable 3.5 GB. LOCAL player still uses VOTV's real `ragdollMode`/`forceGetUp`
  (works -- possessed). P7 split: ue_wrap (engine_mainplayer Read/Start/Stop +
  reflected_offset isRagdoll/dead + cached asset/UFunctions) vs coop (1 sender line
  + 1 edge-latch reconcile). **WHY the WIRE pivot (RULE 1, evidence landed AFTER the banked plan):**
  (a) `isRagdoll` is flipped by EVERY ragdoll cause — manual **C-key**
  (`InpActEvt_ragdoll_K2Node_InputActionEvent_25`, user 2026-05-31), exhaustion
  `faint()`, KO — so ONE bit covers all of them; (b) there is NO readable `passOut`
  field on mainPlayer_C (only a `ragdollMode` PARAM), so the banked payload's
  passOut/death/cause fields were unpopulatable; (c) death is EXCLUDED (native SP
  menu flow ends the session), so this is purely a TRANSIENT, lossy-tolerant
  DISPLAY state — exactly MTA's CPlayerPuresyncPacket flag shape, NOT a discrete
  authority event. The continuous bit rides the proven per-peer pose relay, so it
  NEEDS NONE of the reliable design's machinery (new ReliableKind, relay-whitelist
  #1, lane, defer/latch, connect-replay, OnDisconnectForSlot #7) — those existed
  ONLY to survive the reliable shape's permanent-desync-on-dropped-transition
  failure mode, which the self-healing bit cannot have. Verification: e2e wire
  autotest (`VOTVCOOP_RUN_RAGDOLL_TEST=1`) — CLIENT drives local ragdollMode/
  forceGetUp, HOST observes its slot-1 puppet flip isRagdoll 0->1->0 purely via the
  pose stream. (Supersedes the Inc2a standalone #8 probe — e2e covers that path.)
- **Inc3-visual SHIPPED 2026-05-31 — puppet damage HURT-FLASH (display side, no new
  wire).** User asked for a visual when a puppet takes damage. Each peer already
  streams its health FRACTION (Inc1); a DECREASE on the receiver IS a damage event
  (MTA `CPedSync` continuous-health + gate-on-change shape, NOT a discrete packet --
  RE-validated). `RemotePlayer::SetVitals` (now a .cpp def) edge-detects
  `health01 + kHurtEpsilon < health_` (hasPose_-gated so a late-join puppet of an
  already-damaged peer doesn't spuriously flash) and arms `hurtFlashEndMs_ =
  NowMs()+500` (latest-hit-wins). `RemotePlayer::Tick` toggles the nameplate RED on
  the EDGE of the flash window (one repaint per transition, FPS-independent wall-
  clock). `nameplate::SetFlash` writes the UTextBlock color via
  `ue_wrap::engine::SetTextBlockColor` (red on / translucent-white off); the
  auto-redrawing UWidgetComponent shows it, and SetWidgetText (health-bar refresh)
  never touches color so they don't fight. Works for ANY health drop (fall/fire now;
  enemy hits once Inc3-wire lands). e2e autotest (`VOTVCOOP_RUN_DAMAGE_TEST`): client
  lowers own health -> host puppet `IsHurtFlashing` -> PASS, host RSS stable.
  RESIDUAL (user's other option): Minecraft-style RED MESH OVERLAY -- DEFERRED behind
  a runtime probe (kerfur material tint-param names unknown -> silent no-op on a wrong
  name; a red material asset may not exist in the pak -> creating one edits assets =
  Principle 1). Would share the same hurtFlashEndMs_ trigger.
- **Inc3-wire (the combat loop, NOT yet built) — reliable PlayerDamage (host enemy
  hit -> owner):** host observer on the damage UFunction on a CLIENT puppet (gated
  GetController()==null && remote) -> send to owner -> owner runs LOCAL Add Player
  Damage -> its health drops -> streams -> the Inc3-visual flash fires automatically.
  **MUST-VERIFY #6 BEFORE Inc3-wire: does `addDamage`/`Add Player Damage` read health
  from the ACTOR or the shared saveSlot? If shared, invoking it on a host-side puppet
  corrupts the LOCAL player's saveSlot.** Smoke via `DebugForceHitPuppet`.
- **Inc4 — flinch/burning display (polish).**
- **~~Inc5 — respawn/revive~~ CUT (user decision 2026-05-30, see 4.5):** permadeath-
  rejoinable uses VOTV's native death->menu; no respawn/revive feature. Residual
  (light): confirm client death->menu cleanly disconnects + rejoin-after-death works
  through the existing snapshot (no death->menu intercept needed).

## Must-fix / must-verify ledger (from adversarial verify)
MUST (in design): ~~#1 relay-whitelist PlayerRagdollState~~ MOOT under Inc2b's
continuous-bit pivot (the bit rides the existing per-peer pose relay; no reliable
kind to whitelist); #2 PlayerDamage host-only/not-relayable trust (still applies to
Inc3); #3 reliable-event-is-DEAD/ALIVE-authority (was DEATH-context; faint is
display-only so N/A to Inc2b — death stays native SP flow); #4 sleep/faint vs death
distinction — Inc2b excludes death (the `!dead` sender gate) and sleep is a SEPARATE
system (AdreamBase_C, no unified bool), so the bit is purely "ragdolled, not dead";
#5 stream health FRACTION not absolute (Inc1 SHIPPED). MUST-VERIFY: #6 puppet-damage-
vs-shared-saveSlot (pre-Inc3, OPEN); #8 ragdollMode on unpossessed puppet (Inc2a
PASS). SHOULD: ~~#7 OnDisconnectForSlot~~ MOOT under Inc2b (no per-slot ragdoll
state to drain — the puppet's destroy-on-disconnect + fresh-puppet-reads-false
handles it); #9 re-ragdoll idempotent no-op (DONE — the reconcile reads actual vs
desired, no-op when matching); #10 doc the refuse-to-die trust limit (a peer faking
its own ragdoll bit only flops its OWN puppet — harmless, same as the vitals fraction
trust model); #11 P7 extraction (DONE — ue_wrap vs coop split clean); #12 full
redeploy on the version bump.

UNKNOWNs feeding later RE: `p_health@0x658` semantics; the BP setter of `dead`
(we poll, don't need it); VOTV respawn mechanism (gates Inc5); whether `maxHealth`
changes mid-session (moot — we stream the fraction).
