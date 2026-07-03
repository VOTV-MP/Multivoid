# Hands-on runbook 2026-07-03 — skin take-3 + join-line + TIME instant + inner-mesh

**Deployed: DLL `D9B66A83DCC20D7C` on all 4 installs** (hash-verified; protocol **v96** —
both peers MUST be on this DLL to connect). Includes everything below (smoke x3 PASS).
Autonomous 2-peer LAN smoke: PASS (both peers stable, puppets spawned, no RAM breach); rig log
lines below are from that run. For the test, the inis are pre-set: HOST `player_skin=kerfur_omega`,
CLIENT `player_skin=kerfur_mynet` (change freely in F1).

## What was ACTUALLY wrong in take-2 (your screenshots), root-caused from your logs + bytecode

1. **Violet light on EVERY skin** — the take-2 bitfield guess (template byte XOR class-CDO byte)
   died on the very first real template: the lifeLight template overrides TWO flags in one packed
   byte, so every read logged `multi-bit flag delta t=10 cdo=20 -- default kept` (your log, 25x)
   and the belly light (LightColor 255,156,255 = that violet) got instanced on every rig — the
   "1 SCS comp(s)" in every take-2 rig line WAS the light. FIX: reflection now reads the REAL
   FBoolProperty bit mask (calibrated slot +0x78, bVisible mask 0x20 — confirmed live); the
   template's authored bit is read directly. lifeLight bVisible=FALSE -> never instanced.
2. **mynet effects across the whole screen** — two unhonored template flags:
   - the 11 digital-grid decals author **bAbsoluteRotation=TRUE** (the projection box points
     world-DOWN, always — grid patches on the floor under her limbs). Our KeepRelative attach made
     the boxes tumble with the limb bones until one swallowed the camera -> grid smeared over the
     screen. FIX: SetAbsolute per the template flags after attach.
   - all 17 electricity emitters author **bStartWithTickEnabled=FALSE** (the native sim never
     advances — authored-off decoration). Ours ticked at full rate -> continuous particle flood.
     FIX: tick disabled right after spawn, per the template.
   - bonus: the 3 zapp crackle loops now carry their authored `att_small` attenuation (was null ->
     crackle audible way too far).
3. **mynet double footsteps** — the native mynet's own step() calls lib_C::step with **volume=0**
   (the default surface footstep is MUTED) and plays boltrix_mediumHit itself at the actor
   location, vol 1, att_default. We were playing BOTH the default step and boltrix. FIX: step
   modes per bytecode — REPLACE (mynet): the puppet's lib step runs at volume 0 (trace/water/
   friction still native) + boltrix at native volume; ADDITIVE (keljoy): default step stays,
   squeak layered with the native stepped() math (clamp(MaxWalkSpeed/400, .5, 2)*vol -> /4 volume,
   /2+1 pitch, attached to the body).
4. **(audit catch) mynet step BURST never actually spawned** — since take-1, SpawnStepBurst filled
   the SpawnEmitterAtLocation frame under three WRONG param names (SpawnLocation/SpawnRotation/
   bAutoActivate vs the real Location/Rotation/bAutoActivateSystem), so the emitter spawned inert
   at the world origin — plus 3 ParamFrame error log lines per step. Correctness agent caught it;
   fixed — the per-step electric burst at the feet is expected to be VISIBLE for the first time.

## Also in this build (your request)

**"<nick> joined the game" now fires at the joiner's APPEARANCE** — the moment its puppet spawns
on your screen — instead of ClientWorldReady+5s (measured live: world-ready 15:27:23, puppet
15:27:34 — the old line ran ~6 s before the body). No artificial delay. "Connecting to the
game..." still comes at connect; your own "Joined <host>'s game" keeps its +5 s (loading screen).
Test: host up, client joins — the "joined the game" line must land at the SAME moment the robot
pops in, not before.

## Tests

