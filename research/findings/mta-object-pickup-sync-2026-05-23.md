# MTA — object pickup / held-physics sync pattern
Date: 2026-05-23
Topic: How MTA conveys "player A is grabbing/dragging world object X" to peers.
Author: Recon agent (static analysis of `reference/mtasa-blue/`)

## TL;DR

MTA has **no native "press-E lift-an-object" mechanic** (GTA:SA has no such gameplay
verb). The closest behavioural twins are:

1. **`attachElements()` API** — script-driven rigid binding of one entity to another.
   Once attached, the GTA:SA engine *natively* updates the attached entity's transform
   each frame relative to its parent. **Position is derived locally on each peer — there
   is no per-frame stream for an attached object.**
2. **`CObjectSync` / `CUnoccupiedVehicleSync` "syncer" pattern** — the per-entity
   authoritative player (the *syncer*) streams pos/rot/health to the server, which
   relays it to peers in range. Ownership transfers automatically based on proximity,
   and on contact ("push") for vehicles.
3. **`SET_ELEMENT_VELOCITY` one-shot RPC** — a single broadcast that hands a velocity
   vector to all peers (the "throw").

For VOTV's UE4 PhysicsHandle + Constraint pickup, **the right shape is a hybrid**:
attach-event (one packet) + parent-derived transform on peers + a release packet that
hands over impulse (and optionally returns the prop to a continuous syncer-style stream
afterwards). Details + rationale below.

---

## 1. The "attach" path — the conceptual primary

### 1.1 Wire shape

- Server-side broadcast (lua `attachElements`):
  - `Server/mods/deathmatch/logic/CStaticFunctionDefinitions.cpp:1602-1654`
  - On success, writes a `CElementRPCPacket(pElement, ATTACH_ELEMENTS, …)` containing:
    - `ElementID usAttachedToID`     (4 bytes)
    - `float vecPosition.{x,y,z}`    (12 bytes — local offset)
    - `float vecRotation.{x,y,z}`    (12 bytes — local rotation, radians on the wire)
  - **Total payload: 28 bytes**, broadcast `BroadcastOnlyJoined`.

- Client receiver:
  - `Client/mods/deathmatch/logic/rpc/CElementRPCs.cpp:313-352` (`AttachElements`)
  - Reads the same fields, fires `onClientElementAttach` event, then
    `pSource->SetAttachedOffsets(...)` + `pSource->AttachTo(pAttachedToEntity)`.

### 1.2 Detach wire shape

- Server: `Server/mods/deathmatch/logic/CStaticFunctionDefinitions.cpp:1656-1709`
  - Detach packet contains: `ucTimeContext`, final `vecPosition`, final `vecRotation`
    — i.e., the world transform at the moment of detach (lets peers land the object
    at the right place after the engine releases the bind).
- Client receiver: `Client/mods/deathmatch/logic/rpc/CElementRPCs.cpp:354-405`
  (`DetachElements`).

### 1.3 Initial-state delivery to a player joining late

- `Server/mods/deathmatch/logic/packets/CEntityAddPacket.cpp:122-137` — on first
  streaming of an entity to a player, the "attached-to" element ID + offsets are
  baked into the entity-add packet. A late joiner doesn't need an additional RPC.

### 1.4 What drives the attached object's per-frame transform?

**Nothing on the wire. The GTA:SA engine does it natively.**

- `Client/mods/deathmatch/logic/CClientEntity.cpp:665-719` (`InternalAttachTo`) calls
  `CPhysical::AttachEntityToEntity(parent, vecPos, vecRot)`.
- `Client/game_sa/CPhysicalSA.cpp:289-294` dispatches to GTA:SA's native
  `FUNC_AttachEntityToEntity = 0x54D570` (`Client/game_sa/CPhysicalSA.h:24`).
- For the rare cases the native engine doesn't update (low-LOD objects, two-deep
  attach chains), MTA falls back to a per-frame manual update:
  - `CClientEntity::DoAttaching()` — `Client/mods/deathmatch/logic/CClientEntity.cpp:1183-1196`
    — computes the world matrix from `m_pAttachedToEntity->GetMatrix() * offsets`.
  - Called from `CClientObject::StreamedInPulse()` only when needed
    (`Client/mods/deathmatch/logic/CClientObject.cpp:624-655`).

