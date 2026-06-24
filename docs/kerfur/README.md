# Kerfur — the coop knowledge base (kerfurOmega NPC <-> prop sync)

> Single source of truth for the **kerfur** multiplayer sync problem, mirroring `docs/piles/`.
> Created 2026-06-24 when the user opened the **OFF-state non-replication** track. Living KB:
> every kerfur design / diagnosis / as-built updates a doc here; mark each claim **DESIGN** /
> **AS-BUILT** / **VERIFIED** (never "VERIFIED" from a smoke alone — only a real hands-on or a
> matching real log). Durable RE lives in `research/findings/votv-kerfur-*.md`; this folder holds
> the living design/diagnosis/as-built + the index into those findings.

## TL;DR — what a kerfur IS (the two forms)

A kerfur has **TWO distinct actor forms, different classes** (RE-verified, not a flag toggle):
- **Active = `kerfurOmega_C`** (an NPC / ACharacter sibling; ~20 data-only skin subclasses incl.
  `kerfurOmega_col_C` / `kerfurOmega_col_gamer_C`). Its radial menu is `kerfurOmega_C::actionName`.
- **Off object = `prop_kerfurOmega_C`** (a PROP, `Aprop_C`-derived, has an eid in the prop pipeline;
  skin subclasses likewise). Its radial menu is `prop_kerfurOmega_C::actionOptionIndex`.

**turn_off** (`actionName "turn_off"`) -> ubergraph `if (kill) return;` -> `dropKerfurProp()`
[PE-invisible EX_CallMath]: spawns `prop_kerfurOmega` **at the kerfur's own transform** (or `(0,0,20000)`
only `when isInFleshRoom`), copies `sentient`, optionally drops the carried floppy, then
**K2_DestroyActor()s the NPC**. **turn-on** (`actionOptionIndex Action==8`) -> `spawnKerfuro()`: spawns
the NPC, destroys the prop. So conversion is **destroy-one-class + spawn-the-other-class**, all
**BP-internal / ProcessEvent-INVISIBLE** (the central RE fact -- our single ProcessEvent detour never
sees the verb, the spawn, or the destroy).

## Why coop is hard here (the central trap)

Every spawn/destroy inside the conversion verbs is `EX_CallMath BeginDeferredActorSpawnFromClass` /
by-name `K2_DestroyActor` -- **none dispatch through ProcessEvent**, so interceptors on
`actionName`/`actionOptionIndex`/`BeginDeferred` NEVER fire for the conversion (proven: zero firings in
real sessions). Detection is therefore a **death-watch POLL** (`kerfur_convert::PollKerfurConversions`,
5 Hz): *a kerfur mirror Element whose actor DIED while its wire Element is still present == the local
game just converted it.* See [[lesson-ex-callmath-invisible-to-processevent]].

## The sync architecture (as-built)

| form | sync channel | identity |
|---|---|---|
| active NPC | `npc_sync` -> EntitySpawn/EntityPose (host registers host-owned `Npc` Element, `m_mirror=false`, in `MirrorManager<Npc>`) | host-range eid + stable `KerfurId` (`kerfur_entity`) |
| OFF prop | `kerfur_convert` -> **`KerfurConvert` broadcast** (NOT npc_sync; the off form is a prop) | same `KerfurId`, rebound IN PLACE to a new prop eid via `BindFormActor` |

**The conversion is ONE entity changing form, not a destroy+create across two pipelines** (redesign
10.3): `BindFormActor` rebinds the stable `KerfurId` in place and broadcasts the SOLE transition signal
`KerfurConvert{oldEid, newEid, toForm, class, pose}`. The client `OnKerfurConvert` destroys the old-form
mirror + materializes the new form (adopting its own parked conversion-ghost if it initiated).

## Index

