# 12 — Join-window MASS-MOVE pile dup (2026-07-01)

**Status: the MASS-MOVE DUP is VERIFIED FIXED [V hands-on 19:06 "всё на своих местах"]** — owner (take 3) +
grabbed-clump gate (take 3.1). Two follow-on issues that surfaced during the same test are AS-BUILT + deployed
but NOT yet hands-on: the EHH-on-every-E deny (multi-seam fix) and client FPS (walk alloc-storm). Latest deploy
`3A8BB6AD6D2FC9A3` (4/4 hash-verified; carries the dup fix + EHH + FPS). Owner commits: `d43956f6` (owner) +
`0e7e5349` (grabbed-clump gate). See the "EHH" and "FPS" sections at the bottom for those two threads.

## 18:52 — the owner over-reached onto actively-grabbed piles (fixed 0e7e5349, then VERIFIED 19:06) [V]
Take-3 owner FIRED and worked for the @old-resurrect class: the HOST late-arm delivered late moves (eid 4930 at
drift 2134cm — the 5271-class gap closed), 22 identity-updates, 22 HOST-VACATE, 169 retires. But a new dup
signature appeared: grabbing a pile pulled TWO clumps (a native + a co-located extra). NOT materialize-fail
(`materialize FAILED`=0) and NOT a proxy leak (876 proxy spawn / 875 retire, balanced — mostly join-time
nativization). Root: **the b3 flush was sending PropSnapPos for a pile the host was actively GRABBING/THROWING** —
its live actor is an airborne CLUMP on the throw arc (eid 4965 flush `current=(1637,-269,6375)`, Z=6375 vs save
Z=6098). The client chased those mid-air waypoints — `identity key UPDATED -> (…,6375)` and armed HOST-VACATE
twins at AIRBORNE spots — while the trash-channel convert stream (the real authority) landed the native correctly
(LAND drift 0). Two authorities fighting over the same live pile => the paired dup + bogus mid-air retires. The
flush's own comment said `// mid-carry clump -> skip` but the condition (`!actor || !IsLive`) never skipped a
clump (a clump is a live actor). Fix: `if (!ue_wrap::prop::IsChipPile(actor)) continue;` — only a RESTING
chipPile native gets a correction; a grabbed clump is owned by the convert stream. This is NOT a separate track —
it was the owner over-reaching; the gate confines it to moved-and-settled piles (the case it was built for).

## Take 3 — the OWNER (host-authoritative pile identity through the join tail) [VERIFIED 19:06]
Host + client. NO protocol bump (reuses PropSnapPos). Full RE + timeline:
`research/findings/votv-joinwindow-massmove-dup-RE-2026-07-01.md`. SEPARATE class from `docs/piles/09`
(single held-clump-at-join) and `docs/piles/11` (nativization) — the MASS version. [[project-pile-nativization-2026-06-30]]

Takes 1-4 (`fa8bc344` create-edge, `76257bb0` CONVERT-WINS, `46e35edd` twin-sweep, `110b1bde` DUP-RETIRE) each
removed a slice and left a new-named tail (FLOOR-kept → RE-BIND-resurrect → air) — the classic
[[feedback-recurring-bug-is-architectural]] signal that the PATCH LEVEL was wrong. A 2nd forensic trace (18:17)
nailed the disease: **a host in-window move keeps the SAME eid (same-eid morph), but the client identifies a
keyless pile by its FROZEN save-pos — which lies after a move.** When E's @new mirror pointer GC-churns,
`eidFree(E)` goes true and `RE-BIND-by-position` uses E's stale save-pos to "re-create" E — grabbing the leftover
@old copy and binding it to E (a CORRECT bind of the WRONG actor). Once bound, every unbound-only retire is blind
to it. Four client heuristics (ordinal bind, RE-BIND-by-position, twin sweep, DUP-RETIRE) all inferred identity
from position with NO authority and RACED — RE-BIND resurrected what the retires wanted to kill. And the host's
position flush was a ONE-SHOT, so a pile moved late in the join tail (eid 5271) got NO signal at all.

