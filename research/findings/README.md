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
| `tooling/` | dev-tool + infrastructure designs: the Baritone-analog autonomous bot-director (autonomous-testing driver); the `multivoid.ini` seeder / config-registry / validation / catalog design (`votv-ini-config-registry-DESIGN-2026-07-24.md` — CONVERGED pass 3 "that holds" E19; **ARC 1 BUILT 2026-07-25** (commits `16c8a448..9b6982a6`: tri-state line primitive, registry TU, T7 constant, T1 seeder, T3 ci-writer; instrument PASS — exactly the 4 enumerated F8 verdict moves, T3 drills 11/11; the 'стоит' question was ANSWERED: seeded-active `net.nick=Pelmentor`); **ARC 2 BUILT + AUDIT-FOLDED 2026-07-25** (commits `06e9910d..7f1765ea`: T2-enum registry 103 rows + enum_check.ps1, T4 unified-ci-occurrence flip, Resolve typed reads, T3b registry writer, mint gate, T10 sweep + SETTINGS CHECK panel + T1b reformat, C6 write-TU extraction; corpus gate AFTER==NEWSIM 248/248, drills 35/35, smoke x3; 2 CRIT + 1 IMP audit findings root-fixed; NOT hands-on); **ARC 3 IN BUILD 2026-07-25 eve** per `votv-ini-arc3-impl-DESIGN-2026-07-25.md` (/qf 16r "that holds"; C1 `f88a78cf` typed row defaults + .inc X-macro handles + constexpr ValidateRows, C2 `faa0289d` handle-only Resolve + ResolveString + fonts ENUM rows, C3a `3b9aba38` 62-site flag sweep + IsIniKeyTrue DELETED = read ratchet closed; each Release-clean, NOT smoked; remaining C3b write door + enum_check retire, C4 T2b, C5 registry_gate CI + drills + AFTER-compare); arc 4 (catalog) remains design); the CI autobuild + dev-release lane (`votv-ci-autobuild-dev-release-DESIGN-2026-07-25.md` — CONVERGED R29, then **BUILT + FULL DRILL MATRIX PASSED same day** (§7): ledger ritual + judge scripts + trampoline/release-core/build-core live on main, PR #2 merged with the reworked thin build.yml, rulesets up, fingerprint committed from the smoked cacheless run, b125 consumed/published/retracted by the campaign, proto now 126; the first REAL dev release **v0.9.0n-b126-dev PUBLISHED 2026-07-25** (run 30160029906 green, assets+sha256 on the release page, published ledger row pushed `db4d4790`)) |
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
