# coords_panel extraction — ui_coordinates_C out of console_desk (2026-07-19, AS-BUILT)

The last console_atlas queue item (s23c deferred it over a "two-direction seam").
2-round /qf converged ("that holds" round 2); built + audited + smoke-verified same session.

## What moved

`ue_wrap/desk/console_desk.cpp` 928 -> **822**; NEW `ue_wrap/desk/coords_panel.{h,cpp}` (44 + 173),
namespace `ue_wrap::coords_panel`. Owns the ui_coordinates_C widget concept: class + 5 offsets +
Direction + updCursorLocations resolve (own throttled 2s ResolvePass + `g_required` fast latch),
the validated/cached instance (verbatim incl. the STOLAS template-grab comment), `DishAim`,
`ReadDishAim`, `WriteCursorOnly` (60Hz, NO-dispatch invariant), `WriteDishCommitted`.

## The seams (the reason this was deferred; resolved cleanly)

- **A (panel -> desk):** `console_desk::AtlasUiCoordsSlot()` NEW public — the desk half of the
  instance chain (desk.Widget -> atlas IsLive -> raw ui_coordinates slot, UNVALIDATED). The desk
  owns its field chain + atlas offsets; the panel does class-validate + cache + index-revalidate.
  Guard ORDER preserved (desk/atlas failure = null slot before the panel's class check).
- **B (panel -> desk):** `console_desk::CallUpdateCoordCoords()` NEW public (g_refresh[8] on the
  desk; guard order desk-null -> fn-null -> frame-valid = the pre-split inline tail exactly).
  WriteDishCommitted stays ONE semantic op (locks + both repaints) — anti-smear.
- **C (desk -> panel):** `PlayScanEffects` gates on `coords_panel::Instance()`. Cross-TU both
  directions; NO header cycle (headers self-contained, only .cpps cross-include).
- Resolution: panel publics self-resolve (`if (!g_required) ResolvePass()`); steady state = one
  bool load (CHEAPER than the desk's own unconditional throttled EnsureResolved pattern). The
  desk half of the chain resolves via console_desk::EnsureResolved — every current caller
  (desk_cursor_sync, console_state_sync, desk_diag) already gates on it (audit-verified census).

## Callers

`desk_cursor_sync.cpp` (:211/:301), `console_state_sync.cpp` (:80 static + :431/:546/:597),
`desk_diag.cpp` (:205) -> `CP::`; fresh negative-grep of the four moved names at build time = 0
residue (known-positive: the CP:: hits + the declaration caught).

## Equivalence evidence (frozen-instrument recipe, literal-diff variant)

- `coords_body_diff.py` (scratchpad): set-based stripped-line diff of the six moved HEAD regions
  (109 lines) vs the new files, with ENUMERATED accepted-delta lists (log prefixes
  console_desk->coords_panel, the seam-line substitutions, the g_required entries, fn rename).
  **Known-positive control re-run ON THIS CUT**: `--mutate` (int32_t->int64_t in one moved line)
  -> FAIL flagged; real run -> PASS. Seam lines verified present in the new console_desk.cpp.
- Build Release clean; deploy x4, SHA `325d31833ab7fe8e` (HOST + CLIENT_1/2/3 == build output).
- 120s 2p smoke PASS: `coords_panel: resolved` + `coords_panel: ui_coordinates LIVE instance
  resolved` x1 on BOTH peers (the join dish-snapshot path — the /qf-measured trigger); OLD prefix
  `console_desk: ui_coordinates` = 0 (stale-DLL negative); zero non-perf Warn/Error; client
  desk_diag read a live aim through the new path (`aim(sel=3 dir=1 view=5920,2960 ...)`).
- Audit (code-reviewer agent): 6/6 sections PASS, 0 CRIT / 0 MAJOR. One procedural FLAG:
  console_desk.cpp residual 822 > 800 — NOT single-concept (6 sub-surfaces); (RESOLVED same day:
  the comp-pane cut shipped, `f74d05dc`+`f9dfb5d5` — console_desk 740, see
  votv-comp-pane-extraction-DESIGN-2026-07-19.md) proposed next cut =
  the v70 signal-catch/download-machine surface (~150 LOC) or the comp pane (~110 LOC). Logged in
  MODULARIZATION_PLAN Ledger 3.

## Honest status

Behavior-preservation is literal-diff + smoke + audit proven; NOT hands-on (desk interaction
surfaces ride the standing take-4 runbook).
