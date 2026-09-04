# 06 — OBS-2: a save kerfur OBJECT missing on the client (fresh-spawn OnSpawn'd but never registered a mirror)

> **[corr 2026-09-04 -- swept by `/documentize`.** This is a POINT-IN-TIME June diagnosis and stays as history, exactly as `docs/kerfur/ARC.md` says (*"the older `docs/kerfur/NN-*.md` files are point-in-time diagnoses ... and remain valid as history, but the capability census and the plan live HERE"*). **`:11` still says "(this build, uncommitted)" while the header says `-> COMMITTED`** (the arg-slot fix landed as `8c96d7aa`); the header is right. Still genuinely OPEN: the sub-Q2a class+pose BindAsMirror miss at quiescence (`:58`) and the index-seed-race DEFERRAL (`:16`). **Its `## NEXT` section is NOT a live plan** -- the kerfur plan is `docs/kerfur/ARC.md`, whose front is W1 parked on the gate seam.]**


**Status: ROOT PINNED + FIX VERIFIED hands-on (clean-bracket run 14:59:48) -> COMMITTED. The arg-slot fix
(deferKerfur correct slot) is the complete fix: the probe fired `[OBS-2 PROBE] eid=3147: TWIN ABSENT` at
quiescence -> `no local twin (load tail quiesced)` -> `spawned ... prop_kerfurOmega_C` -> a distinct mirror.
TWIN ABSENT confirmed -> NO BindAsMirror class-family widening needed (arg-fix alone closes OBS-2). Probe
REMOVED (diagnostic served its purpose). Deployed MD5 `F419F594` (probe-free). Push HELD. Surfaced/closed
alongside the fuzzy-gate (doc 07) -- the fuzzy-gate routes the window-turned-off kerfur into THIS fresh-spawn
path, which is how the OBS-2 fix finally got exercised + verified.**

> **AS-BUILT (this build, uncommitted):** (1) ROOT FIX -- `OnSpawn(e.payload, 0, localPlayer, /*fromConvert=*/false,
> /*deferKerfur=*/false)` (deferKerfur in the correct slot) re-enables the fresh-spawn fallback. (2) PROBE -- the
> quiescence fresh-spawn branch logs `[OBS-2 PROBE] ... TWIN PRESENT/ABSENT` (nearest untracked kerfur-prop actor
> within the bind radius + its leaf class + class-match-to-pending) so the hands-on reveals absent-vs-present.
> Probe is diagnostic-only (removed before the final commit). NON-DETERMINISTIC repro: OBS-2 only fires when a
> kerfur loses the index-seed race and is DEFERRED -> the runbook directs multiple connect cycles.

