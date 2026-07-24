# research/findings — point-in-time RE + design log (read this first)

This folder is the project's **append-only, point-in-time** reverse-engineering and design log
(per `docs/ARCHITECTURE.md`: living docs stay current in `docs/`; the dated history lives here). ~195
files, organized into topical subfolders (2026-07-12 reorg). **It is NOT a description of the current
state** — each file is a snapshot from its date.

## How to read it (so you don't mistake an old doc for the truth)

1. **For the CURRENT cross-cutting truth, start in `docs/`, NOT here:**
   - [../../docs/COOP_ENTITY_EXPRESSION_MAP.md](../../docs/COOP_ENTITY_EXPRESSION_MAP.md) — how every
     synced entity gets identity/expression/destroy (code-verified, confidence-tagged).
   - [../../docs/COOP_DISPATCH_VISIBILITY.md](../../docs/COOP_DISPATCH_VISIBILITY.md) — will my hook fire?
     (VISIBLE vs INVISIBLE dispatch). **These two supersede the cross-cutting parts of the RE docs here.**
   - [../../docs/ARCHITECTURE.md](../../docs/ARCHITECTURE.md), [COOP_SCOPE](../../docs/COOP_SCOPE.md),
     [ROADMAP](../../docs/ROADMAP.md), and the auto-memory (`MEMORY.md` index) for the running state.
2. **`*-RE-*` docs are DURABLE** — bytecode/struct/dispatch facts that stay true until the GAME updates
   (e.g. `piles-trash/votv-pile-grab-observable-hook-RE`, `piles-trash/votv-clump-pile-dupe-DECISIVE-RE` —
   both cited by the COOP_* maps as the evidence base). Trust them, but still verify the offset/field
   against the current CXXHeaderDump (WP18: memory decays, the dump is authority).
3. **`*-DESIGN-*` / `*-PLAN-*` / `*-roadmap-*` docs are POINT-IN-TIME** — the design rationale for a
   feature as of its date. Most describe SHIPPED features (the **code is the as-built truth**, not the
   design doc). Some describe APPROACHES THAT WERE ABANDONED — those live in `_archive/`.
4. **`*-AUDIT-*` / `*-RCA-*` docs** are post-mortems of a specific bug/state; the bug is usually long-fixed.

## Folder layout (2026-07-12 reorg; filenames unchanged, only bucketed)

| Folder | What lives there |
|---|---|
| `phase0-bootstrap/` | May-21..23 substrate: proxy loader, reflection bring-up, first orphan pawn, LAN test rig |
| `mta/` | MTA:SA conceptual-precedent REs (keysync, pose interp, entity/NPC sync) + the two MTA-fidelity audits |
| `network/` | GNS integration, connectivity ladder, master server, voice chat, VoidTogether RE, MP menu/browser |
| `architecture-audits/` | codebase/architecture audits, refactor PLANs, migration roadmaps, perf RCAs (findclass bomb, L5 hitch) |
| `join-identity/` | the join-window arc: save transfer, snapshot adoption, purge/quiescence gates, eid/identity binds, the placed-prop 6-root saga RCA |
| `saves/` | SP save system RE: save path, GVAS picker enumerate/create |
| `piles-trash/` | chipPile/clump/trashBits: morphs, dispatch/thunk hooks, carry churn, dup RCAs, garbage Inc designs |
| `props-lifecycle/` | Aprop_C lifecycle, interactables catalog, destroy-seam/host-wipe, crowbar key-divergence, piramid, use-action bindings |
| `physics-grab/` | grab/throw/held-pose pipelines, client-grab chain, physics wake architecture |
| `inventory-items/` | inventory REs, drop-spawn, equip/battery, flashlight, camera stick, starter kit |
| `computers-devices/` | terminals, base computers, keypads, doors/lockers/garage, panels/screens, ticker |
| `player-puppet/` | puppet body/head/IK/sounds, vitals/death, melee damage path, remote-player bring-up |
| `kerfur/` | kerfur/kerfurOmega: headlook, convert, adoption/ghost RCAs, identity-authority redesign |
| `npc-creatures/` | NPC entity surveys/architecture, wisps, killerwisp, stolas |
| `events/` | event system REs, the 4-part events catalog (`_events_catalog_*`), triggers, active-events registry |
| `weather-wind/` | weather subsystem REs (scheduler/rendering/mainGamemode/IDA), wind, sky/celestial |
| `world-systems/` | mushrooms, dirt/window cleaning, fireflies, sleep/nightmare, RNG authority, email, gamerules, notifications, ambient anchors |
| `vehicles/` | ATV/quadbike arc, delivery drone |
| `tooling/` | dev-tool + infrastructure designs: the Baritone-analog autonomous bot-director (autonomous-testing driver); the `multivoid.ini` seeder / config-registry / validation / catalog design (`votv-ini-config-registry-DESIGN-2026-07-24.md` — DESIGN, **TWO `/qf` passes, NEITHER converged** (16 + 15 rounds); the doc was REWRITTEN after pass 2 and now carries F24-F35 + §8's list of mechanisms the pass dissolved. Read §2 and §9 before building; full scope is user-authorized but nothing is certified) |
| `_archive/` | definitively superseded/abandoned approaches (see below) |

Grep tip: filenames were NOT renamed — a bare-filename citation (code comments cite findings by name)
still resolves via `Glob research/findings/**/<name>.md`.

## `_archive/` — definitively superseded / abandoned approaches

Moved out of the active log so they can't be mistaken for a current plan (see `_archive/README.md`).
As of 2026-06-20: the failed pile save-strip + thin-client-sync approaches. **The CURRENT pile/trash
design is [docs/piles/08-HOST-AUTH-TRASH-CHANNEL.md](../../docs/piles/08-HOST-AUTH-TRASH-CHANNEL.md)**;
the full pile living knowledge base (design → as-built → verified per increment, including every
correction/reversal of the 2026-06-21..23 carry/dup arc) is `docs/piles/`. The session-history wall
that used to live in THIS README (2026-06-20..07-09 update blobs, each pointing at its canonical
finding) was extracted verbatim to
[`_archive/README-session-history-2026-06-07.md`](_archive/README-session-history-2026-06-07.md)
during the 2026-07-12 reorg — provenance only, the canonical findings carry the same facts.

## Note on duplication

The pile/trash/clump/snapshot/save-transfer RE docs are ALSO copied verbatim under
`docs/piles/findings/` (the consolidated pile knowledge base). The originals here are the canonical
copies; `docs/piles/` is the curated subset.

> Sweep note (2026-06-20, still true): a full per-file staleness audit of all ~195 point-in-time docs
> has NOT been done (most are durable RE or shipped-feature design — not misleading); only the
> definitively-dead approaches are archived. If a specific topic's docs look contradictory, the
> `docs/` canonical doc + the code win.
