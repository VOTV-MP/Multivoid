# Hands-on runbook — confirm the deterministic re-pile thunk + the triple-grab-cue fix (2026-06-21, take-22)

**Deployed:** `votv-coop.dll` SHA `BA79E705…` to all 4 copies (host / copy / copy2 / dev). Proto **v82**
(UNCHANGED — neither change touches the wire). HEAD `fea04c26`. Build CLEAN, audit folded. The thunk
DETECTION was already VERIFIED in a prior read-only pass; this run confirms the CONVERT + the SOUND fix.

## What changed since the read-only pass (commits `d19ae4d4` + `fea04c26`)
- **`d19ae4d4` — the thunk is now the DETERMINISTIC re-pile CONVERTER (no longer read-only).**
  `OnBeginDeferredSpawnObserve` now calls `OnHostConvert(kToPile)` the SAME tick the clump's `EX_CallMath`
  re-pile spawn fires (it reads `FFrame::Object`@0x18 = the source clump + `*Result` = the new pile). The
  proximity death-watch (`WatchClumpForRepile` / `Tick` / `FindNearestUntrackedChipPile_`) is **DELETED**
  (RULE 2). Net effect you should see: the re-pile converts cleanly with **NO ~5 s vanish-return** (the
  reaper-vs-rebind race is gone by construction).
- **`fea04c26` — the triple grab-cue is FIXED.** The trash ctx-gate was split by packet kind: a CARRY POSE
  now applies ONLY when `ctx == known` (an ahead-of-convert pose is HELD, not applied to the pre-convert
  pile), a RELEASE keeps `ctx >= known`. Net effect: a host grab should fire the grab cue **ONCE**, not
  three times.

## The read-only OFFSET gate — already PASSED (do not re-run)
The prior read-only pass (deployed `B7EEB1BF`) showed many CLEAN `[REPILE]` (`worldCtx` a tracked
garbageClump `[CLUMP] eid≠0`, `Result` a chipPile), and the thunk's `*Result` was **ptr-for-ptr the SAME
pile the death-watch's FindNearest found on every isolated re-pile**. Two independent paths agreed → the
offsets read the right objects → the convert was flipped on. So the offsets are PROVEN on this build; this
run is the gameplay confirmation, not an offset check.

## The test (host does the work; the thunk converts on the HOST)
1. Launch **host** (`mp_host_game.bat`, from `Game_0.9.0n`) + **client** (`mp_client_connect.bat`, from
   `Game_0.9.0n_copy`). **New Game** (fresh save — never an old slot). Let the join settle (world loaded,
   no churn).
2. On the **HOST**: aim at a chipPile, press **E** to grab (a clump appears in hand), carry it, **throw**
   (LMB) so it lands and **re-piles**. Repeat **3–5 times**, at least one inside a **dense cluster**.
3. Watch + listen on BOTH peers.

## What to confirm (the two things this run gates)
1. **SINGLE grab cue (the sound fix):** each host grab plays the grab-in sound **ONCE** (not 2–3x). On the
   client, the grabbed pile becomes a carried clump cleanly — no pre-convert pile-jump, no double cue.
2. **No vanish-return on re-pile (the thunk convert):** when a thrown clump re-piles, the resulting pile
   appears and **stays** — it must NOT vanish for ~5 s and return. The other piles in a cluster stay put
   (no neighbour mis-bind — the thunk is cluster-immune by construction).

## What to capture — paste me the RAW lines (host + client logs)
Host log: `Game_0.9.0n\WindowsNoEditor\VotV\Binaries\Win64\votv-coop.log`
Client log: `Game_0.9.0n_copy\WindowsNoEditor\VotV\Binaries\Win64\votv-coop.log`

```powershell
Select-String -Path 'Game_0.9.0n\WindowsNoEditor\VotV\Binaries\Win64\votv-coop.log' `
  -Pattern '\[PILE\]|\[REPILE\]|RE-PILE\(thunk\)|HOST GRAB ADOPT|CLIENT DROP|SYNC-MIRROR' | ForEach-Object { $_.Line }
```

## Reading the log — PASS looks like
- HOST grab: `[PILE] HOST GRAB ADOPT eid=N` then `[PILE] HOST GRAB(pile->clump) eid=N ctx=…`.
- HOST re-pile: `[PILE] HOST RE-PILE(thunk) eid=N clump=… -> chipPile=… convert IN PLACE (deterministic,
  same tick as the spawn -- no death-watch, no proximity)` then `[PILE] HOST LAND(clump->pile) eid=N ctx=…`.
- CLIENT mirror: `[PILE] CLIENT recv convert GRAB(...) eid=N … [SYNC-MIRROR OK]` and the matching
  `LAND(...)` re-skin, **same eid N** throughout. A `[PILE] CLIENT DROP stale … eid=N` for a late pose is
  GOOD (the ctx-gate working).
- **Same eid N** across the grab + throw + re-pile; the cluster's other piles untouched.

## Watch for the OPEN robustness bug (a separate track — report it but it is NOT this fix)
The **client mirror-staleness dup**: on the client, a join-mirror of a pile can go NOT-LIVE on its own
within ~10 s; then a convert for that eid logs **`mirror NOT-FOUND`** → the client spawns a fresh clump and
the original lingers = a visible dup. **This is NOT fixed by the thunk** (it is client-side staleness, not
the host-side mis-bind the thunk fixes) — design in
`research/findings/votv-pile-mirror-staleness-robustness-DESIGN-2026-06-21.md`. If you see a dup, grab the
`mirror NOT-FOUND` lines from the CLIENT log and note the eid + roughly how long after join — that feeds the
robustness track, not this runbook.

## Honest status
- Thunk DETECTION: **VERIFIED** (the read-only pass agreed ptr-for-ptr with the death-watch).
- Thunk CONVERT (`d19ae4d4`) + the triple-sound fix (`fea04c26`): **AS-BUILT, deployed (`BA79E705`),
  hands-on-PENDING** — this run promotes them. They are NOT VERIFIED until this hands-on (single cue + no
  vanish-return) passes.
- Client mirror-staleness dup: **OPEN** (the robustness track), unaffected by this run.
