# R-4a end-condition: the reconcile window — DESIGN (2026-08-23, 9-round /qf "that holds")

> ## AS-BUILT (2026-08-23 late eve; BUILT + drilled the same evening, NOT hands-on)
>
> Everything in §1 shipped as specified. One drill-cut correction the RED run caught: the
> drill's first target selection used the element Registry, which on a v122 client holds only
> host-bound MIRRORS (five pile mirrors died and healed without touching the seam's broadcast
> path) — retargeted to keyed LOCALS (keyed interactable + NO element bound), a one-shot dev
> walk.
>
> **Acceptance evidence (autonomous smokes):**
> - **RED (VOTVCOOP_EPISODE_DRILL=1 + VOTVCOOP_RECON_DISABLE=1 = the old close edge):** the 5
>   drilled keyed locals BROADCAST — `CLIENT broadcasting DESTROY ... key='...' eid=0` ×5, the
>   exact field class-B signature. The instrument sees the phenomenon.
> - **GREEN (drill, window active):** the SAME five keys suppressed (`suppressed KEYED DESTROY
>   #1..#5 ... inside the RECONCILE window`), 0 drill broadcasts. Lifecycle proven in-log:
>   `RAISED (kind=load by=Arm)` → `reconcile Begin (window up, kind=load kept)` → `LOWERED
>   (SnapshotComplete)`. The [PROP-DROP] latch fires (#1..#3) on the join bracket's spawn churn
>   (the field's ×242 class, now quieted).
> - **joinchurn PASS** (936 incremental expresses, divergence sweep 1× no re-arm, kerfur no
>   flip-flop) and **steady smoke PASS** with ZERO reconcile-window suppressions in steady
>   state (no WARN spam).
> - Residual observed both runs: ~1 organic keyed eid=0 broadcast ~15-20 s AFTER Complete
>   (post-window stragglers; field had 14 — the deliberate Complete-anchored end accepts them,
>   design §2).
> - Wire format unchanged (proto 134).
> - **Post-ship audit folded (PASS, 0 CRITICAL, 2 IMPORTANT, 5 MINOR):** IMPORTANT-1 — the
>   flake-backstop LOWER was in the spec but not wired; fixed via `NoteBracketFlake()` (lowers
>   WITHOUT flipping the classifier — a late real bracket still classifies load). IMPORTANT-2 —
>   this arc pushed net_pump.cpp 799 → 804 (> 800 soft cap): extraction proposed
>   (`MaybeRequestReAnnounce`'s world-changed branch → `coop/session/world_reannounce.cpp`).
>   MINOR-1 (consume re-asserts up) + MINOR-5 (single InEpisode read) fixed; MINOR-2/3 recorded
>   in §3.
>
> The original STATUS line follows; the /qf thread (9 rounds) is at the session scratchpad
> `qf_r4a_end_brief.md`.

## §0 The ask and the measured root

Triage §5-CORR left the R-4a end-condition half open: the joining client broadcast 1,629
destroys of its own join-culled locals into the field's flood minute, 23 s AFTER the
world-load episode had closed. Measured this pass:

- **The episode always closes before the snapshot arrives BY CONSTRUCTION**: the quiescence
  latch that closes it is the same latch that permits the ClientWorldReady announce, and the
  host's bracket only starts after the announce. Machine speed only decides whether the burst
  lands inside (fast dev box) or outside (field) the window — a structural defect masked
  locally.
- **The burst is TWO classes** (sender-line count, correcting §5-CORR's "689 KEYED"):
  (A) 23:20:01-07 ~870 eid-only clumps with client-band eidN = the twin-retire kernel
  (UnmarkKnownKeyedProp defers Element destruction; the actor->eid reverse is live when the
  destroy seam runs). **Class A is ALREADY FIXED cause-scoped** — `save_time_retire_util.h`
  UnmarkAndDestroy now calls MarkIncomingDestroy before the destroy (shipped in the R-4a
  key-scoping arc). (B) 23:20:09-14 ~660 KEYED with eid=0 — engine/BP rebuild churn (local
  twin: 2,174 suppressed-KEYED-eid=0 lines inside the local episode). **Class B's producer is
  untaggable** -> a temporal window is the correct layer for it, and for the [PROP-DROP]
  pending-place flood (x242, ends exactly at the rebuild end 23:20:13; its producer is class-B
  SPAWN churn — OnClientFinishSpawn already skips our own applies via PeekIncomingSpawn).
- Field Arm->Complete on the 9-fps box: 23:19:28 -> ~23:20:06 = **~38 s**.
- The field's quiesce session reason was 'world-change re-announce' — BOTH arm sites fire on
  the real join flow.

## §1 The design (v6 + round 7-9 completions)

**`InEpisode()` is COMPLETELY untouched** — semantics, sites, readers (signal/email/rng keep
it; round 3 measured that extending it would ABSORB a legitimate local append into their
re-prime = a lost write).

**NEW: the reconcile window** in world_load_episode — {up, kind, raisedAt, completeSinceArm}:
- RAISE kind=load at `Arm()` (join bringup; off-GT — see threading) and at the net_pump:373
  world-change re-announce arm (`RaiseReconcileForReload()`; InEpisode NOT raised there — the
  reload lane-park question is FILED until a real cave flow exists; note: this site fires on
  the REAL join flow too, so drill leg 1 exercises it).
- RAISE/refresh at `NoteReconcileBegin()` (event_feed SnapshotBegin case, GT drain): window up
  -> refresh, kind KEPT, no ceiling restamp; window down -> raise with
  kind = completeSinceArm ? midSessionBracket : load. **kind==load iff no SnapshotComplete
  since Arm iff (JOIN path) the curtain never dropped** — the player could act only BLINDLY
  (measured: the curtain is on the overlay's RENDER gate only, NOT AnyOpen,
  imgui_overlay.cpp:548-551 — "blind", not "blocked"; a blind place's doom exposure already
  exists today in the load window since v106, this extends it at the same blindness).
- LOWER at `NoteReconcileComplete()` (SnapshotComplete case; UNCONDITIONALLY sets
  completeSinceArm=true first, then lowers if up — a post-ceiling Complete keeps the tracker
  right), at the existing kBracketFlakeMs backstop branch, at a **180 s rising-edge ceiling**
  (raisedAt stamps only on false->true; checked at the TOP of TickQuiesceProbe BEFORE the
  !probeOpen early-return; driver TickClientReconcile runs every tick unconditionally; 4.7x
  headroom over the field's 38 s), and at `Reset()`.

**Readers** (the census: 8 files):
- `prop_destroy_seam` -> `InEpisode() || InReconcileWindow()` (any kind; belt keeps the old
  guarantee absolute). Suppressed KEYED destroys in the NEW segments (!InEpisode &&
  InReconcileWindow) log WARN with an R-1e-style rate latch (first 5, then decade folds; the
  field would have emitted ~660). Clumps stay INFO.
- `prop_drop_intent` -> `InEpisode() || (InReconcileWindow() && kind==load)`. Genuine
  mid-session places MUST flow (a client's suppressed place has NO delivery channel — the
  census express is HOST-only, prop_snapshot:620/:642 — and the re-bracket sweep would DOOM it
  as unclaimed; today's working flow intent->host-spawn->express->claim is preserved for
  kind==midSessionBracket). Same WARN latch in new segments.
- signal/email/rng: UNCHANGED.

**Threading** (mirrors the file's own audit-fixed shape): `up` + `kindIsLoad` are atomics
(plain stores at raise sites — Arm() is off-GT); raisedAt + completeSinceArm are GT-only,
materialized via a consume-request on the GT tick; both readers are GT; the ceiling starting
one tick late is harmless.

**Wire format: unchanged.** Class A stays cause-scoped (the shipped kernel mark).

## §2 DESIGNED AND DROPPED (do not re-derive)

- One-flag v2/v3 (move InEpisode's close edge): killed by the round-3 measurement — the lane
  parks early-return clearing prime/shadow BEFORE diff+TTL; a mid-session re-raise loses a
  legitimate local append (the false-delete sibling).
- Suppression until the sweep-settle latch: killed by the PROP-DROP timeline (the burst ends
  at Complete) + the post-curtain actionable window (a suppressed genuine destroy is permanent
  divergence).
- Arm() on the net_pump:373 path (raising InEpisode there): "re-prime is their design" was
  INFERRED and the reload flow is unmeasurable today — dropped; only the reconcile raise
  lands there.
- MsSinceQuiesced as the ceiling base: returns -1 when never latched — the ceiling would never
  fire exactly in the degraded path.
- The R4-round claim "a suppressed place self-heals via the reseed census": WRONG — the census
  express is host-only. Fixed by the kind split instead.
- Drop-intent park-and-replay: not built; kind==load windows are blind-player windows and
  kind==midSession windows don't suppress at all.

## §3 Residuals (written, not silent)

- Reload path, bracket never arrives: drop-intent suppresses genuine places up to 180 s with
  no delivery channel — WARN + ceiling; accepted.
- A reload raise while the window is already up does NOT restamp the ceiling (rising edge
  only) -- a late reload inherits the old edge and may get a truncated remaining ceiling for
  its teardown (audit MINOR-2; part of the same cave-travel trip-note).
- The WARN latches are process-lifetime -- a second join in the same process logs only
  decade/century folds (audit MINOR-3; diagnostic visibility only).
- The reload FLOW (a real mid-session cave/world travel) is unmeasurable today (L3
  one-persistent-world); the :373 RAISE SITE is exercised by drill leg 1 (it fires on the real
  join), but the flow's lane-park semantics are FILED with a trip-note: a future cave-travel
  build must drill this seam before shipping the flow.
- Drill legs 2-3 (kind classification / late-Begin) are DOWN-SCOPED to state-diag lines
  (every raise/lower/Begin/Complete logs {up, kind, completeSinceArm}); the staged late-Begin
  drill needs bracket-delay machinery whose cost exceeds its yield while the diag lines make
  the state greppable — recorded deviation from the round-6 commitment.

## §4 Acceptance

1. Drill leg 1 (VOTVCOOP_EPISODE_DRILL=1): the CLIENT destroys 5 keyed locals at its first
   SnapshotBegin. RED on the old close edge (5 "broadcasting DESTROY" — the field reproduced);
   GREEN on the new (5 suppressed + 0 broadcasts). s_1234 restored between runs.
2. joinchurn PASS (regression: incremental delta + sweep + kerfur axes).
3. Steady smoke PASS; no new WARN spam in steady state (rate latch verified by the drill's
   burst); the reconcile diag lines show raise(load)@Arm -> Begin(refresh) -> Complete(lower)
   on a normal join.
4. Wire unchanged (no kProtocolVersion bump).

## §5 /qf round map (9 rounds, fresh critic each)

R1 attribution measured (two classes; §5-CORR's 689 corrected) + class A found already
cause-scope-fixed + the temporal draft retracted for class A; R2 Begin-without-Complete
backstop hole + late-bracket re-raise + reload path folded in + the drill's RED design;
R3 send-census + WARN scoping + the lane-park regression catch (two flags resurrected) +
ceiling base fixed; R4 rising-edge ceiling + ceiling tick placement + the census-backstop
claim (later falsified); R5 the census-is-host-only falsification + the sweep-doom scenario +
window KINDs introduced + drill hygiene; R6 kind-as-raised-property + curtain-anchored
completeSinceArm discrimination + Arm-on-reload dropped (unmeasured) + drill legs 2-3;
R7 written residuals + kind-claim scoping + post-ceiling Complete ordering; R8 field 38 s
measurement + blind-not-blocked re-scope + threading spec; R9 "that holds" + the :373-site
bookkeeping correction.
