# RCA: join-window placed props invisible on the client (FOUR roots, one saga) — 2026-07-11

Status: all four roots AS-BUILT + log-RCA'd from live joins (takes 1-3); hands-on take-4 pending.
Commits: `6d9c6518` (take 1: fresh-defer + fuzzy identity-steal gate), `8a2b04d0` (take 2: SPAWN
REVALIDATION generalization), `8b1b340a` (watchdog quiescence-by-ceiling, audit MEDIUM),
`2fefd161` (take 3: host KEY-UNIQUENESS authority + reconcile-before-doom order — see §take-3 below).
Durable lessons: [[lesson-join-window-wire-expression-provisional]],
[[lesson-prop-mirror-manager-mixes-local-and-wire-rows]], [[feedback-identity-logs-carry-key-and-loc]],
[[lesson-votv-save-ships-duplicate-interactable-keys]].

## User repro chain (live, 2026-07-11)

1. **Take 1 (12:20 session):** host R-hold placed two cblocks from the hotbar while the client was in
   the connection window -> both invisible on the client forever, even after host E-grabs.
2. **Take 2 (13:20 session, on the take-1 fix):** host picked a WORLD rock up into the hotbar DURING
   the connect window and placed it back; a second rock came from pre-connect hotbar stock. The
   pre-connect one APPEARED (take-1 fix works); the hotbar'd-during-window one stayed invisible.

## Root 1 (take 1): fresh mid-episode mirrors are churn-killed

Client log 12:20: OnSpawn fresh-spawned both mirrors at 12:20:37 (keys `Sb3Ni…`/`PSe9L…`, eids
6221/6222) -> at 12:20:39 `grab_hook[destroy-seam]: CLIENT suppressed KEYED DESTROY … inside
world-load episode (loadObjects churn)` destroyed BOTH (the suppression covers only the BROADCAST;
the local destroy proceeds) -> bindings drained at 12:22:20 -> host pose/E-grab streams resolve
nothing. The transferred save had no record of them (placed after capture) so the churn never
recreated them. Only FRESH spawns are vulnerable this way; resolve/converge targets get destroyed
too but normally return as same-key recreates that `join_membership_sweep`'s keyed-churn RE-BIND
re-binds.

Fix: the fresh tail of `remote_prop_spawn::OnSpawn` never spawns while
`world_load_episode::InEpisode()`; the payload is captured for the quiescence drain
(`quiescence_drain::ArmPendingSpawn` — the DESTROY-BEFORE-LOAD sibling, [[feedback-snapshot-before-state-ready]]).

## Root 2 (take 2): converge-bound rows whose churn recreate never comes

Host log 13:20: rock `uwmsjz…` was a WORLD prop (boot eid 2586); host picked it up at 13:20:39 —
BEFORE the save-transfer capture completed with it in the hotbar — and placed it at 13:20:46 (new
eid 5384, same key via loadData). Client log: the snapshot spawn CONVERGED onto the client's
level/save copy at 13:20:49 (`resolves to live actor … d=1902cm`) and bound eid 5384 -> the churn
destroyed that actor at 13:20:51 -> **no recreate existed** (the save world data has no `uwmsjz`
record; it sat inside the saved hotbar) -> the sweep's RE-BIND pass (which resurrected the OTHER
rock `hFTW7…`, in-world at capture) had no candidate -> row 5384 held a dead actor for the session
-> `PropPose … no local match (key or eid)` spam = invisible. The doomed rock died traceless: the
doom sweep logged only a class histogram (`doomed 77 x 'prop_C'`), no per-actor key/loc — which is
what made this RCA slow and prompted the logging rule below.

Fix (generalization, supersedes the take-1 fresh-only arm per RULE 2): OnSpawn CAPTURES **every**
in-episode wire expression (capture point after the trash-proxy branch; eid-dedup index; cap 4096;
`deferKerfur` replayed verbatim); `ApplyPendingSpawns` (RunReconcile step 4 — after
`BindUnboundReCreates`, BEFORE `ApplyPendingDestroys` so a spawn+destroy pair nets zero)
re-expresses ONLY entries whose Registry row is dead/absent (`IsLiveByIndex`; survivors +
RE-BOUND recreates skip O(1)).

## The fuzzy identity-steal gate (surfaced by the user's 4-crowbar question, latent)

Gap-I-1's 30 cm same-class+name fuzzy rekey (built for per-peer-divergent mushroom/garbage keys)
would chain-steal same-spot placements: #2's rekey steals #1's just-spawned mirror, N-1 eids dangle.
With the revalidation drain applying deferred same-class spawns back-to-back at one location this
became deterministic. Gate: a fuzzy match already **wire-mirror-bound** to a different eid is never
position-stolen (fresh-spawn instead). CRITICAL audit catch before ship: `ResolveMirrorEidByActor`
scans `MirrorManager<Prop>` which MIXES census-walk LOCAL rows (every keyed interactable,
`IsMirror()==false`) with wire rows — an unfiltered gate would have killed the legitimate mushroom
dedup. Shipped as `ResolveMirrorEidByActor(actor, wireMirrorOnly)` filtering `IsMirror()`.

## Secondary hardenings (same commits)

- **Watchdog quiescence-by-ceiling** (`8b1b340a`): on the SnapshotBegin-lost flake the episode
  watchdog closed the destroy-suppress latch but `g_sweepFired` never flipped -> all four
  quiescence_drain queues stranded for the session. `TickWatchdog()` now returns the force-close
  edge; `TickClientReconcile` (the flag owner) sets `g_sweepFired` there (`g_sweepPending` stays
  false — no claims exist, the doom sweep must never run off this path).
