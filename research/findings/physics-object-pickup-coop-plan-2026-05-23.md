# Physics-object pickup sync — converged research + plan (2026-05-23)

The user-requested coop feature: when a player presses **E** (short tap) on
a physics-simulating prop in the VOTV world, the player enters either
**lift mode** (small prop, attached via `UPhysicsHandleComponent` at fixed
distance in front of camera) or **drag mode** (heavy/big prop, dragged via
`UPhysicsConstraintComponent` triad). We need this to be visible + driveable
across both peers — including pose of the held prop, drop, throw.

This doc converges THREE parallel investigations (IDA, SDK-reflection,
MTA-fidelity) per the RULE 1 escalation ladder + [[feedback-check-mta-and-document]].

## Sources

- `research/findings/mta-object-pickup-sync-2026-05-23.md` — MTA precedent + 3-packet protocol
- `research/findings/votv-physics-interaction-surface-2026-05-23.md` — VOTV BP class hierarchy + state surface + per-tick writers
- IDA agent report (verbatim in conversation): native RVAs + hook strategy
- VOTV CXX header dump: `Game_0.9.0n/.../CXXHeaderDump/mainPlayer.hpp`, `prop.hpp`, `comp_gravitygun.hpp`

## The ground truth (cross-validated by 3 agents)

### 1. The dispatch surface is pure Blueprint

mainPlayer_C is fully interpreted Blueprint. `playerTryToGrab`,
`pickupObject`, `pickupObjectDirect`, `switchToHeavyDrag`, `dropGrabObject`,
`throwHoldingProp`, `smoothGrab`, the `InpActEvt_use_*` handlers, and
`ExecuteUbergraph_mainPlayer` have NO native exec thunks in the .exe —
they live in the .pak as bytecode + the FName pool. (IDA verified: every
needle absent under ANSI AND UTF-16.)

**Hook strategy = `UObject::ProcessEvent @ 0x141465930`** (already hooked
by `ue_wrap::game_thread::ProcessEventDetour`), filtered by `UFunction-`
`>Name`. NO new injection or AOB work needed.

### 2. The native physics handle layer DOES exist + has RVAs

`UPhysicsHandleComponent::GrabComponentAtLocation` @ `0x1430c64b0`
(exec thunk; native body at `0x142d7a580` via vtable[133])
`UPhysicsHandleComponent::GrabComponentAtLocationWithRotation` @
  `0x1430c65d0` (native `0x142d7a5b0`)
`UPhysicsHandleComponent::ReleaseComponent` @ `0x142fea9b0`
`UPhysicsHandleComponent::SetTargetLocation` @ `0x1430c6ad0`
  (native `0x142d7d3f0`; writes `this+240 = FVector`)
`UPhysicsHandleComponent::SetTargetLocationAndRotation` @ `0x1430c6b60`
  (native `0x142d7d420`; writes loc@+240, quat@+224, derived@+256)

These give us a **clean cross-cut observation point**: "the prop is now
physically held by THIS player" / "released" / "target moved to (x,y,z)",
regardless of which BP path triggered it. These are dispatchable from the
BP VM through ProcessEvent, so the existing hook catches them. IDB
renamed + saved per [[feedback-ida-rename-and-save]].

### 3. VOTV has ONE prop base class — Aprop_C

`Aprop_C : public AActor` (`prop.hpp`, sizeof 0x363). **456 direct + 85
transitive = ~540 grab-surface classes**, ALL sharing the same BPI for
grab/drop/heavy logic. This is enormous leverage: one wire-level pattern
covers every interactable.

Key fields on `Aprop_C`:

- `Aprop_C.propData @ 0x0260` (Fstruct_prop) — the per-prop data row
  loaded from the master DataTable at Init via `propPicker` keyed on
  `Aprop_C.Name`.
- `Fstruct_prop.heavy @ +0x6C` (bool) — **THE heavy flag.** Data-driven,
  NOT a mass comparison. Both peers compute the SAME answer from the SAME
  DataTable → deterministic; we don't need to send mass on the wire.
