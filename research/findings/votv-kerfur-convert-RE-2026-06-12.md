# Kerfur NPC <-> prop conversion RE + the v67 host-authoritative fix

Date: 2026-06-12 (session 12, post-compact). Trigger: user dupe report --
"client sees a turned off kerfur-object, client turns it on, client turns it
off - now host sees 2 kerfurs lying turned off."

Sources: cooked-BP kismet disassembly (research/pak_re workflow: repak `get` ->
kismet-analyzer `to-json` -> render_bp.py / dump_fn.py / at_offset.py), the CXX
header dump, and the REAL-SESSION LOG CENSUS that settled the dispatch question
(below). Assets: `VotV/Content/objects/kerfurOmega.uasset`,
`VotV/Content/objects/prop_kerfurOmega.uasset`.

## 1. The classes

- `AkerfurOmega_C : ACharacter` -- the live NPC (0x9F0). ~20 data-only skin
  subclasses (`kerfurOmega_alien_C`, `_keith_C`, ...). Carries:
  - `dropProp` (TSubclassOf<Aprop_kerfurOmega_C>) @ 0x0850 -- which PROP
    variant a turn-off materializes.
  - `Type` int32 @ 0x07A4, `sentient` bool @ 0x07D8, `kill` bool @ 0x05E0,
    `hasFloppy` @ 0x0810 / `floppyType` @ 0x07A0 / `floppyData` @ 0x0790.
  - `kerfurOmega_col_C` and `kerfurOmega_col_gamer_C` OVERRIDE
    `dropKerfurProp` (collar accessory drop); no variant overrides
    `actionName`.
- `Aprop_kerfurOmega_C : Aprop_C` -- the turned-off prop form (0x390). All
  `prop_kerfurOmega_*` skins are subclasses; none override anything relevant.
  Carries `spawnKerfur` (TSubclassOf<AkerfurOmega_C>) @ 0x0388 + `Type` /
  `sentient` mirrors. The prop IS the spawner (`spawnKerfuro()`).

## 2. The verbs (kismet ground truth)

### turn OFF -- NPC -> prop

Radial menu -> `kerfurOmega_C::actionName(Player, Hit, Name)` [string-switch
dispatcher; ubergraph entry 20350]:

```
if (kill) return;                  // murder-mode kerfur refuses the menu
switch (Name):
  'turn_off'    @21844: dropKerfurProp()          <- the conversion
  'follow'      @21624, 'idle' @21404, 'patrol' @21184,
  'fix_servers' @20950, 'kill' @21859 (startKill()), 'get_reports' @21874
```

`dropKerfurProp()` (30 statements):
1. `if (sentient) { Audio->Activate(true); return; }` -- sentient kerfurs
   REFUSE conversion entirely.
2. If `hasFloppy`: `lib::floppyFromType(floppyType, ...)` -> BeginDeferred +
   FinishSpawning the `Aprop_floppyDisc_C` subclass at own transform; copies
   `data` (floppyData) + `readWrites` via Set*PropertyByName.
3. Spawn transform = own transform (or (0,0,20000) when `isInFleshRoom`);
   BeginDeferred + FinishSpawning **`dropProp`**; DynamicCast to
   prop_kerfurOmega; on success copy `sentient` over and
   **`K2_DestroyActor()` self** (the NPC dies).

### turn ON -- prop -> NPC

Radial menu -> `prop_kerfurOmega_C::actionOptionIndex(Player, Hit, Action,
comp)` -> ubergraph entry 29:

```
if (Action != 8) return;          // 8 = the single menu option (getActionOptions returns options_enum=[8])
spawnKerfuro();
```

`spawnKerfuro()`: spawn transform = `spwn` billboard location + (0,0,50), yaw
only; BeginDeferred + FinishSpawning **`spawnKerfur`**; `IsValid(spawned)` ->
**`K2_DestroyActor()` self** (the prop dies) ; else `lib::addHint(...)` and the
prop SURVIVES. (+ `progressAchievement('kerfuro')`.)

## 3. WHY IT DUPED -- the dispatch ground truth

Every spawn/destroy above is **BP-internal**:
- the spawns are `EX_CallMath` BeginDeferredActorSpawnFromClass /
  FinishSpawningActor (direct native invoke -- never ProcessEvent);
- the destroys are by-name `EX_VirtualFunction 'K2_DestroyActor'` ->
  CallFunction native branch (direct invoke -- never ProcessEvent);
- the verb calls themselves are `EX_LocalVirtualFunction` /
  `EX_LocalFinalFunction` self-calls (ProcessLocalScriptFunction -- the
  documented v44 trap; never ProcessEvent).

Our entire hook engine is ONE MinHook detour on `UObject::ProcessEvent`
(game_thread.cpp) -- so NONE of it ever fired for the conversion flows.

**Log census proof (2026-06-12, all real-session logs across all 4 game
copies):** zero `npc-suppress[client]: skipping` lines and zero
`npc-sync[host]: broadcast EntitySpawn` lines have EVER been emitted by a
game-initiated spawn. The BeginDeferred interceptor only ever fired for OUR
ProcessEvent-dispatched dev-spawns. Real-session NPC sync has been carried
entirely by `RegisterExistingWorldNpcs` (the connect-edge GUObjectArray walk).
The `host_spawn_watcher` header's "BeginDeferred call ... dispatched THROUGH
ProcessEvent -- OBSERVABLE" claim is NOT supported by any live firing in the
logs; do not build on it without a live probe.

The dupe chain therefore was: client turn-on ran fully locally (real
untracked kerfur NPC client-side; the prop mirror self-destroyed client-side
only), then client turn-off spawned a client-local prop + killed the rogue --
while the HOST never saw any packet for any of it. Cross-peer drift on every
step; the "2 kerfurs on host" end-state comes from the host's own original
prop surviving plus the conversion artifacts converging at the next
connect-edge walks/snapshots.