- **Identity-critical logs carry key+loc** (user rule): per-doom lines (cls/key/loc), RE-BIND +
  arm/apply lines with loc, and the dead-row TRIPWIRE ("dead keyed mirror row SURVIVED the re-bind
  pass — eid/key") naming the residual. Destroy-seam lines deliberately carry NO loc: the seam is
  POST-native (actor PendingKill; a PE GetActorLocation dispatch there is unreliable).

## Take 3 (14:35 session, on `B00459CC`): the revalidation WORKED — and exposed roots 3+4

User verdict: "камни не исчезли но они не на своих местах и еще у клиента 2.5fps". Client log: 3
frames/s steady post-join, 285 ms ENGINE-side frames (our hook bodies 0.00 ms/frame), 2323
expressions captured / 2093 skipped-live / **230 re-expressed**, **232 doomed** — and the doomed vs
re-expressed key sets OVERLAP at 200 of 230.

### Root 3 (game data): VOTV's save ships DUPLICATE interactable Keys

Host log: the world-load adopt burst enrolled 85 distinct live `trashBitsPile_C` (distinct pointers,
distinct positions, snapped rotations) across only FOUR keys — clone families 65
(`-GiVAop2Win4wvA1rllELw`, the landfill) + 12 + 5 + 3 (save-born; the 2026-06-24 kerfur doc already
records the sweep dooming "80 trashBitsPile" — same phenomenon, silently eaten for weeks). Every
identity layer assumes Key uniqueness: the family's 65 wire rows all exact-key-resolved ONE client
actor (64x "resolves to live actor -- diverged, converging" on the same key), churn left 63 rows
dead, rebind paired arbitrarily within the family, and the drain then re-expressed 64 rows INTO
positions where a live rebound local already sat -> interpenetrating AWAKE physics pairs -> the
solver storm = 2.5 fps + scattered props.

Fix (`2fefd161`): HOST KEY-UNIQUENESS AUTHORITY at `MarkPropElement` (the ONE enrollment owner;
audit HIGH moved it there from a census-only first cut — the Init-POST late-load catch also
enrolls). A keyed actor enrolling while a DIFFERENT live actor carries its Key is re-keyed
(`prop_synth_key::MintFreshKeyForDuplicate`, `rk_<64-bit-random-hex>`, GT-gated, confirm re-read);
dead incumbent = churn recreate inheriting identity (spared by `FindLiveActorByKey` liveness). The
game re-saves live keys -> bakes into the host save + every transfer. `MarkPropElement` returns the
enrolled key; both broadcast callers (Init POST, takeObj POST) build their payload from the return.

### Root 4 (our order): the doom sweep ran BEFORE the revalidation drain

`RunReconcile` lived at the SWEEP TAIL: locals whose wire expression raced their loadObjects
recreate (never-bound rows — 21 `g_paper_1` at player spawn traced end-to-end, ~150 more) were
doomed as unclaimed, THEN the drain fresh-spawned awake replacements (net actor count ~0, but
settled locals became interpenetrating awake mirrors). Fix: `RunReconcile()` (joinSweep param
retired, RULE 2) runs at the quiescence FIRE EDGE before `RunDivergenceSweep_`, claims still armed —
the re-runs converge-bind + CLAIM their recreates, the sweep spares them; doom judges LAST. The
one-shot L1 orphan census moved to the sweep tail (must reflect doom removals); the valve-abort
duplicate call removed.

## Audit verdicts (feature-dev:code-reviewer, 4 passes)

Pass 1 (pre-ship of `6d9c6518`): CRITICAL mixed-rows gate (fixed via wireMirrorOnly) + HIGH
deferKerfur loss at replay (threaded through the queue). Pass 2 (on `8a2b04d0`): capture placement,
liveness filter (incl. the recycled-slot class — IsLiveByIndex pointer-compares), spawn->destroy
net-zero ordering, cost (all cold-path) VERIFIED OK; MEDIUM flake-strand (fixed `8b1b340a`); HIGH
file-size rule — `remote_prop_spawn.cpp` 1084 LOC touched twice without extraction -> the
fresh-spawn tail extraction is OWED (queued, post-test). Pass 3 (on the take-3 fix): prong-2 order
verified sound on all 6 checks; prong-1 HIGH — census-only detector misses the Init-POST enroll
path (applied: moved into MarkPropElement). Pass 4 (verify): return-key plumbing / threading /
re-entrancy (no setKey observers exist) / broadcast callers all SOUND; MEDIUM detect-vs-index TOCTOU
bounded by the GT gate (a rekey is GT-only, the GT cannot race itself; off-GT trips LOUD) +
one-way-door note documented at the idempotency early-out.

## Where to look FIRST next time

An invisible-on-client prop after a join: client log — the dead-row TRIPWIRE + `[SPAWN-DEFER]`
arm/apply pairs + the per-doom `dooming 'cls' key=… loc=…` lines localize it in seconds. A post-join
fps collapse or scattered props: compare the doomed vs re-expressed KEY SETS (an overlap = the
sweep and the drain fighting) and histogram same-key multiplicity in the host adopt burst (a clone
family = root 3; host log `KEY-UNIQUENESS ... re-keyed` lines show the authority working). The
mechanism docs: `quiescence_drain.h` (queue contract + fire-edge order) + COOP_ENTITY_EXPRESSION_MAP
"Join-window PROVISIONALITY" + "KEY-UNIQUENESS AUTHORITY" bullets.
