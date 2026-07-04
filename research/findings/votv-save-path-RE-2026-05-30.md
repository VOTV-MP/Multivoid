# VOTV save-path RE — PR-FOUNDATION-2 (save-game safety) — 2026-05-30

4-agent reflection-dump + codebase + IDA research (workflow wf_86e7017c). Anchors
cited as `L#` in `Game_0.9.0n/.../UE4SS_ObjectDump_GAMEPLAY_SAVE.txt` and
`CXXHeaderDump/*.hpp`. Open items needing a UE4SS live probe are flagged tier-3.

## 1. Save trigger path

Two save containers, both funnel to engine `GameplayStatics::SaveGameToSlot`
(SYNC, game-thread; the async node has no VOTV caller):
- `mainGamemode_C.saveSlot` @0x4B0 → `saveSlot_C` = the per-playthrough WORLD save.
- `mainGamemode_C.save_main` @0x508 → `save_main_C` = global META save (keybinds,
  achievements, stats, mailbox, store). Low leak risk.

Triggers (all real ProcessEvent UFunctions unless noted):
- `mainGamemode_C:autosave` (L275446) — timer-driven, cadence `autosaveDelay`@0x628
  (from user setting). Delegates to `save`. **Primary risk path.**
- `mainGamemode_C:sleep` (L275447) → `save` (bed/sleep save).
- `mainGamemode_C:save(quicksave,bypassEvent,overwriteSubsave)` (L275452) — outer
  public entry; menu/quicksave/subsave all route here. quicksave/overwriteSubsave
  are inline ubergraph custom events (L231508/L231510), not standalone UFunctions.
- `save_main_C:save` (L166406) — meta save.
- `saveSlot_C:savePlayerOnly` (L296428) → `saveToSlot`.
- A few world triggers (e.g. `trigger_lockerLooker_C`, L397300) call `SaveGameToSlot`
  directly, bypassing the gamemode `save` wrapper but still hitting `saveToSlot`.