**Key takeaway**: when MTA can lean on engine-native attach, the wire cost of a held
object is **one 28-byte RPC at pickup + one ~30-byte RPC at drop, and nothing in
between**. The illusion of "the object follows the player" is purely
parent-derived on each peer.

---

## 2. The `CObjectSync` / "syncer" path — the continuous-stream alternative

### 2.1 Status: **gated off in upstream**

- `Server/mods/deathmatch/logic/CObjectSync.cpp:15` and `:238` — entire file wrapped
  in `#ifdef WITH_OBJECT_SYNC`.
- Grep across the whole tree finds no `#define WITH_OBJECT_SYNC` and the build
  doesn't pass the flag — `CObjectSync` is **dormant code preserved for reference**.
- The closest *enabled* analogue is `CUnoccupiedVehicleSync`, which is the right
  thing to study for an "object physics-simulated by closest player" feature.

### 2.2 The shape (still useful as a pattern)

- Wire packet: `Server/mods/deathmatch/logic/packets/CObjectSyncPacket.{h,cpp}`
- Per-object payload, written by the *syncer* client at rate
  `g_TickRateSettings.iObjectSync = 500 ms` (`Shared/mods/deathmatch/logic/CTickRateSettings.h:22`):
  - `ElementID ID`               (4 bytes)
  - `unsigned char ucSyncTimeContext` (1 byte — replay/old-packet guard)
  - `SIntegerSync<u8, 3> flags`  (3 bits packed)
  - if `flags & 0x1`: `SPositionSync vecPosition` (12 bytes float)
  - if `flags & 0x2`: `SRotationRadiansSync vecRotation` (12 bytes)
  - if `flags & 0x4`: `SObjectHealthSync fHealth` (~2 bytes packed)
- Flags = **delta-only**: only changed fields go on the wire
  (`Client/mods/deathmatch/logic/CObjectSync.cpp:211-268`, `WriteObjectInformation`).
- Reliability: `PACKET_RELIABILITY_UNRELIABLE_SEQUENCED`, `PACKET_PRIORITY_MEDIUM`
  (`Client/mods/deathmatch/logic/CObjectSync.cpp:207`).

### 2.3 Syncer-ownership rules (server-side)

`Server/mods/deathmatch/logic/CObjectSync.cpp`:

- `UpdateObject` (line 83) runs every `SYNC_RATE = 500 ms`:
  - Skip if `!IsSyncable()` or `(IsStatic() && !IsBreakable())` — static furniture
    never needs sync.
  - If current syncer is more than `MAX_PLAYER_SYNC_DISTANCE = 100 m` away (or in a
    different dimension), call `StopSync()`, then `FindSyncer()`.
- `FindPlayerCloseToObject` (line 165) picks the closest joined player; **ties are
  broken by `pPlayer->CountSyncingObjects()`** — prefer a less-loaded player. Cheap
  load balancing.
- `StartSync(player, object)` sends `CObjectStartSyncPacket` to that one player; the
  client adds the object to its sync list and begins reporting its transform every
  500 ms via `CObjectSync::Sync()`.
- `OverrideSyncer(object, player, bPersist)` is the script-level escape hatch (used
  by `setElementSyncer` Lua API) — also used by `CUnoccupiedVehiclePushPacket` for
  contact-based transfer.
- The syncer's report is *trusted* (server only re-broadcasts; it does not
  re-simulate). Anti-cheat is via `ucSyncTimeContext` (replayed/stale rejection).
- Mass / turn-mass / air-resistance / elasticity / center-of-mass are
  **purely client-local** (`SetObjectMass` is local-only on both client and server;
  no RPC). They are baked into the GTA:SA `CObject` interface on each peer
  independently. **Mass is not in the wire.**

### 2.4 The "push transfer" — directly analogous to "press E"

`Server/mods/deathmatch/logic/CUnoccupiedVehicleSync.cpp:476-510`
(`Packet_UnoccupiedVehiclePushSync`):

- Client sends a `CUnoccupiedVehiclePushPacket` when its local physics pushes an
  unoccupied vehicle (collision contact).
- Server validates: player is in radius `iVehicleContactSyncRadius = 30 m`, same
  dimension, no driver, anti-spam window `MIN_PUSH_ANTISPAM_RATE = 1500 ms`.
- On success: `OverrideSyncer(vehicle, player)` — the pusher *becomes* the syncer,
  immediately and authoritatively.

