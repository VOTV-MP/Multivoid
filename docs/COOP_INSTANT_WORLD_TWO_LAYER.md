# Instant-world on connect -- a TWO-LAYER visual layer (AS-BUILT)

**Status: AS-BUILT (2026-06-24) -- design reviewed + seams approved + open-(a) audit CLEAN -> built +
deployed + AUTONOMOUSLY VERIFIED (smokes) + post-deploy audited (2 fixes applied). VISUAL hands-on PENDING
(the dynamic first-second dance-vs-instant needs the user's eye; the autonomous census proves no
stuck-hidden/dup but not the cosmetic fade).** Commits dcf56f3d (layer) + 98bfa5f3 (thread-safety) +
bf770161 (audit fixes). Deployed MD5 `f155181d` (proto v88). HEAD `bf770161` (push HELD).
Goal: the joining client sees the host-world AS-ASSEMBLED IMMEDIATELY, without the ~1-2s visible "dance"
(dup props/kerfurs flicker in, ghosts, wrong positions, self-correct). The current reconcile is correct
in END-STATE; this is a TIMING/VISUAL layer on top.

> **AS-BUILT note on SEAM 3 (the open-(a) refinement #3 below is SUPERSEDED):** the design-review draft
> proposed `IsPendingSaveTimeTwin`/`IsPendingKerfurRetire` predicates in the reconcile files. AS BUILT those
> were REVERTED to honor the hard `git diff reconcile = 0` constraint -- `mirror_defer` instead takes the
> confirmed/hold decision from `payload.hasMatchPos` AT THE HOOK SITE (a save-time-keyed mirror = its local
> twin still visible = HOLD; no key = host-only/derived = reveal at lift). Same partition, zero reconcile
> touch. The reveal backstop is `mirror_defer`'s own `g_hidden` map (every hidden mirror tracked there ->
> revealed at quiescence -> stuck-hidden impossible), not a MirrorManager walk.

## The frame -- two layers, the backup is UNTOUCHED
- **LOWER layer = the current reconcile** (quiescence-gated sweeps: `pile_reconcile::SweepReconcileSaveTimeTwins`,
  `kerfur_reconcile::SweepReconcileSaveTimeKerfurs`, the npc ghost sweep, the divergence sweep + the
  `save_time_retire_util` kernel). **STAYS EXACTLY AS-IS = the SAFE-BACKUP.** It guarantees correctness at
  quiescence (`HasLoadTailQuiesced()`), exactly as today.
- **UPPER layer = NEW, VISUAL-ONLY** (short curtain + deferred-spawn). Tries to make the connect instant.
  It only toggles `bHidden` / `bActorEnableCollision` and draws a cover -- it NEVER changes what the sweep
  destroys. **Worst case = today** (backup catches it, dance briefly visible). **Best case = instant.**
  Correctness is guaranteed by the untouched backup, so the upper layer may be simple/aggressive.

This is the user's smart hybrid of approach #1 (deferred spawn) + #2 (curtain), with a SHORT curtain so it
does NOT add the +2s blank screen that naive #2 would.

## Grounding facts (verified)
- `HasLoadTailQuiesced()` = `g_sweepFired` (`remote_prop_spawn.cpp:1351`) -- flips when the load-tail stops
  changing for `kSweepQuiesceScans=10 x 200ms = 2s` of real async-I/O stability, then the sweeps run. The
  1-2s is genuine async settle, NOT padding -> cannot be shortened safely (the kerfur multi-second load-lag
  raised this value deliberately; tightening = the 2026-06-15 kerfur-dupe regress).
- The connect snapshot arrives as a burst: `SnapshotBegin` -> spawn/bind burst -> `SnapshotComplete`
  (`event_feed.cpp:~403` calls `join_progress::Complete()` at SnapshotComplete). **SnapshotComplete fires
  SECONDS BEFORE quiescence** -- this gap is the curtain's short window.
- The current loading_screen (`loading_screen.cpp:48-138`, drawn in `imgui_overlay.cpp:362-368`) is a SMALL
  centered panel, NOT a full-screen cover -- the world renders behind it. The curtain needs a full-screen
  opaque cover (a new ImGui full-viewport window, or a UE camera fade).
