# Per-player inventory — implementation plan (2026-06-14)

Source: a 5-agent Understand workflow (4 parallel RE agents + synthesis), cross-verified
against the live tree. Design authority: `votv-wisp-and-client-inventory-RE-2026-06-12.md`
(Topic 2). Feature = replace the v56 whole-host-save inventory inheritance with a per-player
inventory persisted host-side at `SaveGamesDir()/<save_name>/coop_players/<guid>.json`, keyed
by a client GUID. **Protocol bump 72 -> 73.**

## SOLID (static-RE-confirmed; increments 1-3 buildable today)
- **Items are DATA, not actors.** Inventory = `TArray<Fstruct_save>` at
  `GObjStack[propInventory.Index].obj_11`, mirrored to `inventoryData@0x02E0`. Apply = an
  array write + a refresh-trio call, NOT a spawn loop. (Report 1 §1 — the key finding.)
- **Byte format:** `Fstruct_save` ARRAY-ELEMENT STRIDE **0x100** (NOT the dump's raw `Size: 0xF8`
  -- it embeds a 16-aligned FTransform, so Align(0xF8,16)=0x100; every embedded Fstruct_save field
  in the dump reports 0x100), `Fstruct_equipment` stride **0x120** (Align(0x118,16); struct_
  equipmentWear.hpp:6 confirms). [CORRECTED 2026-06-14 by the adversarial-verify panel -- the raw
  0xF8/0x118 were a real N>=2 engine-materialize crash. The internal FIELD offsets were always
  correct; only the array strides were wrong.] Optional future hardening: read `FArrayProperty::
  Inner->ElementSize` live to be recook-proof (reflection plumbing; the hardcoded aligned values
  are correct + verified for 0.9.0-n, consistent with the codebase's per-version-offset discipline).
  Wire FName/UClass as STRINGS
  (re-intern StringToFName / FindClass on apply; preserve exact FName case). Transform = 10xf32
  (40B, drop the 2 pads). Drop `signals.image` (unbounded PNG).
- **Transport:** chunk over the existing `blob_chunks` lane + `BlobChunkPayload` (228B,
  ~56KB max). NOT the SaveTransfer bulk lane.
- **Identity:** GUID via `BCryptGenRandom` (bcrypt linked; ole32/rpcrt4 NOT linked so
  CoCreateGuid is out). ini read/write exist (config.cpp:81/:95). JSON via nlohmann (linked).
  Path via save_guard::SaveGamesDir() + a new save_transfer::HostSlot().

## PROBE-GATED (Increment 4 only; the live apply)
Four unknowns the bytecode can't answer (Report 1 §5): (1) are recalculateNames+updateSlotInv+
updateVolumesAndMass SUFFICIENT for a freshly-overwritten obj_11 (drag/extract/hold)? (2)
updateHold re-entrancy mid-session? (3) does updateEquipment reconcile a REMOVAL (incl. PP
overlay teardown)? (4) propInventory.Index stability at the apply moment? -> a dev-gated SP
probe (`coop/dev/inventory_probe.cpp`, ini `inventory_probe=1`) must pass all 4 before Inc 4
ships. Escape hatch: write-saveSlot-then-reload (the exact SP path; the join already force-loads).

## INCREMENTS (dependency order; each independently buildable + LAN-smoke-testable)
1. **GUID identity + Join field + host file scaffold** (recommended first; no live apply):
   config.cpp ReadPlayerGuid() (BCryptGenRandom, 32-hex, ini r/w); harness boot seed ->
   player_handshake::SetLocalGuid(); handshake build(:176)/parse(:247) append [u8 len][guid]
   + g_guidBySlot + GuidForSlot(); protocol 72->73; save_transfer::HostSlot(); new module
   coop/player_inventory_sync.{h,cpp} EnsurePlayerFile(slot) (create coop_players/ +
   empty []-content <guid>.json, tmp+rename); subsystems Install(:69) + ConnectReplayForSlot
   (:142). SMOKE: host log `slot 1 guid=<32hex>`, file exists, client ini gained player_guid=.
2. **Serializer + read layer** (pure data, self-test): ue_wrap/inventory.{h,cpp} (resolve
   saveSlot like economy.cpp:33-44; ReadSaveRecord/ReadEquipRecord over the strides) +
   coop/inventory_wire.{h,cpp} (length-prefixed LE blob, modeled on signal_wire.cpp:17-49;
   reuse signal_dynamic ReadStruct/ReadFString/ReadFNameLeaf, TArrayView). ini
   inventory_selftest=1 round-trip assert.
3. **Transport (client push + host receive->disk, NO apply):** ReliableKind
   PlayerInventoryBlob=69 (THREE-place: protocol enum+doc; session_lanes LaneFor->Bulk + NOT
   relayable; event_feed router case -> host Assembler). Client Tick polls+serializes,
   ContentHash dedup, 1Hz throttle, blob_chunks::SendBlob to slot 0. Host sink reassembles ->
   writes <guid>.json (hash-diff + 15s rate-limit). Wire Tick(:304)/DisconnectSlot(:193)/
   DisconnectAll/shutdown FlushAllToDisk (before Stop). SMOKE: client pickup -> host file updates.
4. **Live apply (host->client on join) -- GATED on the probe:** host SendInventoryToSlot in
   ConnectReplayForSlot; client applies after BootStorySaveBlocking (harness.cpp:331) on the
   game thread: overwrite obj_11 + inventoryData/hold/equipment element-wise (MintFString/
   StringToFName/FindClass) + refresh-trio. First-join -> reset_player_* + Array_Clear(obj_11).
   RULE 2: RETIRES the v56 player-scoped inheritance (override, not parallel path). SMOKE:
   per-player inventory survives rejoin, independent of host, applied items usable.

## RISKS: live-apply safety (probe); blob size/churn (blob_chunks cap + dedup + throttle +
15s rate-limit + drop signals.image); FName/UClass cross-peer (wire names, preserve case);
money OUT of scope (saveSlot.Points stays on balance_sync); SaveGamesDir() empty -> in-memory
fallback + warn; GUID 32-hex validation. New files all << 800 LOC.