**This is the right shape for "player E-grabs a prop":** the grab is the
ownership-transfer trigger. The pusher/grabber's client owns the entity's transform
state until released.

---

## 3. The "throw" — one-shot velocity broadcast

`SET_ELEMENT_VELOCITY` is a regular element RPC (not an interpolated stream).

- Server-side dispatch:
  `Server/mods/deathmatch/logic/CStaticFunctionDefinitions.cpp:1403-1440` — writes
  the velocity vector once and `BroadcastOnlyJoined`.
- Client receiver:
  `Client/mods/deathmatch/logic/rpc/CElementRPCs.cpp:170` (`SetElementVelocity`) —
  applies the velocity to the underlying GTA:SA `CPhysical::SetMoveSpeed`.
- After the RPC, **the engine on each peer continues the simulation locally**. They
  may diverge in fine detail; the syncer's subsequent `CObjectSync` stream (or, in
  the enabled-path, `CUnoccupiedVehicleSync` stream) re-converges peers within a
  half-second.

In effect: throw = "broadcast initial velocity to give every peer a plausible
trajectory, then let the syncer stream correct drift at the regular tick."

---

## 4. The rates / bandwidth budget

| Constant                              | Value  | File:line                                                     |
|---------------------------------------|--------|---------------------------------------------------------------|
| `iObjectSync`                         | 500 ms | `Shared/mods/deathmatch/logic/CTickRateSettings.h:22`         |
| `iUnoccupiedVehicle`                  | 400 ms | `Shared/mods/deathmatch/logic/CTickRateSettings.h:21`         |
| `iPedSync` (player ped)               | 400 ms | `Shared/mods/deathmatch/logic/CTickRateSettings.h:19`         |
| `iPureSync` (driving / car-anim)      | 100 ms | `Shared/mods/deathmatch/logic/CTickRateSettings.h:16`         |
| `MAX_PLAYER_SYNC_DISTANCE` (objects)  | 100 m  | `Server/mods/deathmatch/logic/CObjectSync.cpp:18`             |
| `iUnoccupiedVehicleSyncerDistance`    | 130 m  | `Shared/mods/deathmatch/logic/CTickRateSettings.h:27`         |
| `iVehicleContactSyncRadius` (push)    | 30 m   | `Shared/mods/deathmatch/logic/CTickRateSettings.h:28`         |
| `MIN_PUSH_ANTISPAM_RATE`              | 1500 ms| `Server/mods/deathmatch/logic/CUnoccupiedVehicleSync.h:15`    |

Object sync = **2 Hz** of pos/rot deltas per active object. Player ped = 2.5 Hz
(full ped sync) or 10 Hz (pure-sync while driving). This is MTA's design baseline.

---

## 5. The wire DOES NOT carry…

(Audited — these were investigated and confirmed absent from any object-related packet.)

- **Mass / `fMass`** — Lua `setObjectMass` calls `CDeathmatchObject::SetMass` only
  on the local peer (`Client/.../CStaticFunctionDefinitions.cpp:4262-4276` and
  `Server/.../CStaticFunctionDefinitions.cpp` mass codepath does the local set but
  no broadcast). Mass is presumed identical on every peer because it's a
  client-config / model attribute.
- **Linear/angular velocity for an *attached* object.** Disabled on purpose:
  `Client/mods/deathmatch/logic/CObjectSync.cpp:105` — "No velocity due to issue
  #3522".
- **"Holding pose" flags** — no boolean like `bIsBeingHeld`. The visual "lift"
  state is inferred from the attach relationship and from the *holding* ped's
  animation state (the ped's keysync packet carries the anim — and GTA:SA peds
  don't have a "carry crate" anim in the engine, so MTA scripted-pickup setups
  pair `attachElements` with a `setPedAnimation` RPC).

---

## 6. Translating to VOTV / UE4

### 6.1 What maps cleanly