What IS ProcessEvent-visible (and probe-proven for this family --
`lightswitch_probe` FIRED on `lightswitch_C::actionOptionIndex`): the
CROSS-OBJECT menu dispatchers `actionName` / `actionOptionIndex` (all
in-params, void return -- the no-out-params script-call path goes through
ProcessEvent).

## 4. The v67 fix (coop/kerfur_convert)

MTA request shape (DoorOpenRequest precedent; ambient_spawner_suppress's
script-fn PRE-cancel precedent):

- CLIENT: PRE-interceptors on `kerfurOmega_C::actionName` (Name=='turn_off')
  and `prop_kerfurOmega_C::actionOptionIndex` (Action==8) CANCEL the local
  dispatch + send `KerfurConvertRequest{elementId, toProp}` (ReliableKind 63,
  protocol v67). Untracked target -> cancel WITHOUT request (a client must
  never convert locally; logged).
- HOST request handler (event_feed drain, game thread): resolve eid ->
  element -> actor (MirrorManager<Npc/Prop>), validate IsLiveByIndex + class
  descent, replicate the BP's `kill` guard (field read @ the reflected
  offset), then call the REAL verb via ProcessEvent -- `dropKerfurProp`
  (picking the most-derived declarer: col/col_gamer overrides) or
  `spawnKerfuro`. ProcessEvent runs exactly the UFunction passed (no
  re-virtualization), hence the explicit declarer pick.
- CONVERGE (the BP-internal side effects, post-verb):
  - actor `!IsLiveByIndex` -> `npc_sync::SyncDestroyedNpcActor` (the
    K2-PRE body; pointer-as-map-key only) / `prop_lifecycle::
    SyncDestroyedTrackedProp` (element-based PropDestroy -- reads the wire
    key off the Prop ELEMENT, never the actor memory).
  - new prop(s) -> targeted GUObjectArray walk (untracked live
    prop_kerfurOmega_C- / prop_floppyDisc_C-descendants) ->
    `ExpressSpawnedProp` (latch-deduped; fresh Aprop_C spawns carry a
    UCS-minted NewGuid key, the takeObj machinery's documented behavior).
  - new NPC -> `RegisterExistingWorldNpcs()`, which as of v67 ALSO
    broadcasts EntitySpawn for newly-registered NPCs while connected
    (re-sends to already-mirroring peers are receiver-idempotent).
  - The sentient-refusal / spawn-failure / kill-guard paths converge to
    clean no-ops (actor still live -> no destroy-sync; nothing spawned ->
    walk ingests nothing).
- HOST's own menu use: the same interceptors pass the dispatch through and
  push {actor, internalIdx, eid, toProp} onto a small pending queue (the
  interceptor contract forbids engine calls/Post); `kerfur_convert::Tick()`
  (net pump) converges next tick. eid is captured at push time so the
  converge never needs the actor's memory even if a GC purge lands between
  frames.

## 5. Deliberately out of scope (recorded for later)

- The OTHER actionName verbs (follow/idle/patrol/fix_servers/kill/
  get_reports) still run client-locally on mirrors -- AI-command desync,
  needs the host-authoritative AI command relay (MTA precedent), separate
  feature.
- Generic BP-internal destroy invisibility (any prop a BP destroys outside
  our PE view -- cremator, eaten, burned): project-wide gap; the death-watch
  pattern (host_spawn_watcher / trash piles / super-sponge) is the current
  per-feature answer. The kerfur conversion destroys are now covered
  explicitly by the converge.
- `prop_kerfurWheel_C` is a plain Aprop_C (no spawnKerfuro) -- not a
  conversion target.

## 6. Tooling note

`size_ubergraph.py::size_textconst` was a raising stub; implemented the
UE4.27 FScriptText sizes (Empty/LocalizedText/InvariantText/LiteralString/
StringTableEntry) -- kerfurOmega's ubergraph (36390 bytes, 205 jump targets)
now validates byte-exactly, so at_offset.py traces land on true statement
boundaries. at_offset.py's exec-slice was widened to include size_textconst.

## 7. Round-3 hands-on outcome (2026-06-12 evening): v67 NEVER INSTALLED

The user's round 3 still duped (client turn-ON converted locally; host kept
the prop). Logs: `kerfur_convert: partial resolve (actionName=0 nameOff=-1
...) -- retrying` every ~2 s on BOTH peers, all session -- the install never
completed, so NO interceptors existed and both directions stayed fully local.
`dropKerfurProp`/`spawnKerfuro`/`actionOptionIndex` resolved; `actionName`
did not, despite kerfurOmega.json carrying the export (Exports[26]).

ROOT CAUSE (general, not kerfur-specific): `reflection::NameEquals` compared
rendered FName strings with `wmemcmp` -- CASE-SENSITIVELY. Engine FNames
compare by ComparisonIndex (case-insensitive), and the rendered string
carries whatever casing was registered FIRST in the process. Some
earlier-loaded class registers the shared interactable verb name as
"ActionName" -> kerfurOmega's lowercase-authored verb renders as
"ActionName" -> our lowercase lookup missed. Unique names (dropKerfurProp)
never collide, which is why only the SHARED name failed -- and why the bug is
load-order roulette for any by-name lookup.

FIX (same evening): NameEquals / NameStartsWith / NameContains are now
case-insensitive (`_wcsnicmp`) -- engine-true semantics; two engine names can
never differ only by case, so this is strictly more correct for every
caller. kerfur_convert needs no change: with actionName resolving, the
audited v67 logic goes live. Verify on the next session's logs:
`kerfur_convert: installed` instead of the partial-resolve retry spam.