The owner: `PropSnapPos(E,@new)` IS the host's authoritative "E is now at @new" — the client now USES it for
IDENTITY (not just to nudge the actor), and the host DELIVERS it for every move:
- **Host** (`save_transfer.cpp`): `FlushDivergedPilePositionsForSlot` is LATE-ARMED — re-runs at 2 Hz for 25 s
  past each joiner's world-ready (`TickPileFlushLateArm`, from host `TickHost`), deduped per-(slot,eid) to actual
  position CHANGES. Every in-window move is delivered, whenever it happens.
- **Client**: on `PropSnapPos(E,@new)` — (1) `save_identity_bind::UpdateChipSavePosAndGetOld` retracks our
  save-time identity key to @new, so RE-BIND-by-position searches @new and NEVER resurrects the @old copy (root
  fix for the resurrect); (2) if E moved >50cm, `quiescence_drain::ArmHostVacateTwin` arms a HOST-VACATE twin —
  the sweep retires whatever save-loaded native@old lingers, ON THE HOST'S WORD (force-confirmed, wildcard
  chipType, NO >50% cap — the position-confirm guess is exactly what GC pointer-reuse corrupted).

This makes PropSnapPos the single authoritative identity-sync channel. Verify-before-retire is preserved but the
"verify" is now the HOST'S authority, not a client position guess ([[feedback-deliver-missing-owner-delivery-axis]],
[[feedback-recurring-bug-is-architectural]]). RULE-2 follow-up once proven: collapse the now-redundant position
heuristics (DUP-RETIRE / the event-armed twin confirm) into "retire iff host-vacated". Runbook:
`research/handson_runbook_2026-07-01_massmove_dup.md` (take 3). Air-hang-on-pickup = separate carry track.
`save_transfer.cpp` at 808 LOC → extract the b3 flush cluster to its own file (follow-up).

## 17:50 hands-on re-derivation (2 forensic agents on the full host+client logs) [log-V]
The 46e35edd twin-sweep DID run (hash-verified) and behaved correctly (`27 pending -> 5 confirmed-moved retired
-> settles to 2 stuck`, both stuck = already-converged eids mid-countdown at shutdown, NOT dups). But the corner
was a DIFFERENT survivor set: `join_membership_sweep: completeness FLOOR kept 6 unclaimed 'actorChipPile_C'
(claimed 0 this bracket, INCOMPLETE snapshot)`. Those 6 are BOUND-less save-loaded natives at @old that the
FLOOR (docs/piles/10) KEEPS to avoid a false >50% world-wipe — but `join_membership_sweep.cpp:276` does
`++floorKeptByClass; continue;` and registers them NOWHERE. `ArmPendingSaveTimeTwin` has two callers
(`pile_spawn_bind.cpp`, `remote_prop.cpp` convert LAND) — neither is the FLOOR. So a FLOOR-kept orphan is
**terminal**: it only dies if an independent event happens to key onto its exact position (luck of overlap). The
three prior fixes all patched the event-keyed convert lane, which the joiner's pre-world gate can DROP. Root
(patch level): **the FLOOR holds the orphan but never registers it for deferred retire** — the symmetric hole to
"a convert arrives but nobody applies it". The other two 17:50 symptoms are SEPARATE and NOT this dup: B3
misplaced-snap (applied clean, drift 0 this run) and the air-pickable no-GUI pile (materialize-FAILED ->
NoCollision proxy fallback; own track, below).

## Fix take 2 — CLIENT-ONLY DUP-RETIRE from the save-identity map (`save_identity_bind.cpp`) [AS-BUILT]
The FLOOR orphan is UNBOUND, so it has no eid to arm a confirmable twin with — BUT the client holds its OWN
save-time identity map (`g_chipEntries`, eid<->savePos, sidecar v2; both peers loaded the identical save). The
re-bind loop at `BindUnboundReCreates` already recovers a FLOOR orphan's eid BY POSITION from that map, but only
when the eid is FREE. Added the SYMMETRIC branch: for each map entry `{eid=E, savePos=@old}` whose E is BOUND to
a live native `>50cm` from @old (positive per-eid evidence E moved @new), if an UNBOUND native still sits at @old
-> that native is E's stale @old twin -> `ArmPendingSaveTimeTwin(E, @old, chipType)`. The existing sweep then
re-confirms E moved and retires the orphan PER-EID (confirmed -> NO cap). Runs in the post-purge drain window (a
mass-move IS a purge episode -> `InPurgeEpisode` -> 6s window -> the drain runs). No host change, no protocol
change, no FLOOR change (the FLOOR still guards the wipe; we resolve what it kept). Verify-before-retire preserved
([[feedback-join-reconcile-sweep-safety]]): retire only on positive per-eid move evidence.

