# research/findings — point-in-time RE + design log (read this first)

This folder is the project's **append-only, point-in-time** reverse-engineering and design log
(per `docs/ARCHITECTURE.md`: living docs stay current in `docs/`; the dated history lives here). ~150
files. **It is NOT a description of the current state** — each file is a snapshot from its date.

## How to read it (so you don't mistake an old doc for the truth)

1. **For the CURRENT cross-cutting truth, start in `docs/`, NOT here:**
   - [../../docs/COOP_ENTITY_EXPRESSION_MAP.md](../../docs/COOP_ENTITY_EXPRESSION_MAP.md) — how every
     synced entity gets identity/expression/destroy (code-verified, confidence-tagged).
   - [../../docs/COOP_DISPATCH_VISIBILITY.md](../../docs/COOP_DISPATCH_VISIBILITY.md) — will my hook fire?
     (VISIBLE vs INVISIBLE dispatch). **These two supersede the cross-cutting parts of the RE docs here.**
   - [../../docs/ARCHITECTURE.md](../../docs/ARCHITECTURE.md), [COOP_SCOPE](../../docs/COOP_SCOPE.md),
     [ROADMAP](../../docs/ROADMAP.md), and the auto-memory (`MEMORY.md` index) for the running state.
2. **`*-RE-*` docs are DURABLE** — bytecode/struct/dispatch facts that stay true until the GAME updates
   (e.g. `votv-pile-grab-observable-hook-RE`, `votv-clump-pile-dupe-DECISIVE-RE` — both cited by the
   COOP_* maps as the evidence base). Trust them, but still verify the offset/field against the current
   CXXHeaderDump (WP18: memory decays, the dump is authority).
3. **`*-DESIGN-*` / `*-PLAN-*` / `*-roadmap-*` docs are POINT-IN-TIME** — the design rationale for a
   feature as of its date. Most describe SHIPPED features (the **code is the as-built truth**, not the
   design doc). Some describe APPROACHES THAT WERE ABANDONED — those live in `_archive/`.
4. **`*-AUDIT-*` / `*-RCA-*` docs** are post-mortems of a specific bug/state; the bug is usually long-fixed.