- Mirror spawn choke-points (where VISIBLE host-mirror actors are born): `remote_prop_spawn::OnSpawn` (all
  keyed props + kerfur props + trash proxies route here) and `npc_mirror::SpawnFreshNpcMirror` (all NPC
  mirrors). **Effective N = 2 hooks.**
- **Collision is SEPARATE from visibility** (VOTV `Engine.hpp`): `AActor::bHidden` @0x58 vs
  `bActorEnableCollision` @0x5C; `SetActorHiddenInGame(bool)` is VISUAL-ONLY. So a hidden mirror stays
  collideable + grab-trace-hittable + physics-active -> deferred-hide MUST also `SetActorEnableCollision(false)`
  on real prop/NPC mirrors (pile AStaticMeshActor proxies are already collision-less -> Hide alone). The SDK
  layout settles this -- no runtime probe needed (RE-before-probes).

## The design (the SEAMS to review)

### SEAM 1 -- the SHORT curtain (full-screen cover)
- **Show:** at connect / join start (when the client begins applying the host snapshot).
- **LIFT trigger = "primary world assembled" = `SnapshotComplete` + spawn-queue drained** (the burst of
  PropSpawn/EntitySpawn from the connect snapshot all applied; ~1 frame after `join_progress::Complete()`).
  **NOT `g_sweepFired`** -- this is the KEY that keeps the curtain SHORT (lifts when the host state has
  arrived + bound, ~2s before quiescence; no +2s blank screen).
- **What it hides:** the rawest moment -- the client's own save load-in, the camera settle, the spawn
  burst, and the LOCAL-actor reposition jumps (fuzzy-bind repositions a local save-loaded actor to the host
  position DURING the burst; the curtain covers the whole screen so these jumps are invisible). This is the
  coverage #1-deferred-alone MISSES (local actors aren't our mirrors) -- the curtain closes that gap for the
  PRIMARY window.
- **Mechanism:** a full-viewport opaque ImGui window for the curtain phase (self-contained in our overlay;
  no new UFunction). Add a `Phase::Reconciling`-style state to the join lifecycle but lift at
  SnapshotComplete, not quiescence.

### SEAM 2 -- deferred spawn (the tail-catcher), at the 2 chokes
- Every host-mirror spawns **hidden + collision-off**: at `remote_prop_spawn::OnSpawn` and
  `npc_mirror::SpawnFreshNpcMirror`, immediately after spawn call `SetActorHiddenInGame(true)` +
  (real prop/NPC only) `SetActorEnableCollision(false)`.
- Works BOTH under the curtain (primary spawns hidden) AND after the curtain lifts (TAIL spawns -- late
  twins, the client-off twin-in-air, post-lift fresh-spawns -- stay invisible until revealed).

### SEAM 3 -- the SELECTIVE reveal (the smart part: what shows at curtain-lift vs holds to quiescence)
The crux. If deferred hides ALL mirrors and the curtain lifts at SnapshotComplete, a blanket reveal would
flash the PRIMARY dups (the sweep kills them only at quiescence, +2s later). So the reveal is SELECTIVE:
- **At curtain-lift: reveal the CONFIRMED mirrors** (exact-key binds + unambiguous fresh-spawns of
  host-only props) = the assembled world. These have no pending adjudication -> safe to show now.
- **Hold DEFERRED: the PENDING candidates** -- the mirrors that entered a reconcile-pending set
  (`g_pendingSaveTimeTwin`, `g_pendingRetire`, the fuzzy/ambiguous adopt candidates, the convert
  fresh-spawns). These are exactly the ones the sweep will adjudicate.
- **At quiescence (`g_sweepFired`, the backup): the sweep destroys the ghosts/dups among the pending set;
  the SURVIVORS are revealed** (Show + EnableCollision). This is the TAIL -- invisible (no curtain, but
  deferred-hidden until now). Reveal must run AFTER `RunDivergenceSweep_` returns (last action of the
  reconcile tick), so a ghost is destroyed-while-hidden, never revealed-then-destroyed.
- The confirmed/pending partition reuses the EXISTING pending sets (no new classification logic): a mirror
  is "pending" iff it is in a reconcile pending set or armed for a sweep; else "confirmed."

### SEAM 4 -- interface to the backup (untouched)
- The upper layer READS existing signals only: `SnapshotComplete` (curtain lift + primary reveal),
  `g_sweepFired` (tail reveal), and the existing pending sets (partition). It WRITES only `bHidden` /
  `bActorEnableCollision` + draws the cover. It NEVER alters the sweep's destroy decisions.
