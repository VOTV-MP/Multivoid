# 12 — Join-window MASS-MOVE pile dup (2026-07-01)

**Status (updated 2026-07-03, RE-BIND THREAD as-built, deploy DLL `77BB5D58`):**

- **MASS-MOVE DUP class: VERIFIED FIXED stands** [V hands-on 19:06] — owner (take 3) + grabbed-clump gate
  (take 3.1), `d43956f6` + `0e7e5349`.
- **NEW AS-BUILT `c7a0f5de` — the save-time map LIFECYCLE fix (the 19:23 client-FPS storm root):** the
  per-slot blob maps (`g_blobPileXforms`/`g_blobKerfurXforms`) never emptied, so EVERY steady-state host
  grab kept stamping save-time keys and every kToPile LAND armed the client a HOPELESS pending twin →
  kMaxTwinPasses×250 ms full-array sweeps per drop, all session. Maps now retire at the b3 late-flush
  expiry (~25 s post world-ready — their last consumer). [V 20:2x log: steady-state LANDs no longer
  carry keys; the PILE-1C twin line is GONE from the steady drain.]
- **THE RE-BIND THREAD — AS-BUILT 2026-07-03 (the 20:24-27 unbound-native root, "клиент взаимодействует с
  дюп пайлом своим локальным"): ONE root, five seams, one commit.** The 20:24 log forensics REFUTED the
  "never bound" reading: eid=4435 DID bind at 20:24:36 (k=835 ordinal), then the 20:24:38 **mass-purge**
  freed every row; the game re-created the natives at their **SAVE positions** (loadObjects replays the
  save arrays — a re-create can never spawn at @new), but `UpdateChipSavePosAndGetOld` had RETRACKED the
  one identity key to @new at the 20:24:30 PropSnapPos → the position re-bind searched @new forever → E
  permanently unbindable → native@old = the eid-less client-local dup (native grab, no GrabIntent,
  host-invisible) + the armed pos-correction could never apply and had NO pass cap → HasPendingWork
  pinned → the 4 Hz drain (20:25-27). Two adjacent latent bugs surfaced by the same forensics: the
  retrack also broke the (b) DUP-RETIRE arm (bound-vs-key read 0 cm for exactly the moved piles), and
  raw `IsLive` on row-held FREED pointers misread liveness (the "N unbound kerfur [by key]" walked every
  pass; [DUP-PROBE]'s 17 "wrong-eid 4294967295" labels were the probe reading a locals-only getter).
  The fix (all AS-BUILT, protocol unchanged v94):
  1. **savePos IMMUTABLE + hostPos OVERLAY** (`save_identity_map.h` trailing runtime fields, never
     serialized; `UpdateChipHostPos` writes the overlay). Re-bind = two-phase: @host FIRST (churn
     survivor — the take-3 resurrect protection), excluding natives within 1cm of any FREE entry's @save
     (cross-entry steal guard); @save fallback (the purge re-create) + `CancelPendingSaveTimeTwin` +
     `EnsurePosCorrection(@host)` → the same drain pass snaps it to the host pos (step 5).
  2. **IsLiveByIndex everywhere an element-held actor pointer is tested** (eidFree, twin confirm,
     pos-correction resolve, kerfur retire, OnSpawn eid-dedup, caseII detect) — raw IsLive on freed
     memory can misread TRUE and permanently block a re-bind / phantom-detect a case(ii).
  3. **Twin invariants**: a twin retires ONLY when E is bound to a live actor (hostVacate = the host's
     word + bound; event = bound-FAR); the "unconfirmed retired (no cap-trip)" arm + the >50% cap are
     GONE (RULE 2 — they retired on zero evidence; post-purge with 870 unbound natives the cap never
     trips and that arm would have destroyed E's own re-create one step before the re-bind claimed it —
     the same orphan through the other door; the 20:24 run survived it by timing luck, natives==1).
     Event-arm can no longer overwrite a hostVacate twin (one-directional supersede); an event twin
     whose E is bound AT the key drops immediately (dead premise — was 40 passes of walk+log noise).
  4. **Terminalization**: pos-corrections + kerfur pending-retires now carry the twins' 40-pass bound
     with a LOUD drop log — no residual shape can pin the 4 Hz drain again (the FPS leg-2 kill).
  5. **Keyed churn re-bind** (the eid=2947 upstream + the eid=3129 receive half): (a) the divergence
     sweep pre-collects DEAD-actor keyed mirror rows and RE-BINDS an unclaimed keyed candidate whose key
     matches (claim transferred) instead of dooming the re-create; (b) `CreateOrAdoptPropMirror` HOST
     RE-ASSERT arm — senderSlot==0 + existing MIRROR row + different actor ⇒ SetActor rebind (the
     deny-heal `8c13858f` re-assert is no longer an Install-reject NO-OP), guarded 1:1 via
     Registry::EidForActor (never steal a second row's actor; displaced actors never destroyed);
     (c) kerfur WRONG-OCCUPANT heal — an entry whose eid is occupied by a live actor with a DIFFERENT
     key rebinds to the key-matched native (`consumeDisplaced=false` — the foreign actor lives on).
     KNOWN REMAINING VARIANT (audit F, by design): the HOST RE-ASSERT arm REFUSES when the incoming
     actor still holds a client-LOCAL element (a mid-session re-create seeded as a local — the 1:1
     guard reads its local eid as "another row") — that variant stays owned by the PropDestroy deny
     lane (`d4833b9b`). The healed variants: dead-row + fresh spawn (the observed 3129 shape) and
     join-window re-creates (the sweep lane unmarks the local first).
- Older thread statuses: EHH `2b2e0531` verdict pending; [DUP-PROBE] FIRED on the 20:24 run (680 lines —
  decoded the 17 "wrong-eid 4294967295" MISSes as probe mislabels: it read the LOCALS-ONLY
  GetPropElementIdForActor, fixed to Registry::EidForActor in `2ab718d5`; stays armed); FPS `70e0d899`
  superseded by the map-lifecycle fix above + the re-bind thread's caps/dead-premise drops (the pin is
  now bounded by construction; the walk-merge N-scans->1 remains a pure-optimization backlog item).

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

## Verify (hands-on) — DUP class DONE; ONE residual + the ARMED probe (2026-07-02)
Runbook `research/handson_runbook_2026-07-01_massmove_dup.md`. **19:06 hands-on: "всё на своих местах" = the
corner clears, no dup, no mid-air clumps [V].** The 19:41 run then showed **ONE residual local dup** (user
report). Log forensics of 19:41: the owner FIRED correctly (8 `identity key UPDATED`, 11 HOST-VACATE arms, 1
DUP-RETIRE arm) but the sweep retired **0 of 21** pending twins — every `FindExactMatch` MISSed, then all
dropped at `kMaxTwinPasses`. A MISS is ambiguous between (0) clean / (>1) co-located-ambiguous /
BOUND-to-the-WRONG-eid (excluded from the unbound-only candidate set = invisible to every retire path — the
predicted GC-pointer-reuse tail). **[DUP-PROBE] (`26dea6e4`) instruments exactly that decision** — per-twin
census (unbound@old at 1cm/30cm + bound-natives-near-@old WITH their bound eid + E's distance) + a decoded
verdict line. It did NOT fire on the 20:17 run because `IsIniKeyTrue`'s exact-equality match silently read
every inline-`;`-commented ini flag as absent — root-fixed `f81256e4` (strip comments before matching).
**The probe is ARMED now** (flag set in all client inis): the next mass-move run yields `[DUP-PROBE]` verdicts.
(20:17 sweep data point: 10 twins DID retire that run — 8+1+1 confirmed — so twin-retire works when
FindExactMatch hits; the 21-pending cluster is the anomaly the probe will decode.)

## EHH — deny on EVERY client E-press (`d7620ed5` was INCOMPLETE — the RELEASE seam leaked on E-drop;
## re-root-fixed `2b2e0531`+`9eda3faf` 2026-07-02, gesture-pairing latch; audit all-PASS; verdict pending)

**2026-07-02 hands-on DISPROVED completeness:** "EHHH всё еще есть но только для клиента и когда он
E-RELEASE/DROP делает". The `d7620ed5` `_42` suppressor RE-DERIVED the press conditions at release time —
but `SendThrowIntent` optimistically clears the carry at the PRESS edge (`trash_channel.cpp:283`), so
"carrying?" read false at the release and the post-drop aim missed the cone → the native release ran →
deny. The same re-derivation could also EAT a legit native release (dropping a natively-held prop while
aiming at a pile). Root shape: release-time condition re-derivation is wrong in BOTH directions. Fix =
GESTURE PAIRING: every cancelled client use-PRESS (3 `_41` cancel sites + the `_38` suppressor) arms
`g_cancelPairedUseRelease`; the pairing-only `_42` callback (`OnPileUseReleaseSuppress`) consumes it —
no conditions at release. UE FlushPressedKeys guarantees release delivery → no stale latch (+ reset on
disconnect, `9eda3faf`). Log marker per drop: `[USE-RELEASE] paired E-release CANCELLED`. The original
3-binding RE below stands [V].
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

## GHOST-WEDGE — a pile permanently ungrabbable for the client (root pinned from logs 2026-07-02;
## delivery half `d4833b9b` + UPSTREAM root SHIPPED `2ab718d5` 2026-07-03; hands-on verdict pending)

Hands-on: client grab/dropped piles, then ONE pile refused every grab until the HOST hand-cycled it. Host
log: 12x `[GRAB-INTENT] DENIED eid=2947 -- pile actor not live / not a chipPile` (13:37:44-59) — a SILENT
deny, so the client re-sent the same doomed grab forever. Evidence chain (both logs): eid 2947 = a
save-loaded keyed trashBitsPile (`AtrashBitsPile_C : Aactor_save_C`, NOT chipPile lineage — bytecode-
grounded 2026-06-10 finding), adopted+claimed at join 13:33:32. GC-churn re-created its actor; **keyed
props have NO churn re-bind** (chipPiles re-bind BY POSITION: `save_identity_bind: RE-BIND chipPile`), so
the mirror row kept the freed pointer AND the pointer-keyed sweep claim orphaned → the join sweep doomed
the re-created twin as unclaimed (13:33:43 `trash_pile: wire destroy ... unwatched`); a chipPile later
RECYCLED the freed address → the stale reverse entry mis-resolved the client's aim to the dead eid 2947.
The host-side row was equally unresolvable. Host hand-cycling the real pile self-seeded a fresh eid = the
unwedge.

**Fix `d4833b9b` (the delivery half):** the not-live deny arm now broadcasts `PropDestroy(eid)` — positive
per-eid host-authoritative evidence ([[feedback-join-reconcile-sweep-safety]] shape); every peer DRAINS its
ghost row (`UnregisterPropMirror`; a stale row resolves no live actor → nothing destroyed in-world; absent
rows no-op) and the requester's next aim re-resolves to the real entity. The wrong-class arm is split out:
deny-only + class name logged (never destroy a live entity on mismatch). Log marker:
`broadcasting PropDestroy(eid) so every peer drains its stale ghost row`.

**UPSTREAM ROOT — AS-BUILT `2ab718d5` (2026-07-03, the re-bind thread seam 5):** keyed-prop GC-churn
RE-BIND by KEY shipped in TWO halves: (a) `join_membership_sweep::RunDivergenceSweep_` pre-collects
DEAD-actor keyed mirror rows (key->eid) and RE-BINDS an unclaimed same-key doom candidate onto its
orphaned row (claim transferred, counted claimed) instead of dooming the re-create — the exact 2947
formation point; (b) `CreateOrAdoptPropMirror` HOST RE-ASSERT arm — a senderSlot==0 PropSpawn naming an
existing MIRROR row bound to a DIFFERENT actor re-points the row (SetActor; 1:1-guarded via
Registry::EidForActor — never steals a second row's actor; displaced actors never destroyed), so the
`8c13858f` wrong-class deny heal is no longer an Install-reject NO-OP on the client. KNOWN REMAINING
VARIANT (by design): the re-assert REFUSES when the incoming actor still holds a client-LOCAL element —
that variant stays owned by the `d4833b9b` PropDestroy deny lane. Smoke PASS; hands-on = the runbook
2026-07-03 wedged-pile test (E, wait 2 s, E -> client log `HOST RE-ASSERT rebound row`).

## FPS — client hitch during the mass-move (`70e0d899` = REFUTED-INSUFFICIENT by the 19:41 log) [log-V]

**2026-07-02 verdict:** the NameOf reorder did NOT land as a user-felt fix. The 19:41 client log (the run WITH
`70e0d899` deployed) still shows `sync:npc_client` at 20-51ms (mean 23.5ms, 60/69 samples >20ms) across the whole
17s drain window — even a pass re-binding only 1 chip candidate took 20ms. So the alloc storm was real but NOT
dominant: the raw ~330k-object iteration × MULTIPLE walks per drain pass (BindUnbound + twin sweep + join
reconcile) × 4Hz is the cost, kept hot the whole window because the 21 never-retiring twins pinned
`HasPendingWork`. **The dup residual and the FPS hitch share a root**: twins that never resolve pin the
reconcile hot. Fixing twin resolution (the [DUP-PROBE] target) shortens the pin; the deeper FPS fix is merging
the per-pass walks into ONE shared scan — queued AFTER the dup probe pins the residual. Original (superseded)
analysis below kept for the alloc-storm mechanism, which is still true, just not sufficient:
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

## 2026-07-03 SEAM: pre-sweep TWIN GRAB -> client-local clump chain [log-V; fixes AS-BUILT `45bdb7ac`]

**User repro 11:49 ("the last pile which client grabbed is his local only dupe"), forensics from both logs:**
the client joined 11:48:20; HOST-VACATE twins armed 11:48:23 (eids 5222/5195/5205 — piles the host had moved
pre-join; the save-loaded natives at @old await the sweep). The [PILE-1C] sweep pass ran 11:48:58 — but the
user E-pressed a twin@old at **11:48:51, 7 s BEFORE the sweep**. An unbound pile takes the NATIVE use path
(the GrabIntent intercept only fired for BOUND piles) -> native pile->clump conversion CLIENT-LOCALLY ->
`net: NEW held actor prop_garbageClump_C key='None' eid=0` -> native land spawns ANOTHER unbound pile = a
self-perpetuating client-only chain (native clump holds at 11:48:51, 11:48:55, 11:49:19 — the user saw the
last one). A grabbed twin becomes a CLUMP, which the pile sweep can no longer retire — the chain escapes
every identity reconcile by construction.

**Fix 1 (the class-killer, interaction side): UNBOUND-pile grab gate** — `OnPileUseIntercept`
(trash_collect_sync.cpp) now CANCELS the native use for ANY aimed `actorChipPile_C` that NO eid owns
(twin-pending-sweep / mid-bind-window / chain descendant). On a connected client every pile interaction is
host-authoritative; the pile binds or retires within seconds and a re-press routes through GrabIntent.
Log marker: `[GRAB-INTENT] CLIENT E-PRESS on UNBOUND native pile ... CANCELLED`. This kills the WHOLE
dupe-interaction class regardless of which seam produced the unbound pile.

**Fix 2 (wire hygiene, host side): re-seed CHURN GUARD** — the host log 11:48:59 showed
`prop_element_tracker: re-seed ... added 4 NEW` + 4 `incremental PropSpawn (eid=..., key='')`: the host's
OWN re-pile land actors, re-adopted because the tracker's `g_knownKeyedProps` newness signal is a raw
pointer set, blind to actor churn (the re-bind thread's exact lesson — a 3rd identity map smearing). Every
20 s re-seed re-broadcast every land result as a keyless PropSpawn. `SeedWalk_` newness now consults the
element binding (`GetPropElementIdForActor != invalid` => churned actor of a tracked element, NOT new; set
membership still refreshed for the O(1) snapshot de-dupe). Delivery-axis clean: an element-owned actor was
already delivered by its own channel (Init-POST stamp / trash LAND / connect snapshot).

**Status: AS-BUILT (`45bdb7ac`, DLL `3FDEF8097B7D937E`), NOT hands-on-verified.** Test: client joins while
host moves piles, client immediately spams E on the trash cluster during the first ~10 s — every press on a
not-yet-bound pile must log the CANCELLED marker and produce NO `prop_garbageClump_C key='None'` hold; the
host log must show NO `re-seed ... added N NEW` for its own land actors while piles are being thrown.