1. **Any kerfur skin (the violet report):** cycle omega / ariral / keith / maxwell etc. NO violet
   light on any of them, no light cast on the ground at night. The omega body should now look
   EXACTLY like a crafted NPC kerfur next to it (same mesh+materials, incl. whatever chest glow
   the naturals show — that part is baked in the game's own materials).
2. **Omega face:** live blue animated face on the screen (blinking), `_m` pink, `_h` green.
3. **mynet (the flood report):** standing still — NO screen flood; the correct native look is
   modest: grid glow patches on the FLOOR under her, spark crackle audio nearby only. Walking —
   an electric burst + boltrix hit per step on the PUPPET (host view), and NO default footstep
   under it (electric hit only).
4. **mynet own body (boundary, deliberate):** your OWN steps as mynet still sound default to
   YOUR ears + the visual burst; the electric REPLACEMENT sound can't be layered on the local
   body without doubling (the native local step path is EX-invisible / unmutable). Remote peers
   hear you correctly. If this bothers you, say so — the next step would be a lib_C::step VM
   patch to mute the local default (heavier, doable).
5. **keljoy:** default step + squeak on top (squeak now quieter + slightly pitched-up — the
   native stepped() mix, not the flat 0.6 of take-2).
6. **Ragdoll / skin switching / both-role** as take-2 (rig hides with the plushie, full teardown
   on switch, host-worn skins mirror to the client).

## Log proof (votv-coop.log) — already seen in the smoke run

- `reflection: FBoolProperty payload slot calibrated to +0x78 (SceneComponent CDO bVisible mask 20)`
- `skin_effects: rig 'kerfur_omega' ... 0 SCS comp(s), face=YES (type=0 fmi=1)` (take-2 said
  "1 SCS comp(s)" — that comp was the violet light)
- `skin_effects: rig 'kerfur_mynet' ... 31 SCS comp(s), ... stepSound=YES, stepBurst=YES` (was 32)
- `player_handshake: slot 1 joined the game (announced at puppet appearance)` in the SAME second
  as `net: first remote pose on slot 1 -> auto-spawning puppet`
- NO `scs_rig: multi-bit flag delta` (code deleted), NO `bool property ... not found`, NO
  `bStartWithTickEnabled unresolved`, NO `load MISS`, NO `ParamFrame::Set: unknown param`.

## Known boundaries

- Own-body REPLACE sound (test 4 above).
- The ADDITIVE squeak plays regardless of floor surface; the native game skips it on water
  (water replaces the whole step chain) — puppet squeak on water is a minor fidelity edge.
- Face gated to the 4 omega bodies; maid/krampus are mesh-only skins (their base-class SCS is
  all dormant sentient nodes = nothing instanced, correct).
- The col (paintable) kerfur NPC's picked color is still not on the wire (separate note).

## TIME (your report: "не работает нормально и мгновенно")

Root 1: set-clock wrote only the named clock; the sun re-derives from totalTime every tick and
never moved until the minute pulse. Root 2: the CLIENT's HUD clock/day were structurally FROZEN
(TimeScale=0 = no minute pulse; the wire only carried the sun accumulators). Now: dev-menu
set-clock/sun-slider write the FULL state (sun jumps the same frame, HUD + scheduler + save
consistent), and v96 TimeSync carries the named clock -- the client's clock/day converge <=2 s.
Test: host F1 > set Day/Time -> lighting changes INSTANTLY on host; client sun follows <=2 s and
its HUD clock/day now match the host. Forward day-jumps still fire skipped scheduled events
natively (that part is the game's own settime walk).

## EVENT TRIGGERING + MIRROR (your report) -- what changed + what I need from you

- Trigger dispatch itself is instant (runEvent/runSpecialEvent on the eventer, same frame). BUT
  many rows only ARM a controller that waits for its native day/time window (the menu shows each
  row's native Day + HH:MM) -- with the time fix above, the workflow "trigger -> set clock to the
  row's window" is now actually instant. If you want one-click "fire AT its native time" (auto
  clock jump), say so -- easy to add now.
- MIRROR coverage today: save/story/cosmetic flips + scare arms REPLAY on the client; the 16
  world-actor classes (saucers/ships/UFOs/droppers) mirror live via the WA lane; **Character
  creatures (wisps, boars, grays invasions, buster...) have NO lane yet** -- the client will not
  see those spawns. That's the big known gap, and it's per-creature-type work (identity + pose
  interp like the kerfur lane).
- WHICH events did you trigger when the mirror looked broken? Name 2-3 -- I'll check each against
  the lane matrix and either fix the verdict or build the missing lane next.

## INNER-MESH (sci_v1sc / walter_v1sc / luther_v1sc "inner вылазит")

The GoldSrc ports carry an LD dm_base torso+arms shell under the HD coat (+ sci had a second
Gordon face INSIDE the head + all had an inner mouth plane). The converter now ray-scans every
face group and auto-drops fully-enclosed interior shells (census: 16/16 models clean; shirt/
belt/glasses survive); dm_base/Gordon_head force-stripped for these three. Rebuilt paks are on
models/ + host+client+copy2 installs. Test: pick sci_v1sc / walter_v1sc / luther_v1sc -- no
geometry poking through the coat/head at any animation. Your convert.bat got the fix too
(portable dist rebuilt).
