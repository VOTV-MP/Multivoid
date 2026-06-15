I now have full ground truth. The atv.cpp already resolves fuel/health/brake offsets and has the SpawnMirror GameplayStatics pattern (reusable for explosion_C spawn). The drone.cpp gives the exact precedent for calling a UFunction and SetActive/SetFloatParameter on a tick-off mirror. Let me confirm one detail: how event_feed routes a ReliableKind (the "silent third place" rule) and check the `FindFunction` does-not-climb-super note matters for `updHealth` (it's on ATV_C itself, so it's fine). Let me also check the `wire_key_util` and whether there's a free byte for health in the payload.

The AtvStatePayload is exactly 60 bytes with 1 `_pad` byte at offset 59. Adding `float health` + `float fuel` = +8 bytes → 68 bytes. I have everything needed. Let me write the synthesis.

The brief below is grounded in the verified code (file:line cited) and the RE findings.

---

# ATV Phase-2 Implementation Brief — fuel / health / smoke / repair / broken-explode

Verified against the live source (`atv_sync.cpp` 559 LOC, `atv.cpp` 217 LOC, `protocol.h`), the `drone.cpp` tick-off-mirror FX precedent, and the RE findings. All offsets below are already resolved in `ue_wrap/atv.cpp:49-54`.

---

## 1. GROUND TRUTH (per state + the tick-off resolution)

The decisive constraint, confirmed: `ue_wrap::atv::PrepareMirror` (`atv.cpp:124-129`) does `SetActorSimulatePhysics(false)` + `SetActorTickEnabled(false)` + `SetActorRootNotifyRigidBodyCollision(false)`. So on a mirror the ATV's `ExecuteUbergraph_ATV` (ReceiveTick) is dead — **no self-decrement, no self-damage, no self-runout, no self-smoke-recompute.** Every Phase-2 value must therefore be *driven from the authority*. The good news (RE-verified): the two things that matter visually — the fuel gauge and the smoke — are **read live from the field on demand**, not in tick:

