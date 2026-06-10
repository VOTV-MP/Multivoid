# Sky / stars / celestial bodies cross-peer desync — root cause + host-auth sync design (2026-06-08)

Status: **root cause PROVEN** (reflection + byte-exact BP disassembly). Concrete
host-authoritative design specified, mirroring `time_sync.cpp` / `weather_sync.cpp`.

User report (hands-on 2026-06-08): "The STARS in the sky and the CELESTIAL BODIES
look COMPLETELY DIFFERENT on the client vs the host."

Evidence base: CXX SDK header dumps
(`Game_0.9.0n/.../Binaries/Win64/CXXHeaderDump/{newsky,daynightCycle,spaceRenderer,saveSlot,...}.hpp`),
live BP disassembly via `tools/bp_reflect.py` + `kismet-analyzer gen-cfg`
(`research/bp_reflection/newsky.json`, offset-aware CFG generated to verify the
flow). This is the SAME unseeded-RNG-per-peer pattern already proven twice for trash
piles (`AundergroundGarbageSpawner_C`) and fireflies (`Aticker_fireflySpawner_C`).

---

## 0. TL;DR — the root cause is TWO distinct fields, neither currently synced

| Visual element | Actor.field | Generation | Why it desyncs | Already synced? |
|---|---|---|---|---|
| **Star field orientation** (the whole star dome's yaw) | `Anewsky_C::sky` mesh **relative rotation** | **UNSEEDED** `RandomFloatInRange(-45,-135)` at startup, then a per-tick spin `+= dt/32` | each peer's fresh world rolls a different random yaw, and the spin accumulates from independent start times | **NO** |
| **Moon phase** (crescent ↔ full appearance) | `Anewsky_C::moonPhase_mirror` ← `UsaveSlot_C::moonPhase` (@0x08A4) | read from the SAVE on every `upd` | host loads a progressed save, client boots a BLANK New-Game save → different `moonPhase` float | **NO** |
| Sun/moon **orientation** (day/night arc across sky) | cycle `setSunAndMoonRotation()` from `totalTime`/`Phase` | pure clock math, **NO RNG** | — (deterministic) | YES — via `TimeSync` (v36) |
| Moon **brightness/intensity**, sky colors, fog | derived from `daylightCycleObject.{phase_sin,seasonalExponent,LC_int_amb,...}` | pure clock + curve lookups | — (deterministic) | YES — via `TimeSync` |
| Star **twinkle / cloud scroll** | `newsky.dynmat` params `clouds_opac_offset/offset_A/offset_B` | `RandomFloat()` x3 in `init()` | cosmetic shimmer offsets; not orientation | (cosmetic, see §5) |

So "stars look completely different" = **the random star-dome yaw** (dominant —
a 90° spread of possible orientations). "Celestial bodies look different" =
partly the **moon phase save value**, and partly the bodies appearing against a
differently-rotated star field. Sun/moon *position* and brightness already
converge (TimeSync). **Fix = sync (1) the `sky` mesh rotation and (2) `moonPhase`.**

---

## 1. The actor hierarchy (who owns the visible night sky)

`AdaynightCycle_C` (the clock we already sync via `TimeSync`) owns:
- `skysphere` : `Anewsky_C*` @ **0x02B8** — the visible sky actor (NOT the sun light).
- `light_sun` / `light_moon` (directional lights) @ 0x0248 / 0x0240, oriented by
  `setSunAndMoonRotation()` from the clock.
- `sunAxis` @ 0x02C0, `eff_shootingStar` @ 0x0288, `SStar_active` @ 0x02DC (shooting-star FX).

`Anewsky_C` (`CXXHeaderDump/newsky.hpp`) is the star dome + celestial meshes. Its
**SCS component tree** (resolved from `newsky.json` `ChildNodes`):

```
DefaultSceneRoot
├── dir            (UArrowComponent)
├── starRot        (UArrowComponent)        <-- star-rotation pivot
│   └── sky        (UStaticMeshComponent)   <-- THE STAR DOME (stars are on this mesh's material)
├── Billboard      (UBillboardComponent)
│   ├── m1, m2, m3 (UStaticMeshComponent)   <-- celestial body meshes (planets/moon/blackhole disc)
└── light_blackhole(UDirectionalLightComponent)
```

`Anewsky_C` fields of interest:
`sky`@0x0250, `starRot`@0x0258, `dynmat`@0x0278 (sky material MID),
`moonPhase_mirror`@0x02BC, `moonPhase_normal`@0x02C0, `applyMoonPhase`@0x02E8,
`daylightCycleObject`@0x02E0, `sunHidden`@0x02A0, `skyHidden`@0x02F0.

NOTE — ruled out as "the sky":
- `AspaceRenderer_C` (`spaceRenderer.hpp`) is the radio-telescope / SETI minigame
  (signal coords, `pings` instanced mesh, `SceneCaptureComponent2D`). Not the visible sky.
- `Aceilingstars_C` (`ceilingstars.hpp` : `Aactor_save_C`) + `Aprop_ceilingstarsspawn_C`
  are a GRABBABLE glow-in-the-dark decoration prop, not the sky.

---

## 2. ROOT CAUSE A — the star dome gets an UNSEEDED random yaw (byte-exact)

In `ExecuteUbergraph_newsky`, on the startup path (`ReceiveBeginPlay` →
ubergraph → `init()` → this block), offsets from the gen-cfg disassembly
(`research/bp_reflection/newsky.json`, CFG block @4855→4865):

```
4870: RandomFloatInRange_ReturnValue = RandomFloatInRange(-45.0, -135.0)   // /Script/Engine->KismetMathLibrary->RandomFloatInRange
4908: MakeRotator_ReturnValue_1      = MakeRotator(roll=0, pitch=0, yaw=RandomFloatInRange_ReturnValue)
4955: sky->K2_SetRelativeRotation(MakeRotator_ReturnValue_1, sweep=false, hit, teleport=false)   // SceneComponent->K2_SetRelativeRotation
5007: Delay(0) -> continue at 1930 (upd(true))
```

- `RandomFloatInRange` here is `UKismetMathLibrary::RandomFloatInRange` — the
  **GLOBAL UNSEEDED RNG**, NOT the seeded `RandomFloatInRangeFromStream`. The
  entire `newsky` import table (verified) contains `RandomFloat` + `RandomFloatInRange`
  and **NO** `...FromStream`, `MakeRandomStream`, `SetRandomSeed`, or any seed
  function. (`newsky.json` imports; grep for seed tokens across all VOTV CXX dumps
  returns only engine-internal classes, none of the sky/cycle actors.)
- The yaw is uniform in **[-135°, -45°]** — a 90-degree spread. `sky` is a child of
  `starRot`, so this sets the star dome's orientation. Each peer's fresh world rolls
  independently → **different star orientation = "stars look completely different."**

### Per-tick spin (compounds the desync, deterministic rate)
Also in the ubergraph (CFG block @3276 / @3392, the ReceiveTick path):
```
3354: MakeRotator_ReturnValue = MakeRotator(0, 0, yaw = deltaSeconds / 32)
3392: sky->K2_AddRelativeRotation(MakeRotator_ReturnValue, sweep=false, hit, teleport=false)
```
The dome spins continuously at `dt/32` deg/frame. The **rate** is deterministic, but
because each peer (a) starts from a different random yaw (§2) and (b) began
accumulating at a different wall-clock instant, the live orientations diverge and
stay diverged. Syncing only the rate is insufficient; we must sync the **absolute
rotation** (which also subsumes the random initial offset).

> There is NO `seed` / `starSeed` / `RandomStream` field anywhere on `Anewsky_C`,
> `AdaynightCycle_C`, `UsaveSlot_C`, `mainGameInstance_C`, or `AmainGamemode_C`. A
> seed-broadcast fix is therefore impossible — the dome rotation is genuinely
> ephemeral per-peer. We must snapshot the resulting rotation, not a seed.

---

## 3. ROOT CAUSE B — moon phase comes from the SAVE SLOT (differs host vs client)

Also in the ubergraph (CFG block @3444→3731, runs inside `upd`):
```
3444: getSaveSlot() -> saveSlot
3490: moonPhase_mirror = saveSlot.moonPhase           // UsaveSlot_C::moonPhase @0x08A4 (float)
... moonPhase_normal = |moonPhase_mirror - 0.5| * 2
3687: dynmat->SetScalarParameterValue("moonphase", moonPhase_mirror)   // drives the moon material
```
The moon's *phase* (its crescent/gibbous/full look) is the persisted save float
`UsaveSlot_C::moonPhase`. The HOST loads a progressed save; the CLIENT boots a
**blank** New-Game save (`engine::StartFreshGame(storyMode=true)`), whose `moonPhase`
defaults differently → the moon renders a different phase on each peer. This is the
"celestial bodies look different" half. (Moon *brightness* and *position* are
clock-derived and already converge — see §0.)

`setMoonPhase()` (a separate UFunction) just re-applies the same `moonphase` param;
the value origin is the save field above.

---

## 4. Everything else in the sky is already deterministic (verified)

`AdaynightCycle_C::setSunAndMoonRotation()` — the sun/moon arc — disassembles with
**NO RNG tokens** (verified: `whichfn` scan of `daynightCycle.json`); it is pure math
on `totalTime`/`Phase`/`sunAxis`. `initializeSunRotation`, `timer_changeSunRotation`,
`loadtime`, `ReceiveTick` likewise have no RNG. The cycle's many `RandomFloat*` calls
all live in the WEATHER scheduler (rain/lightning/fog timing) which we already sync
via `weather_sync`/`weather_fog`/`ticker_sync`. So once `TimeSync` converges
`totalTime`/`Day`, the sun, the moon's orbit, the day/night brightness, sky colors,
and fog all already converge. The ONLY un-converged sky elements are the two in §2/§3.

---

## 5. Cosmetic note (NOT part of the fix)

`newsky::init()` sets three sky-material scalars from `RandomFloat()`:
`clouds_opac_offset`, `clouds_offset_A`, `clouds_offset_B` (CFG block ~ offset 9990,
`SetScalarParameterValue`). These are CLOUD-texture scroll offsets — a subtle
per-peer shimmer in the cloud layer, not the stars or bodies. They are unseeded too,
but the user's complaint is the stars/bodies, and these are low-salience. Left out of
the MVP; can be folded into the same payload later if a cloud mismatch is ever
reported (add 3 floats). Flagging for completeness, not fixing now (scope = §0 law).

---

## 6. The host-authoritative fix — design (mirrors time_sync.cpp / weather_sync.cpp)

There is no clean seed (§2), and the bodies/brightness already sync (§4), so the
correct, minimal fix is a **host→client snapshot of the two un-converged values**:
the `sky` mesh rotation and `moonPhase`. This is the exact shape of `TimeSync`:
a tiny host-authoritative push (throttle + connect-edge), client direct-writes,
the BP's own per-tick code keeps rendering from the written state.

### 6.1 New engine-wrapper module: `ue_wrap/skysphere.{h,cpp}` (principle 7)

Resolve `Anewsky_C` (the cycle's `skysphere`@0x02B8, or `FindObjectByClass` once,
cached + revalidated like `daynightcycle::Cycle()`), expose:

```cpp
namespace ue_wrap::skysphere {
  bool EnsureResolved();                  // resolve Anewsky_C UClass + sky/moonPhase_mirror offsets; idempotent
  void* Sky();                            // the live Anewsky_C* (cached, re-resolved if it dies); nullptr until streamed in
  // Read the two authoritative values. false if not resolved (outs untouched).
  bool ReadSky(FRotator& skyRelRot, float& moonPhase);
  // Apply host's values: write the sky component's relative rotation + moonPhase_mirror,
  // then re-apply the moon material param so the change shows without waiting a frame.
  void ApplySky(const FRotator& skyRelRot, float moonPhase);
}
```

Implementation primitives (all already exist or are 1-liners on existing wrappers):
- The `sky` UStaticMeshComponent is `*(void**)((u8*)newsky + 0x0250)`. Its WORLD
  rotation: `engine::GetComponentWorldRotation(sky)` /
  `engine::SetComponentWorldRotation(sky, rot)` (already in `ue_wrap/engine.h:128/135`,
  wrapping `USceneComponent::K2_Get/SetComponentRotation`). World rotation is the
  robust choice (the dome's parent `starRot` and the actor root could differ per
  peer; world rotation captures the final on-screen orientation unambiguously).
  *(If a relative-rotation field write is preferred to avoid the per-tick spin
  fighting the snapshot, see §6.4 — but world-rotation + ~1 Hz push converges fine.)*
- `moonPhase_mirror` is a float @0x02BC — a direct field read/write (like the cycle
  clock's `loadtime`-equivalent direct writes). After writing, call the reflected
  `setMoonPhase()` UFunction (or re-`SetScalarParameterValue("moonphase", ...)`) so
  the material updates immediately; cheap, runs only on the apply.

### 6.2 New gameplay/network module: `coop/sky_sync.{h,cpp}` (principle 7)

A near-verbatim clone of `coop/time_sync.cpp`:
- `Install(session)` — store session, `skysphere::EnsureResolved()`.
- `Tick()` — HOST only, `kPushInterval ≈ 1000 ms` (the dome spins, so push a bit
  faster than the 2 s clock; 1 Hz keeps it visually locked given the slow dt/32 spin
  and client interpolation). Build payload from `skysphere::ReadSky`, `SendReliable`.
- `OnReliable(payload)` — CLIENT only (host is authoritative; early-return on Host).
  `skysphere::ApplySky(rot, moonPhase)`. Optionally short-arc interpolate the yaw
  toward the target (reuse `coop::LerpWindow`, as the kerfur body-yaw sync does) so
  the dome doesn't visibly snap each push; MVP can hard-set (the spin is slow).
- `QueueConnectBroadcastForSlot(peerSlot)` — HOST only, sends one snapshot on the
  connect edge (so a fresh joiner gets the host's current orientation + phase
  immediately, exactly like `time_sync::QueueConnectBroadcastForSlot`). Wire it into
  the same connect-snapshot site that already calls the TimeSync connect broadcast.
- `OnDisconnect()` — reset the throttle clock.

### 6.3 Protocol (`coop/net/protocol.h`)

Next free ReliableKind is **33** (GrimeState=31 is the last enum value; 32 is the
RESERVED "GrimeDestroy" slot per the protocol.h note — do NOT reuse it):

```cpp
SkyState = 33,   // 2026-06-08 (v44): host-authoritative night-sky orientation + moon phase
                 //   (Anewsky_C). The star dome (sky mesh, child of starRot) is given a
                 //   per-game UNSEEDED random yaw RandomFloatInRange(-45,-135) at BeginPlay +
                 //   a per-tick dt/32 spin -> diverges per peer; moonPhase_mirror is read from
                 //   the save (host progressed vs client blank) -> diverges. Host pushes the
                 //   resolved sky WORLD rotation + moonPhase on a ~1Hz throttle + connect edge;
                 //   client writes them (its ReceiveTick keeps rendering). Sun/moon ORBIT +
                 //   brightness already converge via TimeSync(29). Payload: SkyStatePayload.
                 //   RE: research/findings/votv-sky-stars-celestial-sync-RE-2026-06-08.md
```

Payload (16 B, modeled on `TimeSyncPayload`):
```cpp
struct SkyStatePayload {
    float skyPitch;    // sky mesh WORLD rotation (FRotator) — pitch
    float skyYaw;      //   yaw   (carries the random initial offset + accumulated spin)
    float skyRoll;     //   roll
    float moonPhase;   // Anewsky_C::moonPhase_mirror (= UsaveSlot_C::moonPhase)
};
static_assert(sizeof(SkyStatePayload) == 16, "SkyStatePayload must be 16 bytes");
```
Bump `kProtocolVersion` 43 → **44**. Add the dispatch route in `event_feed` /
`net_pump` next to the `TimeSync` case (validate the 4 floats with the same NaN/inf
guard `event_feed` already applies to engine-written floats).

### 6.4 Optional hardening (if the per-tick spin visibly fights the snapshot)

The client's own `ReceiveTick` keeps adding `dt/32` to the dome between pushes. With
a 1 Hz push that is ~`(deg/32)*60` ≈ small per second and the next snapshot corrects
it — fine for MVP. If it ever looks like jitter, the root-cause-clean option is to
write the `sky` **relative** rotation field directly AND host-stream the absolute
relative rotation each push (the spin then just re-derives from the synced base every
push); do NOT suppress the BP's AddRelativeRotation (that would be a crutch — RULE 1).
Keep MVP simple: world-rotation snapshot at 1 Hz + connect edge.

---

## 7. MTA precedent

None directly — a procedurally-randomized skybox is VOTV-specific ambient content
(no GTA:SA/MTA analogue; MTA's sky is static + its weather/time are already first-
class synced state). The *shape* we adopt is our own established host-auth push
(`time_sync.cpp`), which itself mirrors MTA's periodic authoritative state pushes
(e.g. `CClientGame`/`CClock` time sync). Cite `coop/time_sync.cpp` as the in-repo
precedent in the new module header.

---

## 8. Files / evidence

- Sky actor: `Game_0.9.0n/.../CXXHeaderDump/newsky.hpp`; cycle:
  `daynightCycle.hpp` (`skysphere`@0x02B8, `sunAxis`@0x02C0); save:
  `saveSlot.hpp` (`moonPhase`@0x08A4); ruled-out: `spaceRenderer.hpp`,
  `ceilingstars.hpp`, `prop_ceilingstarsspawn.hpp`, `struct_spaceObject.hpp`.
- BP disassembly: `research/bp_reflection/newsky.json` (+ `daynightCycle.json`,
  `spaceRenderer.json`), regenerated via `python tools/bp_reflect.py newsky
  daynightCycle spaceRenderer`. Offset-aware CFG via
  `kismet-analyzer gen-cfg <newsky.uasset> <out>` →
  random yaw at ubergraph offsets 4870/4908/4955; per-tick spin at 3276/3392;
  moonPhase-from-save at 3444/3490/3687.
- Sync templates to clone: `src/votv-coop/src/coop/time_sync.cpp` +
  `include/coop/time_sync.h` + `include/ue_wrap/daynightcycle.h`; weather connect-
  snapshot/throttle idiom in `src/votv-coop/src/coop/weather_sync.cpp`.
- Engine primitives: `ue_wrap/engine.h:128` `GetComponentWorldRotation`, `:135`
  `SetComponentWorldRotation`.
- Protocol: `include/coop/net/protocol.h` (ReliableKind ends at GrimeState=31, 32
  reserved → use 33; TimeSyncPayload @ line 1547 as the payload template;
  `kProtocolVersion` @ line 409).

## 9. Open / deferred
- Cloud-scroll offsets (`clouds_opac_offset/_A/_B`, §5) left unsynced (cosmetic);
  fold 3 floats into `SkyStatePayload` if ever reported.
- Shooting-star FX (`AdaynightCycle_C::eff_shootingStar` + `SStar_active`) are
  transient particle bursts; not addressed (ephemeral, not the user complaint). If a
  synced shooting-star is wanted later it would be a one-shot event packet, not state.
- The blackhole / "Eye" event sky variants (`setEvent`, `Gamerule Is Permanent Black
  Hole`, `light_blackhole`, `setEye`) are story-gated; their on/off state would ride
  TimeSync-adjacent gamemode sync, out of scope here.
