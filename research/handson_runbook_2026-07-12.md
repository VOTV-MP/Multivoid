# Hands-on runbook — 2026-07-12, TAKE 6 (the JOIN BARRIER — architecture, per rule 1)

Take-6 ran on DLL **`7030160D`** (md5 first 8; PASSED — see RESULT below). Currently deployed:
**`4B2E4024`** = the same barrier + the take-7 anti-smear instrumentation (see TAKE 7 at the
bottom). Commits `bbf91f39` (the barrier) + `7847021e` (audit-CRITICAL fix: the off-GT Arm race --
probe session now opens on the GT via an atomic request).
SUPERSEDES take 5 (`0DA4FAE0`) BEFORE it ran: the user mandated the architectural fix of the
two-authority join seam with fresh context; take-5's mechanisms (wire-order netting, spawn
revalidation) are now RETIRED — the barrier makes their bug class structurally unreachable.
Design doc: research/findings/join-identity/votv-join-barrier-DESIGN-2026-07-12.md.

## What this build IS

The MTA join barrier (INITIAL_DATA_STREAM shape): the client announces `ClientWorldReady` only
when its loadObjects tail has SETTLED (the same population-stability probe the doom sweep always
trusted, now owned by `coop/session/world_load_episode`). The host therefore streams the ENTIRE
world state (R2 deletes -> snapshot -> state lanes -> teleport) into a settled world. No wire prop
expression is ever provisional; wire events apply strictly in arrival order. Roots 1/2/4/5 of the
placed-prop saga die structurally; roots 3+6 (duplicate save keys) stay fixed by the take-4
KEY-UNIQUENESS re-key authority (unchanged in this build).

User-visible deltas (expected, by design):
- The joiner stays under the loading cover ~5-20 s LONGER (the world genuinely settling; the
  reveal now lands in a FINISHED world instead of mid-churn).
- The joiner receives NO live host events during its load; the post-settle snapshot carries their
  net effect.

## Test steps (the take-2/4 repro IS the acceptance test)

1. Host: load the usual save ("Host game"). Client: connect; WHILE the client sits in the connect /
   world-load window, host hold-R a world rock into the hotbar and place it back somewhere visible.
2. After the client is in: the placed rock EXISTS on the client, at the host's placement spot.
3. Take-2 regression: a rock from pre-connect hotbar stock placed in-window also appears.
4. Join regression sweep: props/piles/kerfurs present, mushrooms/garbage not doubled.
5. fps sanity post-join (the take-3 2.5fps class).
6. Bare-join host-wipe regression: host keyed-prop census stable across a client join (episode
   destroy-broadcast suppression unchanged but now self-closing).
7. If convenient: cave in/out on the client (travel re-announce waits for the new world's settle —
   host re-replay lands post-settle; nothing should vanish/double).

## What to read in the logs

CLIENT (the load-bearing sequence, in this order):
- `world_load_episode: ARMED ...` then `world_load_episode: quiescence probe ARMED (join world-load)`
- `world_load_episode: load-tail QUIESCED (population stable; session 'join world-load') -- episode
  CLOSED ...` — the barrier edge. A `DEGRADED` variant here = the deadline fired (pathological
  load; the settled-world guarantee did not hold — report it).
- `net_pump: ClientWorldReady announced (world up + registry coherent + load tail quiesced)` —
  STRICTLY AFTER the QUIESCED line, and no PropSpawn/PropDestroy receive lines before it.
- Then the snapshot bracket + `join_membership_sweep: divergence sweep FIRING (probe latched ...)`.
- NO `[SPAWN-DEFER]` / `SUPERSEDED` lines anywhere (that machinery is deleted); NO dead-row
  tripwire for the rock's key.

HOST:
- `[PILE-1C] slot N world-ready -- JOIN-WINDOW CLOSED` arrives LATER than before (at the client's
  settle) — expected.
- First load of a poisoned save: the `KEY-UNIQUENESS ... re-keyed -> 'rk_...'` burst (root-6 fix
  working; `re-key FAILED` lines = regression, stop and report). ~0 after the game re-saves.

## RESULT — TAKE 6 PASSED (2026-07-12 13:21, user hands-on + log-verified)

User verdict: "rock are present" — the in-window placed rock (the take-2/4 repro) exists on the
client. Logs pulled to scratchpad (`take6_host.log` / `take6_client.log`) before any relaunch:

- CLIENT sequence exactly per spec: `world_load_episode: ARMED` 13:20:27 → probe ARMED 13:20:29 →
  `load-tail QUIESCED ... episode CLOSED` 13:20:38 → `ClientWorldReady announced (world up +
  registry coherent + load tail quiesced)` STRICTLY after. **0 PropSpawn/PropDestroy receives
  before the announce line** — the entire connect replay (incl. all rock spawns) landed in a
  settled world. No DEGRADED, no `[SPAWN-DEFER]`, no tripwire.
- Sweep: post-snapshot probe session QUIESCED 13:20:42, `divergence sweep FIRING (probe latched;
  2340ms after arm)`; `BIND SUMMARY -- bound 872/872`.
- HOST: `[PILE-1C] slot 1 world-ready -- JOIN-WINDOW CLOSED` 13:20:38 (at the client's settle, as
  designed). KEY-UNIQUENESS: **162 re-keys, 0 `re-key FAILED`** — the `460da7e4` SuperStruct-climb
  works on the live save.
- Health: 0 `[ERROR]` both peers; WARN census = perf telemetry + kerfur census only; client
  workingset ~3.0 GB stable.
- NOTE for the kerfur anti-smear follow-up: the save had **ZERO kerfurs** (census: 0 NPC + 0 PROP
  both peers) — take-6 exercises NO kerfur layer; live-evidence retirements there need a
  kerfur-present run.

## TAKE 7 — the KERFUR-PRESENT join (anti-smear evidence run)

Deployed DLL: **`4B2E4024`** (md5 first 8), all 4 installs hash-verified. Same barrier build +
anti-smear INSTRUMENTATION ONLY (no behavior change): both adoptions log `poll #N, M ms after arm`
at bind/fresh-spawn; TRIPWIRE WARN if the 60 s adoption timeout fires before quiescence; two stale
spawn-revalidation comment/log references in join_membership_sweep rewritten (RULE 2). NOT smoked
(instrumentation-only build; user at PC).

Steps (any order, one session is enough):
1. Host: load a save (or play until) at least ONE kerfur exists — ideally one OFF (prop form)
   AND one ACTIVE (NPC form). The vending-machine kerfur purchase works too.
2. Client: join. Optionally: while the client loads, host turn a kerfur ON (the in-window
   turn-on exercises the retireOffEid chain).
3. In-game sanity: exactly one of each kerfur on both peers (no doubles, none missing);
   client sees the active kerfur move.

What the logs must show (client, the anti-smear evidence):
- `kerfur-prop-adopt: bound ... poll #N, M ms after arm` — the K-6 collapse case needs **poll #1**
  on every join-path adoption (or no arm at all: exact-key bind won). Any poll #>1 = the wait
  branch is still load-bearing, keep it.
- `npc-adopt: bound ... poll #N ...` — same reading.
- ZERO `TRIPWIRE -- last-resort timeout fired BEFORE load-tail quiescence` lines (a hit = the
  probe/sweep chain wedged — report immediately).
- `[KERFUR CENSUS][CLIENT] TOTAL ...` — 0 UNTRACKED + 0 UNCLAIMED, and TOTAL equal to the host's.
- RE-BIND ledger continues: count `keyed churn RE-BIND` hits (take-6: 0).

## RESULT — TAKE 7 (2026-07-12 13:58, user hands-on + log forensics)

USER: 6 kerfurs / 4 active on BOTH peers — counts match. Adoption evidence (the measurement this
run existed for): 2 NPC adoptions bound **poll #1** (81/93 ms); 2 NPC + 1 prop kerfur had NO local
twin (not in the blob) → fresh-spawned at poll #12 when the sweep latch fired — correct per design.
RE-BIND ledger: 0 again. TRIPWIRE: 0. **K-6 collapse verdict: NOT collapsible — the fresh-spawn
fallback fired on a real join (twins genuinely absent), so the wait-then-fresh-spawn branch is
load-bearing. The K-6/npc_adoption join-wait machinery STAYS. The anti-smear candidate list for
adoption is CLOSED (keep).**

NEW BUG found by the user (floating CCTVs) → root-caused + fixed, see TAKE 8.

## TAKE 8 — the CHILD-ACTOR EXCLUSION (floating-CCTV root fix, per rule 1)

