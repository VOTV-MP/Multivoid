# Hands-on runbook 2026-07-08 — two CLEAN isolation repros (host-wipe + rock)

**Deployed: DLL `753bb549` (rock `[ROCK-DROP]` diagnostics) on all 4 peers. HEAD `8cae8597`.**
Folders renamed this session: HOST=`Game_0.9.0n_HOST`, clients=`Game_0.9.0n_CLIENT_1/2/3`.
Full RE: `research/findings/votv-destroy-seam-hostwipe-and-rock-rdrop-RE-2026-07-08.md`.

Both bugs were seen in ONE braided log (join + pile-throw + no rock). These two repros ISOLATE each.
**Piles are ALREADY FIXED — do NOT throw piles in either repro; it only re-confounds the log.**

## Repro 1 — HOST-WIPE from a BARE join — FIXED + USER-VERIFIED 2026-07-08
**RESULT (11:54 bare-join): host wiped on a bare join, zero player action.** `grab/throw`=0 both peers; CLIENT
broadcast 3140 DESTROY (2270 keyed + 871 pile) 11:54:35-39 -> HOST 3345->1255 keyed props, never recovered.
**RESULT (15:43-15:45 `[HOSTWIPE-CALLER]` probe, DLL `f2fda78`): caller = loadObjects, 2270/2270 =
`mainGamemode_C`**; two legit post-join destroys = SELF-caller (`prop_foodBox_C` food morph, `trashBitsPile_C`
trash-E). => root closed: v106 broadcasts the client's loadObjects world-load churn.
**FIX SHIPPED (v107, `3180c4ab`, DLL `04ebfdb0`) + USER-VERIFIED:** source-anchored CLIENT-scoped world-load
EPISODE LATCH (`coop::world_load_episode`) — arm at the client join boot (CAUSAL), clear at load-tail
quiescence, suppress OUTBOUND KEYED destroy broadcasts while in-episode. User confirmed the host world survives
a bare join. `[HOSTWIPE-CALLER]` probe retired. Steps below kept as a REGRESSION check (re-run to confirm the
host props survive a bare join). Log markers: client `world_load_episode: ARMED` -> N `CLIENT suppressed KEYED
DESTROY ... inside world-load episode` -> `world_load_episode: CLOSED at load-tail quiescence`; host re-seed
stays high (no 3345->1255 drop).

Goal: prove the host loses its keyed props on a PLAIN join (no rock, no pile-throwing) — isolates the
join-purge destroy-seam leak from the pile-throw stress.
1. Host launches (`mp_host_game.bat`), reaches gameplay with its normal world of props.
2. Note the host's props are present.
3. Client joins (`mp_client_connect.bat`). Do NOTHING else — no grabbing, no throwing, no rocks.
4. After the join settles (~15-20 s), look at the HOST world: did its props vanish?
5. Quit both.

Read: HOST `Game_0.9.0n_HOST/.../votv-coop.log` — count `remote_prop::OnDestroy ... eid=0 -> destroying
local actor` in the join window; CLIENT `Game_0.9.0n_CLIENT_1/.../votv-coop.log` — count
`grab_hook[destroy-seam]: CLIENT broadcasting DESTROY ... eid=0`. If the host wipes on a BARE join → the
leak fires without any manual action → root analysis proceeds (classify-the-churn fix). If it does NOT →
the pile-throw-during-join is a trigger, and the RE narrows to that interaction.

## Repro 2 — ROCK R-drop invisibility (clean, no join, no piles)
Goal: capture the rock's single destroy/spawn in a log with no join-sweep churn.
1. Host + client already connected and SETTLED (join window long over — do this minutes after joining).
2. No pile-throwing at any point.
3. Client: look at ONE pre-existing rock → press **R** (pick into hand) → press **R** again (place) →
   look at the HOST: is the rock there? → press **E** (grab) → check host again → quit + reload.

Read (CLIENT log): on R-drop expect `[ROCK-DROP] CLIENT Aprop spawn NOT authored (host-auth skip): ...
key=... eid=...` (the smoking gun); on E-grab either `net: NEW held actor ... -> BROADCAST` (E authored)
or `[ROCK-DROP] EnsureHeldItemBroadcast DECLINE (tracker-known)`. HOST log: a `CLIENT broadcasting DESTROY
eid=X` (pickup) → `OnDestroy ... eid=X -> destroying local actor` (host loses its copy), then NO spawn on
the drop.

## After the repros
- **HOST-WIPE: FIXED + USER-VERIFIED** (v107 world-load episode latch, `3180c4ab`, DLL `04ebfdb0`). Separate
  from the rock (a hold-R pickup fires OUTSIDE the load episode). See the finding's FIX AS-BUILT section.
- **ROCK (Bug B): design = MINIMAL-A, per-rule-1 green-lit 2026-07-08, NOT built (deferred to tomorrow).** The
  `/qf` thread ran to **24 rounds** (full transcript `<scratchpad>/qf_thread.md`). It first converged on a
  HOST-AUTHORITATIVE "B-lite" (client eid-park + host HIDES its copy during the hold), then the user asked
  **"is hiding a crutch?"** → **YES** → reversed to **MINIMAL-A**: the pickup is already correct (destroys the
  rock into inventory on all peers, faithful to SP), **fix ONLY the drop** = author the fresh drop `Aprop_C` via
  a **plain `SendPropSpawn`** (piggyback `host_spawn_watcher`'s FinishSpawn seam + defer ≥1 tick so `loadData`
  restored the Key; author iff the Key is in a client PARK SET recorded at the pickup destroy seam). eid churns
  (faithful; identity = the Key). No host changes, no new ReliableKind. See the finding's "Fix shape — SETTLED =
  MINIMAL-A". Measured gates GREEN: **[H3]** FFrame::Node CLEAN (pickup = `mainPlayer::"Hold Object"`), **[H1]**
  Key PRESERVED, **[H4]** drop detection = FinishSpawn Func-patch (loadData/simulateDrop EX_Local* invisible).
- **#1 GATE before wiring — BRANCH A (Repro 2 confirms it, no rebuild on `04ebfdb0`):** a SETTLED-session client
  hold-R pickup must show HOST `remote_prop::OnDestroy: ... eid=X -> destroying local actor` (the host removed
  its copy). Act AFTER the `world_load_episode: CLOSED at load-tail quiescence` log line (settled → outside the
  episode → predicted to cross). If the host KEEPS a stale copy (branch B), the pickup also needs handling.
- **Autonomous test = NONE feasible** (/qf 20-24): input-driver = over-build + UE4SS-dispatch-risk (RULE 3);
  synthetic self-test infeasible (detector reads a real UObject). The fix is **human-e2e-only**; autonomise the
  `pile-test-assert.ps1` log-assert (primary = host gains a prop_C with the recorded Key at the drop pos;
  secondary = branch A; positive control = an out-of-episode grab/throw crossing) + a regression-only smoke.
- **F1 (host moves a rock during the client join) — SEPARATE host→client, deferred.** Needs an ON-SETTLE rest
  trigger (send the host rock's rest when its body sleeps), NOT a single quiescence-edge transform diff (which
  can sample mid-motion). Not designed.