## `_archive/` — definitively superseded / abandoned approaches
Moved out of the active log so they can't be mistaken for a current plan (see `_archive/README.md`). As
of 2026-06-20: the failed pile save-strip + thin-client-sync approaches. **The CURRENT pile/trash design
is [docs/piles/08-HOST-AUTH-TRASH-CHANNEL.md](../../docs/piles/08-HOST-AUTH-TRASH-CHANNEL.md)** — the
morph (07) was retired 2026-06-21 (its smoke "VERIFIED" was refuted by a real hands-on); the
`*-clump-*`/`*-pile-*` RE docs here are durable bytecode facts, but their design conclusions defer to 08
+ the COOP_* maps. **Correction (2026-06-21):** the `...-pass2` RE's "BeginDeferred (from the chipPile/clump
ubergraph) is `EX_CallMath` → unobservable" is **TRUE** (verified by a live hands-on: 0 host_spawn_watcher
fires, commit `0e56ca39`); it was the s35/**08** "observability reversal" (claiming that POST is observable)
that was FALSE — now corrected in 08 + the COOP_* maps. New durable RE:
`votv-chippile-dispatch-and-thunk-hook-RE-2026-06-21.md`. **Correction (2026-06-22):** the CARRY mirror's
**FREEZE is now FIXED, hands-on VERIFIED** (the `!carrying` release-edge gate in `local_streams.cpp` — the
client clump updates through the carry). The `*-staleness-*-2026-06-21` finding's "carry MIRRORS on a settled
join / it was the JOIN RACE" was FALSE, and so was the interim "contact-re-pile churn" diagnosis: the actual
release-edge cause was `updateHold` PUPPET RECREATION (the `heldActor` ptr changing with `pendingSettle=0`),
NOT a re-pile. Option 2 (the `holdPlayer` convert/ctx gate) is **DISPROVEN by bytecode** — `holdPlayer` is set
once on grab and never cleared in any BP. **Update (2026-06-22, HEAD `29069f05`, deployed `c2a5f49cc98add31`):**
carry **JANK FIXED [V]** (fixed-delay snapshot interp, `df158728` — the keyless-pose theory was DISPROVEN, the
real root was an interp phase-stall); proxy **SCALE BUILT v83** (`df158728`, hands-on PENDING); the
throw-velocity **VERB-FLIP is DEAD** (both `simulateDrop` AND `dropGrabObject` Func-thunks fired ZERO) →
REPLACED by carry/flight stream-**continuity** (`136ed779`, hands-on PENDING — the arc); the **ORPHAN dup SPLIT**
— derived gone [V], ORIGINAL (level-placed) piles dup, root CONFIRMED (the client's native level-pile coexists
with the proxy; the eid-race theory superseded), a read-only PILE-PROBE shipped (`29069f05`), DESTROY-fix NEXT.
The canonical carry root + all open-item fixes: `votv-chippile-carry-churn-holdplayer-gate-2026-06-22.md`.
**Update (2026-06-23, HEAD `54a3a332`):** a real hands-on of the v85 chain found 5 layers.
**L3 carry-JITTER + L4 wild-THROW FIXED [V hands-on + V harness]** (`92a76f27` — the carry shook because the
clump SIMULATED physics vs our teleport, fix kinematic-on-grab; E flung it from a fixed 871cm/s, fix inherit
the hand velocity). **L5 FPS PARKED** — 4 walk passes, the incremental tail-scan KILLED the walks
(`[WALK-TIME]` 69->12) but the ~3-4s hitch HELD -> walks were never the root (likely GC, cosmetic). **L1
orphan: adopt DEAD (3 gates), the "12/871 destroys" was a LOG artifact (fired ~801), real bug = ~70
host-drift orphans, fix designed (reconcile after `HasLoadTailQuiesced`).** **L2 RE done** (mod-drive the
native `ui_UI_C` window; suppress `PlaySound2D(use_deny)`; eye-anchor). Full RE + the L5 lesson:
`votv-pile-L1orphan-L2window-L5fps-reckoning-2026-06-23.md`.
**Update (2026-06-23 LATEST, decisive hands-on) — L1 ROOT REVERSED: it is a JOIN-WINDOW two-channel DUP,
NOT a join orphan.** A controlled hands-on (kerfur control clean, pile dups) proved a chipPile moved
DURING the join-load window dups on the client (scratch-save native @old + broadcast-convert proxy @new,
>1cm → the 1cm dedup can't match). Pre-connect divergence does NOT exist (fresh scratch save at connect
captures it) → every census=0 was correct → the "orphan census + absence-removal at the JOIN" line is
CANCELLED. Dup is CLIENT-LOCAL (host correct) + persistent. **FIX BUILT (NOT hold-broadcast/dedup-by-eid —
both died on the gate checks): match the save-loaded native by the pile's SAVE-TIME position, reconciled at
the POST-QUIESCENCE sweep (take-1 proved the key is bit-for-bit correct but the world-ready twin-destroy runs
before the native async-loads = a load-tail timing race). Commits `4c286cae`+`124fbc9d`, audit GO, deployed
`F9B6589E1F62955F` v86, HANDS-ON take-2 PENDING, push HELD.** Canonical (full as-built + take-1 timing RE):
**`votv-pile-dup-join-window-two-channel-RE-2026-06-23.md`**. (Pile fix later reached take-4 self-seed,
committed `5b01bc0e`, fix#1 auto-verified P 0->870; end-to-end move-in-window hands-on still pending.)

**Update (2026-06-23 — L5 persistent ~2s FPS-hitch ROOT PROVEN by data; fix NOT verified yet):** the
all-game both-peers stutter = `interactable_sync`'s 6 channels each FULL-walking the ~237k GUObjectArray
every 2s (`Channel::RebuildIndex`, `kRetryRebuildThrottle=2s`). GC hypothesis refuted twice with data
([HITCH]/[HITCH-SRC] + perf_probe buckets + per-sync ScopedWalkTimer). Fix = migrate to
`coop::util::IncrementalObjectScan`; ATTEMPTED TWICE, BOTH FAIL the N-match gate (doors stream progressively
into recycled slots -> tail-scan misses them: take-1 door 57->19, take-2 stream-settle door 57->50). NOT
verified, working-tree only, deployed `FD9D2DC...`. device_occupancy (80x30ms) is the co-dominant 2nd
culprit. Design fork pending. Canonical: **`votv-L5-fps-hitch-interactable-fullwalk-RE-2026-06-23.md`**.

**Update (2026-06-22 FINAL, HEAD `a5282f57`, deployed `015F0AC9590B6B23`, proto v83 — all committed, push held):**
the throw-arc/probe state above is now the SHIPPED+VERIFIED final state. **Pile-landing ROTATION + throw SOUND
both FIXED [V hands-on take-30]** — rotation re-read from the SETTLED pile at the land-settle COMMIT
(`trash_channel.cpp:248`); the pickup-sound-on-throw KILLED (the flight branch streams the carry key without a
re-StartDrive; a trash eid sends no PropRelease; `OnHostRelease` RETIRED, RULE 2). The **throw ARC flies** —
VERIFIED [V hands-on take-29 + harness] (the user: "дуга ЛЕТИТ"; the autotest does a real directional throw).
The **Z/HEIGHT regressed in take-31** (the pile transform read from `newActor` at the BeginDeferred POST was
`(0,0,0)` — unpositioned pre-FinishSpawning → derived piles snapped to world origin); the **take-32 FIX re-reads
the pile's REAL transform at the land-settle COMMIT** (`trash_channel.cpp:248-256`) — drift=0cm [V harness] (the
native-destroy was INNOCENT, harness-confirmed; the bug was the `(0,0,0)` loc). The **LEVEL-PILE DUP DESTROY is
BUILT + VERIFIED [V harness]** (`remote_prop_spawn.cpp:387-410` destroys the co-located native at a pile
proxy-spawn — exact ~1cm + chipType + IsLive, exact-or-skip on >1, gated on the join bracket only; harness 12
twins / 0 SKIP) — this REPLACES the read-only PILE-PROBE (RULE 2, retired). A **FPS stutter fix** also shipped
[V harness] (`net_pump.cpp:559` guards the steady-world re-seed on the GUObjectArray high-water mark + a ~20s
census so the ~237k walk is skipped at rest; re-seed rate 0.073/s, was ~0.25/s). **Proxy SCALE** is AS-BUILT
(re-read `GetActorScale3D` at the same COMMIT). The verification standard this session became the **autonomous
log-truth harness** (`tools/pile-test-assert.ps1`, 13 invariants, VERDICT PASS) driven by the scripted
`autotest_chippile.cpp` — [V harness] means the harness asserted it against real autonomous-run logs (NOT a
human hands-on). Still OPEN: the WHOOSH throw sound (no ReliableKind exists; user-deprioritized, best confirmed
by hearing), and the dead `dropGrabObject` read-only thunk (to be retired, RULE 2). Canonical finding:
`votv-chippile-carry-churn-holdplayer-gate-2026-06-22.md`; the harness: [[reference-pile-test-harness]].

## 2026-06-22 — chipPile CLIENT-grab direction (Increment 2) HOST-SIDE, BUILT + [V harness]

After the carry arc, the CLIENT-grab direction's **host-side** was de-risked then built + harness-verified.
Two new findings: **`votv-puppet-grab-feasibility-RE-2026-06-22.md`** (the gate — RE + a runtime probe
proved `playerGrabbed` ENGAGES + HOLDS on an unpossessed puppet with NO controller dependency, but the puppet
tick does NOT drive the PHC, so the host must kinematically drive the held-clump pose) and
**`votv-increment2-clientgrab-host-side-DESIGN-2026-06-22.md`** (the build blueprint: proto v84 `GrabIntent`
wire + 3-place router + `trash_channel::OnGrabIntent` + the new `coop/puppet_carry_drive`). Shipped commits
`32ccd1bc`(gate) · `81e8e687`(host-side) · `2dc5d06e`(audit MEDIUM-1: hold lifetime tied to clump liveness).
Verified on the log-truth harness (synthetic `VOTVCOOP_RUN_GRAB_INTENT_TEST`): VERDICT PASS; carry regression
PASS; audit GO. HEAD `c6473d49`, proto v84. **SUPERSEDED 2026-06-23 by the full chain (below) — the host-side
DESIGN doc is now historical; the `host-side-DESIGN` finding carries a superseded banner.**

## 2026-06-23 — chipPile CLIENT-grab FULL CHAIN, AS-BUILT + [V harness] (v85)

The complete client-initiated chain shipped: a client AIMS at a mirrored pile, GRABS, CARRIES, THROWS, it
re-piles. **`votv-increment2-clientgrab-FULL-CHAIN-AS-BUILT-2026-06-23.md`** is the canonical as-built record
(supersedes the host-side DESIGN doc). Two gating discoveries: (1) a bare AStaticMeshActor proxy can NEVER be
`lookAtActor` (the `int_player_C` filter at LookAtFunction @3250) AND the worn pile mesh has no simple
collision body (harness `trace-gate hit=0`) → the trace/`lookatActorCurrent`/proxy-collision approach is
RETIRED (RULE 2); recognition is a **camera-ray cone**. (2) A client only drives pose slot 0 + the relay
can't echo to the grabber → the carry pose MUST be host-ORIGINATED (the new per-eid `MsgType::TrashCarryPose`
batch, the `StoreRemoteWorldActorBatch` pattern). The interp was extracted to `coop/active_drive.h`
(remote_prop 1375→1222). Harness VERDICT PASS 0-CRITICAL via the REAL E-press path (camera-cone recognition,
7 PUBLISH→9 APPLY, throw→re-pile drift 0cm) + carry-regression PASS. Audit HIGH fixed (clump-lost-mid-carry →
`ReleaseClientHold` broadcasts `PropDestroy`). **NEXT (greenlight):** a `garbageCollider`-analog SHAPE
component on the proxy (occlusion-correct aim + movement-block — the cone ignores walls, the proxy is
walk-through). HEAD `29353191`, deployed `BB94A120A969A51E`, proto v85, committed, push held.

## 2026-07-09 (late) — client prop grab/drop RE-ROOTED to a host-auth INTENT lane + F1 join-window reconcile

**`votv-keyed-prop-grabdrop-intent-lane-DESIGN-2026-07-09.md`** — the client-places-a-prop-invisible-on-host
bug (F2) after a full `/qf 15`: the seam-catching fixes (v2 FinishSpawn author, v3 destroy-suppress, v4
exclude-hand) are ALL crutches on a wrong layer (a MISSING SINGLE OWNER) and were REVERTED to a clean
baseline (they duped on grab). Proper fix = extend the EXISTING host-authoritative `GrabIntent=78` lane
(chipPiles use it) to keyed world props: client sends grab/drop INTENT, host runs the native op on its own
prop. Full RE of the GrabIntent template + native grab/drop verbs (all `mainPlayer_C`, puppet-callable).
Button map: `[[reference-votv-prop-interaction-buttons]]` (E=physics-grab; hold-R=pick into hand/place; R
tap=hotbar). SCOPE: Inc-1 = a clean host-auth DROP intent (the grab-destroy already crosses). Clean baseline
deployed `13a372a3`, NOT built. Supersedes `votv-destroy-seam-hostwipe-and-rock-rdrop-RE-2026-07-08.md` Bug B.

**`votv-F1-host-moved-prop-join-window-DESIGN-2026-07-09.md`** — a keyed prop the host MOVED during a
client's join window shows at the SAVE pos on the joiner (loadObjects clobbers the snapshot pos; the keyed
RE-BIND migrates identity WITHOUT position). `/qf` R1-R5 CONVERGED on the direction (generalize the b3 pile
reconcile into ONE host-authoritative live-pos reconcile for keyed props + piles) but GATED it behind a
read-only probe (MEASURE the root first). The user's grab-during-window edge case exposed a LATENT bug in
the SHIPPED pile lane: `ApplyPendingPosCorrections` re-validates only liveness at apply-time, so a grab
between arm and apply snaps a carried clump to a ghost pos. DESIGN PROVISIONAL, probe not yet built.

## 2026-07-09 — world-rules / game-settings RE + coop verdict + F1 panel AS-BUILT

**`votv-gamerules-settings-RE-2026-07-09.md`** — world rules = `Fstruct_gameRules` (~36 fields: fall
damage, difficulty, funny, custom content, seasons, minigames, decay...); the runtime authority is the
per-peer `mainGameInstance.gameRules`, and a joining client boots from the host's live-captured save so
the host's rules populate the client's copy → **host-authoritative for free, NO gameplay build** (same
root as `docs/COOP_WORLD_PROP_DIVERGENCE.md`; a broadcast was rejected as a divergence-mask). **G1
CONFIRMED** on a 2-peer smoke (client == host, 36 rules, matching log) — caveat: both story-default, so
a non-default rule is the fuller human discriminator. **AS-BUILT**: F1 > World > Rules read-only panel
for everyone (`ue_wrap/game_rules` + `ui/world_rules_panel` + `EnumerateStructFields` + dev_menu tab),
commit `27994f09`, DLL `bee181a6`, pushed. Category B (graphics/audio/sensitivity) = separate object,
never synced. customContent asset-availability = open product question.

## Note on duplication
The pile/trash/clump/snapshot/save-transfer RE docs are ALSO copied verbatim under
`docs/piles/findings/` (the consolidated pile knowledge base). The originals here are the canonical
copies; `docs/piles/` is the curated subset.

> Sweep note (2026-06-20): this index was added during the "fix ourselves" pass. A full per-file
> staleness audit of all ~150 point-in-time docs was NOT done (most are durable RE or shipped-feature
> design — not misleading); only the definitively-dead approaches were archived. If a specific topic's
> docs look contradictory, the `docs/` canonical doc + the code win.
