# Kerfur — the coop knowledge base (kerfurOmega NPC <-> prop sync)

*[↑ docs index](../README.md)*

> There are TWO robots. The regular one is `kerfus` (`Ap_kerfus_C`, a corded prop with three radial
> actions) and it has no sync of any kind yet; `kerfurOmega_C` (a character with ten actions) is what
> everything below is about. This README states the sync as it is; the diagnosis records it grew from
> are kept outside the repository.

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

## Where the design came from

The window-activation bug (a host activating kerfurs inside the join window: a duplicate object, a
camera with no body, an identity collision) was diagnosed and fixed in June 2026 through one
save-time exact-key reconcile for the duplicate and the collision, a load-tail gate on the client's
ghost sweep, an argument-slot fix in the deferred spawn path, and an anti-collision fuzzy gate in
prop adoption. The dated records of that work are kept outside the repository.

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