- Upper hits (confirmed correctly) -> the sweep at quiescence finds 0 divergence among revealed mirrors ->
  no-op (fast). Upper misjudges (reveals a dup early, or holds a correct mirror) -> worst case the dance is
  briefly visible OR a correct mirror appears at quiescence instead of at lift = TODAY's behavior. No
  correctness loss -- the backup is authoritative.

## Coverage (honest)
- PRIMARY local-actor reposition jumps -> hidden by the CURTAIN (whole-screen, during the burst). [covered]
- PRIMARY host-mirror dups/ghosts -> hidden by DEFERRED (pending held past lift) -> resolved at quiescence
  invisibly. [covered]
- TAIL (client-off twin-in-air, late twins, post-lift fresh-spawns) -> DEFERRED until quiescence reveal.
  [covered -- this is why the twin-in-air, doc 08, is fixed by THIS layer, one mechanism]
- RESIDUAL: a LOCAL save-loaded actor that repositions AFTER the curtain lifts (rare -- most fuzzy-binds
  happen during the burst). The short curtain is down and deferred can't hide a non-mirror local actor ->
  this jump stays visible. The backup still makes it correct. This is the honest cost of a SHORT curtain
  (vs a quiescence-long curtain that would add the +2s the user rejected). Quantify in the first hands-on;
  if material, a per-actor brief re-hide on reposition is a follow-up (do not over-build now).

## Acceptance (design review gate)
1. Curtain is SHORT -- lifts on "primary world assembled" (SnapshotComplete + drain), NOT `g_sweepFired`.
2. Deferred hide+collision-off at BOTH chokes; works under the curtain AND after lift (tail).
3. Selective reveal: confirmed at lift, pending held to quiescence -> no primary-dup flash, no empty world
   at lift.
4. Collision-off paired with hide on real prop/NPC mirrors (SDK-proven), re-enabled on reveal -- no
   grab-air / physics-pushback on a hidden mirror (esp. the tail after the curtain lifts).
5. Backup UNTOUCHED (worst case = today, best case = instant); upper layer is read-only on the backup.
6. The twin-in-air (doc 08) is covered by the deferred tail (one mechanism, not a separate fix).

## Scope (honest)
Medium. ~3-5 files: a curtain module (full-viewport cover + the SnapshotComplete-lift lifecycle), the
deferred-visibility hooks at the 2 chokes + a reveal driver (2 reveal events: selective-at-lift +
tail-at-quiescence), 2 new UFunction resolves (`SetActorHiddenInGame`, `SetActorEnableCollision` via AOB/
reflection). ~150-250 LOC. No wire/protocol change. Backup untouched. One-feature files per the modular rule.

## Open (a) AUDIT -- CLEAN (2026-06-24, agent + own cross-check). SEAM 3 ROBUST -> cleared to build.
Every fresh mirror is confirmed-safe-at-lift OR in an existing pending set -- NO third category.
- **(a1) funnel:** all fresh mirror spawns route through the 2 chokes (props/kerfur-props/proxies via
  `OnSpawn`, NPCs via `SpawnFreshNpcMirror`). Bind-local paths (exact-key/fuzzy/keyless/adopt) create NO new
  actor -> SEAM-1 curtain covers their reposition jumps. ONE bypass: the remote-player PUPPET
  (`net_pump.cpp:934`) -- no local twin (no dup), never hidden (not stuck) -> cosmetic, OUT OF SCOPE.
- **(a2) reveal reach:** every fresh mirror is Install'd into `MirrorManager<Prop>`/`<Npc>` (the trash proxy
  IS a `MirrorManager<Prop>` element, not only `trash_proxy`'s local map) -> the quiescence full-walk
  backstop catches all -> NO stuck-hidden possible.
- **(a3) whitelist:** CONFIRMED = exact-key/fuzzy/keyless binds + host-only-transient fresh-spawns. HOLD =
  `g_pendingSaveTimeTwin` (pile) / `g_pendingRetire` (kerfur off->active) / adoption + ghost-sweep pending.

