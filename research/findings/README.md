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

## Note on duplication
The pile/trash/clump/snapshot/save-transfer RE docs are ALSO copied verbatim under
`docs/piles/findings/` (the consolidated pile knowledge base). The originals here are the canonical
copies; `docs/piles/` is the curated subset.

> Sweep note (2026-06-20): this index was added during the "fix ourselves" pass. A full per-file
> staleness audit of all ~150 point-in-time docs was NOT done (most are durable RE or shipped-feature
> design — not misleading); only the definitively-dead approaches were archived. If a specific topic's
> docs look contradictory, the `docs/` canonical doc + the code win.
