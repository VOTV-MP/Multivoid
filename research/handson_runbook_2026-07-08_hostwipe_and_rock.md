# Hands-on runbook 2026-07-08 — two CLEAN isolation repros (host-wipe + rock)

**Deployed: DLL `753bb549` (rock `[ROCK-DROP]` diagnostics) on all 4 peers. HEAD `8cae8597`.**
Folders renamed this session: HOST=`Game_0.9.0n_HOST`, clients=`Game_0.9.0n_CLIENT_1/2/3`.
Full RE: `research/findings/votv-destroy-seam-hostwipe-and-rock-rdrop-RE-2026-07-08.md`.

Both bugs were seen in ONE braided log (join + pile-throw + no rock). These two repros ISOLATE each.
**Piles are ALREADY FIXED — do NOT throw piles in either repro; it only re-confounds the log.**

## Repro 1 — HOST-WIPE from a BARE join — DONE + ROOT ANALYSIS WRITTEN 2026-07-08
**RESULT (11:54 bare-join): host wiped on a bare join, zero player action.** `grab/throw`=0 both peers; CLIENT
broadcast 3140 DESTROY (2270 keyed + 871 pile) 11:54:35-39 -> HOST 3345->1255 keyed props, never recovered.
**RESULT (15:43-15:45 `[HOSTWIPE-CALLER]` probe, DLL `f2fda78cafe167c7`): caller = loadObjects, 2270/2270 =
`mainGamemode_C`**; two legit post-join destroys = SELF-caller (`prop_foodBox_C` food morph, `trashBitsPile_C`
trash-E). => root closed: v106 broadcasts the client's loadObjects world-load churn. **FIX = peer-symmetric
world-load EPISODE gate (DESIGN-only, NOT built)** — see the finding's ROOT ANALYSIS section. NEXT = one /qf
vetting round of the design + per-rule-1 green-light. Steps below kept for re-runs / regression checks.

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
- **HOST-WIPE: root MEASURED, fix DESIGNED (world-load episode gate), NOT built.** Repro 1 + the 15:43-15:45
  probe closed it. NEXT = one /qf vetting round of the design (esp. window-bounding) -> per-rule-1 green-light
  -> implement `g_inWorldLoad` latch -> smoke + hands-on. The gate does NOT touch the rock (R-pickup fires
  outside the load episode) — they stay SEPARATE.
- **ROCK (Bug B): still OPEN, RE-derived only, not log-captured, no fix.** Repro 2 (below) still pending.
