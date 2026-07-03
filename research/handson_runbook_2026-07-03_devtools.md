# Hands-on runbook 2026-07-03 (take 2) — ragdoll bone visualizer + events menu completion

> **UPDATE (take 4, same day): two menu fixes on the latest DLL.**
> 1. **F1 clock panel fixed** (your "день 4029 и тикает" report): the old panel printed the
>    cycle's raw `Day` float — a within-day ACCUMULATOR (the midnight-cascade counter), not the
>    day number (same root as the save-browser "Day 3566" bug). Now it reads the game's own
>    running clock `timeZ` and shows **"Day N — HH:MM"** (N = the displayed day, same convention
>    as the game's save rows), with the sun fraction alongside. Setting: enter day + h + m →
>    **Set clock** writes `timeZ`; the game's next minute pulse feeds it through its own
>    `saveSlot.settime` (save time persists natively). CAUTION by design: jumping the day
>    FORWARD fires every skipped scheduled story event at once (native settime behavior — and
>    they now mirror to the client via EventFire). The slider is now labeled **Sun position** —
>    visual only, as before.
> 2. **Tilde scoreboard: "Teleport to me" is no longer dev-gated** — the host always sees it in
>    a client row's popup, next to Kick/Ban (host-standard admin verb).

**Deployed (UPDATED take 3):** DLL `5A60579C076A0B06` on all 4 installs (hash-verified; contains
the re-bind thread `2ab718d5` — test its runbook `handson_runbook_2026-07-03_rebind_thread.md` in
the same session — plus these two dev features PLUS the v95 EventFire channel,
`handson_runbook_2026-07-03_eventfire.md`). Protocol now v95 (both peers need the new DLL). Host
ini already carries `[dev] ragdoll_bone_overlay=1` from my autonomous verify — remove it if you
want the checkbox to start OFF.

## 1. Ragdoll bone visualizer (your ask: "checkbox which visualizes the bones ... of the RAGDOLL")

**Where:** F1 → Player → HUD → **"Ragdoll bone skeleton"** (host-only, like every dev overlay).

**What it draws:** for every ACTIVE ragdoll body — bone->parent lines + joint dots projected on
screen (ESP style, visible through walls; ImGui, zero engine objects — the native DrawDebugLine
is compiled OUT of the shipping exe, IDA-verified, so engine debug-draw was a dead end):
- **orange** = YOUR own native ragdoll (press **C**; also faints/trips). The game's ragdoll is a
  separate invisible actor (`playerRagdoll_C` — invisible because ragdollMode marks its mesh
  SceneCapture-only; your camera rides its head bone).
- **cyan** = a remote peer's MIRROR ragdoll body (spawns while that peer is ragdolled — the v22
  pelvis-coupled body).

**Verified autonomously:** the full chain ran in-process on a live real-ragdollMode body:
`[BONE-OVERLAY-CHAIN] bones=6 parented-lines=5` — note the ragdoll body's skeleton is a
SIMPLIFIED 6-bone physics rig (pelvis/chest/head/limb bodies), so expect ~5 lines per body, not
a full anim skeleton. The status line (top-right, "ragdoll bones: ...") renders live — screenshot-
proven. The lines themselves kept dodging the autonomous screenshots (the native auto-getup ends
a still ragdoll in ~4 s and the capture pipeline is slower) — your 10-second check: tick the box,
press C, look down/around; on recover the lines vanish.

**Your original question — "map/attach every bone of the puppet to the ragdoll":** assessment
from the RE: there is NO runtime per-bone write UFunction (bone writes are AnimGraph-only), so a
bone-by-bone copy is out. The promising one-call mechanism is
`SetMasterPoseComponent(ragdollBodyMesh)` on the puppet's two kel meshes while the mirror body is
active (engine-driven full-bone copy, restore with nullptr on recover) — reflection-callable,
UNVERIFIED (needs its own probe; the 2026-06-01 "master-pose issues" memory was about a different
approach). With only 6 bones on the ragdoll rig, the visual gain over the current pelvis-attach
may be modest — the visualizer is exactly the tool to judge that by eye. Say the word and I build
the master-pose probe next.

## 2. Events menu — completion + your campfire question

**The campfire + map-wide smoke event = `treehouse_0`** (day-16: the ariral treehouse camp SE of
the base; one of its four artifacts is `campfire_2.runTrigger(_,1)` → the `eff_campfireSmoke`
column visible across the map; the same event also advances the treehouse build stage — that is
one event by design). **It is already in the menu:** F1 → Game → Events → Story →
**`treehouse_0`** (red = Dangerous → **Ctrl+click**).

**UPDATE (same day, take 3): the EventFire channel is BUILT (v95, DLL `DF0C0295`)** — the
campfire now replays on the client too. This section's caveat is superseded; the campfire test
moved to `handson_runbook_2026-07-03_eventfire.md` test 1.

**Menu fixes you asked for ("не все ивенты в списке"):**
- `arirGraff` was a SILENT NO-OP (the game's switch has only per-variant cases) — replaced with
  **arirGraff_0 … arirGraff_6** (7 graffiti variants, Prop category).
- NEW **Weather** category — the ambient layer that never went through the eventer (fired by
  daynightCycle/mainGamemode timers+RNG; each button calls the exact UFunction the timer calls):
  `spawnFog` (thick rolling fog NOW, 5-20 min), `rain ON`/`rain OFF`, `spawnRedSky`,
  `spawnBlackFog` (black fog + eyer ghosts — Caution), `badSun`, `flowerSpawner`,
  `trySpawnInsomniac` (internally gated, may no-op).
- Deliberately NOT exposed (documented in the header): `superFogEvent` (a literal 5% roll inside
  — a deterministic lever needs the superFog_C spawn transform, follow-up), fleshRain/errorObject
  (need transforms), Eye Moon / jellyfish (verb owner unverified in the current dump).

**Test:** F1 → Game → Events → Weather → `spawnFog` — fog rolls in within seconds on the host
(clients already mirror rolling fog via the weather channel — check the client sees it too);
`rain ON` / `rain OFF` toggles rain. Story → `treehouse_0` Ctrl+click → campfire+smoke on BOTH
peers now (the v95 EventFire channel — full steps in `handson_runbook_2026-07-03_eventfire.md`).
