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
| `join-identity/` | the join-window arc: save transfer, snapshot adoption, purge/quiescence gates, eid/identity binds, the placed-prop 6-root saga RCA. **2026-07-27: `votv-nickname-arbitration-roster-id-DESIGN-2026-07-27.md` — nickname uniqueness + TAB ID + international text. TWO passes: arcs A/B/C (42-round /qf, converged) and ARC D §9b (19 rounds, NOT converged, stopped once the questions moved onto the drill). **ARC A IS BUILT, DRILLED AND AUDITED 2026-07-27** (proto 130, commits `72805d96`..`63a488a7`; §3 AS-BUILT carries the evidence, THREE corrections the build measured against the design, all FOUR drills -- departure / replacement-with-loss-injection / successor-ban / idempotency-by-count -- and two audits at 0 CRITICAL. Still NOT hands-on.) **ARCS B AND D1 ARE BUILT 2026-07-28** (host-assigned names + one UTF-8 codec owning BOTH directions; proto 132). **ARC D2 IS BUILT 2026-07-28** (`5947d391`, proto 132 unchanged, drilled on DX11+DX12, NOT hands-on) — **decision of record §9d, AS-BUILT §9e**, and §9e corrects three of §9d's mechanism claims by measurement. The delegated product question was MALFORMED (it assumed the font set decides which names are ACCEPTED; the sanitizer is a DENYLIST and already accepts every hanzi), and the real question -- where arc B's uniqueness survives to the PIXEL -- is answered by folding out-of-repertoire codepoints to ONE sentinel in the arbiter, which makes uniqueness FONT-INDEPENDENT. Exactly one donor is embedded (single-codepoint emoji, whole cmap, +689 KB); CJK and Hangul stay OUT and stay unique. §9e also records a defect the drill exposed that is OLDER than the arc: `%ls` in the logger's `vsnprintf` fails on any codepoint above U+007F and MSVC empties the buffer, so **every log line naming a Cyrillic, CJK or emoji peer had been vanishing whole**, which made the first gate run read as a relay failure. **Section 9d.4 records what was designed and DELETED so it is not re-derived**, and 9d.2 lists five defects LIVE at HEAD. Read it before touching the roster, per-slot person-state teardown, slot-addressed moderation, OR any text/encoding/font path: it measured that a client's TAB listed only itself and the host (arc A fixed that), that a recycled slot goes X->Y with no absence in between, that a ban modal targets a snapshot-time slot index, that there is no UTF-8 decode at entry at all, that the UTF-8 codec already ships in `chat_sync.cpp`, and that emoji are blocked by `imconfig.h:65` rather than by fonts -- which is in a git SUBMODULE, so the define must ride a PUBLIC compile definition rather than an edit. Arc A is independent; arcs B+D were ONE delivery and ALL FOUR ARCS ARE NOW BUILT. **§9f is the open front: the user's mixed-script smoke (`mp.py smoke_i18n`, en/ru/zh/ja, messages TYPED via WM_CHAR) proves Latin+Cyrillic+Chinese+emoji end-to-end but NOT the Japanese message, and a `/qf` on the scenario's design is owed — §9f.4 is the pre-written brief.** |
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
| `tooling/` | dev-tool + infrastructure designs: the Baritone-analog autonomous bot-director (autonomous-testing driver); the `multivoid.ini` seeder / config-registry / validation / catalog design (`votv-ini-config-registry-DESIGN-2026-07-24.md` — CONVERGED pass 3 "that holds" E19; **ARC 1 BUILT 2026-07-25** (commits `16c8a448..9b6982a6`: tri-state line primitive, registry TU, T7 constant, T1 seeder, T3 ci-writer; instrument PASS — exactly the 4 enumerated F8 verdict moves, T3 drills 11/11; the 'стоит' question was ANSWERED: seeded-active `net.nick=Pelmentor`); **ARC 2 BUILT + AUDIT-FOLDED 2026-07-25** (commits `06e9910d..7f1765ea`: T2-enum registry 103 rows + enum_check.ps1, T4 unified-ci-occurrence flip, Resolve typed reads, T3b registry writer, mint gate, T10 sweep + SETTINGS CHECK panel + T1b reformat, C6 write-TU extraction; corpus gate AFTER==NEWSIM 248/248, drills 35/35, smoke x3; 2 CRIT + 1 IMP audit findings root-fixed; NOT hands-on); **ARC 3 BUILT WHOLE + VERIFY GREEN 2026-07-25 night** per `votv-ini-arc3-impl-DESIGN-2026-07-25.md` (/qf 16r "that holds"; C1 `f88a78cf` typed defaults+handles+ValidateRows, C2 `faa0289d` handle-only reads, C3a `3b9aba38` flag sweep IsIniKeyTrue DELETED, C3b `1a7c70fb` handle-keyed write door, C4 `beb73208` T2b duplicate resolvers retired, C5 `ad15ae7c` registry_gate.ps1 CI gate + enum_check.ps1 RETIRED + `ce035619` selftest twins/drills, `fed851cb` config_selftest cut; ratchet closed both directions, compile-proof recorded; AFTER-compare 100/100, corpus 248/248, drills 15/15, smoke ×2 PASS `2ba9014653033cd5`, perf audit 0 CRIT; NOT hands-on); **ARC 4 BUILT 2026-07-25 night** per `votv-ini-arc4-T8-catalog-impl-DESIGN-2026-07-25.md` (/qf 12r; `fd3481cd` T8 multivoid.ini.example: desc×108+gatedBy columns, generator+6-detector drill green on live boot bytes, 9 controls + locale canary; `0bbc0525` F34 latches; `42fabf77` binary-writer audit fix; CI RED drill run 30168118925 red at the gate step; **the whole ini workstream is BUILT**, not hands-on)); the CI autobuild + dev-release lane (`votv-ci-autobuild-dev-release-DESIGN-2026-07-25.md` — CONVERGED R29, then **BUILT + FULL DRILL MATRIX PASSED same day** (§7): ledger ritual + judge scripts + trampoline/release-core/build-core live on main, PR #2 merged with the reworked thin build.yml, rulesets up, fingerprint committed from the smoked cacheless run, b125 consumed/published/retracted by the campaign, proto now 126; the first REAL dev release **v0.9.0n-b126-dev PUBLISHED 2026-07-25** (run 30160029906 green, assets+sha256 on the release page, published ledger row pushed `db4d4790`)) |
| `_archive/` | definitively superseded/abandoned approaches (see below) |

**`tooling/` — 2026-07-26 additions (kept out of the giant row above so they stay findable):**

| Doc | What it is |
|---|---|
| `votv-imgui-dx12-overlay-DESIGN-2026-07-26.md` | AS-BUILT: the overlay's RHI split (`ui/overlay_backend.h`) + the DX12 renderer + presenting-queue capture. Drilled autonomously (first frame rendered, menu screenshot, 2 resizes); **NOT hands-on**. |
| `votv-release-notes-install-DESIGN-2026-07-26.md` | AS-BUILT: the changelog + install pipeline — `tools/release/notes/b<N>.md` as the changelog authority, `New-ReleaseBody` v2 (one writer, three paths), `docs/INSTALL.md` as the single owner of install prose, and the five gates (judge NOTES_OK, publish backstops, INSTALL_STALENESS / INSTALL_CONSISTENT / NOTES_DRIFT). Shipped in b128. |
| `votv-ue4ss-coexistence-FACTS-2026-07-26.md` | FACT BASE (22 agents, 3 rounds): can UE4SS + UE4SS mods coexist with an installed Multivoid. No proxy-filename collision on any current channel; one ProcessEvent double-detour surface (UE4SS 3.0.1 cohort, eager+unconditional; today's experimental is lazy); Lua Func-wipe falsified; the dominant risk is semantic (adopt+amplify / fight / silent-drift lane classes). Named runtime probes remain. |
| `votv-ue4ss-switch-decision-QF-WIP-2026-07-26.md` | **DECIDED F1 (26 rounds, converged + user re-confirmed same day).** Should the substrate move onto UE4SS's C++ API? Measured: 2,404 replaceable LOC (1.6%), 5 repair commits in 1,282 (2 of which a framework absorbs); UEPseudo is Epic-linkage-gated (NOT "un-buildable" — corrected same day): the structural leg is that a public repo cannot vendor or depend on it. §3+§5 remain the fact base; the living record + trip-wires + ledger = `docs/VERSION_MIGRATION.md` §11. |

**`tooling/` — 2026-07-30 addition:**

| Doc | What it is |
|---|---|
| `votv-imgui-192-upgrade-DESIGN-2026-07-30.md` | **BUILT + SMOKE-MEASURED ON BOTH RHIs (C1+C2a); NOT hands-on. Submodule pinned v1.92.9 since `b33aae30`.** The ImGui 1.91.5 -> 1.92.9 migration. Answers the glyph thread's root -- **CJK is unaffordable on 1.91.5 whatever the SOURCE and whatever the TRIGGER**, because there is no dynamic atlas, and the five recorded objections collapse to that one root. **The capability flag is deliberately CLEARED ON BOTH RHIs, so the build delivers NO new glyphs yet** -- one build, one repertoire, by design. Measured: the legacy regime LOCKS the atlas (`imgui.cpp:9089`), so drawing is behaviour-identical to 1.91.5 -- but `FindGlyphNoFallback` BAKES on a miss OUTSIDE a frame, which is where our boot selftest ran, flipping `TexIsBuilt` and poisoning every later frame; `GlyphRanges` is **dead input** once the flag is on (arc D2's fold==bake construction is **dissolved**, not violated); `ImGui_ImplDX12_UpdateTexture` ends in an **unbounded** `WaitForSingleObject(..., INFINITE)`; `InitInfo` does NOT strip the flag, so C2a silently switched the dynamic atlas on for DX12 until a smoke caught a 512x128/0.0 ms lazy atlas. Holds the C2b/C3 plan, the widened fold table computed for real (**+5,078**, and why subtracting Unicode `Cn` would make the table a function of the generator's Python version), three live defects, and the fold's dated EXPIRY. Supersedes RF3's framing in `../join-identity/votv-arc-d-gate-measurements-2026-07-28.md`. |

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