## ROOT (CERTAIN -- log + code proven): a positional-argument-slot bug disables the deferred-kerfur fresh-spawn fallback
The fresh-spawn fallback call in [kerfur_prop_adoption.cpp:150-151](../../src/votv-coop/src/coop/kerfur_prop_adoption.cpp#L150-L151) is:
```cpp
coop::remote_prop_spawn::OnSpawn(e.payload, /*senderSlot=*/0, localPlayer, /*deferKerfur=*/false);
```
But the OnSpawn signature ([include/coop/remote_prop_spawn.h:73-75](../../src/votv-coop/include/coop/remote_prop_spawn.h#L73-L75)) is:
```cpp
void OnSpawn(payload, senderSlot, void* localPlayer,
             bool fromConvert = false, bool deferKerfur = true,    // deferKerfur is param #5
             void** outSpawned = nullptr, bool skipBind = false);
```
The 4th positional `false` binds to **`fromConvert`** (param #4), NOT `deferKerfur` (param #5). The comment is a
LIE: `fromConvert` is set to false (already its default) and **`deferKerfur` stays at its DEFAULT `true`.**

**Consequence (the exact 13:15 log trace):** the "fresh-spawn" re-enters OnSpawn with `deferKerfur=true`. It
sails past the silent `existing`-miss (line ~384, a miss logs nothing) and the silent `fuzzy`-miss (line ~596,
a miss logs nothing), then reaches the K-6 defer at
[remote_prop_spawn.cpp:704](../../src/votv-coop/src/coop/remote_prop_spawn.cpp#L704)
(`if (deferKerfur && classW.find("prop_kerfurOmega"))` -> TRUE) -> calls `kerfur_prop_adoption::Arm(payload)`.
eid 3150 is **already in `g_pending`** (ResolvePending is mid-iteration, hasn't popped it yet) -> `Arm` matches
the existing entry, REFRESHES it, and **returns silently** ([kerfur_prop_adoption.cpp:178-184](../../src/votv-coop/src/coop/kerfur_prop_adoption.cpp#L178-L184)
-- no log; the "armed deferred adoption" log at line 188 only fires on a NEW push_back). OnSpawn spawns NOTHING
and returns. Back in `ResolvePending`, `resolved=true` (line 152) -> the entry is swap-popped (lines 155-156) ->
**eid 3150 is removed from `g_pending` forever.** The host broadcasts each prop once, so there is no retry. The
kerfur object is **permanently MISSING on the client.**

This is exhaustively consistent with the log: OnSpawn header at 22595, then NO `spawned`, NO `RegisterPropMirror`,
NO WARN -- because the only path taken was the silent `Arm`-refresh-existing return. (My earlier SEH-swallow
hypothesis was WRONG and is discarded: there is no crash; the arg-slot bug fully accounts for the silent return.)

### The two preconditions that put s1l33p uniquely on the broken path
1. **Index-seed race (sub-Q1 ANSWERED -- the twin DID load).** s1l33p's FIRST OnSpawn ran at **13:15:15
   (line 15946)**, ~4 log lines BEFORE the key-index re-seed (lines 15950-15951: "re-seed... added 2132 NEW...
   stale-index self-heal re-seeded 2132 new keyed prop(s)"). So `ResolveLiveActorByKey(s1l33p)` missed a STALE
   index -> fuzzy miss -> K-6 deferred. The OTHER 4 kerfurs' OnSpawn ran at **13:15:16 (lines 19542+), AFTER**
   the re-seed -> exact-key HIT -> bound immediately via the exact-key path (`resolves to live actor ... already
   aligned` + RegisterPropMirror eid 4346-4349), NEVER deferred. (Confirmed: `bound LOCAL save kerfur prop`
   [BindAsMirror] = ZERO occurrences this session -- nobody used the deferred adopt-bind; the 4 used exact-key.)
   So the symptom framing "the twin didn't load" is WRONG: the twin loaded; s1l33p just lost a ~1s race with
   index seeding and fell onto the deferred path.
2. **class+pose BindAsMirror miss at quiescence (sub-Q2a -- OPEN edge, see below).** At 13:15:19 the deferred
   poll's class+pose scan ALSO found no candidate (logged "no local twin") -> it took the fresh-spawn branch ->
   hit the arg bug. WHY the class+pose scan missed an existing twin is the one open edge (most-likely a
   skin-variant class mismatch: `BindAsMirror` requires `cands[c].cls == e.actorClass` where
   `e.actorClass = FindClass("prop_kerfurOmega_C")` [the base broadcast className], but the real local twin may
   be a skin leaf class e.g. `prop_kerfurOmega_col_gamer_C` -> exact-class compare fails. The exact-KEY path that
   bound the other 4 is class-agnostic, which is why it masked this for them).

### The fix-shape edge to settle BEFORE building (does s1l33p's twin exist at quiescence?)
The arg-slot bug is a REAL bug regardless and must be fixed (it silently disables the documented fresh-spawn
fallback for EVERY genuinely-absent deferred kerfur prop). BUT the correct overall fix depends on whether
s1l33p's twin is actually present at 13:15:19:
- **Twin ABSENT** (genuinely no local copy): the arg-bug fix alone is correct -> the fallback fresh-spawns the
  mirror.
- **Twin PRESENT** (loaded but class+pose-unmatchable): the arg-bug fix alone would FRESH-SPAWN A DUPLICATE
  beside the unmatched twin. The proper fix is then to ALSO make BindAsMirror match it (class-family match, not
  exact-class) so it BINDS the existing twin instead of spawning.
- **Settle it** with a tiny targeted probe at the quiescence branch: log the count of live `IsKerfurPropClass`
  actors not-yet-mirrored near the pending pose (and their leaf class) right before the fresh-spawn call. That
  tells us absent-vs-present-but-class-mismatched and picks the fix shape. (No build yet -- this probe rides the
  same build as the fix once greenlit.)

## The symptom (user hands-on, 13:15 session)
Host has **5 kerfur OBJECTs** (turned-off props) + 1 active. Client shows **4 OBJECTs** + 1 active. The active
matched (reverse fix held, no ghost). **One off-prop kerfur OBJECT did not materialize on the client.**

**NOT the reverse / not in-window-turn_off:** the host log has ZERO `executing turn_off` / `BindFormActor ->prop`
this session -- so the missing object is a SAVE object, not a kerfur the host turned off in the window. The
reverse fix is COMPLETE; this is a distinct off-prop/save-object bug.

## What the log establishes (FACTS)
The missing object = key `s1l33pEyzXNNzdcPk_HGag` (eid 3150), at loc (1728.1, -534.0, 6135.6).
- Client received ALL 5 kerfur-object broadcasts (5 `remote_prop::OnSpawn cls='prop_kerfurOmega_C'`).
- **4 of 5 bound a mirror via the EXACT-KEY path on the FIRST OnSpawn** (`resolves to live actor ... already
  aligned` -> `RegisterPropMirror` eid 4346-4349: keys gPXK / CZvy / p0KP / Nrby, all at 13:15:16). They were
  NEVER deferred. (`bound LOCAL save kerfur prop` [the deferred BindAsMirror log] = ZERO this session -- nobody
  used the deferred adopt-bind; the 4 resolved by key.)
- **s1l33p (eid 3150) was the only one DEFERRED.** First OnSpawn (13:15:15, line 15946) -> `kerfur-prop-adopt:
  armed deferred adoption eid=3150 (awaiting local twin)` -- because it ran ~4 lines BEFORE the key-index
  re-seed (see precondition 1 above). At quiescence (13:15:19) -> `kerfur-prop-adopt: eid=3150 -- no local
  twin (load tail quiesced); fresh-spawning a mirror` -> calls `remote_prop_spawn::OnSpawn(payload,
  senderSlot=0, localPlayer, /*deferKerfur=*/false)` ([kerfur_prop_adoption.cpp:150-151](../../src/votv-coop/src/coop/kerfur_prop_adoption.cpp#L150-L151)).
- **That fresh-spawn OnSpawn logged ONLY the OnSpawn header line -- NO `Gap-I-1` match, NO `spawned ... of`, NO
  `RegisterPropMirror`, NO `collision restored`, NO WARN.** Total kerfur prop mirrors bound on the client = **4,
  not 5**. Session ran to 13:16:47 (88s), so it is NOT a truncation -- the 5th genuinely never registered. The
  silent return is the `Arm`-refresh-existing path forced by the deferKerfur arg-slot bug (see ROOT above).

## Sub-questions -- BOTH ANSWERED (see ROOT above)
1. **Why was s1l33p deferred when the other 4 weren't?** -> ANSWERED: index-seed race. Its OnSpawn ran ~4 lines
   before the key-index re-seed; the other 4 ran after. The twin DID load. (Precondition 1.)
2. **Why did the "fresh-spawn" not register a mirror?** -> ANSWERED: the `OnSpawn(..., /*deferKerfur=*/false)`
   call sets `fromConvert=false` and leaves `deferKerfur` at its DEFAULT `true` (arg-slot bug). So OnSpawn
   re-enters the K-6 defer, `Arm`s the already-pending eid (silent refresh + return), spawns nothing, and
   ResolvePending then pops the entry -> dropped. (ROOT.)

One open EDGE (a fix-shape question, NOT a diagnosis gap): does s1l33p's twin actually EXIST at quiescence
(precondition 2 -- class+pose BindAsMirror miss)? That decides whether the arg-bug fix alone is correct (twin
absent) or whether BindAsMirror also needs a class-family match (twin present -> else fix would DUP). Settle
with the targeted probe described in the ROOT section, in the same build as the fix.

## Connection to symptom 2 (camera) -- CONFIRMED DIFFERENT (no shared root)
Symptom 2 (camera, doc 04 pending) is a fresh-spawn problem on the **NPC** path
(`npc_mirror::SpawnFreshNpcMirror`, the v74 floating-camera deferred spawn). OBS-2's root is a
**kerfur_prop_adoption-specific argument-slot bug** in the PROP-path fresh-spawn CALL -- it is not a shared
spawn/registration helper failure, so it CANNOT share a root with the NPC camera path. **CONFIRMED: OBS-2 and
symptom-2 are independent; OBS-2's fix (correct the OnSpawn call args + settle the BindAsMirror edge) does not
touch the NPC mirror path.**

## Relation to off-prop scope A (doc 03) -- DISTINCT
Scope A is a **dedup** fix (twin-destroy the join-window DUPLICATE by save-time key). OBS-2 is a **MISSING**
object (no twin loaded + fresh-spawn didn't register). A dedup does NOT close a missing object -> **scope A will
not fix OBS-2.** Order (user 2026-06-24): diagnose+fix OBS-2 (visible) FIRST, then off-prop scope A.

## NEXT (root pinned -> ready to build on greenlight)
The diagnosis is complete; the root is named and proven. The fix (on greenlight, NOT yet built):
1. **Correct the OnSpawn call args** at [kerfur_prop_adoption.cpp:150-151](../../src/votv-coop/src/coop/kerfur_prop_adoption.cpp#L150-L151):
   pass `deferKerfur=false` in the RIGHT slot, e.g. `OnSpawn(e.payload, /*senderSlot=*/0, localPlayer,
   /*fromConvert=*/false, /*deferKerfur=*/false)`. This is the load-bearing one-line root fix -- it re-enables
   the documented fresh-spawn fallback so a genuinely-absent deferred kerfur prop actually materializes instead
   of being silently dropped.
2. **Settle the BindAsMirror edge** (precondition 2) with a targeted probe in the same build: at the quiescence
   fresh-spawn branch, log the count + leaf class of live not-yet-mirrored `IsKerfurPropClass` actors near the
   pending pose. If a twin IS present (class-mismatched), widen `BindAsMirror`'s exact-class compare to a
   class-FAMILY match (kerfurOmega prop lineage) so it BINDS the twin rather than fresh-spawning a duplicate.
   If absent, the arg fix alone is the complete fix.
3. Re-verify hands-on: 5 host OBJECTs -> 5 client OBJECTs (the missing s1l33p materializes), no dup.

Do NOT build until the user greenlights (the user's OBS-2-first order is satisfied: the root is now clear).
