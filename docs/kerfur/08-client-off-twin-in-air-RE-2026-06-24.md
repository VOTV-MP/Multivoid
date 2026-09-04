# 08 -- client-side turn-OFF spawns a twin-in-air (18:24/18:30) -- ROOT RE

[withdrawn at the author's request]


**Status: ROOT RE'd (log+code proven). PRE-EXISTING (NOT the mirror-identity extract). Cosmetic flicker
(end-state self-resolves). Fix = the two-layer instant-world deferred-spawn upper layer (hides it), see
`docs/COOP_INSTANT_WORLD_TWO_LAYER.md`; the underlying convert-ghost<->adoption handoff is a separate,
lower-priority correctness churn the backup already absorbs.** Diagnosed 2026-06-24 from the real 18:30
client log (`Game_0.9.0n_copy/.../votv-coop.log`).

## Symptom (user, 18:24 AND 18:30, deterministic)
CLIENT turns a kerfur OFF -> a normal kerfur off-object is born AND a SECOND off-object hangs IN THE AIR ->
~4s later the second is deleted. CLIENT-ONLY (host clean), stable, not native.

## Verdict: NOT an extract regress (3 independent proofs)
1. **Code scope:** `git diff 24ee5220 HEAD` -- the extract touched ONLY `kerfur_reconcile.cpp`,
   `pile_reconcile.cpp`, `save_time_retire_util.h`. The whole client-off path (`kerfur_convert`,
   `kerfur_prop_adoption`, `remote_prop`, `npc_*`) is UNTOUCHED.
2. **The log names the culprit:** the twin at 18:30:44 is `kerfur-prop-adopt: ... fresh-spawning a mirror`
   -> `remote_prop::OnSpawn: spawned ... at (1732.1,-745.5,6206.7)`. The ONLY `kerfur_reconcile` lines in
   the run are the scope-A FORWARD retire of a different event (eid 3149, host turn-ON: ARMED 18:30:19 ->
   bound mirror 18:30:22 -> sweep-retire 18:30:34 -- all correct).
3. **Byte-equivalence:** the audit proved `SweepReconcileSaveTimeKerfurs` byte-identical; it is gated on
   `g_pendingRetire`, armed ONLY by host forward off->active (npc EntitySpawn hasMatchPos). A client
   turn-OFF never arms it -> the extract's sweep cannot fire on this path; even timing is unchanged.

**Why it looked new:** client-side turn-OFF was simply never tested before (scope A = host turn-ON; the 3
extract tests = host-driven/save-load). 18:30 is the first client-toggle test.

## Mechanism (pre-existing kerfur_convert <-> kerfur_prop_adoption seam)
1. `[18:30:44] kerfur_convert: POLL turn_off (kerfur NPC eid=3149 died invisibly) -> client requests host`
   -- the client's death-watch poll caught the local turn-off.
2. `kerfur_convert[client]: claimed 1 local conversion ghost (prop turn-off) near (1732,-745,6207) --
   parked for host adoption` -- the client's OWN local off-prop is parked, awaiting host adoption.
3. Host confirms `KerfurConvert K=3150 oldEid=3149 -> newEid=5276 (turn_off)`.
4. `kerfur-prop-adopt eid=5276`: the adoption fuzzy-matches wire key `coopkerfur#5276`, finds a
   `neighbor with a DIFFERENT key '2q0bEaT5...' -- anti-collision: not stealing it, fresh-spawning instead`
   -> `remote_prop::OnSpawn: spawned ... at (1732.1,-745.5,6206.7)`. **A SECOND off-prop, at the active
   NPC's elevated Z=6206.7 (the props belong on the ground ~6135) = IN AIR.**
5. `[18:30:48] kerfur_convert: orphan conversion ghost ... un-adopted after 4022ms -- destroying` -- the
   PARKED local ghost is destroyed (the adoption fresh-spawned instead of adopting it) = "second deleted."

**The seam:** the conversion-ghost-claim (`kerfur_convert` parks the local off-prop) and the prop-adoption
(`kerfur_prop_adoption` fresh-spawns the host mirror) DON'T hand off -- the adoption never sees the parked
ghost as its local twin, so it fresh-spawns a duplicate at the broadcast (NPC, elevated) position instead
of adopting the parked one in place. The fuzzy-gate (fix#1) correctly refuses to STEAL a different-key
neighbor, but there is no fallback to ADOPT THE PARKED GHOST -> fresh-spawn dup. **"In air"** = the
fresh-spawn sits at the NPC's standing height; the mirror is kinematic (no physics) so it stays there;
the real local off-prop has physics and settles to the ground. This is the convert-ghost/adoption handoff
family (kin to OBS-2 / doc 04 camera), NOT mirror-identity, NOT the extract.

## Autonomous re-confirmation (2026-06-24, kerfurtoggle smoke on the instant-world build d7b535d7)
A `mp.py kerfurtoggle` run reproduced it deterministically: client turn-off at 21:02:00 ->
`remote_prop::OnSpawn: kerfur fuzzy match ... resolves to a neighbor with a DIFFERENT key ... anti-collision:
not stealing it, fresh-spawning instead` -> `kerfur-prop-adopt eid=3218 ... no local twin ... fresh-spawning a
mirror` -> harness verdict `PROP ADOPT FAILED ... + 1 orphan-destroy: an adopt MISSED and the ghost timed out
(a transient dupe before cleanup)`. This IS the twin (fresh-spawn instead of adopt-the-parked-ghost, then the
orphan-destroy cleanup). Two things it PROVED: (1) NOT an instant-world regress -- `mirror_defer` was DISARMED
(quiescence-reveal 21:01:34) before the toggle (21:02:00), so OnMirrorSpawned no-op'd (the IsArmed gate); (2)
NOT an extract regress -- the Gap-I-1 fuzzy machinery is intact (it de-duped `prop_camera_good_C` within 30cm
the same run); only the kerfur convert-ghost<->adoption handoff fresh-spawns. The harness "FAIL" is it expecting
an adopt the (correct) anti-collision gate refuses -- the documented pre-existing twin, cosmetic.

## End-state is correct (cosmetic)
The dup self-resolves (the orphan/backup cleanup deletes one) -> a VISIBLE flicker, not a persistent dup.
Matches the user's "world mirrors well, but an annoying artifact." So this is a TIMING/visual problem ->
the right fix is the deferred-spawn upper layer (it never shows the air-born mirror); see the two-layer
design. A deeper correctness fix (make the adoption adopt the parked ghost instead of fresh-spawning) is a
separate backlog item the quiescence backup currently absorbs.
