# docs/notifications/ — VOTV on-screen TOAST / HINT / NOTIFICATION knowledge base

The living catalog + mechanism map for VOTV's corner popups (`SAVE ERROR: invalid mode`,
`Cannot be used when held`, the `SERVER "X" is down` notices, achievement toasts, subtitles).
Born 2026-07-09 for the coop feature: **suppress the misleading native toasts a CLIENT
self-generates from its diverged local sim, and MIRROR the HOST's authoritative ones.**

## The coop problem (why this exists)
Every VOTV peer (host AND client) runs its OWN `mainGamemode` + world/server sim. Toasts are
**never networked** — each machine's own gamemode/serverBox evaluates its (possibly diverged)
local state and calls `addHint` on **its own** on-screen widget. So a client whose local
server/world sim diverges from the host shows a **false** toast (e.g. "SERVER X is down" when
the host's authoritative server is fine). The fix is host-authoritative: suppress the client's
self-computed WORLD/SERVER toasts, mirror the host's real ones; leave purely-local player
toasts (`Cannot be used when held`) alone.

## Read in this order
1. **[TOAST_SYSTEM_RE.md](TOAST_SYSTEM_RE.md)** — the MECHANISM (code-verified 2026-07-09):
   the single `addHint` primitive, the `ui_hints_C` widget/queue, the **dispatch invisibility**
   (`EX_LocalVirtualFunction` → invisible to BOTH our ProcessEvent detour AND the Func-patch
   seam) and what that means for suppression, plus the secondary systems (subtitles, achievement
   popups, geiger). READ FIRST — the invisibility fact drives the whole design.
2. **[TOAST_CATALOG.md](TOAST_CATALOG.md)** — every toast the game can show: message, trigger,
   caller, category, and the **coop-relevance verdict** (suppress-on-client-&-mirror / local-only
   / ambiguous). The `SERVER/SIGNAL` family (the user's priority) is fully expanded.
3. (design + as-built land here once ratified via `/qf` — see the memory topic + the finding
   in research/findings/.)

## Status (2026-07-09)
- RE: system mechanism + catalog DONE (agent-verified vs bp_reflection + bytecode).
- Design: **`/qf`-converged**, census + scope + M1/M2 gates DONE. **INCREMENT 1 BUILT** (2026-07-09):
  `coop/interactables/serverbox_sync.{h,cpp}` — the host-authoritative server-STATE mirror (host polls +
  broadcasts `ServerState=91`; client drive-reals via raw-write `IsBroken` + reflected `check()`; client
  neutralizes its own `ticker_serverBreaker`). Agent-audited (0 CRITICAL/HIGH), LAN-smoke-clean, deployed
  (DLL `3B2762CA`). **NOT hands-on-verified** (smoke world was all-healthy). Inc-2 = forward the host break
  EDGE (email/console/alarm true notice). See `research/findings/votv-notifications-suppress-mirror-DESIGN-2026-07-09.md`.
- Confidence tags in the docs: **[V]** verified-from-code/reflection · **[RD]** from-a-dump/
  RE-doc · **[?]** needs a runtime probe (e.g. `ui_hints_C`/`ui_hint_C` internals, `enum_notifyType`
  value meanings, the exact server-down localized string — all flagged in TOAST_SYSTEM_RE.md).