| State | Field | How it works (RE-cited) | Tick-off mirror resolution |
|---|---|---|---|
| **Fuel/gas** | `fuel@0x05D4` float 0..100 | Consumed ONLY in tick, gated on `isDrive@0x05D0`: `fuel -= dt * (turbo?0.2:0.1) * diff_fuel` (`ATV.txt @37705/@37715`). Refuel = gascan overlap `fuelUp` (`ATV.txt @11246`). Gauge = look-at text reads the live field (`ATV.txt @15851`), an *interaction event* not tick. | **Stream the float; poke `fuel@0x05D4`.** Gauge reads it live → correct for free. NO BP call, NO tick. Mandatory (mirror can't decrement). |
| **Engine on/off** | `isDrive@0x05D0` bool | `runout()` flips it false at fuel≤0 (`ATV.txt @10752`). Separate from `isDriven@0x05F7` (seated) which we already stream as stateBit0. | Stream as a **new stateBit** so the mirror reflects engine-stopped-at-empty. |
| **Health/damage** | `health@0x05E4` float 0..100 | All damage paths converge on the apply block `@19716`: `health -= rawDmg*2*bumperMult`; then `updHealth()`; then explode test. Damage = physics hits (collisions/flips/enemies). | **Stream the resulting float; poke `health@0x05E4` then CALL `updHealth()`.** |
| **Smoke VFX** | `eff_carSmoke@0x0500` `UParticleSystemComponent` (Cascade) | Driven SOLELY by `updHealth()` (`ATV.txt @10955`), a parameterless `BlueprintCallable` UFunction: `a=1-clamp(health,0,100)/100`; `SetFloatParameter('freq', Lerp(0,4,a*…))` + `SetColorParameter('color', greylerp)` + `Activate(true)`. Event-driven (called from damage/repair/load/kill), **NOT in tick.** | **Call `updHealth()` on the mirror after poking health.** The particle component's *own* component-tick survives `SetActorTickEnabled(false)` (exactly the v69 drone-dust precedent, `drone.cpp:238-284`); the comp follows the streamed actor pose for free (it rides the root Mesh). Smoke = pure function of health → no wire field of its own. |
| **Repair** | `toolboxFix()` | Toolbox (`Aprop_toolbox_C`) held-LMB channel; on completion `toolboxFix()` → `health=100; brokenn=false; updHealth(); PlaySound2D(car_fix)` (`ATV.txt @16631`). FULL restore (not incremental). `toolboxCanFix = health<100`; `toolboxFixTime = Lerp(15,3,health/100)`. The channel/timer/use-count is LOCAL to the repairer. | **No new packet.** Repair is a `health 0..99 → 100` jump + `brokenn 1→0` riding the same health/broken stream. Mirror: poke health=100 + brokenn=false + `updHealth()` (smoke vanishes, freq=0). The one-on-decrease guard MUST whitelist increases. |
| **Broken** | `brokenn@0x05E9` bool | Latched true in `runout()` when health≤0 (`ATV.txt @10792`); gates driving (`Empty||brokenn||battery≤0||underwater||zapped`, `ATV.txt @23156`). `broken()/broken_fire()` are EMPTY stubs — `brokenn` IS the state. Cleared by `toolboxFix`. | **Stream as a stateBit; poke `brokenn@0x05E9`.** No BP call (stubs are no-ops). Mirror never self-latches it (tick off) → correct, single-authority crossing. |
| **Explode** | `explode(FVector)` | Discrete edge: health crosses `oldHealth≥0 → newHealth≤0` (`ATV.txt @20042/20086`) → `explode(loc)` BeginDeferred-spawns `explosion_C` at the `tp@0x0560` component + `shake_explosion` + ejects driver (`ATV.txt @23675`). **Does NOT destroy the ATV** — it survives as a smoking wreck. | **Discrete reliable edge** (NOT per-mirror threshold — would double-spawn N explosions). Mirror spawns `explosion_C` VFX-only at the streamed loc via the *existing* `SpawnMirror` GameplayStatics path; does NOT call `explode()` (which re-impulses + ejects, both invalid on a physics-off puppet). |

**Not synced (RE-confirmed vestigial/transient):** `fire/ignite/fireDamage/extinguishFire` are empty stubs (no burning VFX exists — smoke IS the whole damage VFX); `imp@0x05E8` is transient impact-squish; `audio_car_damage@0x0418` is optional cosmetic (defer). `Empty@0x05D8` is internal gating with no proven mirror-visible effect — let it ride implicitly (folded behind the fuel value + engine-off bit).

---

## 2. WIRE DESIGN

**This is v77, not a new bump.** Head is mid-flight bumping to v77 for ATV grab/purchased (`protocol.h:697`, already committed in the working tree as v76→v77). Phase-2 fuel/health/smoke/repair/broken is a **second facet of the same ATV-entity v77** — fold it into the in-flight bump rather than minting v78. Update the v77 changelog comment block to enumerate the Phase-2 additions alongside the purchased/grab additions. (If v77 has already shipped a smoke when this lands, it becomes v78 — coordinate with whoever owns the in-flight v77 commit; the *design* is bump-agnostic.)

**Grow `AtvStatePayload` — do NOT split into a separate message.** Rationale: fuel and health are both per-ATV continuous-ish values that share the exact authority, cadence, relay path, and connect-snapshot as the pose. A separate `AtvState2`/on-change message would duplicate the key index lookup, the authority gate, the relay, and the snapshot for zero benefit, and would race the pose stream (ordering). The RE design note and MTA's `CVehiclePuresyncPacket` both fold health into the per-vehicle sync packet. The payload is reliable-ARQ and 60B today; +8B → 68B is far under `kMaxReliablePayload` (256-20-8 = 228).

```c
struct AtvStatePayload {            // v77 Phase-2 -- was 60B, now 68B
    WireKey  key;          // 32
    float    x, y, z;      // 12
    float    pitch, yaw, roll;  // 12
    float    fuel;         // 4  -- NEW: fuel@0x05D4 (0..100)
    float    health;       // 4  -- NEW: health@0x05E4 (0..100); smoke is derived (updHealth)
    uint8_t  occupantSlot; // 1
    uint8_t  stateBits;    // 1  -- bit0=isDriven(seated) bit1=brake bit2=grabbed bit3=authored
                           //       bit4=engineOn(isDrive@0x05D0) bit5=broken(brokenn@0x05E9)
    uint8_t  adopt;        // 1
    uint8_t  _pad;         // 1
};
static_assert(sizeof(AtvStatePayload) == 68, ...);
```

bit4/bit5 use the previously-noted free upper bits. (bit6 reserved for a future "exploded" if we ever want the wreck-on-snapshot to re-play; see explode note — NOT needed now.)

**Cadence — on-change folded into the existing 20Hz authority pump, NOT a new throttle.** `ReadPayload` (`atv_sync.cpp:176`) already builds the packet from a live read every send. Just add `p.fuel = A::GetFuel(actor); p.health = A::GetHealth(actor)` there — they ride every authority packet the driver/grabber already sends (`atv_sync.cpp:526-531`). Fuel changes ~0.1-0.25/s so 20Hz vastly over-samples it, but it costs 8 bytes on a packet that's already flying and avoids a second on-change state machine + its own latch. **Do not add a separate `|Δfuel|≥0.5` gate** — that's a crutch (RULE 1) buying nothing when the carrier packet is already in flight; the mirror's apply is idempotent (last-writer-wins on a monotone value).

**Discrete edges:** 
- **Repair** → no edge packet; it's a health=100/broken=0 jump on the next authority packet (the order-economy "state follows, don't replay the action" shape). BUT a *non-occupied, non-grabbed* ATV is not currently streamed (`atv_sync.cpp:533` only streams when `authority`). A repair of a **parked** ATV would therefore not propagate. **Resolution:** see §3 authority — the repairer must momentarily author the ATV (or the host re-commits). 
- **Explode** → a one-shot reliable. Reuse the existing key + add `ReliableKind::AtvExplode` carrying `{WireKey key; float locX,locY,locZ;}` (the `tp`-offset spawn loc the authority computed). The health-stream already carries health→0 + broken→1; AtvExplode is purely the *one-shot VFX trigger* so each peer spawns exactly one `explosion_C`. Reliable lane, same Normal in-order channel as AtvState (health→0 arrives, then the explosion edge).

---

## 3. MIRROR APPLY + AUTHORITY

**`ue_wrap/atv` additions** (mirror the drone.cpp FX-resolve discipline — lazy `EnsureFxResolved()` separate from the pose resolve, so the smoke UFunction resolution never gates the pose path):

```
void  DriveFuel(void* atv, float fuel);     // poke fuel@0x05D4 (no BP call -- gauge reads live)
void  DriveHealth(void* atv, float health); // poke health@0x05E4 THEN CallUpdHealth() -> smoke
void  SetBroken(void* atv, bool broken);    // poke brokenn@0x05E9 (broken()/_fire() are stubs -> no call)
void  SetEngineOn(void* atv, bool on);      // poke isDrive@0x05D0 (mirror reflects runout engine-stop)
void  PlayExplosion(void* atv, const FVector& loc);  // spawn explosion_C VFX-only at loc
```

- **`DriveHealth`**: poke the float, then call the ATV's own parameterless `updHealth()` via the resolved UFunction thunk (`R::FindFunction(g_cls, L"updHealth")` — it's declared on `ATV_C` itself so `FindFunction` finds it directly; the super-chain gotcha that bit drone's `SetFloatParameter` does NOT apply here). `updHealth` is pure/self-contained (no tick/physics dep) → runs correctly on a `PrepareMirror`'d actor (same discipline as keypad/window state-apply). This single call repaints `eff_carSmoke` freq+color+Activate exactly as the BP — **byte-exact, zero recompute on our side.** Recommended over re-implementing the 3 component calls (path B in the findings).
- **`SetEngineOn`/`SetBroken`**: raw bool pokes (no BP fn).
- **`PlayExplosion`**: reuse the existing `SpawnMirror` GameplayStatics machinery (`atv.cpp:138-202`) — generalize it to spawn any class by name, spawn `explosion_C` at `loc` (physics-on, self-destructing VFX actor). Do NOT call `explode()` (it re-impulses + ejects the local driver — invalid on a physics-off puppet; the findings confirm option-(a) VFX-only spawn is cleaner/crash-safe).

**`atv_sync` apply (in `OnReliable`, after the existing pose apply, `atv_sync.cpp:367`):**
```
A::DriveFuel(e.actor, payload.fuel);
const bool incomingBroken = (payload.stateBits & 0x20);
// health: accept decreases always; accept an INCREASE only as a repair (broken->whole) or full restore.
A::DriveHealth(e.actor, payload.health);   // updHealth() inside -> smoke matches
A::SetBroken(e.actor, incomingBroken);
A::SetEngineOn(e.actor, payload.stateBits & 0x10);
```
The MTA `CVehiclePuresyncPacket` "apply-on-decrease" guard is **not** ported as a hard floor here — last-writer-wins from a single authority over reliable in-order ARQ means no stale-relayed-copy feedback (the relay preserves order). A repair is an increase and must pass; gating on decrease-only would break repair. (If a future unreliable transport is adopted per the Phase-1.5 note, revisit with the on-decrease + repair-whitelist rule.)

**Authority:**
- **Fuel/health continuous** → occupant-OR-grabber streams it (the existing `IsLocalAuthority`, `atv_sync.cpp:167`). Decrement/damage happen only on the authority's live (tick-on) ATV; mirrors receive results.
- **Broken/explode** → the authority that crosses health≤0 computes the explode edge ONCE and sends `AtvExplode` reliably; broken rides the health stream's stateBit. Single crossing = no double-explosion.
- **Repair** → host-authoritative re-commit, like the order economy. The repairer's *completed* `toolboxFix` is a local outcome (health=100/broken=0 on their copy). **The gap:** a parked ATV isn't streamed (`atv_sync.cpp:533`). Two correct options:
  1. **(recommended, minimal)** When a peer runs `toolboxFix` locally, it briefly becomes the authority for that ATV (it's interacting with it). Simplest hook: on the local health-rising edge (detect `health` jumped up since last tick on a non-mirror ATV), send one authority-style `AtvState` packet with the new health/broken even when not seated. This is a *targeted* one-shot, not a parked-ATV continuous stream.
  2. **(cleaner, host-authoritative)** A reliable `AtvRepair{key}` request → host runs/commits health=100+broken=0 on its copy and the host's next snapshot/stream carries it. Heavier; only needed if repairer≠host divergence matters on LAN. Defer to option 1 for Phase-2; note option 2 as the host-authoritative upgrade.

**Connect-snapshot:** `ReadPayload` already runs for `adopt=1` (`atv_sync.cpp:466`) — fuel/health/broken/engineOn ride it automatically once added to `ReadPayload`. A late joiner sees the right tank level, damage smoke, and wreck state immediately. The existing idle-vs-authored adopt branch (`atv_sync.cpp:357`) is unaffected — an idle damaged ATV adopts physics-on but still gets its health poked + `updHealth` called so it smokes correctly while grabbable.

---

## 4. FILE-BY-FILE PLAN + MODULAR-CAP

| File | Change | LOC impact |
|---|---|---|
| `protocol.h` | Grow `AtvStatePayload` +2 floats (68B) + static_assert; add stateBits bit4/bit5 doc; add `ReliableKind::AtvExplode=74` + `AtvExplodePayload{key,locX/Y/Z}` (~16B); extend the v77 changelog block. | +~25 LOC (single-feature constants header, cap-exempt) |
| `ue_wrap/atv.h` / `.cpp` | Add `DriveFuel/DriveHealth/SetBroken/SetEngineOn/PlayExplosion`; resolve `updHealth` UFunction + `isDrive@0x05D0`/`brokenn@0x05E9` offsets (lazy `EnsureFxResolved`-style, mirroring drone.cpp:68); generalize the `explosion_C` spawn off `SpawnMirror`. | `.cpp` 217 → ~300. **Under 800 — fine.** |
| `coop/atv_sync.cpp` | `ReadPayload` +fuel/health/engineOn/broken; `OnReliable` apply block; new `AtvExplode` send (authority crosses 0) + `OnAtvExplode` receiver + the repair one-shot edge (option 1). | **559 → ~660.** Still under the 800 soft cap. |
| `coop/atv_sync.h` | Declare `OnAtvExplode`; doc the Phase-2 stateBits. | +~8 |
| `event_feed.cpp` (the **silent third place**, per `[[feedback-reliablekind-router-checklist]]`) | Route `AtvExplode` → `atv_sync::OnAtvExplode` in the master family-dispatch list. **Easy to miss — wire it.** | +~4 |

**Modular-cap verdict:** atv_sync.cpp lands at ~660 LOC — **does not cross 800**, so no extraction is forced *now*. BUT it's the convergence file for v76 (release) + v77 (purchased) + Phase-2, and the prompt flags it growing. **Pre-emptive extraction proposal (separate commit, BEFORE adding Phase-2):** extract the v77 purchased-ATV identity block — `g_savePlacedKeys`/`g_synthForActor`/`SendAtvSpawn`/`SendAtvDestroy`/`OnAtvSpawn`/`OnAtvDestroy`/`IsSynthKey`/the RebuildIndex synth-classification (~150 LOC, `atv_sync.cpp:82-138,402-445`) → `coop/atv_purchased.{h,cpp}`. That drops atv_sync.cpp to ~410 and leaves Phase-2 landing at ~510, with clean headroom. This is the §4 "extract first if past cap AND distinct subsystem" trigger applied proactively — purchased-ATV identity is a genuinely distinct subsystem from the pose/state stream. (If the in-flight v77 commit isn't yours to touch, land Phase-2 in atv_sync.cpp at ~660 and file the extraction as the immediate follow-up — it's under hard cap.)

---

## 5. RISKS + TEST (repro per state)

**Risks:**
- **Double-explosion** (the classic gotcha): mitigated by the single-authority `AtvExplode` reliable edge + mirrors spawning VFX-only (never calling `explode()`). Test: damage to 0 with 3 peers → exactly ONE `explosion_C` per peer, no driver-eject on puppets.
- **`updHealth` UFunction unresolved** (class load timing): lazy-resolve + log-once fallback like `drone.cpp:90`; if unresolved, smoke degrades to "field poked, no repaint" — never crashes. (Optionally fall back to path-B 3-call drive if `updHealth` is null.)
- **Repair on a parked ATV not propagating** (the §3 authority gap): covered by option-1 one-shot edge; without it, a parked-ATV repair would be invisible to peers — verify explicitly in the repair test.
- **Mirror smoke component frustum-cull / wrong anchor**: NOT a risk here (unlike drone dust) — `eff_carSmoke` is attached to the actor root Mesh, follows the streamed pose natively; no `bAbsoluteLocation` world-origin pin needed.
- **Float byte-cost on the 20Hz stream**: +8B/packet × 20Hz × N ATVs is negligible (LAN); confirmed under kMaxReliablePayload.
- **`brokenn` poked but engine still drives on mirror**: irrelevant — the mirror's driving logic (tick) is off; broken is purely visual + (on re-grab) the authority re-evaluates.

**Test matrix (2-3 peers, fresh New Game save):**
1. **Fuel burn**: P1 drives the ATV; on P2's screen the look-at gauge `Fuel:` ticks down matching P1. Drive to empty → P2 sees engine stop (engineOn bit) + gauge at ~0.
2. **Refuel**: P1 gascan-refuels; P2's gauge jumps up on the next packet.
3. **Damage→smoke**: P1 flips/rams the ATV; P2 sees `eff_carSmoke` ramp on (freq grows with damage, grey→dark color). Verify the smoke follows the body pose on P2.
4. **Repair→un-smoke**: P1 toolbox-repairs (held LMB ~15s); P2 sees health→100, smoke vanish (freq=0), broken clear. **Also test repairing a PARKED ATV** (authority-gap repro).
5. **Destroy→explode**: damage to 0; ALL peers see exactly one `explosion_C` + the ATV becomes a broken (`brokenn`) smoking wreck (NOT despawned), un-driveable.
6. **Late-join snapshot**: damage + leave an ATV at half-health/smoking, then a 3rd peer joins → it immediately sees the correct fuel/smoke/broken state via the connect-snapshot (`adopt=1`).

**Pre-deploy:** this lands as a v77 (or v78) protocol bump → both peers must run the matched DLL. Run the §-checklist smoke (30s LAN, host+client log clean, RSS stable, hot-path audit table — the 20Hz authority stream is the only hot path and it just adds 2 field reads to an existing send) before any "ready" handoff.

---

**Key file paths:** `d:\Projects\Programming\VOTV_MP\src\votv-coop\src\coop\atv_sync.cpp` (559 LOC, apply/stream/snapshot), `...\src\ue_wrap\atv.cpp` (217 LOC, add Drive*/SetBroken/PlayExplosion; fuel/health/brake offsets already resolved at `atv.cpp:49-54`), `...\include\coop\net\protocol.h` (`AtvStatePayload` @2474, v77 changelog @697, ReliableKind block @1179), `...\src\ue_wrap\drone.cpp` (the load-bearing tick-off-mirror particle/UFunction precedent at `drone.cpp:238-303`), and `...\src\coop\event_feed.cpp` (the silent-third-place reliable router — MUST wire `AtvExplode`).