| doc | what | status |
|---|---|---|
| [02-window-activation-three-symptoms-2026-06-24.md](02-window-activation-three-symptoms-2026-06-24.md) | **THE BUG**: host ACTIVATES kerfurs IN THE JOIN WINDOW -> 3 symptoms (dup object+active / camera-no-body / identity-collision). Same window two-channel class as the L1 pile dup; kerfur NOT covered by the L1 save-time reconcile; binding is 30cm position-fuzzy (collision). | **CONFIRMED by log+code (repro 2026-06-24)** |
| [03-fix-design-symptoms-1and3-savetime-key-2026-06-24.md](03-fix-design-symptoms-1and3-savetime-key-2026-06-24.md) | FIX DESIGN for 1+3 (off-prop-at-blob): save-time EXACT-position key (L1 Path 1c generalized) + post-quiescence sweep. Q2 RESOLVED (capture=prop-eid, KerfurId=post-conversion carry, cross-peer=save-pos); 4 edges VETTED; (a) one-map-no-gap + (b) explicit teardown CLOSED; active-AT-blob scoped OUT (-> doc 05) | **DESIGN READY (off-prop scope; NOT built; awaiting reverse-log root-split before commit to one-build-vs-two)** |
| [05-reverse-active-at-blob-turnoff-window-2026-06-24.md](05-reverse-active-at-blob-turnoff-window-2026-06-24.md) | **REVERSE follow-ghost: CLOSED.** The client async-load spawns a DUPLICATE active kerfur; `npc_adoption`'s one-shot ghost sweep was gated only on `g_pending.empty()` NOT `HasLoadTailQuiesced()` -> fired before the late twin spawned (12:30:17 vs tail 12:30:23) -> latched -> twin = follow-ghost. turn_off retire was a RED HERRING. FIX: gate the sweep on `HasLoadTailQuiesced` (reuse the proven signal). | **FIX VERIFIED hands-on (13:15) + COMMITTED `91948b83` (push held); MD5 `239b231c`** |
| [06-obs2-missing-object-fresh-spawn-no-register-2026-06-24.md](06-obs2-missing-object-fresh-spawn-no-register-2026-06-24.md) | **OBS-2: a save kerfur OBJECT missing on the client (4 of 5).** NOT the reverse (no turn_off that session) -> a save-object bug. `s1l33p` (eid 3150) had no local save-twin -> fresh-spawn `OnSpawn(deferKerfur=false)` ran but NEVER `RegisterPropMirror`'d. A MISSING object (NOT a dup -> scope A won't fix it). PRELIMINARY: prop fresh-spawn path != camera's npc path. | **DIAGNOSIS IN-PROGRESS; root not pinned; NOT built** |
| 04 (pending) | FIX DESIGN for symptom 2 (camera): fresh-spawn vs adopt floating-camera cascade. Separate from 1+3; build AFTER 1+3 | **not written yet** |
| [01-off-state-host-turnoff-replication-diagnosis-2026-06-24.md](01-off-state-host-turnoff-replication-diagnosis-2026-06-24.md) | turn_off on a CONNECTED client (out of window) -- NOT the bug (clean out of window). Static proof the convert channel is built + correct -> WHY 02 concludes "window-timing, not the channel" | **SUPERSEDED as bug hypothesis; static analysis still valid** |

**The fix split: TWO fixes, not three.** 1+3 share a root (kerfur object identity falls to position-fuzzy +
no save-time reconcile) -> ONE save-time-exact-key fix (doc 03). Symptom 2 (camera) is independent (doc 04).
Order: 1+3 first (kills dup + collision), then 2 (gives the hopeless kerfur a body).

## Durable RE findings (research/findings/) — the kismet/disassembly ground truth

- `votv-kerfur-convert-RE-2026-06-12.md` — the turn_off/turn-on kismet (dropKerfurProp / spawnKerfuro,
  spawn position, the `kill` guard). **The canonical convert RE.**
- `votv-kerfur-sync-REDESIGN-2026-06-16.md` — the KerfurId/BindFormActor redesign (one-entity-two-forms).
- `votv-kerfur-convert-dupe-savePersisted-RCA-2026-06-15.md` — the client-toggle dupe RCA.
- `votv-kerfur-prop-join-adoption-RCA-AND-DESIGN-2026-06-16.md` — join-time prop adoption.
- `votv-kerfur-savetransfer-ghost-prop-RCA-2026-06-15.md` — save-transfer ghost prop.
- `votv-kerfurOmega-coop-double-and-camera-RE-2026-06-14.md` · `votv-kerfur-bodyfacing-RE-2026-06-07.md`
  · `votv-kerfur-headlook-AnimBP-RE-and-coop-sync-2026-06-07.md` · `votv-kerfur-headlook-BP-disassembly-2026-06-07.md`.

## Source map (as-built)

- `coop/kerfur_convert.{cpp,h}` — the death-watch poll + host converge (`ConvergeAfterConversion`) +
  client apply (`OnKerfurConvert` / `MaterializeKerfurMirror`) + conversion-ghost claim/park/adopt.
- `coop/kerfur_entity.{cpp,h}` — the stable `KerfurId` table + `BindFormActor` (the KerfurConvert broadcast).
- `coop/kerfur_command.{cpp,h}` — v74 host-auth menu-verb relay (follow/idle/patrol/fix_* etc.; NOT turn_off).
- `coop/kerfur_menu_input.{cpp,h}` — client radial-menu verb detect.
- `coop/kerfur_prop_adoption.{cpp,h}` — join-time adoption of save-loaded kerfur props.
- `coop/npc_sync.{cpp,h}` · `coop/npc_mirror.*` — the active-NPC channel.
- `ue_wrap/kerfur.{cpp,h}` — engine wrapper (NeutralizeAiTimers etc.).
- `coop/dev/kerfur_toggle.{cpp,h}` — the autonomous kerfurtoggle test harness.
