# rope — tie two things together   (STATUS: RE)

The cheapest doc in the folder, because the RE found the punchline early:
**`rope_C` : `hook_C`** — the deployed rope IS a hook subclass. It overrides only
`ReceiveBeginPlay`, the PhysicsConstraint `ConstraintBroken` handler, and
`actionOptionIndex` (4 functions total). Everything else — two ends A/B, dist,
Cable, attach_a/attach_b, assign, save via attachKeys, unhook/drophook — is inherited
verbatim from the hook ([hook.md](hook.md) is the canon). [bytecode]

- **`prop_rope_C` : prop_C** — the hand ITEM. Simpler than prop_hook: NO throw path.
  LMB press (uber @15): needs a blocking hit under the crosshair → spawn `rope_C`,
  **`attach_a(hitResult)` immediately** (point-blank plant), tick on (drag phase).
  LMB release (@801): blocking hit → `attach_b(hitResult)` (resetLen=true → dist =
  |A−B|); no hit → destroy the rope. activeRope := null either way. [bytecode]
- No flight, no `playerHooked` climb phase (the player never hangs on it — B goes
  world/prop only via the press-drag-release gesture).

## Coop design

One line: **rope ships automatically with the hook sync** — same class family, the
anchored-phase half only (attach_a at press → the drag is owner-local aiming → the
attach_b commit-intent → host adopt). The class allowlist / row map gets a `rope_C`
row next to `hook_C`; no rope-specific code expected. Lesson for the shared pattern:
subclass items = ROWS, not lanes.

## Verification

2026-07-06 static RE (prop_rope uber 27 stmts full; rope_C = 4-fn diff vs hook_C).
No live probe. Ships with hook — no separate build item.
