# 02 — JOIN-WINDOW kerfur activation: THREE symptoms (the real bug)

**Status: CONFIRMED by log + code (user repro 2026-06-24, host log + client log gripped). All 3 symptoms
named at their break, separated. Fix design for 1+3 = doc 03; symptom 2 (camera) design = doc 04 (pending).**

## CONFIRMATION (the 2026-06-24 repro log -- host Game_0.9.0n + client Game_0.9.0n_copy, run 11:01-11:13)
- **#1 DUP confirmed:** client materialized the host's `prop_kerfurOmega_C` as `remote_prop` mirrors (eid
  4351-4354, random per-peer BP keys gPXK.../CZvy...) at 11:02:42; `[PILE-CENSUS] totalLiveNatives=0` at
  11:02:45 -> kerfur objects are invisible to the pile sweep -> no auto-resolve.
- **#2 CAMERA confirmed:** 11:02:46 `npc-adopt: eid=3145/3147 -- no local twin ... fresh-spawning a mirror`
  -> `SpawnFreshNpcMirror` (the v74 floating-camera deferred path; fresh-spawn BECAUSE the save had them as
  OFF props, no NPC twin to adopt).
- **#3 COLLISION confirmed (user's correction right, my camera-slot guess refuted):** binding is position-
  fuzzy -- 11:06:33 `Gap-I-1 FUZZY MATCH ... within 30.0 cm ... rekeying`; 11:07:22 `kerfur-prop-adopt: bound
  LOCAL save kerfur prop ... (class+pose match)`. Broadcast key `coopkerfur#<eid>` vs save key gPXK... cannot
  match -> 30 cm fallback -> nearest body wins. The hopeless kerfur has no body (#2) -> loses its own fuzzy
  match -> a neighbor claims it. #2 and #3 COMPOUND. (Honest: the mis-bind is SILENT -- no "X stole Y" log
  line; what's confirmed is the 30 cm/class+pose binding + the missing-body compounding.)

## The reframing (critical)

The bug is **NOT** turn_off on a connected client (out of window = CLEAN). It is **host ACTIVATING kerfurs
DURING the join window** [save taken ... client load-100%] -- the SAME window two-channel class as the L1
pile dup. The kerfur was a "clean control" in the L1 pile tests ONLY because we never activated it in the
window. Now we do, and three symptoms surface. **The kerfur_convert channel itself is sound (doc 01); the
break is window-TIMING + the kerfur not being covered by the L1 save-time reconcile/sweep.**

## THREE SEPARATE symptoms (do NOT assume one fix closes all three)

1. **DUP (object + active both on the client).** Host activates a kerfur in the window -> client ends up with
   BOTH the off OBJECT and the active NPC. Hypothesis: two channels in the window -- the transferred SAVE
   carries the kerfur in its OFF state (client load -> off `prop_kerfurOmega` mirror), and the host's
   in-window activation broadcasts the ACTIVE form (KerfurConvert turn-on / EntitySpawn); the client applies
   BOTH and nothing destroys the save-object (the convert's destroy-old missed it -- eid/KerfurId not yet
   bound, or the object loaded AFTER the convert). Direct analogy to the L1 pile two-channel dup.

2. **CAMERA without a body.** Where an active kerfur should be, the client shows only a CAMERA (the
   always-invisible component attached ONLY to active kerfurs) -- no body/actor. Hypothesis: this is the
   KNOWN v74 "floating camera" issue (npc_mirror.cpp:398-401) resurfacing: in the window the client
   FRESH-SPAWNs the NPC (`SpawnFreshNpcMirror`, BeginDeferred/FinishSpawning) instead of ADOPTING a parked
   ghost -- the deferred-spawn path orphans the camera child cascade, while adopt is camera-safe. So
   materialize of the active partially fails -> camera child, no body. A SEPARATE break from #1.

3. **Object POSITION not updating.** The off kerfur object sits at its SAVE position, not the host's current
   position -- exactly like moved-in-window piles BEFORE the L1 fix. Hypothesis: the kerfur object is NOT
   covered by the L1 save-time reconcile/sweep (`pile_reconcile` is pile-only); no save-time twin-destroy
   re-homes it. A SEPARATE break from #1/#2.

**Direction (hypothesis, NOT to build): the L1 window fix (save-time reconcile + post-quiescence sweep) cured
PILES; kerfurs in the window show the same family but are NOT covered by it. Possibly "extend the L1 save-time
reconcile/sweep to the kerfur channel" -- but the three symptoms may be three different breaks (dup =
two-channel; camera = materialize/fresh-spawn; position = sweep-not-covering). Do NOT assume one fix.**

## Repro (user drives; window-timing, like the L1 in-window pile drop)

Host in-world with kerfurs **turned OFF** -> client connects -> between the **`JOIN-WINDOW OPEN`** and
**`CLOSED`** chat cues (already on via `pile_delta_probe=1`) the host **ACTIVATES** the kerfurs -> client
finishes loading -> observe the three symptoms. Normal base kerfur (NOT flesh-room).

## Decision tree -- EXISTING logging is sufficient for the first log (no new marker needed)

Window bound: host `[PILE-1C] slot N world-ready -- JOIN-WINDOW CLOSED` + the save_transfer OPEN cue.
Everything between = in-window. Per symptom (client log unless noted):

- **#1 DUP:** in-window, BOTH a `prop_kerfurOmega` materialize (the save-loaded object, via remote_prop_spawn)
  AND `npc-sync[client OnSpawn]: materialized mirror ... kerfurOmega` (the active) for a correlated
  eid/KerfurId. Plus `kerfur_convert[client]: applied KerfurConvert ... ->NPC(turn-on) oldEid=.. -> newEid=..`
  -- did the convert run + did its destroy-old (`remote_prop::OnDestroy(oldEid)`) actually remove the
  save-object? If the object persists, the destroy missed it (KerfurId/eid not bound in the window).

- **#2 CAMERA:** which spawn path ran for the active -- `SpawnFreshNpcMirror ... materialized mirror`
  (deferred fresh-spawn = the floating-camera-prone path) vs `AdoptExistingNpcAsMirror` (camera-safe). Plus
  any ERROR: `half-spawned state` / `FinishSpawningActor call failed` / `BeginDeferred returned null` (a
  partial spawn -> camera child only). Fresh-spawn-in-window = the v74 cascade resurfacing.

- **#3 POSITION:** the object materialize `loc=(x,y,z)` vs where the host has the kerfur. SAVE position =>
  not re-homed (the object is not in any save-time twin-destroy/sweep -- kerfur != pile).

After the first log, if a symptom is ambiguous (most likely #3's exact delta), add ONE targeted marker (a
`[KERFUR-DELTA]` analog to `[PILE-DELTA]`) and re-run -- do NOT speculatively instrument all three.

## NEXT
User replays the window-activation repro -> "done" -> grep the per-symptom decision tree above -> the FIRST
missing/anomalous line in each symptom's chain names that symptom's break. Propose fixes ONLY after the log
separates the three (L5 lesson: fact not guess; do not assume one fix for three symptoms).