Deployed DLL: **`DE4B438A`** (md5 first 8), all 4 installs hash-verified. Audit: 1 CRITICAL found
(the steady re-seed incremental-express lane bypassed the first 4 gates AND the gated destroy seam
would have made its orphans permanent) → closed same session at SeedWalk_ + BuildPropSpawnPayload_
(now 6 surfaces, one predicate); all other audit dimensions CONFIRMED sound.

THE BUG (take-7, user: «камеры внутри 3 керфуров ... в воздухе cctv у них троих в груди»):
kerfur eye cameras are `prop_camera_good_C` — keyed Aprop-lineage CHILD ACTORS (UChildActorComponent
`cam` on both kerfur forms). Our prop identity layer treated them as independent world props:
- HOST enrolled + broadcast them (`name='cam_b_1'` PropSpawns, waves at every kerfur toggle) →
  the joiner materialized STANDALONE floating camera mirrors at each kerfur's chest (eid
  9558/9567/9569 = exactly the 3 visible CCTVs).
- The JOINER's own eye cams (keys are per-peer random — kerfur loadData never restores Key) were
  unclaimed at the sweep → DOOMED (take-7 log: `doomed 2 x 'prop_camera_good_C'`).
- Gap-I-1 30 cm fuzzy sometimes STOLE a live eye cam under a wire key (eid 9568/9575/9576/9580).

THE FIX (mirrors the game's own rule, `Aprop_C::ignoreSave = ignoreSav || IsChildActor()`,
prop_base bytecode): ChildActorComponent-owned actors are excluded from the independent prop
identity universe. ONE predicate `ue_wrap::engine::IsChildActor` (raw reflected
AActor.ParentComponent weak-ptr read, off-GT-safe), consulted at: MarkPropElement (the ONE enroll
owner → element table, key index, R2, snapshot, reaper, sweep universe all covered), the Init-POST
broadcast catch, the destroy seam, and the FindNearbySameClass fuzzy candidate walk.

Test steps (kerfur save, both peers):
1. Client joins → NO floating CCTVs anywhere; kerfur counts still match (6/6, 4/4).
2. Client log: ZERO `remote_prop::OnSpawn: cls='prop_camera_good_C' name='cam_b_1'` lines and
   ZERO `doomed N x 'prop_camera_good_C'` in the sweep. Expected instead (host+client):
   `child-actor enroll REFUSED cls='prop_camera_good_C'` / `skip (parent-owned sub-actor...)`.
3. Host toggles a kerfur off + on while the client watches → no camera appears/orphans on either
   peer; the kerfur morphs correctly both ways.
4. Regression: a USER-PLACED standalone camera (prop_camera_s/cursed/good bought+placed) still
   syncs both ways (place, move, stick) — standalone cameras are NOT child actors and keep their
   wire identity (take-7 log showed cursed+s cameras syncing; that must not regress).
5. fps sanity + the take-6 rock repro still green (barrier untouched).

## RESULT — TAKE 8 (2026-07-12 14:43): cameras FIXED, new TOGGLE-DUPE found + root-caused

USER: «камер нету» — the child-actor exclusion is hands-on VERIFIED. New bug: kerfur toggles
dupe (old kerfur stays + off-prop appears). Log forensics (scratchpad take8_{host,client}.log):
five toggles 14:43:07-14 — per the pos-diag trail the presses were made BY THE HOST PLAYER
walking the kerfur line (the client puppet stood parked at (1228,-457) throughout; user's
"клиент выключает" read differently — report if I misread which window you drove). Host log,
five for five: fresh off-prop expressed GENERICALLY by the spawn-seam drain
(`Init POST: HOST broadcasting SPAWN prop_kerfurOmega_C` + `spawn-seam adopted eid=N`) →
kerfur poll: `POLL turn_off (died invisibly)` → `turn_off converge -- no new kerfur prop near
... releasing dead NPC (no broadcast)`. NO KerfurConvert, NO EntityDestroy → the CLIENT kept
all five NPC mirrors AND materialized five generic prop mirrors = the dupe (client census
14:43:09+ shows both forms).

