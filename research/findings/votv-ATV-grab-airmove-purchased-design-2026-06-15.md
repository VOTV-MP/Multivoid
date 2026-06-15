# ATV grab/air-move sync + purchased-ATV identity -- RE + design (Gap A + Gap B)

Date: 2026-06-15. Trigger: user "sync mirror when a peer E grabs an ATV ... and moves it in air ...
players might buy another one which will require sync mirroring too". Method: a 28-agent workflow
(4 RE lenses + adversarial verify + synthesis; 23 findings, all 23 confirmed, 0 refuted).

GAP A (grab-carry sync) IS BUILT -- see section 4 "Gap A". Authority widened to occupant-OR-grabber
+ AtvRelease=71 (proto v76). Built clean, 2 adversarial audits PASS (0 CRIT/HIGH), deployed x4
(SHA 8D8C38D6). Known scope boundary (flagged, NOT a regression): after a peer DRIVES+parks+exits an
ATV, its mirror stays physics-off (frozen) on other peers -> ungrabbable until re-streamed; the
clean follow-up is an occupant-EXIT release reusing the same un-freeze machinery (deferred). The
default base ATV starts physics-on, so the user's literal "grab it" scenario works.

GAP B (purchased-ATV identity) is DEFERRED behind a keysHash confirmation: buy a 2nd ATV, compare
the `atv: index rebuilt ... keysHash=0x...` log line HOST vs CLIENT. Divergent (expected -- same
generateRandomKey chain as the kerfur) -> implement the host-eid EntitySpawn + class-match adoption
path (section 3.2). Stable -> the existing key-index already covers it (gap B moot).

---

# Implementation Brief: ATV Grab/Air-Move Sync + Purchased-ATV Identity (Gap A + Gap B)

## 1. GROUND TRUTH

### 1.1 The grab mechanic on AATV_C

**How E-grab + air-move works.** The ATV is grabbed by the SAME player-side mechanic as the trash clump — the light **PhysicsHandle (PHC) grav-hand grab**, NOT a vehicle-specific path and NOT the heavy "Hold Object" interface. The player's `useArm` SphereTrace hits the ATV's simulating root; `pickupObject(HitResult)` gates on `IsSimulatingPhysics`, stamps `mainPlayer.grabbing_actor := HitResult.HitActor`, and calls `grabHandle->GrabComponentAtLocationWithRotation(...)`; per-tick `grabHandle->SetTargetLocationAndRotation(handLoc + fwd*grabLen, handRot)` spring-drags the still-simulating body toward the hand.

**Reconciling the one conflict in the findings.** Two lenses disagreed on light-vs-heavy:
- The **bytecode-level findings** (lens `grab-mechanic-RE`, verdicts CONFIRMED) prove via `mainPlayer.json`/`ATV.json` that the ATV rides the **light PHC `grabbing_actor` path** (`GrabComponentAtLocationWithRotation`, gated on `IsSimulatingPhysics`, with NO `SetSimulatePhysics(false)`/`AttachToActor`/`setPropProps` anywhere in the grab block), and the heavy "Hold Object" path is provably **unreachable** because `canBePickedUp` returns FALSE.
- An **older RE doc** (`votv-ATV-quadbike-...-2026-06-08.md` §3.6) called it the "heavy physics-grab via setPropProps." The newer bytecode disassembly **REFUTES** that older reconstruction (`setPropProps` is never invoked in the ATV ubergraph; `switchToHeavyDrag` returns `isHeavy=false`).