- `Aprop_C.Key @ 0x02E0` — **the stable cross-instance identifier.**
  This is the prerequisite for syncing physics interaction — UE4 actor
  pointers are per-process; the prop Key (assigned at spawn) is what
  identifies "the same prop" on both peers.
- `Ucomp_physicsImpact_C.Mass @ +0x01F0` — cached `GetMass() *
  propData.massMultiply` at Init; used only for throw-impulse + impact
  damage. Not on the wire.

### 4. The per-tick writer is smoothGrab()

`AmainPlayer_C::smoothGrab()` is called from `ReceiveTick` and is the
SINGLE place the held prop's world transform is updated per frame
(light-grab path). Heavy drag rides the same tick path through
`Set Heavy Pull Rot()` writing into the `heavyPull` constraint. Hooking
`smoothGrab` (via ProcessEvent filter) gives us the live transform stream
to broadcast to peers.

Light path: `UPhysicsHandleComponent grabHandle @ +0x0688` chasing
`grabrot @ +0x0680` arrow target (camera-relative).
Heavy path: triad `heavyPull/heavyGrab/heavyPull_loc @ +0x05B8/+0x04F0/
+0x04E8` with chase-target meshes `pullComp`/`pullSubcomp`.

### 5. Five multicast delegates already fire on grab/drop events

We don't need to invent hook points — the BP already broadcasts:

| Delegate | Field offset | Fires on |
|---|---|---|
| `heavyObjectGrabbed` | +0x0DF0 | Heavy grab (drag mode begin) |
| `heavyObjectPulled` | +0x0E00 | Heavy pull initial yank |
| `heavyObjectDropped` | +0x0E10 | Heavy drop |
| `objectDropped` | (per SDK agent) | Any drop |
| `holdObjectChanged_pre/post` | (per SDK agent) | Hold actor transitions |

Hook these via ProcessEvent name filter on the BROADCAST UFunctions to
get clean events for the GRAB / RELEASE packets.

### 6. E-key timing: press / release / hold

Three dispatch slots (`InpActEvt_use_K2Node_InputActionEvent_38/41/42`):
press + release for the primary binding plus a third for a secondary
binding. The short-press-vs-long-hold discrimination is
TIMER-based, not BP-edge-based:

1. On press, `simulate_usePressed` starts `timeUseTimerHandle` for
   `holdDelayInit @ +0x0DDC` seconds.