ROOT (architectural, per rule 1 + user green light): the poll converge's premise "the verb's
fresh prop is UNTRACKED" died with spawn_authority Inc-1 (2026-07-10) — the per-tick
FinishSpawningActor seam drain always out-races the 5 Hz poll. The census/incremental lane
already had the kerfur OWNER BOUNDARY (IsKerfurActor skip + ExpressIncrementalKerfurOffProp);
the drain lane was added WITHOUT it — the missed-lane smear class again.

## TAKE 9 — KERFUR FIRST-REFUSAL (event-driven converge)

Deployed DLL: **`3E023931`** (md5 first 8; == `BC244011` + a comment-only doc pass), all 4
installs hash-verified. Audit: 1 CRITICAL
found + fixed same session (the first cut lacked the untracked-only guard — via the payload
builder site, which is Registry-fed = every candidate tracked, it could only ever STEAL a
standing off-prop's identity during a join-that-races-a-toggle; now it mirrors
FindNewFormKerfurActor's tracked-skip and the builder site is structurally inert until a future
lane feeds an untracked kerfur prop). All other audit dimensions CONFIRMED (no double-converge,
turn-on untouched, solo-host poll path preserved, re-entrancy clean, gate ordering cheap-first).

THE FIX: `kerfur_convert::TryAdoptFreshKerfurProp` — every generic express lane offers a fresh
kerfur PROP-form actor to the kerfur layer FIRST (consults: the shared express body in
prop_lifecycle [Init-POST + the seam drain] + BuildPropSpawnPayload_ [the one payload builder]).
A dead un-handled kerfur NPC watch within 5 m ⇒ this IS the conversion product: converge NOW
(reuse-or-mint eid silently, release the dead NPC, BindFormActor → KerfurConvert, floppies) —
event-driven at the spawn edge instead of the losing 5 Hz poll (which stays as the solo-host /
seam-missed backstop). No dead kerfur nearby ⇒ ordinary kerfur prop (hand-place, purchase) keeps
the generic same-tick path.

Test steps (kerfur save, both peers watching):
1. HOST toggles a kerfur OFF → on BOTH peers: the NPC disappears, exactly one off-prop appears
   (no dupe). Host log: `FIRST-REFUSAL turn_off converge ... KerfurConvert broadcast, NO generic
   PropSpawn`; NO `no new kerfur prop near` lines; client log: `applied KerfurConvert`.
2. HOST toggles it back ON → single NPC both peers (turn-on path unchanged — verify no regress).
3. CLIENT toggles a kerfur OFF and ON (the request path, take-7-proven) → still clean both peers.
4. Hand-place regression: host hold-R picks a kerfur off-prop into the hand and places it
   elsewhere → the placed prop appears on the client at the new spot (ordinary-spawn first-refusal
   returns false → generic express — must still work same-tick).
5. Camera regression stays green (no CCTVs — take-8 verified, must not regress).

### RESULT (take-9, user hands-on 2026-07-13 11:51): «наполовину хорошо» — turn_OFF green, TWO turn-ON bugs

What worked: all four client turn_offs converged via the request path (BindFormActor x4,
KerfurConvert applied on the client each time); cameras stayed gone; hand-place not regressed.
NOTE: the runbook's step-1 host turn_offs happened DURING the client's join window on
still-unenrolled NPCs (the kerfur_entity reserve wave came 8 s later) → they expressed as
generic keyed props (9931/9932) — no dupe, but the take-8 first-refusal itself is STILL not
live-verified on a tracked NPC.

BUG 1 (kerfur DELETED on both peers): client turned prop 9932 ON → the local verb destroyed the
mirror actor → the CLIENT DESTROY SEAM relayed a keyed DESTROY (eid=0, host key readable off the
actor) → the host destroyed its AUTHORITATIVE prop → the turn-on request then found "no live
prop -- dropped" → the client's parked ghost reaped after 4.2 s → gone everywhere. Eid 9931 sent
the SAME relay but the request won the race — pure lottery. ROOT: the kerfur conversion axis has
ONE owner (kerfur_convert), but the destroy chokepoint never offered it first refusal — the
take-8 lesson's exact class, 3rd instance, now on the DESTROY edge.

BUG 2 (host turn-on → client keeps a stale off prop): the host's OWN turn-on has structurally NO
converge: the destroy seam drains the prop Element synchronously with the actor death, so the
poll's "element present + actor dead" premise NEVER holds for a host prop (zero host "POLL
turn-on" lines in ANY session log — it was always dead). The generic PropDestroy(eid=9950,
key='BvshDeV6…') went out instead; the client drained the row by eid but resolved the ACTOR by
KEY — the ghost-adopted mirror carries a local key ('Y25kr…', row key 'coopkerfur#9950') → no
match → the actor survived as `*** UNCLAIMED (no host mirror -- stale local off-prop) ***`
(census-proven) and no NPC ever arrived.