## Symptom (hands-on 16:42 + 17:10)
The host, DURING the client's join window, mass-grabs+throws a CLUSTER of chipPiles (clears a building
corner). The client finishes joining and sees the **corner still FILLED** = stale piles `@old`. Census:
CLIENT 874 vs HOST 869 keyless chipPiles = **~5 client-only dups**. (A secondary EHHH `use_deny` on those
piles is a claim-state symptom — unrecognized/unbound → the grab interceptor returns false → native denies —
NOT a sound bug; it clears when identity is fixed.)

## Root (2 read-only forensic agents + census converged) [RD + log-V]
At 17:09:53 THREE guards each named "5" and KEPT the stale `native@old`:
```
join_membership_sweep: completeness FLOOR kept 5 unclaimed 'actorChipPile_C'   (docs/piles/10 guard)
[PILE-1C] sweep-reconcile ABORTED -- 4 twin removals of 5 live native(s) (>50%) -- keeping all natives
save_identity_bind: OVERFLOW -- 5 chipPile spawn(s) exceeded the mapped count
```
**Primary = the aggregate `>50%` cap on `SweepReconcileSaveTimeTwins`.** A legit cluster-clear makes the moved
piles >50% of the region's live natives → the cap reads it as a racing/incomplete-bracket world-wipe → aborts
→ the stale `@old` twins survive. **Worse: the sweep then `g_pendingSaveTimeTwin.clear()`'d the twin map, and
it ran ~4 s BEFORE the moved piles' `@new` PropSpawn+materialize arrived (sweep 17:09:53 vs `@new` 17:09:57)** —
so at sweep time none were confirmable, and clearing them meant no later pass could retire them. Compounding:
the sweep only retires UNBOUND natives, but a GC pointer-reuse `RE-BIND-by-position` re-bound some `@old`
natives to WRONG eids (e.g. `4965↔4967`), so they were never in the doom set.

Recurred after 2 targeted fixes (fa8bc344 create-edge claim, 76257bb0 CONVERT-WINS) → the patch LEVEL was
wrong; re-derived from logs per [[feedback-recurring-bug-is-architectural]].

## Fix — per-eid CONFIRMED-move twin retire (`46e35edd`) [AS-BUILT]
Rewrote `SweepReconcileSaveTimeTwins` (`quiescence_drain.cpp`):
- Split each pending twin into **CONFIRMED-moved** — E's currently-bound native lives `>50cm` from the twin's
  save-pos = the host moved E `@new`, positive per-eid evidence → retire the stale `@old` with **NO aggregate
  cap** — vs **UNCONFIRMED** (E not yet bound `@new`).
- The `>50%` cap now applies **ONLY to the unconfirmed remainder** (the racing-bracket case it was born for).
- **Unconfirmed/unmatched twins are KEPT pending** (bounded, `kMaxTwinPasses=40 ≈ 10 s`) instead of cleared —
  so the NEXT drain pass, once `@new` binds E, confirms + retires `@old` per-eid.

"Per-eid convert IS the proof; the `>50%` cap becomes the fallback" — the design the user proposed. Preserves
the world-wipe protection (verify-before-retire, [[feedback-join-reconcile-sweep-safety]]).

## RESIDUAL / open
- **GC pointer-reuse class (1–2 of 5):** an `@old` native re-bound to a WRONG eid is excluded by the
  UNBOUND-only walk → may persist. Separate follow-up if hands-on still shows 1–2 after the primary fix.
