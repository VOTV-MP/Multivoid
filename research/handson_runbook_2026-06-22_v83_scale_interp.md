# Hands-on runbook — take-28 — carry JANK (fixed-delay interp) + proxy SCALE (#3a)

**Deployed:** `votv-coop.dll` SHA **`f82943bcd7560724`** to all 4 copies (host / copy / copy2 / dev) — verified
MATCH x4. Proto **v83** (UP from v82 — `PropConvertPayload` gained `scaleX/Y/Z`; both peers must be this
build). Build CLEAN (Release, 0 errors). **NOT autonomously smoked** — you are on the PC, so this is prepared
ground and YOUR hands-on is the test ([[feedback-user-tests-claude-prepares-ground]]).

> Continues `2026-06-22_closeb_carry.md` (take-26/27): the carry-FREEZE was fixed there (`!carrying`,
> `EE0DD83C`). This build fixes the two issues that run found: the carry **JANK** and the proxy **SCALE**.

## What this build changes (two independent fixes)

### 1. Carry JANK — the interpolation phase-stall (ROOT, code-proven; the MAIN fix)
The carry updated but moved JANKILY. Root (proven from code + the `EE0DD83C` host/client logs, NOT a guess —
the prior "key.len=4 mis-route" theory was DISPROVEN by bytecode: `GetKey` returns the FName `"None"`, which
the receiver already guards `keyW != "None"` -> eid; the log showed a clean 60 fps eid-driven proxy):

- `remote_prop::Tick` calls `BeginLerpToPose` (sets `lerpStartMs = nowMs`) then `AdvanceLerp` **in the same
  tick** with the **same** `nowMs` -> `alpha = (nowMs - lerpStartMs)/dur = 0` -> renders the start pos, ZERO
  movement on every new-pose tick. At vsync-60 (pose rate ~= tick rate) almost every tick is a new-pose tick,
  so the proxy barely advanced and lurched on the rare pose-free tick. Classic netcode error (reset the interp
  clock to "now", then sample at "now").
- **FIX:** fixed-delay snapshot interpolation (`reference/mtasa-blue` CClientVehicle shape) — buffer the two
  most recent timestamped poses (`prevLoc/prevPoseMs`, `lastLoc/lastPoseMs`), render at `now - span` BEHIND
  the newest (span = the measured inter-pose interval, ~16 ms = one frame at 60 fps), `alpha` by REAL
  timestamps. The render clock advances EVERY tick independent of pose arrival -> smooth at any frame rate.
- **Edges (confirmed by code, not promise):** the ~16 ms render delay is cosmetic and the form-changing
  converts are position-authoritative (ToClump re-skins in place; ToPile `ClearAnyDriveFor` + `SetActorLocation`
  to the host's landed spot), so the land never mis-places the morph. On a stream STOP, `alpha` clamps to 1 ->
  the proxy reaches the last pose and FREEZES (no extrapolation); the interp is destroyed at both reliable
  edges (`OnRelease` throw, `OnConvert` land) so it never fights the convert's snap or any physics (the proxy
  has none).

### 2. Proxy SCALE — #3a (the proxy rendered SMALLER than the host's pile/clump)
`PropConvertPayload` had **no** scale fields (the prior finding's "it has scaleX/Y/Z" was WRONG — that was
`PropSpawnPayload`). The AStaticMeshActor proxy kept its default unit scale.
- **FIX (proto v83):** `PropConvertPayload` gains `scaleX/Y/Z`; `BroadcastConvert` sends the host's real
  `GetActorScale3D(newActor)` PER FORM (a clump and a pile differ); the proxy applies it (new
  `ue_wrap::engine::SetActorScale3D`, guarded > 0.001 to reject zero/NaN) on every convert (`ReskinProxy`) and
  on spawn (`SpawnProxy` — covers join-placed piles + convert-beat-spawn).

## The test — grab a pile near a cluster, carry, throw (host grabs; client watches)
1. **Carry (the MAIN check).** Host taps E on a pile, carries it around for ~10 s (near a cluster is fine —
   CLOSE-B suppresses the churn). On the CLIENT: the clump should follow **SMOOTHLY — like a normal object in
   the host's hand**, no lurch/stutter. Compare directly to a normal held prop: they should look equally
   smooth (the proxy trails by ~16 ms, imperceptible).
2. **Scale.** The carried clump AND the landed pile on the CLIENT should be the **SAME SIZE as on the host**
   (not shrunken). The "smaller = proxy" size-marker should be GONE.
3. **Drop / throw.** Host drops/throws. On the CLIENT: ONE clump->pile morph at the landing spot. NOTE: the
   throw still **teleports** (lands at the spot, no arc) — this is EXPECTED this build (`simulateDrop` thunk
   is read-only, the velocity flip is the NEXT step), NOT a failure.

## Acceptance criteria (agreed)
1. **carry SMOOTH** like a normal held object (jank gone) — the direct test of the fixed-delay fix. **MAIN.**
2. **pile HOST-SIZE** — the size-marker disappears (proxy == host size).
3. **dup is now unambiguous** — with size no longer distinguishing proxy from orphan, any non-removed pile is
   guaranteed an **orphan** (`isProxy=0`). If you see a dup: grep the client log `isProxy=0` + neighbouring
   lines and bring them — that finally cracks the #3b eid-resolve race (which never reproduced). If NO dup all
   run, #3b stays dormant (noted as not-reproducing).
4. **throw-teleport EXPECTED** (the `simulateDrop` flip is the next pass, after carry-smooth + scale + dup).

## Read the logs (`...\Win64\votv-coop-client.log` / `-host.log`)
- CLIENT `Select-String "drive #"` — the proxy drive (still ~60/s; the fix is HOW it renders between them).
- CLIENT `Select-String "recv convert"` — ONE ToClump (grab) + ONE ToPile (land) per carry, with `isProxy=1`.
- CLIENT `Select-String "isProxy=0"` — should be ABSENT; its presence = the orphan dup (bring those lines).
- HOST `Select-String "TRASH-CH.*BROADCAST"` — the converts now log `variant=` (scale rides the same packet).

## Honest status
- Built CLEAN (Release, 0 compile errors), deployed `f82943bcd7560724` to all 4 copies (hash MATCH x4),
  proto v83. Two audits (perf/hot-path + interp correctness) were spawned at deploy; fold their verdicts
  before relying on this. **NOT verified** — no autonomous smoke (you're on the PC); your hands-on is the test.
- Root + design: `research/findings/votv-chippile-carry-churn-holdplayer-gate-2026-06-22.md` (the jank-root
  correction + the interp-stall + scale).
- After your run, tell me: (1) is the carry smooth like a normal object? (2) is the pile host-size? (3) any
  dup -> the `isProxy=0` lines. Then I flip the `simulateDrop` thunk (throw velocity) or crack #3b.
