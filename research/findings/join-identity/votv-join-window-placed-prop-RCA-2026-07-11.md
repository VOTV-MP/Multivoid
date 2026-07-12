# RCA: join-window placed props invisible on the client (SIX roots, one saga) — 2026-07-11/12

Status: CLOSED BY ARCHITECTURE 2026-07-12 — the JOIN BARRIER (`bbf91f39`+`7847021e`, take-6
hands-on pending) removed the two-authority overlap at the source; the takes-1/2/4 compensation
machinery this doc's fixes built (spawn capture/revalidation, wire-order netting) was RULE-2
deleted the same day. Roots 3+6 (duplicate save keys) stay fixed by the take-4 re-key authority.
This doc is the measured record of the six roots + the saga that proved the seam architectural.
The redesign: research/findings/join-identity/votv-join-barrier-DESIGN-2026-07-12.md.
Commits: `6d9c6518` (take 1: fresh-defer + fuzzy identity-steal gate), `8a2b04d0` (take 2: SPAWN
REVALIDATION generalization), `8b1b340a` (watchdog quiescence-by-ceiling, audit MEDIUM),
`2fefd161` (take 3: host KEY-UNIQUENESS authority + reconcile-before-doom order — see §take-3 below),
`460da7e4` (take 4, 2026-07-12: wire-order queue netting + setKey SuperStruct climb — see §take-4 below).
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

## Take 4 (2026-07-12 10:50 session, on `3D250B83`): the same rock gone again — TWO more roots

User verdict: the rock the host hotbar'd and re-placed during the client join window is STILL gone
(take-2 repro on the take-3 build). Logs (scratchpad take4_host/client.log) traced key `uwmsjz…`
end-to-end.

### Root 5 (our order, deeper than root 4): the PHASE drain inverts per-key wire order