| MTA concept                            | VOTV UE4 equivalent                                        |
|----------------------------------------|------------------------------------------------------------|
| `attachElements()`                     | `USceneComponent::AttachToComponent` on the grabbed prop, parented to the holder's camera/hand socket on each peer |
| `AttachTo` clears physics + parents    | `UPrimitiveComponent::SetSimulatePhysics(false)` while held, parent transform follows |
| GTA's native attach-tick               | UE's scene-graph: child's world transform updates from parent automatically per tick |
| `CObjectSync` "syncer" stream          | The "holding peer" is the temporary authoritative source for the prop's transform |
| `CUnoccupiedVehiclePushPacket` syncer transfer | The "press E" packet that asserts grabber-ownership |
| `SET_ELEMENT_VELOCITY` RPC             | The "release/throw" packet carrying impulse vector         |
| `ucSyncTimeContext` (8-bit gen)        | We already have an RFC1982 seq; the per-prop generation can ride on a single u8 |
| `CEntityAddPacket` "attached-to" baked in | Our session-join packet should ship the current grab state for every grab-pinned prop |

### 6.2 What does **not** map cleanly

- **Physics-handle / spring-constraint feel.** In VOTV the prop is *not* rigidly
  parented to the camera — it's a `PhysicsHandle` (lift mode) or a `PhysicsConstraint`
  (drag mode) with finite stiffness and damping. The local engine on the holder
  computes a wobbly trajectory from input + constraint forces. Pure parent-attach on
  the receiving peer would drop the wobble and look stiff.

  → **Implication**: a pure attach-only stream (MTA's primary path) is **insufficient
  fidelity** for VOTV. The held prop's actual world pose isn't a fixed offset from
  the camera — it lags, swings, collides. Receivers need either (a) a continuous
  pose stream from the syncer, or (b) to run the same handle/constraint locally
  with the holder's input echoed.

- **Heavy vs light visual.** MTA has no built-in concept; everything is script-set.
  In VOTV the lift-vs-drag decision is made *locally* on the holder based on the
  prop's mass. **We can replicate VOTV's behaviour by sending only the mode flag (1
  bit) and letting each peer pose its own puppet arms accordingly** (the prop's
  mass is identical on every peer because they all loaded the same SP-authored
  asset — same as MTA's mass assumption).

- **No server.** VOTV is host-authoritative P2P (host = pseudo-server in the
  MTA-shape session model already used in `project-phase3-udp-transport`). The
  syncer pattern collapses to "host owns every prop by default; transfers
  ownership to a peer on grab; transfers back on release after `N` ms".

### 6.3 The pickup is a *physics-state ownership change*, not just an attach

A subtle but important distinction:

- In MTA, an `attachElements` parent-bind **disables physics** on the child
  (GTA:SA's native attach detaches the child from the physics sim). So MTA's
  attached-object pattern is **kinematic** — no peer-side physics for held things.
  Position is parent + offset, period.

- In VOTV, even when held, the prop is **still physically simulated** on the
  holder's client (that's how lift/drag wobble works). The other peer's instance
  needs to *mirror* that simulation, and the only honest way is to stream the
  prop's pose from the holder.

This pushes VOTV toward the `CObjectSync` continuous-stream path, **not** the
attach-only path — even though the attach-only path is what MTA actually ships
(because GTA's mechanic is kinematic, not physical).

---

## 7. Recommended wire protocol for VOTV (concrete shape)

### 7.1 Three packet types

**(a) GRAB event (reliable, one-shot)**
- Sent by the host (after the grabbing peer asks for it, or by the host directly
  on its own grab).
- Broadcast to all peers.
- Wire: `prop_id u32 | holder_peer_id u16 | grab_mode u8 (0=lift,1=drag) | grab_seq u8 | local_offset Vec3 | grab_distance f32`
- On receive each peer:
  - Marks the prop as "held by `holder_peer_id`" in a per-prop registry.
  - Disables the prop's *engine* physics? **No — keep it on**. Just stop the host
    from running its own host-side authority over this prop. Receivers will be
    driven by the upcoming continuous-stream packets.
  - Plays the appropriate "lift" or "drag" pose on the holder's puppet (no extra
    bits needed: `grab_mode` already says which).

**(b) HELD-PROP POSE stream (unreliable, sequenced)**
- Sent by the *holder's* client at the existing pose-sync tick rate (current VOTV
  build pumps puppet pose at 60 Hz interp-driven from 20-Hz packets per
  `project-remote-player-interp`; reuse that rate).
- Bundle into the same UDP datagram as the player-pose packet so it costs ~0 frames
  of extra latency.
- Wire (per held prop, attached to the player-pose packet):
  `prop_id u32 | pos Vec3 (12B) | rot Quat 32-bit-compressed (e.g. smallest-three, 4B) | linear_vel Vec3 (12B) | seq u8`
- ~33 bytes per held prop per packet. With a typical 1 held prop and the existing
  pose tick at 20 Hz that's 660 B/s — negligible.
- Receivers feed this directly into the same pose-interpolator used for `RemotePlayer`
  (see `project-remote-player-interp`). The 50 ms LERP window, velocity-scaled snap,
  and dirty-skip rules carry over.

**(c) RELEASE / THROW event (reliable, one-shot)**
- Sent by the holder when E is released.
- Wire: `prop_id u32 | release_pos Vec3 | release_rot Quat | release_linear_vel Vec3 | release_angular_vel Vec3 | release_seq u8`
- On receive each peer:
  - Clears the "held by …" registry entry.
  - Sets the prop's velocity to the released vector (UE4 `SetPhysicsLinearVelocity`
    / `SetPhysicsAngularVelocity`).
  - **Host then takes ownership back** and begins broadcasting the prop's pose at a
    lower rate (e.g. 5 Hz `iObjectSync`-style) until the prop comes to rest, then
    stops. Resting detection: `linear_vel.SizeSquared() < eps && angular_vel.SizeSquared() < eps` for N consecutive ticks.