### 3 build refinements (folded in; backup untouched)
1. **Proxy hide placement:** hide in `trash_proxy::SpawnProxy` (proxy born by E::SpawnActor, NOT the OnSpawn
   BeginDeferred pipeline); **Hide-only** (proxies already collision-less, `trash_proxy.cpp:186`); NOT on the
   idempotent re-spawn convergence return (`trash_proxy.cpp:165-169`).
2. **Hide AFTER register:** place the deferred-hide after `RegisterPropMirror`/`Install` (OnSpawn:947 /
   npc_mirror Install) so an actor is never hidden-but-unenumerable.
3. **Confirmed test checks the 2 save-time sets:** lift-reveal gates on new `IsPendingSaveTimeTwin(eid)`
   (pile_reconcile) + `IsPendingKerfurRetire(eid)` (kerfur_reconcile) predicates -- their LOCAL twins are
   still visible at lift = the dup-flash to avoid.

### Post-deploy audit (2026-06-24, code-reviewer agent) -- 2 fixes applied, 2 modularity flags
No CRITICAL. PASS on perf (spawn hooks O(1), join-burst-only via IsArmed gate; reveals iterate g_hidden once;
curtain Render O(1)), thread-safety (mirror_defer game-thread-asserted; join_curtain atomic state + read-only
Render), correctness (IsArmed gate / g_revealed guard / liveness-gated reveal / hasMatchPos on both payloads /
collision asymmetry), and backup-untouched (git diff reconcile = 0). FIXED: (1) the curtain fade is rendered
via a curtain-aware RENDER gate (`AnyOpen() || hud::IsActive() || join_curtain::IsActive()`) so the 0.4s
alpha-fade isn't skipped when the loading panel drops at SnapshotComplete; (2) `mirror_defer::Arm()` now
reveals-then-clears (a soft re-bracket can't strand a hidden mirror). MODULARITY FLAGS (not blocking, no
extraction done autonomously): `engine.cpp` 884 LOC and `remote_prop_spawn.cpp` 1401 LOC are past the 800 soft
cap. Reasoning to NOT extract now: `engine.cpp` is the cohesive home for ALL AActor UFunction thunks
(GetActorLocation / SetActorTickEnabled / ... / SetActorHiddenInGame are siblings, one concern) -- splitting
only the 2 visibility ones would be inconsistent; a real split is an engine-wide by-concern refactor.
`remote_prop_spawn.cpp`'s OnSpawn convergence triple is tightly coupled to the shared claim state -- a separate
planned extraction, not triggered by +23 wiring lines. Both flagged for a future dedicated refactor.

### Decisions
- Curtain = **ImGui full-viewport cover with ALPHA-FADE dismissal** (~0.3-0.5s, 1->0 on SnapshotComplete; NOT
  UE `ClientSetCameraFade` -- our surface, our trigger, smooth reveal). NOT a hard snap.
- The residual post-lift LOCAL reposition (rare) + the puppet spawn-pop: measure in hands-on before adding a
  re-hide. Do NOT over-build now.

## Approaches considered (the assessment that led to this hybrid)
| approach | scale | risk to backup | coverage | chosen |
|---|---|---|---|---|
| 1. Deferred spawn only | small-med | none | all spawn-dance + twin; MISSES local fuzzy-bind reposition | folded in (SEAM 2) |
| 2. Snapshot curtain only | medium | none | full incl. reposition, BUT +2s blank screen past SnapshotComplete | folded in but SHORT (SEAM 1) |
| 3. Tighten quiescence | trivial | **HIGH (proven kerfur regress)** | n/a (lower-layer speedup) | REJECTED w/o a probe |
| 4. Hybrid 1+2 (smart) | medium | none | full coverage, NO +2s (short curtain) | **CHOSEN** |
The naive #2 (curtain held to quiescence) was rejected for the +2s blank. The SHORT curtain (lift at
SnapshotComplete) + deferred tail gives #2's full coverage without the blank -- the user's smart hybrid.

## Relationship to the other tracks
- Fixes the client-off twin-in-air (`docs/kerfur/08`) as a side effect (deferred tail). The deeper
  convert-ghost<->adoption handoff correctness fix stays a separate, lower-priority backlog item (the
  backup absorbs it).
- The reconcile backup includes the just-built `save_time_retire_util` kernel + scope A -- NONE of it is
  touched by this layer. Push of the extract (`597ae6f7`) is independent; the clean 3-instance re-verify
  still gates the push and is compatible with testing this layer later.