- `disableSave`@0xE40 (L231222) — global gate. [ANSWERED 2026-07-04, bytecode
  research/pak_re/saveSlot.json + mainGamemode.json: the ONLY reader lives at the HEAD
  of `saveSlot_C:save` (NOT the gamemode ubergraph) — it gates gather+write for EVERY
  gamemode trigger (autosave/sleep/menu/quicksave all funnel through `saveSlot:save`;
  mainGamemode never calls `saveToSlot` directly). NOT gated: `savePlayerOnly` + the
  direct `SaveGameToSlot` trigger callers (the disk hook covers those). NOTHING in the
  bytecode ever WRITES disableSave. Exploited by save_block part 3 — a coop CLIENT
  holds it true = the native cycle off at the game's own gate (commit `99eb4566`).]

Populate → serialize sequence:
- Populate: `mainGamemode_C:saveObjects(quicksave)` (L275634), `Save Primitives`
  (L277324), `saveTriggers` (L276133), `saveHoldObj` (L276539) + inline ubergraph
  write `playerTransform`@0x210, `inventoryData`@0x2E0, `subLevelPlayerPositions`@0xD98.
- **Write funnel: `saveSlot_C:saveToSlot(quicksave,overwriteSubsave,isForcedSave)`
  (L296279)** — contains ALL 4 `SaveGameToSlot` calls (main slot + 3 subsave/level
  files: L296307/296332/296348/296353). Both `saveSlot:save` and `savePlayerOnly`
  route through it. **The tightest single PRE/POST bracket.**

## 2. Leak surface — what coop state pollutes the save

`saveObjects` does `GetAllActorsWithInterface(int_objects_C)` → DynamicCast to
`int_save_C` → per-actor `ignoreSave()` gate → `getData()` → `struct_save` record
into the save arrays. Key save fields (element = `struct_save`, identity =
`key`@0x40 NameProperty):
- **`saveSlot_C.objectsData`@0x300 (Array<struct_save>) — CRITICAL** main world-object list.
- **`saveSlot_C.GObjStack`@0x198 — CRITICAL** grouped object stack.
- `dishData`@0x2F0 / `trashPilesData`@0x818 / `grimeData`@0x808 — prop families.
- `primitivesData`@0xE30 (struct_primitiveSave, via int_primitive_C).
- `subLevelData_objects/_triggers/_primitive`@0xCF8/0xD48/0xDE0 (per-level maps).
- `photos`@0x480 / `photos_phys`@0x7B0 / `photosData`@0xCF0 (~19 MB of the slot).

CONFIRMED leak vectors:
- **Mirror PROPS** (`Aprop_C`-derived, spawned by remote_prop_spawn.cpp, carry a Key,
  implement int_objects_C + int_save_C) → gathered + pass the cast → written to
  objectsData/GObjStack. On reload, `getObjectFromKey` (L275946) resurrects phantom
  duplicate props. **CRITICAL.**
- **NPC mirrors** (npc classes implement int_save_C, e.g. npc_goreSlither_C:getData
  L248785) → serialized identically. **HIGH.**
- **playerTransform is SAFE** — sourced via `playerPositionToTransform`→`GetPlayerPawn`
  (single pawn), NOT actor-enumerated, so the puppet cannot pollute it.
- **Puppet is gated OUT of struct_save arrays** — `mainPlayer_C` implements
  int_objects_C but NOT int_save_C, so the DynamicCast fails → not written. (It IS
  gathered transiently.) BUT see the mainPlayer@0x630 overwrite risk below.

## 3. The engine-native opt-out (ROOT-CAUSE FIX, RULE №1)

`int_save_C.setIgnoreSave(bool)` / `ignoreSave(bool&)` (int_save.hpp:9-10). `saveObjects`
skips any gathered actor whose `ignoreSave()` returns true. **Calling
`setIgnoreSave(true)` on every coop mirror prop / NPC mirror at spawn makes the GAME's
OWN save walk exclude them — no array mutation, no scrub/restore.** This is the
preferred mechanism over a post-populate array scrub. CAVEATS (tier-3 verify):
- Confirm each mirror class's DynamicCast-to-int_save_C succeeds AND that
  `ignoreSave` returns the bool `setIgnoreSave` writes (decompile the member offset).
- Synth keys ALSO propagate into gamemode `keyObj_key/obj` bookkeeping
  (remote_prop_spawn.cpp:240) — a second residue site (confirm offsets).

## 4. Coop-injected state inventory + discriminators

| Coop state | Max | Runtime discriminator | Save catches? |
|---|---|---|---|
| Puppet (unpossessed mainPlayer_C orphan) | 3 | `GetController(actor)==null` for mainPlayer_C (players_registry.cpp:68); `Registry::IsPuppet` (:148); arrays g_puppets (net_pump.cpp:52), puppetByPeer_ | YES (real mainPlayer_C) but gated out of struct_save by failed int_save cast; transform-leak gated by GetPlayerPawn |
| gamemode.mainPlayer@0x630 overwrite | 1 ptr | compare ReadPtr(gm,0x630) vs Registry::Local() (capture/restore puppet.cpp:305/:328) | a ptr field; re-assert before serialize (residual hole if gm not live at spawn, puppet.cpp:306-310) |
| Synth-keyed prop | many | Key starts `cs_` (prop_synth_key.cpp:66); BUT Aprop descendants skip synth-keying (:51) so NOT exhaustive | actor may be genuine (convergence); key/keyObj is the residue |
| NPC mirror | host NPC count | MirrorManager<Npc>::Instance(); IsMirror()==true (npc_mirror.cpp:397) | YES (client only) |
| Prop mirror (wire) | per-peer | MirrorManager<Prop>::Instance(); IsMirror()==true; coop-only drain = DrainMirrorsOnly() | YES if fresh-spawned; converged reuse genuine actor |
| Prop local shadow | ~2000 | same manager IsMirror()==FALSE (prop_element_tracker.cpp:263) | actor GENUINE — must NOT scrub |

**Cleanest cross-cutting discriminator = the Element `IsMirror()` flag** (already
used by the disconnect drains). Puppets are the one category NOT covered by IsMirror
(use IsPuppet / null-Controller).

## 5. On-disk layout + atomic gap + backup requirement

- Save root: `%LOCALAPPDATA%\VotV\Saved\SaveGames\`. Each slot = a single flat
  `<slot>.sav` GVAS/UE4.27 file (test slot `s_may2026.sav` ~19.6 MB; photos are
  serialized INSIDE the .sav). Companions: `data.sav` (meta/settings/gallery),
  `b_*.sav` (backups), `_bin\` (asset blobs, empty for test save). Subsaves are
  additional top-level `.sav` files with name-mangled slot names.
- **NON-ATOMIC write gap CONFIRMED:** stock `SaveGameToSlot` → `FFileHelper::
  SaveArrayToFile` truncates+overwrites in place (no .tmp+rename). 4 sequential
  in-place writes per save. A crash mid-write leaves a truncated, corrupt .sav with
  no engine recovery. → (1) session-start backup is the ONLY recovery path; (2) any
  coop-side persistence WE add must implement its own atomic discipline (.tmp + fsync
  + MoveFileEx replace).
- **Backup-on-session-start must copy:** `<slot>*.sav` (main + subsaves) +
  `data.sav` (+ `_bin\` if non-empty), into `SaveGames\coop_backup\<timestamp>\`.

## 6. Clean pre/post signals
- No engine PreSave hook/delegate exists — synthesize it by detouring `saveToSlot`
  (and/or `mainGamemode:save`/`autosave`) PRE.
- `gameSaved` multicast delegate @0x10D0 (L231294, no params) — candidate clean
  PostSaveRestore signal; post-write ordering UNVERIFIED (tier-3 probe).

## 7. Tier-3 (UE4SS live probe) open items
1. [ANSWERED 2026-07-04, static bytecode — no live probe needed; see the disableSave
   note in §1: all gamemode-funnel triggers YES; savePlayerOnly + direct SaveGameToSlot
   callers NO (disk-hook belt covers them).]
2. Is `gameSaved` strictly AFTER the last `SaveGameToSlot` returns?
3. Do mirror/puppet classes inherit `Aactor_save_C` (auto-serialized base)? Does
   setIgnoreSave's bool back ignoreSave's read?

## 8. Design implication (the recommended shape)
PRIMARY = engine-native opt-out: `setIgnoreSave(true)` on every coop mirror actor at
spawn (props + NPCs) so the game's own walk excludes them. SECONDARY = a PRE/POST
bracket on `saveSlot_C:saveToSlot` that (a) re-asserts gamemode.mainPlayer==Local(),
(b) belt-and-suspenders scrubs any IsMirror() actor that slipped the opt-out, (c)
restores on POST/gameSaved. PLUS a zero-RE session-start backup of the SaveGames
slot (the only recovery path given the non-atomic write). The client-save SEMANTICS
(does a client persist host state? do story/day/signals sync?) is an OPEN SCOPE
DECISION — see the shared-save model question.
