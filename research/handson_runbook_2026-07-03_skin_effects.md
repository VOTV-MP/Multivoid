# Hands-on runbook 2026-07-03 — skin take-3 + join-line + TIME instant + NORMALS + EVENTS-NOW

**Deployed: DLL `121C31D2BEFE85B4` on all 4 installs** (hash-verified; protocol **v96** —
both peers MUST be on this DLL to connect). Supersedes `1CDD6079A5241162` (adds only the
env-gated eventforce autotest — zero behavior change in your hands; same wire). Evening
additions NOW smoke-tested (2026-07-03 late eve, you left the PC): baseline 2-peer LAN
smoke PASS (90 s steady, RSS flat, zero errors) AND the events feature verified end to end
autonomously — `eventforce_test: VERDICT PASS`: obelisk PRE armed=0 shots=1 → NOW!-path
force → POST shots=0 ([FIRED]), host arm broadcast + client `REPLAY runEvent 'obelisk'`
in the same second. What autonomy CANNOT see: the visible obelisk chain on screen — your
hands-on below still decides that.

## EVENING ADDITIONS (after your two reports)

### 1. DARK-SUIT root fix (all v1sc suits) — rebuilt paks deployed
Your re-diagnosis was right: not inner geometry — NORMALS. The converter recomputed
shading normals from face windings; the GoldSrc scientist COAT is a double-sided sheet
(outer+inner copies), so the accumulation cancelled itself -> near-black white coat from
most angles (your screenshot: white sleeves = single-sided, dark body = doubled). Root
fix: the mdl's AUTHORED normals (studiomdl smoothing groups) now ride the whole pipeline
(extract -> repose rotation -> cook pack); the recompute is deleted. Offline lambert
renders (game-matching backface cull): coat charcoal -> proper white cloth shading,
front/side/back. ALL 15 deployed models rebuilt on their same profiles + redeployed
(walter `cd369fed` / sci `3cacd30b` / luther `99043feb` / einstein `3bf9d7ac` ...), your
source-folder paks + convert.bat dist updated too.
TEST: sci/walter/luther/einstein etc. — the coat must read as WHITE cloth with normal
shading from every angle (no more charcoal side/back).

### 2. EVENTS: почему "тыкаю и ничего" + the NOW! button
RE of the map trigger graph (votv-event-trigger-graph-RE-2026-07-03.md): for 14 events
the fire button only ARMS a level VOLUME; the effect fires when you WALK INTO it
(obelisk's volume = the base entrance — exactly your "сработал когда зашёл на базу").
The F1 events tab now shows, per volume-gated row:
  [volume-gated] -> idle;  [ARMED - walk-in pending] -> fire pressed, waiting for the
  walk-in;  [FIRED] -> consumed (N=0). Badges refresh ~1 Hz while the tab is open.
And a **NOW! button** per such row = arm (clients still get the arm broadcast) + drive
the volume's OWN overlap handler with your pawn — the native walk-in dispatch, no faked
state. Dangerous rows keep the Ctrl+click guard.
Non-volume rows got gate notes in the tooltip (signals = instant in the SETI pool;
bedEvent = fires on next sleep; agrav = needs isPhysicalEvents; treehouse = instant
build step; etc).
Found + documented a GAME bug: paperGray's activator carries bigmRoar's box key (arming
paperGray activates the wrong volume). NOW! on paperGray still works (drives the box
directly); its ARMED badge can never light up through the native arm.
TEST: F1 > Events > obelisk -> row shows [volume-gated]; press the fire button -> badge
goes [ARMED]; press NOW! -> the obelisk chain completes immediately (alarm etc.) without
walking anywhere. Try wisps / mann / vent the same way.

---
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

## WISP LANE (late eve, "Go next"): the wisps event now MIRRORS -- first creature-event lane

The `wisps` swarm (the biggest creature-event gap) is now host-authoritative + mirrored:
the swarm's 32 wisp_C spawn via EX_CallMath (interceptor-blind) -> caught by a source-gated
Func-thunk on BeginDeferred -> enrolled + broadcast; clients materialize mirrors that fall,
FADE IN AT LANDING (the lane drives the fade edge the parked mirror can't compute), wander
(pose stream, >31-NPC fair rotation), and vanish on BOTH peers at dawn/approach (the
PE-invisible self-despawn is caught by a new pose-walk dead-retire -> EntityDestroy).
Ambient forest wisps (the ticker's) deliberately stay per-peer local -- same decision as the
colored wisps. Autonomous e2e smoke: 32/32 enrolled -> 32/32 mirrored -> forced midday ->
32/32 dead-retired -> 32/32 client teardowns, zero errors.
TEST (needs NIGHT): host F1 > Events > wisps -> NOW! -> BOTH peers should see the same
glowing swarm land in the forest ring (~0.5-0.75 km out) and wander; jump the clock to day
-> they fade out on both. KNOWN fidelity edge: the mirror's fade-in fires when the streamed
pose reads grounded -- if a wisp descends "not falling" (CMC mode) the fade could fire mid-air.
KNOWN OPEN (your report, recorded): killerwisp IGNORES peers -- its special kill sequence
(grab-arm/canReach/socket-lift/fatality) is hard-bound to player 0; the June relay is not
enough. Next thread: probe target acquisition live, then build the full socket-hold sequence
vs puppets.

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

## INNER-MESH -- **REVERTED** (user 18:3x: "ты удалил им всем руки")

The interior-scan strip was WRONG: dm_base is not a hidden inner shell on these models -- it
carries the VISIBLE forearms+hands (in bind pose the outstretched arms live in the same narrow
Z band 36-61 as the coat torso, which the bbox analysis misread as "coat band only"). All three
models lost their arms. Full revert `9963078f` (converter back to pre-scan, RULE 2: the
enclosure heuristic + strip/keep flags deleted); paks rebuilt from the original MDLs with the
pre-scan converter (the exact code state of the verified 25-skin census `805ae0f8`) and
redeployed: sci `5c4dcd1b` / walter `8839e725` / luther `2fc3593c` -- hash-identical across
your source folder + models/ + host+client+copy2. Portable dist rebuilt reverted; your own
convert.bat copy (walter2 folder) was still the clean 07-02 build, untouched.
The original "inner вылазит" complaint is REOPENED -- a future fix needs per-model VISUAL
before/after proof (rendered), not a geometric enclosure heuristic.
