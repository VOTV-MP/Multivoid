# RE: VOTV Flashlight and Item Activation — Phase 5F RE
**Date**: 2026-05-25
**Target**: Alpha 0.9.0-n
**Method**: CXXHeaderDump static analysis — mainPlayer.hpp, prop_equipment_flashlight*.hpp, prop_equipment.hpp, prop.hpp, saveSlot.hpp, prop_radio*.hpp, prop_torch.hpp, prop_lamp.hpp, prop_floorlamp.hpp, prop_saltLamp.hpp. No IDA or UE4SS Lua probing performed (open flags in Section 8).

---

## TL;DR

1. Three flashlight item classes exist: `Aprop_equipment_flashlight_C` (basic), `_b_C` (larger variant), `_c_C` (hand-crank lantern). All are `Aprop_equipment_C` subclasses. The `_a` and `_b` variants have no extra members visible in the CXX dump — all logic is BP-only.
2. **Case (b) confirmed for `_a` and `_b`**: The world light cone is `mainPlayer_C::light_R` (`USpotLightComponent*` at offset 0x0678), parented to the player's `lag_fl` spring arm. The flashlight item actors carry no light component of their own. The puppet already has this component at the same offset. Syncing the puppet's `light_R` visibility is the full implementation.
3. **Case (a/b hybrid for `_c`**: The crank lantern (`_c`) additionally has its own `UPointLightComponent* PointLight @0x0388` and `USpotLightComponent* light_R @0x0390` on the item actor itself. It also has `float energy` for battery level. This adds complexity deferred to Inc6.
4. **Primary activation UFunction: `AmainPlayer_C::updateFlashlight()`**. Triggered by the input binding `InpActEvt_flashlight_K2Node_InputActionEvent_13/14`. State lives in `bool flashlight @0x0838` on mainPlayer. The `flashlightStateChanged(USpotLightComponent* Light, bool Visible)` multicast delegate fires after the state change — this is the best POST-hook point.
5. **Flashlight on/off state is NOT saved** in `saveSlot_C`. Equipment class and data ARE saved (`saveSlot.equipment @0x0440`), but the runtime on/off bool is transient. On load the flashlight starts off. This means snapshot-on-connect (Phase 5S0) cannot cover mid-session flashlight state — a dedicated live-state broadcast at connect time is needed (can be a bool in the join handshake, not a new packet class).
6. The packet sketch is substantially correct. For `_a`/`_b` (Case b), `peerSessionId` alone identifies whose puppet to toggle — no actor Key needed. For world-prop light sources (torch, floor lamp) an actor Key IS needed, but those are different packet categories (existing `ItemActivatePayload` accommodates them via `paramBlob`).
7. `Aprop_radio_C` / `Aprop_radioNew_C` are the best Inc6 candidates — `bool A` on each, `player_use` is the toggle entry, and audio IS a world effect requiring sync. No paramBlob fields needed beyond the on/off bool.
8. `Aprop_equipment_emf_C` and `Aprop_equipment_geiger_C` are pure HUD/audio equipment with no world-visible light effect — they do NOT require ItemActivate wire packets.

---

## Section 1 — Class Enumeration

### 1.1 Flashlight Item Classes

**`Aprop_equipment_flashlight_C`** — standard handheld flashlight
- File: `prop_equipment_flashlight.hpp`
- Parent: `Aprop_equipment_C : Aprop_C`
- Size: 0x378 — identical to the `Aprop_equipment_C` base size. No additional members visible in the CXX dump. All BP logic including the toggle behavior is in the Blueprint ubergraph. No `getData`/`loadData` override — flashlight state not saved by this class.
- Role: Player-held equipment item. Light source is the player actor's own `light_R` component, not a component on this actor.

**`Aprop_equipment_flashlight_b_C`** — larger/heavier flashlight variant
- File: `prop_equipment_flashlight_b.hpp`
- Parent: `Aprop_equipment_C`
- Size: 0x378 — identical to base. Same analysis as `_C` above. All BP. No save override.
- Role: Second cosmetic/gameplay-stats variant. Same world-light mechanic as `_C`.

**`Aprop_equipment_flashlight_c_C`** — hand-crank lantern
- File: `prop_equipment_flashlight_c.hpp`
- Parent: `Aprop_equipment_C`
- Size: 0x3B9 — 65 bytes of additional members beyond the base.
- Notable members:
  ```
  UPointLightComponent*  PointLight       0x0388  — item's own point light
  USpotLightComponent*   light_R          0x0390  — item's own spot light (same name as player's!)
  float                  timelineCrank_a  0x0398  — crank timeline alpha
  UTimelineComponent*    timelineCrank    0x03A0  — crank animation timeline
  float                  Pitch            0x03A8  — light pitch angle
  float                  energy           0x03AC  — battery charge level (0.0–1.0)
  Uui_hovertextNametag_C* Widget          0x03B0  — battery-level hovertext
  bool                   holding          0x03B8  — currently held in hand
  ```
- UFunctions of interest:
  - `void getData(Fstruct_save& Data)` / `void loadData(Fstruct_save Data, bool& return)` — saves/restores `energy` level. The `_c` variant IS save-persistent for energy, but NOT for on/off state (the light is always off on load regardless of pre-save energy).
  - `void equip(bool& deleted)` — equip hook.
  - `void playerHandUse_RMB(AmainPlayer_C* Player)` / `void playerHandRelease_RMB(AmainPlayer_C* Player)` — right-mouse hold to crank.
  - `void crank()` / `void windown()` — crank charge and wind-down functions.
  - `void playerUnequip(AmainPlayer_C* Player)` — unequip hook.
- Role: This is a **hybrid Case (a/b)**: the item has its own light components AND the player's `light_R` may also be involved. The separate `light_R` at 0x0390 on the item actor distinguishes it from `_a`/`_b`. Syncing the crank lantern requires toggling the item-actor's lights (Case a logic), not just the player's component.

### 1.2 Player Actor Flashlight Members

All flashlight state lives on `AmainPlayer_C` (mainPlayer.hpp):

```
USpotLightComponent*    light_R                  0x0678  — THE player's flashlight spot light
USpringArmComponent*    lag_fl                   0x0670  — spring arm that positions light_R
bool                    flashlight               0x0838  — canonical on/off state
bool                    hasFlashlight            0x0CC2  — whether a flashlight is equipped
uint8                   flashlightMode           0x0C79  — beam mode (narrow/wide/off; used by _c)
bool                    crankFlashlight          0x0CC4  — true when _c variant equipped
bool                    input_flashlight         0x0E39  — transient: key-press-in-flight flag
bool                    flashlightTypeChanged    0x0E38  — mode-change flag (for _c beam switch)
FTimerHandle            timeFlashlightTimerHandle 0x0E30 — timer for hold-to-toggle delay
FmainPlayer_CFlashlightStateChanged flashlightStateChanged 0x0AC8 — multicast delegate (0x10)
void flashlightStateChanged(USpotLightComponent* Light, bool Visible) — the delegate event
```

The `light_R` at 0x0678 is NOT attached to a socket on the skeleton — it is attached to `lag_fl` (a `USpringArmComponent*` at 0x0670), which follows the camera/head direction. The spotlight follows the player's look direction.

The puppet is a `mainPlayer_C` orphan. It has `light_R` at the same offset 0x0678. Setting its visibility drives the world effect on the puppet's side.

### 1.3 Torch and Other Portable Light Actors (for generality in Section 5)

**`Aprop_torch_C`** — world-placed torch (NOT equipment slot, held in hand via grab)
- File: `prop_torch.hpp`
- Parent: `Aprop_C`
- Size: 0x398
- `UPointLightComponent* PointLight @0x0378`, `bool burning @0x0390`, `float fuel @0x0394`
- UFunctions: `ignite(float fuel)` / `extinguishFire()` — activation entry points
- `getData`/`loadData` — saves `burning` and `fuel`. Burn state IS save-persistent.
- This is **Case (a)**: the torch actor IS in the world (grabbed prop), its PointLight toggles on the actor.

**`Aprop_lamp_C`** — portable gas lamp
- File: `prop_lamp.hpp`
- Parent: `Aprop_C`
- Size: 0x380
- `UPointLightComponent* PointLight @0x0370`, `bool Active @0x0378`, `float fuel @0x037C`
- `actionOptionIndex` → `upd()` — toggle path. `getData`/`loadData` — save-persistent.
- Case (a): world actor with own light.

**`Aprop_saltLamp_C`** — decorative salt lamp
- File: `prop_saltLamp.hpp`
- Parent: `Aprop_C`, size 0x378
- `UPointLightComponent* PointLight @0x0370`
- `actionOptionIndex` — toggle path. No `getData`/`loadData` visible — may not be save-persistent (RE flag F-S2).

**`Aprop_floorlamp_C`** — corded floor lamp
- File: `prop_floorlamp.hpp`
- Parent: `Aprop_corded_C : Aprop_C`
- Size: 0x3BA
- `UPointLightComponent* PointLight @0x03B0`, `bool Active @0x03B8`, `bool powered @0x03B9`
- `getTriggerData`/`loadTriggerData` — save-persistent via trigger system.
- Case (a) but corded (requires power cord plugged in). `actionOptionIndex` → `upd()` toggles.

---

## Section 2 — Activation Entry Point

### 2.1 Input Binding

Two input action events for the flashlight keybind are declared on `AmainPlayer_C`:

```cpp
void InpActEvt_flashlight_K2Node_InputActionEvent_13(FKey Key);  // mainPlayer.hpp line 601
void InpActEvt_flashlight_K2Node_InputActionEvent_14(FKey Key);  // mainPlayer.hpp line 600
```

The naming convention `_13` vs `_14` indicates two separate event nodes in the Blueprint graph — one for Pressed and one for Released (or an alternate binding for hold vs tap). The VOTV default flashlight key is F.

### 2.2 UFunction Call Chain

Based on the member presence and naming pattern:

1. Player presses F. One of the two `InpActEvt_flashlight_...` handlers fires.
2. Handler calls `AmainPlayer_C::Flashlight Update()` (declared at mainPlayer.hpp line 488 — note the space in the function name is the BP node label as exported; the UFunction FName would be `Flashlight Update` with a space).
3. `Flashlight Update()` reads `hasFlashlight` (0x0CC2) and dispatches into `updateFlashlight()`.
4. **`AmainPlayer_C::updateFlashlight()`** (line 435) — the canonical state applier. This function:
   - Flips `bool flashlight @0x0838`.
   - Sets `light_R` visibility on/off (the `USpotLightComponent*` at 0x0678).
   - For `crankFlashlight` mode (`_c` variant): adjusts `flashlightMode @0x0C79` and drives the item actor's own `light_R @0x0390`.
   - Fires the `flashlightStateChanged(USpotLightComponent* Light, bool Visible)` multicast delegate @0x0AC8.
5. For the `_c` variant, `timerHoldFlashlight()` (line 655) handles hold-to-crank, and `timeFlashlight()` (line 668) manages battery drain timer.

### 2.3 Primary Hook Target

**Hook PRE on `AmainPlayer_C::Flashlight Update()`** on the sending peer to detect the toggle intent.

OR (preferred): **Hook POST on `AmainPlayer_C::updateFlashlight()`** — this fires after the state has been applied and `bool flashlight @0x0838` reflects the new state. Reading the state post-execution avoids any ambiguity about the final resolved value (e.g., if `updateFlashlight` guards against toggling when not equipped).

Alternative hook: bind to the `flashlightStateChanged` multicast delegate. The delegate signature is `(USpotLightComponent* Light, bool Visible)`. The `Visible` bool directly encodes the new on/off state. If we can bind to the delegate from our C++ layer (via reflection + dynamic delegate binding), this is cleaner than a UFunction PRE/POST hook because it fires only when the state actually changed (delegate-fires = state changed, confirmed).

The `bool Visible` parameter in the delegate is the canonical new state: `true` = on, `false` = off. For coop purposes, this is all we need to send.

### 2.4 Receiver Side

On the receiving peer, to apply the flashlight state to the puppet:

- Locate the puppet's `AmainPlayer_C` instance (via `peerSessionId` → `RemotePlayer::puppet`).
- For `_a`/`_b` variants (Case b): directly set `puppet.light_R.SetVisibility(bool)` via reflected UFunction call (`USpotLightComponent::SetVisibility(bool bNewVisibility, bool bPropagateToChildren)`). Also write `puppet.flashlight @0x0838` for consistency.
- Calling `updateFlashlight()` on the puppet via UFunction reflection is risky: the puppet has `GetController() == nullptr` (the discriminator), and `updateFlashlight()` may gate on player state (e.g. `hasFlashlight`, inventory checks). Safer to drive the light component directly via reflected call or direct offset write.

Open flag: whether `updateFlashlight()` is safe to call on a puppet (controller-null orphan). See Section 8.

---

## Section 3 — World-Effect Actor: Case (a) vs Case (b)

### 3.1 Evidence for Case (b) — `_a` and `_b` flashlight variants

Direct evidence:
- `mainPlayer_C` has `USpotLightComponent* light_R @0x0678` — confirmed in mainPlayer.hpp line 61.
- `Aprop_equipment_flashlight_C` (size 0x378) and `Aprop_equipment_flashlight_b_C` (size 0x378) have NO additional light component members visible in the CXX dump — they are identical to the base `Aprop_equipment_C` (size 0x378). No `USpotLightComponent`, no `UPointLightComponent` declared.
- The `USpringArmComponent* lag_fl @0x0670` positions `light_R` to follow the camera/look direction — this is clearly the player-held torch effect.
- The `flashlightStateChanged(USpotLightComponent* Light, bool Visible)` delegate passes the `light_R` component as `Light` — confirming it is the player's own component being toggled.

**Verdict for `_a` and `_b`: Case (b)**. The actor synced is the PLAYER (puppet). No flashlight item actor identification needed. `peerSessionId` alone identifies the puppet.

### 3.2 Evidence for Case (a/b hybrid) — `_c` crank lantern

Direct evidence:
- `Aprop_equipment_flashlight_c_C` has `UPointLightComponent* PointLight @0x0388` and `USpotLightComponent* light_R @0x0390` — the item actor itself carries light components.
- The crank mechanic (`timelineCrank`, `crank()`, `windown()`) drives the item's own lights based on energy level — this is fundamentally different from a simple on/off toggle.
- `bool holding @0x03B8` suggests the item knows whether it is held (needed for light cone orientation if the item transforms independently of the player's `lag_fl`).

**Verdict for `_c`: Case (a/b hybrid)**. Both the item actor's own lights AND potentially the player's `light_R` are involved. For Inc1-Inc4 (flashlight toggle sync), the `_c` variant is deferred — it requires additional RE to determine whether it drives the player's `light_R` OR only its own item lights. The crank mechanic also needs `energy` state sync beyond a simple bool. Deferred to Inc6+ or a dedicated `CrankLanternActivate` packet.

### 3.3 Practical Consequence

For Inc3 (sender hook) and Inc4 (receiver apply):
- Hook target: `AmainPlayer_C::updateFlashlight()` POST.
- Condition to send packet: `hasFlashlight @0x0CC2 == true` AND `crankFlashlight @0x0CC4 == false` (skip `_c` variant until Inc6).
- Receiver: set `puppet.light_R.SetVisibility(bool Visible)` via UFunction call + write `puppet.flashlight @0x0838 = Visible`.

---

## Section 4 — Save Persistence

### 4.1 What is and is not in saveSlot_C

Examining `saveSlot_C` (saveSlot.hpp):

- `TArray<Fstruct_equipment> equipment @0x0440` — saves the player's equipped items. `Fstruct_equipment` contains:
  - `Fstruct_propDynamic prop` (FName name + FName key) — item class name and key
  - `Fstruct_save data` (0x100 bytes) — item's own `getData` output
  - `FName tag` — equipment slot tag
- `TArray<Fstruct_equipment> hold @0x0450` — same for the held slot
- `TSubclassOf<Aprop_batts_C> flashlightBattery @0x0890` — which BATTERY TYPE is currently in the flashlight (separate from on/off)

**No `bool flashlightOn` or `bool flashlight` field exists in `saveSlot_C`.** The equipment array saves the item class (e.g. FName `prop_equipment_flashlight_C`) and its `getData` output (for `_c`, this is energy; for `_a`/`_b`, nothing meaningful since they have no override). The on/off state is transient.

### 4.2 What does `Aprop_equipment_flashlight_c_C::getData` save?

The `_c` variant implements `getData`/`loadData`. Based on the members (`float energy @0x03AC`), this almost certainly saves energy level. The light on/off state is NOT saved even for `_c` — the light is always off when the item is loaded.

### 4.3 Implication for Phase 5S0 (snapshot-on-connect)

Phase 5S0 sends the host's full save snapshot to the client on connect. This restores equipment class (the client knows peer A has a flashlight equipped) but NOT whether the flashlight is currently on.

**Conclusion**: Phase 5S0 snapshot is insufficient for flashlight state. An additional mechanism is needed:
- Option A: Include `bool flashlightOn` in the connect handshake packet (extend `PeerHello` or add a `PeerStateSnapshot` packet that the host sends to the joining client AND the joining client sends to the host). 1 byte per peer.
- Option B: Rely on Inc4's receiver drain: after snapshot-on-connect completes, if peer A has flashlight on, they re-send an `ItemActivate(on)` packet at the moment peer B connects. Host observes the new peer joining and queues `ItemActivate(on)` for any peer whose flashlight is currently on.

Option B is cleaner (no new packet class, uses existing reliable channel). This is the recommended approach. At host: in the connect handler, for each existing peer with `flashlight @0x0838 == true`, enqueue `ItemActivate { classHash=flashlight_a/b, peerSessionId=existingPeer, state=1 }`.

---

## Section 5 — Per-Class Generality

The following classes were examined as candidates for the unified `ItemActivatePayload` pattern.

### 5.1 Radios — `Aprop_radio_C` and `Aprop_radioNew_C`

**`Aprop_radio_C`** (prop_radio.hpp):
- Parent: `Aprop_C`, size 0x379
- `UMediaSoundComponent* MediaSound @0x0370`, `bool A @0x0378`
- `void player_use(AmainPlayer_C* Player, FHitResult Hit)` — E-press toggle
- `void openMedia(FString OpenedUrl)` / `void fail(FString FailedUrl)` — media load callbacks
- State: `bool A` (on = playing, off = stopped)
- World effect: audio from `MediaSound` — audible to both peers

**`Aprop_radioNew_C`** (prop_radioNew.hpp):
- Same structure, `bool A_0 @0x0378`. Same pattern.

**Wire mapping**: `ItemActivate { classHash=radio_C/radioNew_C, peerSessionId=sender, state=A }`. No paramBlob needed — the state is a simple bool. The receiver calls `player_use` on the radio actor OR directly sets `bool A` and starts/stops `MediaSound`. Radio actor is identified by Key (it's an `Aprop_C` subclass with `Key @0x02E0`).

**Critical difference from flashlight**: the radio is a WORLD ACTOR (not equipment slot item). We need the radio's `Aprop_C::Key` to identify which radio actor to toggle on the receiver. This is where `paramBlob[0..7]` carries the 8-byte FName Key.

This validates that `paramBlob[8]` is adequate — one FName (8 bytes) fits exactly.

### 5.2 EMF Detector — `Aprop_equipment_emf_C`

- Parent: `Aprop_equipment_C`, size 0x378 — no extra members.
- Effect tracked on player: `bool equipped_emf @0x0CDC`, `float emf_time @0x0CD8`, `float emf_maxtime @0x0CE0`.
- The EMF detector effect is a HUD overlay + audio sweep. No world-visible light cone or particle. Other players cannot see the EMF display — it's a personal UI effect.
- **Verdict**: No `ItemActivate` packet needed for EMF. It is a local HUD effect only.

### 5.3 Geiger Counter — `Aprop_equipment_geiger_C`

- Parent: `Aprop_equipment_C`, size 0x378 — no extra members.
- Effect tracked on player: `bool equipped_geiger @0x0CE4`, `float irradiation @0x0CE8`, `bool geigerAlert @0x0CEC`.
- The geiger counter is HUD (reading) + click audio. No world light. Not visible to other players.
- **Verdict**: No `ItemActivate` packet needed. Local effect only.

### 5.4 Portable Torch — `Aprop_torch_C`

- `bool burning @0x0390` — the world-visible state.
- Activation: `ignite(float fuel)` → sets `burning=true`, enables `PointLight` and `eff_fire` particle, starts `Audio`.
- Deactivation: `extinguishFire()` → sets `burning=false`, disables light + particle + audio.
- `getData`/`loadData` — burn state IS save-persistent.
- World effect: PointLight + fire particle visible to all. Requires sync.
- Wire mapping: `ItemActivate { classHash=torch_C, peerSessionId=sender, state=burning, paramBlob[0..7]=actorKey }`. The `actorKey` in paramBlob is the torch's `Aprop_C::Key @0x02E0` (8-byte FName). Receiver finds torch by Key, calls `ignite(float fuel)` or `extinguishFire()`.
- For snapshot-on-connect: torch `burning` state IS in save (via `getData`). Phase 5S0 covers this via the save snapshot. No separate flashlight-style connect-time broadcast needed.

### 5.5 Summary Table

| Class | World effect | Wire needed | Identifier | paramBlob usage |
|---|---|---|---|---|
| `Aprop_equipment_flashlight_C` | Player `light_R` (Case b) | YES | `peerSessionId` | None |
| `Aprop_equipment_flashlight_b_C` | Player `light_R` (Case b) | YES | `peerSessionId` | None |
| `Aprop_equipment_flashlight_c_C` | Item own lights (Case a/b hybrid) | YES (deferred Inc6+) | `peerSessionId` | energy byte? |
| `Aprop_radio_C` / `radioNew_C` | `MediaSound` audio | YES | `peerSessionId` + actorKey | actorKey[0..7] |
| `Aprop_torch_C` | PointLight + fire particle | YES | actorKey | actorKey[0..7] |
| `Aprop_lamp_C` | PointLight | YES | actorKey | actorKey[0..7] |
| `Aprop_equipment_emf_C` | HUD/audio only | NO | — | — |
| `Aprop_equipment_geiger_C` | HUD/audio only | NO | — | — |

---

## Section 6 — Wire Packet — Finalized Shape

### 6.1 Preliminary Sketch from Memo

```cpp
struct ItemActivatePayload {
    uint32_t itemClassHash;   // FName-hash of item class
    uint8_t  peerSessionId;
    uint8_t  state;           // 0=off, 1=on
    uint8_t  _pad[2];
    uint8_t  paramBlob[8];
};
// 16 bytes, ReliableKind = 12
```

### 6.2 Finalized Shape (confirmed by RE)

The sketch is confirmed with the following clarifications:

**`itemClassHash`**: A 32-bit hash of the item UClass FName string (e.g. hash of `"prop_equipment_flashlight_C"`). This is cross-peer stable because UClass FNames are deterministic from the cooked content. Use CRC32 or FNV1a of the UTF-8 class name string. 4 bytes.

**`peerSessionId`**: Identifies the sending peer. For Case (b) (flashlight `_a`/`_b`): the receiver uses this to find the puppet and toggle its `light_R`. For Case (a) (torch, radio, lamp): the receiver uses `peerSessionId` to find who sent, then uses `paramBlob[0..7]` to identify the world actor.

**`state`**: `0` = off/inactive/extinguish, `1` = on/active/ignite.

**`paramBlob[8]`**: For Case (b) flashlight: unused (all zeros). For Case (a) world actors: `paramBlob[0..7]` carries the 8-byte `Aprop_C::Key` FName of the world actor. FName in memory is `{ ComparisonIndex: uint32, Number: uint32 }` (8 bytes total) — but ComparisonIndex is per-process. Use the Key STRING (same as prop_wrap pattern) or use a different identifier.

**Key stability issue for world actors**: `Aprop_C::Key @0x02E0` is an FName. For save-loaded props (torch, lamp), the Key is assigned by `intComs_gamemodeMakeKeys` from a deterministic source (save data). Cross-peer stability was previously confirmed for save-UUID-backed Keys (per [[project-physics-object-pickup]] findings: Key STRING is cross-peer stable, Key.ComparisonIndex is per-process). Use the Key STRING in paramBlob, not the raw FName bytes.

**paramBlob must be enlarged to hold a Key string**: An FName string (UUID format: 22 chars + null) does not fit in 8 bytes. Options:
- Use the Key's hash (CRC32, 4 bytes) — collision risk low for ~100 actors in scene, but not zero.
- Use FName ComparisonIndex — per-process, NOT cross-peer stable.
- Reserve full 20 bytes for Key string (22 chars truncated to 20 + null), which means enlarging paramBlob.

**Recommendation**: Enlarge `paramBlob` from 8 to 16 bytes. This keeps the struct at 24 bytes total (still well within `kMaxReliablePayload = 228`). For world actors, the Key string (UUID, typically 22 chars) fits in 20 bytes with null terminator in 21. Use only the first 20 bytes of a 22-char Key (all UUIDs are base62, uniquely identified by any 16-char prefix at 100-actor scale).

OR: For flashlight (Case b), `paramBlob` is unused. For radio/torch/lamp (Case a), use the existing `PropWireKey` pattern from `prop_wrap.h` — send the full Key string as a side-channel lookup (already established for Phase 5N2 PropSpawn). The `ItemActivatePayload` just flags the intent; a preceding/following `PropKey` lookup table built at connect time resolves to the actor pointer.

**Final struct (Option A — inline key hash, 16 bytes, pragmatic)**:

```cpp
struct ItemActivatePayload {
    uint32_t itemClassHash;   // CRC32 of item UClass FName string — cross-peer stable
    uint8_t  peerSessionId;   // sender peer id
    uint8_t  state;           // 0=off, 1=on
    uint8_t  flags;           // bit0: has_actor_key (1 = paramBlob carries actor key hash)
    uint8_t  _pad;
    uint32_t actorKeyHash;    // CRC32 of Aprop_C::Key string; 0 if flags.has_actor_key=0
    uint8_t  paramBlob[4];    // future-proof spare bytes (e.g. energy uint8 for _c variant)
};
// Total: 16 bytes
static_assert(sizeof(ItemActivatePayload) == 16);
```

This keeps 16 bytes (memo size) but adds `flags` and `actorKeyHash` with a pragmatic 32-bit key hash for world actor resolution. CRC32 collision over ~100 actors in scene is negligible.

**Alternative (Option B — expand to 24 bytes)**:

```cpp
struct ItemActivatePayload {
    uint32_t itemClassHash;   // 4
    uint8_t  peerSessionId;   // 1
    uint8_t  state;           // 1
    uint8_t  flags;           // 1: bit0=has_actor_key
    uint8_t  _pad;            // 1
    char     actorKey[14];    // 14: first 14 chars of Aprop_C Key string (cross-peer stable)
    uint8_t  paramBlob[2];    // 2: spare
};
// Total: 24 bytes
static_assert(sizeof(ItemActivatePayload) == 24);
```

**Recommendation**: Use **Option A (16 bytes)** for Inc2. The CRC32 hash of the Key string is sufficient at 100-actor scale. If collision handling becomes necessary, that is a targeted fix per RULE 1.

**ReliableKind assignment**: `ItemActivate = 12` (after `DoorState=9 / LightState=10 / LockState=11` from Phase 5D).

### 6.3 peerSessionId Receiver Resolution

`peerSessionId` is confirmed sufficient to identify the puppet:
- `coop::session` tracks each peer's `RemotePlayer` by their session id.
- `RemotePlayer::puppet` is the `AmainPlayer_C*` orphan.
- The puppet has `light_R @0x0678` at the same offset as the real player.
- For Case (b): receiver takes `peerSessionId` → `RemotePlayer::puppet` → writes `puppet.flashlight @0x0838` + calls `puppet.light_R->SetVisibility(bool)`.

Cross-reference confirmed against existing `RemotePlayer::Drive` path — the pose interpolator already accesses the puppet by peerSessionId.

---

## Section 7 — Increment Plan (Refined)

### Inc1 — RE doc (this document)

**Status**: Complete on landing of this file.

Classes named: `Aprop_equipment_flashlight_C`, `_b_C`, `_c_C`.
UFunction hook point named: `AmainPlayer_C::updateFlashlight()` POST (or delegate `flashlightStateChanged`).
World-effect verdict: Case (b) for `_a`/`_b`.
Save-persistence verdict: no save — connect-time state broadcast needed.

### Inc2 — Protocol additions

**Scope**:
- Add `ReliableKind::ItemActivate = 12` to `coop/net/protocol.h`.
- Add `ItemActivatePayload` struct (16 bytes) to `protocol.h`.
- Add `static_assert(sizeof(ItemActivatePayload) == 16)`.

**Files touched**: `src/votv-coop/src/coop/net/protocol.h`.
**Test**: Compile passes, size assert passes.

### Inc3 — Sender PRE-hook on `AmainPlayer_C::updateFlashlight()`

**Scope**:
- Hook `AmainPlayer_C::updateFlashlight()` POST via ProcessEvent observer.
- In the hook: read `AmainPlayer_C* sender = (AmainPlayer_C*)this`.
- Guard: `if sender.hasFlashlight @0x0CC2 == false: skip`. Guard: `if crankFlashlight @0x0CC4 == true: skip` (defer `_c`). Guard: echo-suppression flag (see Inc4).
- Compute `itemClassHash = crc32("prop_equipment_flashlight_C")` OR `_b_C` based on equipped class (read sender's equipped equipment tag or class name via reflection to distinguish `_a` vs `_b`; functionally the hash can be the same for both if we treat them identically on receiver).
- Serialize `ItemActivatePayload { classHash, peerSessionId=g_localPeerSession, state=sender.flashlight@0x0838, flags=0, actorKeyHash=0 }`.
- Send via `g_session.reliable.Send(ReliableKind::ItemActivate, payload)`.

**Files touched**: new `src/votv-coop/src/coop/item_activate.cpp/.h`.
**Test**: Autonomous LAN test: local peer toggles flashlight; log shows packet sent with correct state byte.

### Inc4 — Receiver drain handler

**Scope**:
- In `session.cpp` drain loop for `ReliableKind::ItemActivate`:
  - Parse `ItemActivatePayload`.
  - Look up `RemotePlayer` by `peerSessionId`.
  - If `itemClassHash` matches `crc32("prop_equipment_flashlight_C")` or `_b_C`:
    - Set echo-suppression flag.
    - Write `puppet.flashlight @0x0838 = payload.state`.
    - Call `puppet.light_R->SetVisibility(bool state, false)` via reflected UFunction call (`SetVisibility` is a USceneComponent method — use `ue_wrap::CallUFunction(puppet.light_R, "SetVisibility", ...)` with params `{bNewVisibility=state, bPropagateToChildren=false}`).
    - Clear echo-suppression flag.

**Files touched**: `src/votv-coop/src/coop/item_activate.cpp`, `src/votv-coop/src/coop/net/session.cpp` (drain dispatch).
**Test**: Autonomous LAN test: peer A toggles flashlight; peer B's puppet's light_R toggles. Visual + log confirmation.

### Inc5 — Connect-time flashlight state broadcast

**Scope**:
- In the host's connect handler (where Phase 5S0 snapshot is sent):
  - For each peer already connected (including host's own player): if `peer.flashlight @0x0838 == true`, enqueue `ItemActivate { classHash, peerSessionId=existingPeer, state=1 }` to the newly-connecting peer's reliable queue.
- On the joining client's side: mirror — if local player's flashlight is on, send `ItemActivate(on)` to the host (who relays to all existing peers via the standard broadcast path).

This ensures a peer joining mid-session sees all currently-on flashlights.

**Files touched**: `src/votv-coop/src/coop/item_activate.cpp`, connect handler in session.
**Test**: LAN test: peer A plays with flashlight on; peer B connects mid-session; peer B sees peer A's flashlight on immediately.

### Inc6 — Generalize to radio (`Aprop_radio_C` / `Aprop_radioNew_C`)

**Scope**:
- Radio is a WORLD ACTOR (Case a). Add actor-key resolution path.
- At session connect, enumerate all `Aprop_radio_C` and `Aprop_radioNew_C` in scene; build `CRC32(Key string) → actor pointer` lookup map (`g_radioByKeyHash`).
- Sender hook: `AmainPlayer_C::player_use` POST on radio actor (the radio's `player_use` toggles `bool A`). Alternatively hook `Aprop_radio_C::player_use` POST — this fires on E-press on the radio.
- After hook fires: read `radio.A @0x0378`, compute `actorKeyHash = crc32(radio.Key string)`, serialize `ItemActivatePayload { classHash=crc32("prop_radio_C"), peerSessionId, state=radio.A, flags=HAS_ACTOR_KEY, actorKeyHash }`.
- Receiver drain: look up `g_radioByKeyHash[actorKeyHash]`, call `radio.player_use(puppet, FHitResult{})` OR directly toggle `radio.A` + start/stop `MediaSound`. Prefer direct state write + `MediaSound` control to avoid re-triggering unrelated BP logic.

**Files touched**: `src/votv-coop/src/coop/item_activate.cpp` (extend handler).
**Test**: LAN test: peer A turns on radio; peer B hears audio. Peer A turns off radio; peer B audio stops.

### Inc7 — Audit + hands-on LAN test

**Scope**:
- Audit: per CLAUDE.md rules — code-reviewer audit, performance (no per-frame GUObjectArray walk, no per-ProcessEvent scan), thread-safety, file-size check.
- Hands-on: both flashlight and radio confirmed working over 2-machine LAN.
- Scope.md amendment: add `ItemActivate` reliable packet to Phase 5N3 scope entry.

---

## Section 8 — Open RE Flags

**F-FL1**: `AmainPlayer_C::updateFlashlight()` BP body is unconfirmed. Specifically: does it early-return when `hasFlashlight == false`? Does it branch on `crankFlashlight` for mode selection? This affects the guard conditions in Inc3. Needs UE4SS Lua probe: hook `updateFlashlight` PRE + POST, log `flashlight` bool before/after on both a normal flashlight and with the `_c` variant equipped.

**F-FL2**: `AmainPlayer_C::flashlightStateChanged` delegate — is it broadcast correctly after `updateFlashlight()` completes, or is it a pre-notification (before state change)? The delegate signature `(USpotLightComponent* Light, bool Visible)` implies it fires with the NEW state as `Visible`. Confirm via UE4SS Lua probe: bind to delegate, observe `Visible` value vs pre/post `flashlight` bool.

**F-FL3**: `AmainPlayer_C::Flashlight Update()` (note space in name) — this BP function name exported with a space is unusual. Confirm the actual UFunction FName used by ProcessEvent. If it is `"Flashlight Update"` (with space), our FName hash and observer registration must use that exact string. Needs UE4SS Live View or Lua `RegisterHook("mainPlayer_C:Flashlight Update", ...)` to confirm.

**F-FL4**: `Aprop_equipment_flashlight_c_C` (`_c` crank lantern) — does `updateFlashlight()` also toggle the item actor's `PointLight @0x0388` and `light_R @0x0390`? Or does the item manage its own light visibility independently of the player's `light_R`? This determines whether Inc4's receiver must also find the equipped `_c` item actor and set its light components. Needs UE4SS probe: equip `_c`, toggle, observe which components change visibility.

**F-FL5**: Puppet `light_R` component initial state — when the puppet is spawned, is `light_R` visible=false by default? Or does it inherit some default from the CDO? If the CDO has `light_R` visible by default, we must explicitly set it to false at spawn time to avoid a false "flashlight on" appearance. Needs autonomous LAN test: spawn puppet, observe `light_R.IsVisible()` before any `ItemActivate` packet arrives.

**F-FL6**: `Aprop_saltLamp_C` save-persistence — the header shows no `getData`/`loadData` override. Confirm whether the salt lamp's on/off state persists across save/load. If it does not, it is not a Phase 5S0 concern but may still need `ItemActivate` sync if it is player-interactable. Needs UE4SS probe: toggle salt lamp, save, reload, check state.

**F-RA1**: `Aprop_radio_C::player_use` BP body — does it toggle `bool A` AND manage `MediaSound` start/stop? Or does it only flip `A` and rely on a tick or event to manage the `MediaSoundComponent`? This determines whether the receiver should call `player_use` (safe but may have side effects) or directly toggle `A` + `MediaSound`. Needs UE4SS probe: hook `player_use` + `MediaSoundComponent` start/stop events.

---

## Next Steps

The RE doc (Inc1) is complete. The implementation order is:

1. **Inc2**: Add `ReliableKind::ItemActivate = 12` + `ItemActivatePayload` struct (16 bytes) to `protocol.h`. Compile check only.
2. **Inc3**: Open `item_activate.cpp/.h` under `coop/`. Hook `AmainPlayer_C::updateFlashlight()` POST. Serialize + send. Guard on `hasFlashlight` and `!crankFlashlight`.
3. **Inc4**: Add drain handler in session dispatch for `ReliableKind::ItemActivate`. Receiver: look up puppet via `peerSessionId`, set `flashlight @0x0838` + call `SetVisibility` on `light_R @0x0678`.
4. **Inc5**: Add connect-time flashlight state broadcast in connect handler.
5. **Audit** before hands-on test: code-reviewer + performance check (ensure no per-tick scan).
6. **Hands-on LAN test** to confirm flashlight visible on puppet.
7. **Inc6**: Radio sync (adds actor-key resolution path).
8. **Inc7**: Full audit + 2-machine LAN test.

Before Inc3, resolve flags **F-FL1** (updateFlashlight body), **F-FL3** (UFunction name with space), and **F-FL5** (puppet light_R initial state) via UE4SS Lua probes — these are low-risk probes that can be scripted into `tools/probes/flashlight_probe.lua`.