## TAKE 10 — KERFUR FIRST-REFUSAL, DESTROY EDGE (the turn-on twin) + eid-first OnDestroy

Deployed DLL: **`12204C8F`** (sha256 first 8), all 4 installs hash-verified. Audit: 2 agents ran
pre-handoff; perf PASS (reject path pointer-cheap, no hot-path walk — the final cut walks only
the fresh-stamp list, not GUObjectArray); correctness found 3 findings (conf 80-82), ALL FIXED
before this deploy: (1+2) "unowned within 5 m" alone would let an enrollment-gap wanderer NPC
suppress a legit relay / get identity-welded — closed by the FRESH-SPAWN STAMP (below); (3) the
seam-converged slot could go stale on host-own toggles — closed by the request-verb bracket.

THE FIX (four parts, per rule 1 — completing the kerfur owner boundary):
1. `kerfur_convert::TryCaptureKerfurPropDestroy` — the destroy seam offers every dying KEYED
   kerfur PROP to the kerfur layer FIRST. spawnKerfuro is kismet-proven SPAWN-then-DESTROY, so
   the conversion-product NPC exists ZERO ticks old at the seam — a synchronous premise no
   periodic claimer can beat. Fresh STAMPED unowned kerfur NPC within 5 m ⇒ conversion churn:
   CLIENT suppresses the keyed-destroy relay (bug 1; the poll stays the request driver);
   HOST converges INLINE at the destroy edge (RegisterHostNpcSilent → BindFormActor →
   ONE KerfurConvert, no generic PropDestroy — bug 2's converge). No stamped NPC nearby ⇒
   genuine destroy (hold-R pickup, incinerator) keeps the generic relay.
2. FRESH-SPAWN STAMPS (`NoteFreshKerfurNpcSpawn`): the FinishSpawningActor Func seam
   (host_spawn_watcher, BOTH roles) stamps every freshly-finished kerfur NPC (2 s expiry).
   The stamp is the load-bearing freshness discriminator — a save-loaded / long-lived
   "unowned" kerfur is NEVER stamped, so proximity+untracked alone can no longer misfire.
3. OnConvertRequest turn-on brackets its verb (`g_requestVerbEid`) and consumes
   `g_seamConvergedEid` (recorded ONLY inside the bracket — no double converge, no stale slot,
   reject echo preserved).
4. `remote_prop::OnDestroy` resolves the doomed actor EID-FIRST (MTA Packet_EntityRemove shape;
   key = join-bootstrap fallback) — a synthetic-key kerfur mirror now dies correctly (bug 2's
   client half), and the drained row can no longer outlive its actor.

Test steps (kerfur save, both peers, LET THE JOIN SETTLE ~30 s before touching kerfurs so the
enroll wave lands — separates the fix's axis from the pre-enroll window):
1. CLIENT turns a kerfur prop ON → single NPC on both peers, NOTHING deleted. Client log:
   `CLIENT destroy-edge first refusal ... relay SUPPRESSED`; host log: `HOST executing turn-on`
   + `already converged at the destroy edge` OR a normal converge — and ZERO
   `remote_prop::OnDestroy ... -> destroying local actor` for a kerfur key on the host.
2. HOST turns a kerfur prop ON → NPC appears on the CLIENT too (old off-prop gone, no UNCLAIMED
   census line). Host log: `FIRST-REFUSAL turn-on converge ... NO generic PropDestroy`.
3. HOST toggles a TRACKED kerfur OFF (post-enroll — wait for the census to list it as NPC) →
   single off-prop both peers; host log `FIRST-REFUSAL turn_off converge` (take-8's fix, still
   never live-verified on a tracked NPC).
4. CLIENT toggles OFF/ON once more (request path regression) → clean both peers.
5. Hand-place regression: hold-R a kerfur off-prop into the hand, place elsewhere → appears on
   the other peer at the new spot (no NPC nearby ⇒ capture declines ⇒ generic path intact).
6. Cameras stay gone (take-8, must not regress).