- **Air-clumps (≤2, mid-air):** a SEPARATE track (mid-air Z, not the ground `@old` dup). NOT a leaked proxy
  (the traced clump proxies were all `RETIRE-ACTOR-ONLY`'d). Leading candidate: the `known=0 carry pose holds
  forever` pattern (a clump proxy driven by a stale/ahead carry-pose stream whose convert-gen was never
  adopted). Diagnose separately.
- **Superseded here:** the CONVERT-WINS extension (`76257bb0`, `save_identity_bind` `PROXY-WINS`→`CONVERT-WINS`
  + `ConsumeLocalActor` un-root) is CORRECT but was insufficient alone (fired only on the proxy sub-case). It
  stays (defense-in-depth); the twin-sweep rewrite is the actual mass-move fix.

## Verify (hands-on) — DUP DONE
Runbook `research/handson_runbook_2026-07-01_massmove_dup.md`. **19:06 hands-on: "всё на своих местах" = the
corner clears, no dup, no mid-air clumps [V].** The GC-pointer-reuse residual + air-clumps notes above were the
take-2 concern; the owner + grabbed-clump gate resolved them (the 19:06 run was clean).

## EHH — deny on EVERY client E-press (AS-BUILT `d7620ed5`, deployed `3A8BB6AD6D2FC9A3`, NOT hands-on) [RD]
Surfaced 19:06: `use_deny` "EHHH" plays on every client E-grab/release even for a recognized pile whose grab
succeeds — and the log shows the `_41` interceptor DID cancel (`native use CANCELLED, no use_deny`). So the deny
is on a SEPARATE seam than `_41`. RE (`mainPlayer.json` Export 483 `InputActionDelegateBinding_0`): the "use"
action has **THREE** delegate bindings — `_41` (IE_Pressed, the grab press we hook), `_38` (a SECOND IE_Pressed),
`_42` (IE_Released) — all three reach `useAction`'s `use_deny`. Hooking only `_41` left `_38` firing on every
E-press and playing the deny in parallel with our cancelled grab. The `dc8bd6af` comment's claim that cancelling
the `_41` dispatch "subsumes" the deny was WRONG (covers 1 of 3 seams). Fix: a side-effect-free deny-suppressor
(`OnPileUseDenySuppress`, `trash_collect_sync.cpp`) on `_38` + `_42` — client-only; on a pile interaction
(carrying, or aimed at a bound-native/proxy pile) it ONLY cancels the dispatch (no grab/throw intent, no cue;
`_42`-on-release must never throw). `_41` stays the sole intent sender. RE finding:
`research/findings/votv-use-action-three-bindings-RE-2026-07-01.md`. Lesson: [[lesson-input-action-multiple-delegate-bindings]].

## FPS — client hitch during the mass-move (AS-BUILT `70e0d899`, deployed `3A8BB6AD6D2FC9A3`, NOT hands-on) [RD]
Surfaced 19:06: `net_pump::Tick` hitching 48-57ms. `[HITCH-SRC]` attributed it to OUR code (not GC); `[WALK-TIME]`
named `sync:npc_client` = `npc_mirror::TickClientNpcs` → `join_membership_sweep::TickClientReconcile` →
`quiescence_drain` reconcile. Root: the reconcile's re-bind walk (`save_identity_bind::BindUnboundReCreates`) ran
`NameOf` (a wstring ALLOCATION) + `NameStartsWith` on ALL ~330k GUObjectArray objects EVERY pass, 4 Hz while
HOST-VACATE/twin pending work kept the reconcile hot — BEFORE the cheap class filter. 330k allocs/pass = the
spike. Fix: reorder so the alloc-free class filter (`IsChipPile`/`IsKerfurPropClass`) runs FIRST; `NameOf` now
runs only for the ~870 real piles/kerfurs. Behavior-preserving. The twin sweep already filtered cheap-first —
only this walk was mis-ordered. Lesson: [[lesson-full-array-walk-cheap-filter-before-nameof]]. Follow-up if still
hitchy: merge sweep+re-bind into one shared walk (2 scans→1); `sync:interactable` 33ms was likely a one-time
re-index (chase only if it persists).