### 7.2 Mass / heavy flag

**Don't sync mass.** Both peers loaded the same prop asset → same mass at runtime.
The host's grab-decision (lift vs drag) is the only mode bit on the wire (1 byte in
the GRAB event). Each peer's puppet plays a different arm pose based purely on
`grab_mode`.

### 7.3 Ownership / authority model

**Host-authoritative by default, transfer-on-grab.**

- Default: host's instance is authoritative for every prop. Host broadcasts pose at
  low rate when prop is in motion; goes silent when at rest.
- On GRAB: ownership transfers to the grabbing peer (host or client) for the
  duration of the hold. The owner streams pose continuously.
- On RELEASE: ownership returns to host. Host continues low-rate broadcast until
  rest.
- This is *exactly* the `CUnoccupiedVehiclePushPacket` → `OverrideSyncer()` shape
  from MTA, lifted into a P2P-with-host-relay model.

### 7.4 Anti-cheat / robustness (mirrors MTA's `ucSyncTimeContext`)

- Per-prop 8-bit `grab_seq` (incremented on every GRAB or RELEASE on the host)
  rejects stale GRAB/RELEASE packets after an ownership change.
- Host accepts a peer's HELD-PROP POSE only if `peer == prop.current_holder`.
- Host validates `release_pos` is within a sanity radius of the holder's last known
  position (cheap teleport guard).
- Same NaN / AABB validation already used for player pose (per `project-phase3-udp-transport`).

### 7.5 Why NOT the pure-attach pattern

It's tempting to copy MTA's primary shape (attach-only, no per-frame stream). It
**won't work for VOTV** because:

1. UE4 attachment is rigid — the prop's transform = parent transform * offset
   exactly. VOTV's pickup uses a physics handle that lags + wobbles + collides.
   A pure-attach receiver shows a stiff prop glued to the camera; the holder sees
   a swinging crate. Visible desync, bad fidelity.
2. The prop collides with the world on the holder's side. If it's parent-attached
   on receivers, it cannot collide (UE will continuously snap it back to
   parent-offset). Drag mode in particular is the prop *bouncing off geometry* — a
   parent-attached receiver loses that entirely.
3. Throwing requires a final velocity broadcast; MTA does this with a separate
   `SET_ELEMENT_VELOCITY` RPC. We need the equivalent regardless, so we're not
   actually saving bandwidth by going attach-only.

→ The MTA *attach pattern is conceptually instructive* (it tells us pickup is an
**ownership/identity** event, not a stream-of-positions event) but the
*implementation* must be continuous-stream because UE4 physics handles aren't
kinematic.

---

## 8. Open questions for the architect

1. **Does VOTV's E-pickup work on every physics prop, or a curated set?** If
   curated (e.g., only props with a specific UE tag/class), the per-prop registry
   is bounded and we can index by a small `prop_id` instead of a 32-bit hash. MTA
   uses `ElementID` (16-bit), which is plenty.
2. **Throw velocity authority.** Does the holder's local prediction of the
   throw velocity (camera direction + force) need to be re-validated by the host?
   MTA trusts the syncer entirely. We probably can too, with a sanity cap on
   magnitude.