The wire (reliable, ordered) delivered for key `uwmsjz`: snapshot SPAWN eid=2586 (10:51:06, captured
in-episode) -> DESTROY eid=2586 (10:51:09, host pickup — APPLIED, actor was bound) -> DESTROY eid=0
x2 (10:51:10, the hand actor: #1 applied to the churn recreate, #2 found nothing -> DEFERRED) ->
SPAWN eid=5383 (10:51:10, the placement — captured). At the drain (10:51:17) the PHASE order (all
spawns, then all destroys) inverted it: eid=2586 re-expressed (resurrecting a row a wire destroy
legitimately killed — its capture was never invalidated), eid=5383 re-expressed converging onto THE
SAME actor (one key), then the stale deferred destroy applied LAST and killed it. Tripwire fired for
5383; the rock invisible for the session — the exact user report.

Fix (take 4): pre-net the queues per identity at CAPTURE time so no identity ever holds both a
pending spawn and a pending destroy (phase order then can't invert anything):
- destroy side — `remote_prop::OnDestroy` (wire-event path only, never the TryApplyDestroy re-apply)
  calls `quiescence_drain::CancelPendingSpawnsForWireDestroy`: a captured in-episode spawn matching
  by eid (or key bytes when the destroy carries a key) is cancelled — the wire says that incarnation
  is dead. Kills the eid=2586 resurrect. The destroy itself is NOT consumed: it must still defer to
  kill a late loadObjects recreate (the destroy-before-load 5-vs-7 dup class).
- spawn side — `ArmPendingSpawn` erases any pending deferred destroy matching the same identity: a
  later same-key wire spawn supersedes it (destroy->spawn nets to the SPAWN, as delivered). This is
  what saves the placed rock.
Observed wart (same log): the host broadcast the eid=0 hand-actor DESTROY twice (17028/17029, same
actor same second) — client-side the dedup + the supersede absorb it; host-side seam double-fire not
yet dug.

### Root 6 (root-3 fix was INERT): setKey resolution failed on the actor_save lineage

Host log: 162x `synth-key: rekey -- setKey UFunction not found on 'trashBitsPile_C' (nor Aprop_C
base)` — every duplicate enrolled under its duplicate key ("pre-fix behavior"). `AtrashBitsPile_C`
is **Aactor_save_C lineage, not Aprop_C** (ue_wrap/prop.cpp:199: key @0x0230 direct): the
`IsDescendantOfProp` gate skipped the hardcoded Aprop_C fallback, and `trashBitsPile_C` itself
declares no setKey — it lives on `actor_save_C` (bp_reflect actor_save.functions.txt line 2; the
ubergraph writes event param -> the class `key` property, verified in actor_save.json bytecode; the
param is NameProperty "key", matched case-insensitively by ParamFrame). Third strike of
[[lesson-findfunction-exact-owner-no-superstruct-climb]].

Fix (take 4): `reflection::SuperStructOf(cls)` (the wrapper-layer primitive) + `ResolveSetKeyFn` now
CLIMBS the SuperStruct chain (<=16 hops) to the declaring ancestor, cached per starting class; the
Aprop_C special-case in `MintFreshKeyForDuplicate` removed (RULE 2 — the climb subsumes it).
`EnsureKeyForBroadcast` (cs_ mints) inherits the same repair through the shared resolver.

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
arm/apply pairs + the per-doom `dooming 'cls' key=… loc=…` lines localize it in seconds. Then grep
the PROP'S KEY across BOTH logs and lay out the full wire event sequence in arrival order (spawns,
destroys, applied-vs-deferred) — take 4's root was the DRAIN re-ordering a destroy->spawn pair, and
only the per-key arrival-order timeline exposed it ([[feedback-map-all-wire-events-before-fixing-missing-sync]]).
The netting markers: `[SPAWN-DEFER] CLIENT CANCELLED captured … a later wire DESTROY` and
`[DESTROY-DEFER] CLIENT SUPERSEDED deferred destroy … by a later same-identity wire spawn`. A post-join
fps collapse or scattered props: compare the doomed vs re-expressed KEY SETS (an overlap = the
sweep and the drain fighting) and histogram same-key multiplicity in the host adopt burst (a clone
family = root 3; host log `KEY-UNIQUENESS ... re-keyed` lines show the authority working). The
mechanism docs: `quiescence_drain.h` (queue contract + fire-edge order) + COOP_ENTITY_EXPRESSION_MAP
"Join-window PROVISIONALITY" + "KEY-UNIQUENESS AUTHORITY" bullets.

## Architectural verdict (2026-07-12, user decision) — the saga IS the signal; next session goes per rule 1

Of the six roots, FOUR (1, 2, 4, 5) live on ONE seam: **during the join window the client world has
TWO concurrent authorities** — the transferred-save loader (loadObjects churn: destroy+recreate of
every keyed prop, the PAST) and the live wire stream (the PRESENT). Every take added another
compensation layer on that seam (episode gate, capture queues, deferred destroys, twins,
pos-corrections, RE-BIND, doom sweep, wire-order netting); root 5 was literally our own deferral
machinery destroying an ordering the reliable channel had already guaranteed.
[[feedback-recurring-bug-is-architectural]] formally tripped at take 3. MTA has no such seam by
design: no client save-load, ONE ordered entity stream, one authority.

**DECISION (user, 2026-07-12): the next session approaches this architecturally per RULE 1** (full
green light), with fresh context. The two candidate consolidation stages sketched at decision time
(to be properly designed then, not now):
1. **Single ordered wire-event JOURNAL for the whole episode** — one arrival-ordered log of every
   in-episode wire event, replayed ONCE at quiescence; absorbs the per-type queues + hand-tuned
   phase order (the take-4 netting is already the per-identity last-event-wins equivalent for
   spawn/destroy — the journal is its general form, absorbing pos-corrections/twins too).
2. **Remove the conflict at the source** — suppress loadObjects churn (destroy+recreate) for
   wire-tracked keyed props entirely: the wire becomes the ONLY existence authority for tracked
   entities during the join; the save feeds only the untracked world.
Inputs the architecture session must read first: this finding (all six roots), `quiescence_drain.h`
(the queue census — SIX specialized queues is the debt metric), `docs/COOP_ENTITY_EXPRESSION_MAP.md`
join-window bullets, `reference/mtasa-blue/` entity-stream shape (CEntityAddPacket), and the take-5
verdict. The owed `remote_prop_spawn.cpp` extraction (audit HIGH x2) sits on the same seam — fold it
into the architecture work rather than doing it as a standalone shuffle first.
TRIPWIRE agreed with the user: any NEW post-join prop bug of this class (wrong place / wrong count /
missing after join) before the consolidation = stop patching, start the redesign immediately.
