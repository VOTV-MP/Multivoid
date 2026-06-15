# Killer Wisp coop catch/kill/mirror — RE + hardened design (2026-06-13)

Source: 4 RE passes (control-flow window, mainPlayer death->menu, FX inventory,
client-kill asymmetry) + a 5-dimension adversarial design-review workflow (34 findings).
Status: **design LOCKED + de-risked; Inc1a BUILT.** Implementation follows this doc.

## The problem
The host's `killerwisp_C` grabs+kills only the HOST and the tear plays only on the host.
Make it (1) catch/kill the player it ACTUALLY targeted (incl. clients) and (2) mirror the
tear (montage + 4 limb gibs + blood particles) to all peers. Death policy is settled
(vitals-death RE 4.5): permadeath-rejoinable; client death -> native ragdollMode death ->
menu -> rejoin; host death ends the session for all.

## RE GROUND TRUTH (kismet-proven; cite-backed in research/bp_reflection/killerwisp.json + mainPlayer.json)
- **Targeting already sees client puppets.** `scanForActors` = proximity+LOS+nearest over
  {mainPlayer_C, kerfurOmega_C, fossilhound_C}; our puppets are unpossessed mainPlayer_C
  orphans -> already valid targets. `Target`@0x0610 read ONCE at acquisition (@12313).