2. If the timer fires before release → `releaseEToUse = true` → the
   long-hold action runs (`hotkeyAction_holdE`, typically "grab the
   thing").
3. If release happens before the timer → short-press action runs
   (`hotkeyAction_pressE`, typically "use" / "interact").

For coop, we don't need to replicate the timer — we just observe the
RESULTING UFunction call (`pickupObject` or short-press equivalent) via
the ProcessEvent hook.

### 7. Mass / size / threshold is NOT on the wire

Confirmed by both SDK and MTA agents:
- VOTV: heavy bit lives in `propData.heavy` (DataTable). Same DataTable
  on both peers, so both compute the same heavy decision from the prop
  Key alone.
- MTA: never streams mass; `SetObjectMass` is local-only on both client
  and server. Same precedent.

Implication: we ONLY need to send a 1-bit `grab_mode` (lift/drag) on
the GRAB packet — and even THAT could be derived locally from the prop
Key. We ship it as a tiebreaker (cheap, robust against future per-prop
override BP nodes the SDK agent flagged in 72 classes).

## The wire protocol — host-authoritative MVP

Mirrors MTA's `CUnoccupiedVehiclePushPacket` ownership-transfer pattern
+ the `ucSyncTimeContext` sequence-number rejection of stale events.

### Packet 1: GRAB (reliable, one-shot)

```c
struct GrabMsg {
    uint32  propKey;        // Aprop_C.Key @ +0x02E0 (stable cross-peer id)
    uint16  holderPeer;     // peer id of the grabber
    uint8   grabMode;       // 0 = lift (PhysicsHandle path)
                            // 1 = drag (constraint triad path)
    uint8   grabSeq;        // per-prop sequence (rejects stale events)
    FVector localAnchor;    // grabRelativeLocation @ +0x0DE4 (offset on the
                            //   primitive the grab handle attached to)
    float   grabDistance;   // grabLen @ +0x09D0 (distance from camera)
};
// ~26 bytes incl. headers
```

### Packet 2: HELD-PROP POSE (unreliable, sequenced — piggyback on existing 20 Hz pose)

Extends `coop::net::PoseSnapshot` with an OPTIONAL trailing held-prop
record (single prop per holder; ~99% case). If multiple props held
later, repeat the record N times preceded by a count byte.

```c
struct HeldPropPose {
    uint32  propKey;
    FVector worldLoc;       // held-prop world location, written each tick by smoothGrab
    FQuat   worldRot;       // compressed to 6-byte or 8-byte quantization
    FVector linearVel;      // optional, for receiver-side smoothing only
    uint8   poseSeq;        // wraps with grabSeq
};
// ~33 bytes per held prop
```

Riding the existing pose channel keeps bandwidth predictable (no new
flow). Drop the trailing held-prop record when `holding_actor == nullptr`
(saves the 33 B when not carrying).

### Packet 3: RELEASE / THROW (reliable, one-shot)

```c
struct ReleaseMsg {
    uint32  propKey;
    uint8   releaseSeq;     // must match grabSeq+1 of the holder
    FVector releaseLoc;
    FQuat   releaseRot;     // compressed
    FVector linearVel;      // initial velocity at release (zero for drop, non-zero for throw)
    FVector angularVel;     // optional
};
// ~50 bytes
```

After release, the host streams the prop's pose at a LOW RATE until rest
detection (epsilon + N consecutive ticks), then goes silent. This
mirrors MTA's "syncer streams briefly post-throw then quiet" pattern.

### Conflict resolution

- Same-frame double-grab (both peers press E on the same prop in the
  same RTT window): host arbitrates. First GRAB to host wins; loser
  gets a reliable DENY packet and rolls back local prediction.
- Stale GRAB/RELEASE rejection: `grabSeq` is per-prop and monotonic.
  After a host-acked grab transfer, prior GRABs with lower seq are
  discarded.

### Host authority

The MVP is host-authoritative on grab ownership:
- Client requests grab → reliable GRAB packet to host.
- Host checks: prop currently ungrabbed? If yes, broadcast GRAB to all
  peers (including the requester, as an ACK). If no, reliable DENY to
  the requester only.
- Holder peer streams HELD-PROP POSE while holding.
- Holder peer sends RELEASE, host broadcasts to all.

Host's own grab follows the same path but skips the request/ack round
trip (host writes the ack to its own state immediately).

## Receiver-side strategy (puppet path)

When a peer's GRAB arrives:

1. Look up the prop locally by `propKey` (DataTable consistency means
   both peers have it).
2. On the LOCAL instance of the prop, `SetSimulatePhysics(false)` — the
   prop becomes kinematic on the puppet's machine so the source's
   streamed pose fully owns its transform.
3. Each HELD-PROP POSE tick → `SetActorLocationAndRotation(worldLoc,
   worldRot)`. Use the same MTA-style linear LERP interpolator already
   shipped for `RemotePlayer` (see [[project-remote-player-interp]]).
4. On the PUPPET (the visual representation of the grabber), drive its
   AnimBP / arm IK to reach the held prop using the satellite-ACharacter
   pattern from [[project-bug2-locomotion-anim]]. For drag mode, the
   puppet's body posture differs (leaning forward) — drive that via a
   new bool on the AnimBP (set by the GRAB packet's `grabMode`).

On RELEASE/THROW:

1. `SetSimulatePhysics(true)` on the LOCAL prop instance.
2. Apply the streamed `linearVel` + `angularVel` to the prop's
   PrimitiveComponent.
3. Local physics continues from there. Source streams briefly until
   `propThrown.fin()` fires, with a "post-throw" bit in the packet
   header signaling "the source has stopped streaming".

## Open issues

1. **Aprop_C.Key cross-peer agreement is a hard prerequisite.** The
   spawn-from-craft / spawn-from-menu paths in VOTV assign Key on the
   spawning machine. For coop, prop spawns MUST route through the host
   so both peers see the same Key. This is OUT OF SCOPE for the
   physics-pickup feature itself but blocks shipping it. Tracked as a
   separate scope item.
   
2. **Post-throw streaming.** Source's `propThrown` watches the first
   hit via `OnComponentHit`, which won't fire on a `SimulatePhysics=
   false` puppet. Mitigation: keep streaming `HELD-PROP POSE` for a
   short post-release window with a "post-throw" bit until source's
   first-hit fires + a short tail. (~250 ms in MTA's vehicle-push
   analogue.)

3. **Gravity gun (`Ucomp_gravitygun_C`).** Uses the same
   `UPhysicsHandleComponent` underneath — wire-level identical, only
   cosmetics (beam/light effects) differ. Cosmetics are local-derived
   from the per-tick HELD-PROP POSE stream + the holder's view direction
   (already in PoseSnapshot).

4. **WidgetInteractionComponent @ +0x0628** (the E-press also hits
   interactive UMG widgets on consoles in-world). NOT in scope for
   physics pickup — but the SHARED E-press input means the same
   `InpActEvt_use_*` handlers dispatch BOTH paths. Filter is via
   `playerTryToGrab` outcome (the BP graph picks one branch).

## Implementation checklist (Phase 4.x or new Phase)

Pending user approval to add to `docs/COOP_SCOPE.md`:

1. **Prerequisite**: prop-Key cross-peer agreement (spawn routing
   through host).
2. **Hook layer**: filter ProcessEvent for the BP UFunctions named
   above (pickupObject, pickupObjectDirect, switchToHeavyDrag,
   dropGrabObject, throwHoldingProp, smoothGrab) + the native
   PhysicsHandle layer (already RVA'd).
3. **State snapshot**: at each hook fire, snapshot pawn fields
   holding_actor/grabbing_actor/grabbing_component/Heavy/grabsHeavy/
   grabLen/grabRelativeLocation.
4. **Wire**: add GrabMsg / ReleaseMsg as reliable channel messages
   (existing chat-channel pattern from [[project-coop-chat-feed]]);
   add HeldPropPose as a trailing optional record on PoseSnapshot
   (protocol v4 bump alongside the bCrouched bit from Option G).
5. **Receiver**: prop lookup by Key, kinematic toggle, interp,
   release with velocity.
6. **Puppet AnimBP**: drag-mode posture flag.

This is a non-trivial coop feature — comparable in scope to Phase 3
itself. Phase prefix is open for the architect's pass.

## Files referenced

- `D:\Projects\Programming\VOTV_MP\Game_0.9.0n\WindowsNoEditor\VotV\Binaries\Win64\CXXHeaderDump\mainPlayer.hpp` (lines 11-43, 91-243, 320-465, 558-615, 666-728)
- `D:\Projects\Programming\VOTV_MP\Game_0.9.0n\WindowsNoEditor\VotV\Binaries\Win64\CXXHeaderDump\prop.hpp`
- `D:\Projects\Programming\VOTV_MP\Game_0.9.0n\WindowsNoEditor\VotV\Binaries\Win64\CXXHeaderDump\comp_gravitygun.hpp`
- `D:\Projects\Programming\VOTV_MP\research\findings\mta-object-pickup-sync-2026-05-23.md`
- `D:\Projects\Programming\VOTV_MP\research\findings\votv-physics-interaction-surface-2026-05-23.md`
- VotV-Win64-Shipping.exe @ IDB (renamed Native physics-handle RVAs + comment on ProcessEvent)