## Inc 4 apply-chain RE (2026-06-14, partial -- before coding the apply)
The live inventory is NOT a flat array. Chain (SDK-verified): mainPlayer -> its player
Aprop_container_C -> a UpropInventory_C component (propInventory.hpp): Index@0xB0, recalculate
Names(), getObj(Index,...), takeObj(Index,...), addObject(AActor* Actor, int insertIndex,...),
Init(). The actual item list = gamemode.GObjStack[propInventory.Index].obj_11 (TArray<Fstruct_
save>); saveSlot.inventoryData@0x2E0 is the MIRROR (what Inc 2/3 read+serialize). GObjStack lives
in prop_container.hpp/saveSlot.hpp (struct_mObject element has obj_11). saveSlot reset funcs:
reset_player_inventory/hand/equipment (saveSlot.hpp:114-117). mainPlayer: updateEquipment(),
updateHold(bool&,FString&).
KEY APPLY UNKNOWNS (still need a DESTRUCTIVE SP probe + HANDS-ON, not resolvable read-only):
(1) write target -- do we overwrite GObjStack[Index].obj_11 (the live source) + recalculateNames
to sync inventoryData, or write inventoryData + a func to rebuild obj_11? (2) addObject takes a
live AActor*, NOT Fstruct_save -- so re-materializing from saved data is the LOAD path, not
addObject; the safe apply is likely WRITE saveSlot.inventoryData + force the BootStorySaveBlocking
reload (the RE-doc escape hatch -- native, sidesteps the live-rematerialize unknowns) rather than
a live obj_11 poke. (3) the refresh-trio sufficiency + updateHold/updateEquipment re-entrancy +
Index stability -- behavioral, need eyes. RECOMMENDATION for Inc 4: prefer the WRITE-saveSlot-
THEN-RELOAD apply (Option B, native load path, no obj_11 TArray manipulation / double-free risk),
injected BEFORE or as a second BootStorySaveBlocking at harness.cpp:331; the join already
force-loads so the reload is mechanically free. Build a dev probe (inventory_probe=1) that
write-saveSlot+reloads the LOCAL player's own (unchanged) inventory in SP -> verify items are
back + usable (hands-on). Inc 1-3 (read+serialize+transport+persist) are DONE+verified; Inc 4 is
the only remaining piece + it is the genuinely-risky, behavioral-verification one.

## Inc 4 -- AS BUILT (2026-06-14)
The apply is NOT a second reload and NOT a live obj_11 poke. It SUBSTITUTES the inventory in the
REGISTERED save object BEFORE the game's native loadObjects() materializes it on the SINGLE join
load -- so the game's own code builds the live inventory from our data. This sidesteps ALL FOUR
mid-session live-apply unknowns (refresh-trio / updateHold / updateEquipment / Index stability):
materialization is the game's load path, exercised exactly as a normal load.

MECHANISM (the chain):
1. `ue_wrap::reflection::EngineAlloc(size,align)` (NEW) -- allocates on the engine heap via
   `FMalloc::Realloc(nullptr,size,align)` (the SAME vtable slot kSigFMemoryRealloc is matched
   from, +0x20; NOT the never-exercised Malloc slot). Allocator-matched with EngineFree so the
   engine's later Array-realloc / GC free of a buffer WE built does not corrupt the heap.