**Resolution for the design:** treat it as the **light `grabbing_actor` PHC path** — the grabber identity is `mainPlayer.grabbing_actor == thisATV` (already read by `local_streams.cpp:197` via `ReadMainPlayerGrabState`). This is the SAFE choice because:
1. The detection field is **already in active production use** for held props (`local_streams.cpp:197` reads `gs.grabbingActor` ungated; the `IsKeyedInteractable` gate at `:203-204` applies ONLY to the `holdingActor` fallback, never to `grabbing_actor`).
2. The design is **robust to all three field outcomes**: whether the grab lands in `grabbing_actor` (primary), `holding_actor` (fallback — but `IsKeyedInteractable(ATV)=false` would block it), or neither, the **grabber-authority predicate will test BOTH `grabbingActor` AND `holdingActor` against the indexed ATV** (mirroring `local_streams.cpp`'s dual-field discipline), so it cannot be wrong-footed. A one-frame hands-on grab-state probe (below) confirms which field the ATV lands in before the gate is finalized.

**State that marks "grabbed + by whom":** There is **NO grabbed/held/stick bool on the ATV**. `isDriven@0x05F7` stays FALSE and `Player@0x05B0` stays NULL during a grab — both are written ONLY on the SEATING path (`playerSit`/possess block sets `isDriven:=true` + `Player:=mounter`; cleared on dismount). The ATV's own `playerGrabbed`/`playerGrabbed_pre`/`beginHoldingObject` BP handlers are **empty no-ops** (bare `EX_PopExecutionFlow`). Therefore "who grabs this ATV" is answered ENTIRELY on the player side: `mainPlayer.grabbing_actor == thisATV`.

**Physics mode while grabbed:** the root `Mesh@0x0570` (which IS the actor root, so actor-transform == body-transform) stays **PhysX-SIMULATING** — a free rigid body dragged by the PHC spring, not kinematic, not attached. So on the **grabber** the live actor transform is the ground truth to stream every tick (`atv_sync` already reads it via `GetRootTransform` → actor transform); on **receivers** `PrepareMirror` (physics off) is correct as-is. **6-DoF rotation is needed and already streamed** — the ATV tips/flips when air-moved/thrown, and `AtvStatePayload` already carries full `pitch/yaw/roll` (`protocol.h:2441`).

### 1.2 Purchased-ATV identity

- **Same class.** A bought ATV is the SAME `AATV_C` (no `prop_atv` placeholder). It is delivered by `AorderPlace_C::spawnOrder` via `BeginDeferredActorSpawnFromClass`/`FinishSpawningActor` (the class read from `Fstruct_store.object`) — a **deferred `EX_CallMath` spawn that our ProcessEvent interceptor never sees**. The `crafted()` path (`ub 9167`) is the same.
- **NOT cross-peer-stable at spawn.** UCS → `getKey` → `lib_C::assignKey`: when `key==None` it calls `generateRandomKey()` (16 random bytes → Base64Url → fresh ~22-char FName). So a freshly-purchased (never-saved) ATV mints a **different random key on each peer** — `K_host != K_client`. This is the **exact kerfur v74 failure mode**.
- **Becomes deterministic only after a save round-trip.** The ATV's `loadData` (SuperStruct=0 full override) DOES restore `key` (`data.key=='None' ? 'atv' : data.key`) — UNLIKE `kerfurOmega::loadData` which drops it. So after the next save+reload the key aligns across peers.
- **The default base ATV works today** precisely because it is save-placed/save-persisted → its key is deterministic on both peers, and `atv_sync`'s key-index + connect-snapshot already cover it.

## 2. THE TWO GAPS (exact lines)

**GAP A — a grabbed (air-moved) ATV streams NOTHING.** The single authority gate is `IsLocalOccupant` (`atv_sync.cpp:85-87`): `return localPlayer && A::IsDriven(actor) && A::GetOccupantPlayer(actor) == localPlayer;`. It is the ONLY steady-state stream gate (`Tick()` `:270`, the sole `SendReliable` at `:283` lives inside its true-branch; the `else if (e.hasPose)` at `:285` only drives mirror interp, never sends). A grabbed-not-seated ATV has `isDriven==false`/`Player==null`, so `IsLocalOccupant` is false for EVERY peer → `:270` is never taken → nothing streamed → receivers hold the ATV **frozen at its last pose** (the air-move is invisible). The receiver guard at `:227` and connect-snapshot at `:233-250` are unaffected — the gap is the missing *second* stream authority.

**GAP A-bis (pipeline conflict).** When `grabbing_actor==ATV`, `local_streams.cpp:197` (`heldActor=gs.grabbingActor`, ungated) WOULD pick the ATV into the held-PROP pose stream: `EnsureHeldItemBroadcast(ATV)` returns false (`trash_collect_sync.cpp:157` — `IsKeyedInteractable(ATV)=false` since `AATV_C : public APawn`), so no prop-mirror spawns, BUT `local_streams.cpp:251-291` still emits `PropPoseSnapshot` packets (key empty, eid=0) that receivers drop as "no local match" (`remote_prop.cpp:349`). So the ATV is half-handled by the wrong pipeline = wire spam + log noise. **RULE 2 (one path) requires excluding the ATV from `local_streams` so only `atv_sync` owns it.**

**GAP B — a purchased ATV is never indexed / never matched.** `RebuildIndex` (`atv_sync.cpp:159-196`) reads `key = A::GetKeyString(obj)` and at `:171` `if (key.empty() || key == L"None") continue;` — a keyless ATV is skipped before the push at `:172`, never entering `g_atvs` (`:72`, `std::unordered_map<std::wstring,AtvEntry>`, keyed by the key STRING). `OnReliable` resolves by `g_atvs.find(key)` (`:221`). For a purchased ATV with per-peer random keys, `K_host != K_client` → `find()` misses → mirrors nothing. The connect-snapshot (`:239 for (auto& kv : g_atvs)`) inherits this — a keyless purchased ATV is never snapshotted to a joiner, so the joiner never sees it exists.

## 3. THE FIX DESIGN

### 3.1 GAP A — extend single-syncer authority from "occupant" to "occupant OR grabber"

**Root-cause shape (RULE 1):** widen the authority predicate. The ATV is a single-authority entity; today the only authority state is "seated driver." Add a second, mutually-exclusive authority state "local grav-hand grabber." This is the **MTA `CUnoccupiedVehicleSync` shape** (`reference/mtasa-blue/Client/mods/deathmatch/logic/CUnoccupiedVehicleSync.cpp`): the server **elects ONE syncer** for an unoccupied vehicle and START/STOP-SYNC packets hand the streaming authority around. Here the "election" is purely local and deterministic — *whoever's `mainPlayer` is holding the ATV is its syncer* — no server round-trip needed (you cannot be seated and grav-hand-holding the same ATV simultaneously). The carry-stream itself mirrors **`local_streams.cpp`'s held-prop `PropPose` stream** (the real grab-carry precedent), and the release-edge discipline mirrors **`remote_prop.cpp::OnRelease`** + the FIFO ordering from **`prop_stick_sync`**.

**Grabber-authority predicate (in `atv_sync`, sibling of `IsLocalOccupant`):**
```cpp
// True iff THIS peer's local player is grav-hand-holding `actor` (grabbed, not seated).
bool IsLocalGrabber(void* actor, void* localPlayer) {
    if (!localPlayer || !actor) return false;
    ue_wrap::engine::MainPlayerGrabState gs{};
    if (!ue_wrap::engine::ReadMainPlayerGrabState(localPlayer, gs)) return false;
    // Dual-field (mirror local_streams.cpp:197/203): primary grabbing_actor, fallback holding_actor.
    return gs.grabbingActor == actor || gs.holdingActor == actor;
}
bool IsLocalAuthority(void* actor, void* localPlayer) {
    return IsLocalOccupant(actor, localPlayer) || IsLocalGrabber(actor, localPlayer);
}
```
Replace the three `IsLocalOccupant` checks with `IsLocalAuthority`:
- `Tick()` `:270` sender gate → `IsLocalAuthority` (the grabber now reaches the `:279-284` stream block UNCHANGED — `ReadPayload` already reads actor transform + full 6-DoF rotation).
- `OnReliable` `:227` receiver guard → `IsLocalAuthority` (the grabber ignores incoming poses).
- The mirror path is **reused verbatim**: receivers `PrepareMirror` (physics+tick off) + LerpWindow interp — a grabbed ATV mirrors exactly like a driven one. **Air-move needs no new mirror machinery** — full rotation is already streamed.

**`stateBits` extension (deterministic suppression, not inference):** add `bit2 = grabbed` to `stateBits` (set in `ReadPayload` when the authority is the grabber, not the occupant). Receivers don't strictly need it for pose (PrepareMirror is identical), but it lets the receiver **distinguish "driven" vs "grabbed"** for the release path and avoids inferring grab-state from `occupantSlot==0xFF` ambiguity. No payload size change (the field exists).

**The grab→release/throw edge (release-beats-stick).** This is the genuinely new work. Today `atv_sync` has NO release message — an unoccupied ATV just freezes at last pose (`ReleaseMirror` fires only on become-occupant `:273-278` and disconnect `:294-297`). A thrown ATV must un-freeze, inherit velocity, and free-fall/land on receivers. Add an **`AtvRelease`** message mirroring `remote_prop`:

- **Sender (in `atv_sync::Tick`):** track `g_lastGrabbedActor` (the ATV this peer was the grabber of last tick). On the held→not-held edge (`IsLocalGrabber` was true, now false, AND we are not now the occupant):
  1. Read root velocity via `A::GetRootVelocity` (exists, `atv.cpp:83-87`, currently unused) — captured on the same tick the grab clears, exactly as `local_streams.cpp:314-323` reads `GetActorRootPhysicsVelocity` on the prop release edge.
  2. `s->SendReliable(ReliableKind::AtvRelease, {key, linVel, angVel})`.
- **Receiver (`atv_sync::OnAtvRelease`):** strict order matching `remote_prop.cpp:554-559`: **`ReleaseMirror(actor)` FIRST** (re-enables sim+tick), **THEN** `ue_wrap::engine::SetActorRootPhysicsVelocity(actor, lin, ang)` (a kinematic body's velocity write would be ignored / chase a kinematic target). Clear `preparedAsMirror`/`hasPose`/`window` so the ATV resumes its own physics and lands. A non-throw drop (low speed) just settles; a hard throw arcs — the larger ATV mass makes the velocity hand-off matter more than for a light prop.

**FIFO ordering (structural, no delays — `prop_stick_sync` shape):** `AtvState` (pose stream) and the new `AtvRelease` both fall to `Lane::Normal` (`session_lanes.h:71 default`) — the SAME lane PropStickState/PropRelease use (`:69-70`). GNS guarantees in-order delivery **within a lane**, so the last pose always reaches the receiver before the release. And `atv_sync::Tick` already runs inside `TickGameplay` (`subsystems.cpp:290`) **BEFORE** `local_streams::Tick` (`net_pump.cpp:694`), so emitting the release in `atv_sync::Tick` is naturally lane-ordered after the final pose. **No settle delay, no streak gate needed** — unlike props, the ATV mirror's release is driven by an explicit `AtvRelease` (never inferred from a stream gap), so a 1-packet drop can't spuriously release it. Keep `PrepareMirror`'s physics-off until the explicit `AtvRelease`; only `ReleaseMirror+velocity` on it.

**GAP A-bis fix (one-line exclusion in `local_streams.cpp`):** right after `heldActor = gs.grabbingActor` (`:197`), add `if (heldActor && ue_wrap::atv::IsAtv(heldActor)) heldActor = nullptr;` (the ATV is owned by `atv_sync`, not the prop pipeline). `IsAtv` already exists (`atv.cpp:61-67`). This kills the PropPose spam + "no local match" warnings and enforces RULE 2 (one pipeline per concept).

### 3.2 GAP B — host-authoritative eid spawn-broadcast for purchased ATVs (the kerfur v75 shape)

A purchased ATV CANNOT be matched by key (per-peer random until save). The correct identity path — **consistent with how NPCs and props already handle non-deterministic-key runtime spawns** — is the **`EntitySpawn` + deferred class-match adoption** machinery (`coop/npc_adoption`, protocol v75, `EntitySpawnPayload.savePersisted`), NOT a key heuristic. **Pick this, not key-equality** (key-equality is the v74 mistake the kerfur arc already paid for).

**Identity decision:**
- **Default save-placed ATV** → its key is deterministic → **keep the existing key-index path** (RebuildIndex + key-keyed pose stream + connect-snapshot). Do NOT churn what works.
- **Purchased/crafted ATV** → host-allocated `eid`. The host discovers it via the **existing 2s `RebuildIndex` GUObjectArray walk** (the `EX_CallMath` spawn is un-hookable, so a poll is the only discovery path — same as the kerfur). On discovering an ATV not yet eid-tracked **whose key is per-peer / unstable**, the host:
  1. `eid = coop::element::Registry::AllocHostId()`.
  2. broadcasts `EntitySpawn{className="ATV_C", elementId=eid, savePersisted=0, loc/rot}` (savePersisted=0 → a runtime non-save twin; the client fresh-spawns OR adopts by class+nearest-pose).
  3. streams pose by **eid** thereafter (the pose carries eid, not key, for these ATVs).

**How a joiner and an already-connected peer both come to mirror a freshly-bought ATV:**
- **Already-connected peer:** host's next `RebuildIndex` (≤2s) discovers the new ATV → host emits `EntitySpawn(eid)` to all clients → each client binds its local ATV twin to that eid by **deferred class-match poll** (the buyer adopts ITS OWN locally-spawned ATV; non-buyer clients fresh-spawn a mirror) → pose streams by eid.
- **Joiner (connect-snapshot):** host's `QueueConnectBroadcastForSlot` additionally replays an `EntitySpawn(eid)` for each eid-tracked (purchased) ATV before its pose, so the joiner learns the ATV exists, then adopts/spawns + mirrors it.

**Convergence/distinction rule (NOT a heuristic):** "purchased vs default" is decided by **key determinism**, which the existing `keysHash` diagnostic already measures (`atv_sync.cpp:191-193` logs a per-peer `keysHash`). The implementation's first step is to **confirm divergence with that keysHash (host vs client) on a real purchased ATV** before wiring the eid path — if a purchased ATV's key turns out stable (e.g. the game pre-seeds it), the cheap key-path already covers it and gap B is moot. The RE strongly indicates divergence (identical `getKey→lib::assignKey→generateRandomKey` chain as the kerfur), so plan for the eid path.

**Scope note:** Gap B is the larger, riskier change (new eid identity track inside a key-keyed module). Recommend **shipping Gap A first** (self-contained, high user value — the default base ATV is what most players grab) and **Gap B second** behind the keysHash confirmation. Both are RULE-1 root-cause; neither is a crutch.

### 3.3 Precedents cited
- **MTA:** `CUnoccupiedVehicleSync.cpp` (single-syncer election + START/STOP-SYNC for unoccupied vehicles) → the "occupant OR grabber = single authority" model. `CElementRPCs` `SET_ELEMENT_FROZEN`/`ATTACH_ELEMENTS` (state messages, not event replay) → the release-as-state-message shape.
- **`local_streams.cpp:180-337`** (held-prop `PropPose` stream: `grabbing_actor` read at `:197`, stream at `:251-291`, release-edge velocity capture at `:303-334`) → the carry-stream shape.
- **`remote_prop.cpp::OnRelease` `:553-559`** (simulate-true BEFORE velocity write) → the `AtvRelease` apply order.
- **`prop_stick_sync` (`prop_stick_sync.h:30-52`, `.cpp:82` Tick-before-local_streams)** → the FIFO-lane release-after-pose structural ordering.
- **`coop/npc_adoption.h` + `EntitySpawnPayload` (protocol v75)** → the gap-B host-eid + deferred class-match adoption.

## 4. FILE-BY-FILE CHANGE PLAN

### Gap A (ship first)

**`src/votv-coop/src/ue_wrap/atv.cpp` / `include/ue_wrap/atv.h`** (110 LOC — ample room)
- No new functions strictly required: `IsAtv`, `GetRootVelocity`, `PrepareMirror`, `ReleaseMirror`, `GetRootTransform` all already exist. (Optional convenience `IsGrabbedByPlayer(atv, player)` — but per Principle 7 the grab read belongs in `coop` via `engine::ReadMainPlayerGrabState`, NOT in `ue_wrap::atv` which owns no player/grab knowledge. **Keep the grabber test in `atv_sync` (coop layer).**)

**`src/votv-coop/src/coop/atv_sync.cpp`** (277 LOC → ~+50 LOC; stays well under 800)
- Add `IsLocalGrabber(actor, localPlayer)` (dual-field `ReadMainPlayerGrabState`) + `IsLocalAuthority(actor, localPlayer)` (occupant OR grabber).
- Swap `IsLocalOccupant` → `IsLocalAuthority` at `:227` (receiver guard) and `:270` (sender gate).
- In `ReadPayload`: set `stateBits bit2` when the authority is the grabber (read once, pass an `isGrabbed` arg or re-test).
- Add `g_lastGrabbedActor`/`g_lastGrabbedKey` (or per-actor `wasGrabber` flag on `AtvEntry`) + the release-edge detection in `Tick` → `SendReliable(AtvRelease, {...})` with `A::GetRootVelocity`.
- Add `OnAtvRelease(const AtvReleasePayload&, uint8_t senderSlot)`: `ReleaseMirror` then `SetActorRootPhysicsVelocity`; clear interp state.
- Add `void OnAtvRelease(...)` to `atv_sync.h`.

**`src/votv-coop/src/coop/local_streams.cpp`** (362 LOC) — one line after `:197`: `if (heldActor && ue_wrap::atv::IsAtv(heldActor)) heldActor = nullptr;` + `#include "ue_wrap/atv.h"`.

**`src/votv-coop/include/coop/net/protocol.h`** — new `struct AtvReleasePayload { WireKey key; float linX,linY,linZ, angX,angY,angZ; }` (32+24 = 56B, `static_assert`). New `ReliableKind::AtvRelease = <next>`. Bump `kProtocolVersion` 75 → **76** with a changelog line.

**Three-place ReliableKind wiring (the `feedback-reliablekind-router-checklist` rule):**
1. **enum + payload** — `protocol.h` (above).
2. **family dispatcher case** — `event_dispatch_state.cpp`: add `case ReliableKind::AtvRelease` → range-check + `std::memcpy` + `atv_sync::OnAtvRelease`.
3. **master-router list (the silent third)** — `event_feed.cpp:389-413`: add `case net::ReliableKind::AtvRelease:` to the keyed-device-state family fallthrough so it routes to `DispatchStateReliable`.
- **Relay whitelist** — `session_lanes.h:97` `IsClientRelayableReliableKind`: add `case ReliableKind::AtvRelease: return true;` (a client grabber's release must reach other clients, like `AtvState`).
- **Lane** — `AtvRelease` falls to `Lane::Normal` via `default:` (same lane as `AtvState`); no explicit case needed, but optionally pin it next to `PropRelease` for clarity.

### Gap B (ship second, after keysHash confirmation)

**`atv_sync.cpp`** — add an eid-track alongside `g_atvs`: on `RebuildIndex` discovering an unstable-key ATV, host `AllocHostId` + `SendReliable(EntitySpawn)`; pose stream + connect-snapshot for eid-tracked ATVs key on eid. **Watch the 800-LOC soft cap** — gap B pushes `atv_sync.cpp` toward ~400+ LOC; if it crosses ~600 with the eid machinery, **extract the eid/adoption logic to `coop/atv_adoption.{h,cpp}`** (mirroring `npc_adoption`) in a separate commit before adding it (the extraction-trigger rule).
- Reuse `coop/npc_adoption`'s shape (or a thin `atv_adoption`): `ArmAdoption` on `EntitySpawn`, deferred GUObjectArray class-match poll, `SpawnFreshNpcMirror`-equivalent fallback, ghost-sweep gated on SnapshotComplete.
- `QueueConnectBroadcastForSlot` — replay `EntitySpawn(eid)` before pose for eid-tracked ATVs.
- `EntitySpawnPayload.savePersisted=0` for purchased ATVs (runtime, no shared key) → client fresh-spawns or class-adopts.

## 5. RISKS / TEST PLAN

**Primary repros (the user-stated cases):**
1. **Grab + air-move (Gap A):** Peer A walks to the **default base ATV**, presses E (grav-grab), lifts it and flings it through the air. Peer B (and host) must see it **track in real time, including roll/flip**, then **free-fall + land** on release (not freeze mid-air). Verify on host-grabber AND client-grabber (host relays the client grabber's stream).
2. **Buy a 2nd ATV (Gap B):** Peer buys an ATV from the shop; it's delivered to the orderPlace. Both an **already-connected peer** and a **fresh joiner** must see the new ATV appear and be able to **mirror its grab/drive**.

**Edge cases to test:**
- **Grab while another peer drives a DIFFERENT ATV** — independent authorities, no cross-talk (keyed/eid per-actor).
- **Two peers grab the SAME ATV** — physically the second grab steals the PHC; only one peer's `grabbing_actor` points at it at a time. The receiver guard (`IsLocalAuthority` at `:227`) means a peer ignores incoming poses only for an ATV IT holds — so the active holder streams, the ex-holder reverts to mirror. Verify no oscillation/tug-of-war; if both briefly claim authority the FIFO lane + last-writer-wins pose converges.
- **Throw/release velocity** — light drop settles in place; hard fling arcs and lands. Confirm `SetActorRootPhysicsVelocity` ordering (simulate-true first).
- **Host vs client grabber** — both directions; client grabber's `AtvState`+`AtvRelease` relayed by host to other clients (`IsClientRelayableReliableKind`).
- **Disconnect mid-grab** — `OnDisconnect` (`:293-302`) already `ReleaseMirror`s every mirrored ATV → the held ATV un-freezes (doesn't stay floating) on remaining peers.
- **ATV flips/tips** — full 6-DoF already streamed; verify roll mirrors.
- **Grab then SIT (authority handoff grabber→occupant)** — `Tick` `:273-278` `ReleaseMirror` on become-occupant already handles the mirror→driver flip; confirm `IsLocalGrabber` clears when seated.
- **Gap-A-bis regression** — after the `IsAtv` exclusion, confirm the prop pipeline no longer logs "PropPose no local match" for a grabbed ATV (read both logs).

**Diagnostic to run BEFORE finalizing the grabber field-gate (per `feedback-re-blueprints-before-probes`, this is the one thing the bytecode can't fully settle):** a 1-frame grab-state probe on a grabbed ATV logging `gs.grabbingActor`/`gs.holdingActor` vs the ATV pointer — confirms which field the light PHC grab actually populates for the ATV. The dual-field predicate is robust either way, but the probe removes the last uncertainty cheaply. **Run this in an autonomous scenario, not the hands-on `play` (no `HighResShot`).**

**Gap-B confirmation gate:** before wiring the eid path, log the per-peer `keysHash` (`atv_sync.cpp:191`) for a freshly-purchased ATV on host vs client. Host==client → key path already covers it (gap B moot). Host!=client → proceed with the `EntitySpawn`+adoption path.

**Modularity/perf flags:** `atv_sync.cpp` 277 LOC (Gap A → ~330, fine). `local_streams.cpp` 362 (one line). `atv.cpp` 110. **Gap B is the cap risk** → extract `coop/atv_adoption.{h,cpp}` if `atv_sync.cpp` would cross ~600. No new per-frame GUObjectArray walks (reuse the existing 2s-throttled `RebuildIndex`); `IsLocalGrabber` adds one `ReadMainPlayerGrabState` per indexed ATV per tick — cheap, but if ATV count grows, read grab-state ONCE per tick and compare, not per-ATV.

**Files (all absolute):**
- `D:\Projects\Programming\VOTV_MP\src\votv-coop\src\coop\atv_sync.cpp` (gate widen + AtvRelease + grabber detection)
- `D:\Projects\Programming\VOTV_MP\src\votv-coop\include\coop\atv_sync.h` (OnAtvRelease decl)
- `D:\Projects\Programming\VOTV_MP\src\votv-coop\src\ue_wrap\atv.cpp` / `include\ue_wrap\atv.h` (no change for Gap A; existing `GetRootVelocity`/`IsAtv`/`Prepare/ReleaseMirror` reused)
- `D:\Projects\Programming\VOTV_MP\src\votv-coop\src\coop\local_streams.cpp` (`IsAtv` exclusion, line ~197)
- `D:\Projects\Programming\VOTV_MP\src\votv-coop\include\coop\net\protocol.h` (AtvReleasePayload + ReliableKind::AtvRelease + version bump 76)
- `D:\Projects\Programming\VOTV_MP\src\votv-coop\src\coop\event_dispatch_state.cpp` (AtvRelease case → atv_sync::OnAtvRelease)
- `D:\Projects\Programming\VOTV_MP\src\votv-coop\src\coop\event_feed.cpp` (master-router fallthrough, ~line 398)
- `D:\Projects\Programming\VOTV_MP\src\votv-coop\src\coop\net\session_lanes.h` (relay whitelist for AtvRelease)
- Gap B: new `D:\Projects\Programming\VOTV_MP\src\votv-coop\src\coop\atv_adoption.{h,cpp}` (if extracted) + `atv_sync.cpp` eid track + `subsystems.cpp` connect-snapshot EntitySpawn replay.