- **The kill is montage-notify-driven (THE WINDOW).** `capture()`@13229 only STARTS the
  `fatality` montage (latent). Notifies fire in montage time: grab(@5532)->d1(@5188)->d2
  (@4537)->d3(@6749)->d4(@7082)->drop(@7415). `grab:=true`@5978. **THE KILL =
  `getMainPlayer().ragdollMode(true,false,true)`@7560 fires at the DROP notify** = >=6 notify
  frames (seconds) after grab. A per-tick host poll sees grab==true,killed==false for the
  whole tear = a WIDE neutralize window. (Absolute notify ms live in the anim asset, not
  the kismet — but we DON'T need them; structure proves the window.)
- **All kill verbs hit getMainPlayer()/GetPlayerPawn(0) (the LOCAL/host P1), NEVER Target**
  (47 calls). Nulling Target after capture does NOT redirect/stop the kill. Per-limb
  AddPlayerDamage(24)@4916 x4 + forceDrop@5151 on d1..d4; playerDamaged:=true@5188 (first d*).
- **releasePlayer()@13289 = the BP-native grab CANCEL**: detaches, held:=false@13833, restore,
  unsit, `ragdollMode(true,false, playerDamaged)`@14174 (death flag = the VARIABLE
  playerDamaged -> NON-LETHAL if no limb torn yet), then resets grab/tryGrab after 1s.
  `Montage_Stop` halts pending notifies (empty no-op blend handlers) but does NOT detach/heal.
- **mainPlayer death->menu = ~10s, NOT field-abortable.** ragdollMode(...,death=true)->
  fallen(true)-> dead:=true@37412 -> RetriggerableDelay(5s)@37423 -> blackScreen@4353 ->
  RetriggerableDelay(5s)@4542 -> lib_C::loadLevel('menu')@4277. The resume path reads NO field
  -> clearing dead/isRagdoll/forceGetUp does NOT abort once ragdollMode ran. **Only PRE-EMPTION
  works** (stop ragdollMode from running on the host). `canRagdoll`@0xD10=false early-outs
  ragdollMode. `startInvinc`@0xD42 only no-ops `kill()` (the wisp calls ragdollMode DIRECTLY).
- **ragdollMode(true,false,true) WILL kill a CLIENT's own player (no asymmetry).** mainPlayer_C
  has ZERO networking primitives (full scan: no HasAuthority/NetMode/Role); ragdollMode's only
  guard is canRagdoll (default true, no clearer). **THE ADD-PLAYER-DAMAGE NO-OP CRACKED**: its
  guard @841 = `immortal || isDreaming || dead || startInvinc` (NOT authority; startInvinc
  defaults TRUE, clears 1 tick into BeginPlay @3658). ragdollMode does NOT share that guard.
  => use DIRECT `ragdollMode(true,false,true)` for the victim kill, NOT kill(). (This also
  un-blocks the old 2026-05-31 combat-loop owner-apply — see vitals-death-RE addendum.)
- **FX**: only code-spawned actors are the 4 limb gibs — g1=prop_bloodGib_arm_C->arm_L,
  g2=prop_bloodGib_leg_C->leg_R(LEG_R), g3=prop_bloodGib_leg_C->leg_L, g4=prop_bloodGib_arm_C
  ->arm_R (identity transform + weld, on d1..d4). **Blood-paint particles ride the gib props as
  baked UParticleSystemComponents -> FREE once gibs exist.** No decals in killerwisp/mainPlayer.
  Gibs ARE Aprop_C derivatives (Key + Init) -> prop-pipeline eligible (but spawned welded; may
  carry empty Key). Montage = killerWispAnim1_Skeleton_Montage, sections grab/d1-d4/drop/dr1-4/
  fatality/init. Sockets to attach victim: 'playerGrab'.
- **The mirrored wisp on clients is a KINEMATIC puppet** (npc_sync disables actor+CMC tick).

## HARDENED DESIGN (adversarial-review must-fixes folded in)
**A. Host detect + per-victim routing (polled each tick, ue_wrap::wisp::ReadState):**
classify Target via GetController (non-null=host P1 -> DO NOTHING, host dies for real;
null=client puppet -> route). For a client victim:
  - **Neutralize via a PRE-cancel on `mainPlayer.AddPlayerDamage`** (the wisp->player call is
    cross-object = PE-visible) — zero the damage when a false-grab-vs-client is flagged. This
    is the PRIMARY mechanism (collapses the grab->d1 race + the shared-saveSlot health
    corruption). The actor-tick poll is a BACKSTOP that only needs to beat DROP. Snapshot/
    restore host health as belt-and-suspenders. **Destroy the host's BP wisp after relay**
    (kills the re-grab loop) rather than release-and-re-neutralize.
  - Relay reliable **WispGrab{wispEid,victimEid}** to the victim slot; broadcast reliable
    **WispTear{wispEid,victimSlot}** to all (host runs the tear on the puppet too, since its BP
    wisp was aborted).
**B. Victim kill (per-peer-authoritative):** on WispGrab the victim runs `ragdollMode
(true,false,true)` on its OWN player after a **host-scheduled FIXED-DELAY timer** (NOT a
montage notify — the kinematic mirror won't fire notifies). Native death->menu->rejoin. Park
the victim CMC (SetMovementMode None) during the tear so the socket-attach holds.
**C. Tear mirror (all non-victim peers + host's puppet view):** **explicit Montage_Play
(killerWispAnim1)+JumpToSection('fatality') via ue_wrap/wisp, and FORCE the parked mirror
mesh to tick** (invert DisableCharacterTicks). Spawn+weld the 4 gibs to the wisp limb comps
(blood particles free). **Held victim display = a NEW socket-attach mode in RemotePlayer**
(attach puppet to wisp 'playerGrab' + early-return the pose-drive), DISTINCT from
StartRagdollDisplay (mutually exclusive — one AttachParent; the host's SetActorLocation would
otherwise fight the socket). Suppress gib PropSpawn echo + reap gibs on tear-end/despawn.
**D. Spawners (reuse, don't reinvent):** killerwisp_C is ALREADY in kNpcAllowlist + covered by
NpcSuppress_Interceptor + RegisterExistingWorldNpcs (the EX_CallMath detector). Reduce to an
event-driven `gamemode.killerWisps` (<=4 entries) diff feeding RegisterExistingWorldNpcs +
loop-level client suppression of killerWispSpawner/ticker_yellowWispSpawner (ReceiveTick, like
pineconeSpawner). NO per-tick GUObjectArray scan.
**E. Protocol:** TWO kinds (WispGrab host->victim, WispTear host->all), both host-only
(senderPeerSlot==0 gate) + NOT client-relayable; **explicit event_feed master-switch inline
cases** (the silent-third miss — DeskLogLine/SleepState precedent) alongside RestoreVitals/
PlayerDamage/TeleportClient, NOT folded into a family handler; trust + addressed-victim
(victimEid==own) + bounds checks; victimSlot = cross-peer Registry index; wire-exercising dev
button in the FIRST smoke. Version bump.
**F. Peer-death item-drop + file-delete (DEFERRED, no stubs):** ANY client death (any cause)
sends reliable PeerDied{location} -> host drops the peer's items as world props at the death
loc + deletes <save>/coop_players/<guid>.json. Host death = no mechanic. GATED on the per-peer
inventory system (NOT built) + a per-peer Join-GUID wire field (today no remote guid crosses
the wire -> a naive delete would wipe the HOST's own file). Death-vs-quit gating + grab-time
snapshot designed in; ZERO file I/O ships until the inventory system + Join-guid exist.

## Modules + increments
- **Inc1a DONE (BUILT):** `ue_wrap/wisp.{h,cpp}` — EnsureResolved + IsKillerWisp + ReadState
  {grab,tryGrab,killed,playerDamaged,harmless,target} + ReadLimbComponent + CallReleasePlayer
  (reflection-first, dump-verified offset fallbacks). In CMakeLists.
- **Inc1b:** add montage substrate to ue_wrap/wisp — Montage_Play('fatality') + force-mesh-tick
  (engine API, no game-BP RE needed; read puppet.cpp DisableCharacterTicks to invert).
- **Inc2:** `coop/wisp_attack_sync.{h,cpp}` (A+E + AddPlayerDamage PRE-cancel + destroy BP wisp)
  + `coop/wisp_tear_mirror.{h,cpp}` (B + C). Test via the dev-button spawn (positions a client
  puppet as nearest). Keep npc_sync.cpp untouched (~915 LOC, over cap).
- **Inc3:** D (killerWisps diff -> RegisterExistingWorldNpcs + loop-suppress spawners).
- **Inc4 (deferred):** F in `coop/victim_death_drop.{h,cpp}` — builds with the inventory system.

## Adversarial review must-fixes (the 7 CRITICAL, all folded above)
C1 kill via ragdollMode not Write(Health,0); C2 host-scheduled fixed-delay not montage notify;
C3 AddPlayerDamage PRE-cancel not the actor-poll race; C4 host saveSlot.health snapshot/restore;
C5 explicit Montage_Play + force-tick (mirror never plays the BP montage); C6/C7 held = new
socket-attach mode, not StartRagdollDisplay, + early-return the host pose-drive. (Full list +
HIGH/MED in the workflow output; the design here is the resolved form.)