2. `ue_wrap::inventory::ApplyToSaveObject(saveSlot, inv)` (NEW) -- overwrites inventoryData /
   equipment / hold with engine-built `TArray<Fstruct_save>` / `TArray<Fstruct_equipment>`:
   value-group nested TArrays via EngineAlloc, FStrings engine-minted (fstring_utils::MintFString),
   FNames interned (fname_utils::StringToFName), UClasses FindClass'd, signals via the proven
   signal_dynamic::WriteStructLive. The PREVIOUS array buffers are intentionally ORPHANED (a
   bounded one-time-per-join leak of a few KB -- recursively freeing the old nested Fstruct_save
   sub-arrays is far more crash-prone than leaking them; the engine never double-frees a pointer
   it has lost). EngineAlloc failure -> arrays degrade to EMPTY (never corrupt); a top-level
   GMalloc-unresolved -> the whole apply refuses (returns false) rather than wipe a real inventory.
3. `ue_wrap::engine::SetSaveObjectReadyHook(fn)` (NEW) -- engine.cpp fires `fn(g_storySave)` ONCE,
   on the game thread, right after LoadStorySave's disk-load (or StartFreshGame's create) and
   BEFORE setSaveSlotObject + travel -- the one window where the inventory arrays exist but the
   world has not been built from them. The coop hook self-gates to a no-op off a client join.
4. host->client push: the HOST sends each joiner its persisted `<guid>.json` blob (FNV-verified,
   .bak fallback, EMPTY on missing/corrupt -- fail-safe, never leaks another player's items) over
   PlayerInventoryBlob to that ONE slot (blob_chunks::SendBlobToSlot, NEW). Driven from the host
   TICK's connect-edge detector (player_inventory_sync::HostPersistTick: connected + Join-GUID
   arrived + not-yet-sent -> send once) -- NOT ConnectReplayForSlot, which fires at world-ready
   (AFTER the joiner already loaded, too late for the pre-materialize hook). The tick edge fires
   right after the Join, in the joiner's PRE-WORLD save-transfer window.
5. client apply: OnReliable (CLIENT branch, sender==host slot 0) reassembles + deserializes the
   blob into g_pendingApply + sets HasPendingApply(). DriveMenuModeJoinWorldBoot WAITS (bounded
   10s, pumping net) for HasPendingApply before BootStorySaveBlocking, so the hook always has the
   data. The hook (player_inventory_sync::OnSaveObjectReady) then calls ApplyToSaveObject.

DEV PROBE (inventory_probe=1, coop/dev/inventory_probe.cpp): SP one-shot ~5s after world-up.
ReadAll -> Serialize -> Deserialize -> ApplyToSaveObject(live saveSlot) -> ReadAll -> compare.
PASS iff the whole loop is byte-identity (blob0==blob1==blob2) -- proves the engine WRITE path
(EngineAlloc + nested TArray construction + FName/FString/UClass faithful reconstruction) round-
trips with no fault. Non-destructive (writes the saveSlot MIRROR only; obj_11 untouched). It does
NOT prove "items usable after a real load" -- that is the materialize path (loadObjects), verified
only by the multiplayer-join HANDS-ON (a client with inventory_apply=1 rejoining + confirming its
items are present + usable). I CANNOT certify items-usable autonomously.

### v56 inventory-inheritance RETIREMENT PLAN (RULE 2)
The v56 whole-host-save transfer gives a joining client the host's ENTIRE save, including the
host's inventory (implicit inheritance -- there is no separate "inherit inventory" code; it is
emergent from loading the host's .sav). Inc 4 OVERRIDES the inventory portion of that loaded save
(the apply runs after the disk-load, before materialize), so when the apply is ON the client gets
ITS per-player inventory, never the host's -- the override IS the retirement (the world/props/etc
of the host save are still used; only the player inventory is per-player). STAGED ROLLOUT: the
apply is behind ini `inventory_apply` (DEFAULT OFF) -- a transitional gate with THIS written
retirement plan (the project's allowed exception). Gating criteria to flip default-ON + delete
the gate: (a) inventory_probe=1 logs PASS in SP; (b) a 2-peer hands-on with inventory_apply=1
confirms a client's items survive a rejoin, are independent of the host, and are USABLE. Until
both pass, OFF -> the v56 inheritance stays (no half-working apply ships). When both pass: set the
default ON and remove the `inventory_apply` branch (full RULE-2 retirement, no parallel path).

STATUS: built + (pending) compile-clean. NOT deployed, NOT launched (user may be on PC). Needs the
probe run + the hands-on before inventory_apply is trusted/flipped on.