3. **Two peers grab the same prop in the same frame.** MTA's `MIN_PUSH_ANTISPAM_RATE
   = 1500 ms` handles this for vehicles. For VOTV the host arbitrates: first
   GRAB request to reach the host wins; the other gets a "denied" reply and its
   local grab is rolled back. Needs a small reliable RPC for the grant/deny.

---

## 9. File reference index

MTA primary attach path:
- `Client/mods/deathmatch/logic/CClientEntity.h:248` — `DoAttaching()` declaration.
- `Client/mods/deathmatch/logic/CClientEntity.cpp:630-719` — `AttachTo` + `InternalAttachTo`.
- `Client/mods/deathmatch/logic/CClientEntity.cpp:1183-1196` — `DoAttaching` (fallback).
- `Client/mods/deathmatch/logic/CClientObject.cpp:191-200, 624-655` — object-specific paths.
- `Client/game_sa/CPhysicalSA.cpp:289-294` + `CPhysicalSA.h:24` — native engine
  `FUNC_AttachEntityToEntity = 0x54D570`.
- `Client/mods/deathmatch/logic/rpc/CElementRPCs.cpp:313-405` — `AttachElements` /
  `DetachElements` RPC receivers.
- `Server/mods/deathmatch/logic/CStaticFunctionDefinitions.cpp:1602-1709` —
  `AttachElements` / `DetachElements` server-side broadcasters.
- `Server/mods/deathmatch/logic/packets/CEntityAddPacket.cpp:122-137` —
  attached-state baked into entity-add for late joiners.

MTA syncer / continuous-stream pattern:
- `Server/mods/deathmatch/logic/CObjectSync.cpp` (entire file, dormant under
  `#ifdef WITH_OBJECT_SYNC`).
- `Server/mods/deathmatch/logic/packets/CObjectSyncPacket.{h,cpp}` — wire format
  (`ID | timeContext | flags(3 bit) | [pos] [rot] [health]`, delta-only).
- `Client/mods/deathmatch/logic/CObjectSync.cpp:193-268` — client-side `Sync()`
  and `WriteObjectInformation()`.
- `Server/mods/deathmatch/logic/CUnoccupiedVehicleSync.cpp:84-156` (`UpdateVehicle`),
  `:165-192` (`FindSyncer`), `:476-510` (`Packet_UnoccupiedVehiclePushSync` — the
  press-E-equivalent ownership transfer).
- `Shared/sdk/net/SyncStructures.h:723-849` — `SUnoccupiedVehicleSync` flag-bits
  delta layout.

Throw / velocity:
- `Server/mods/deathmatch/logic/CStaticFunctionDefinitions.cpp:1403-1440` —
  `SetElementVelocity` server broadcaster.
- `Client/mods/deathmatch/logic/rpc/CElementRPCs.cpp:170` — receiver.

Rates / distances:
- `Shared/mods/deathmatch/logic/CTickRateSettings.h:11-46` — all sync tick
  intervals + syncer distances.
- `Server/mods/deathmatch/logic/CObjectSync.cpp:17-18` — `SYNC_RATE = 500 ms`,
  `MAX_PLAYER_SYNC_DISTANCE = 100 m`.
- `Server/mods/deathmatch/logic/CUnoccupiedVehicleSync.h:14-15` —
  `MIN_ROTATION_DIFF`, `MIN_PUSH_ANTISPAM_RATE = 1500 ms`.

Mass (purely local, never on wire):
- `Client/mods/deathmatch/logic/CStaticFunctionDefinitions.cpp:4262-4276` —
  `SetObjectMass` local-only.
- `Client/mods/deathmatch/logic/CClientObject.h:110-111, 153` — `m_fMass` field.
- `Shared/sdk/net/rpc_enums.h:33-280` — confirms no `SET_OBJECT_MASS` RPC.

Status flag for the dormant path:
- `WITH_OBJECT_SYNC` is referenced in 13 files but **defined in zero**. Grep
  results confirm the entire `CObjectSync` server-side scheduler/dispatcher is
  dead code in upstream. The pattern is correct; the upstream chose not to
  enable it (likely because the attach-based path covers script use cases and
  generic per-prop physics streaming is bandwidth-heavy on the GTA:SA-era
  network budget).
