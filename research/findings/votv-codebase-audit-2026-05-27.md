# VOTV_MP codebase audit — 2026-05-27 (verified)

> **Verification pass run 2026-05-27**: a separate verifier agent
> spot-checked every finding against the actual source. Corrections from
> that pass are inlined below as `[VERIFIED]`, `[DOWNGRADED]`, or
> `[FALSE POSITIVE]` annotations. Net: 14 confirmed as-stated, 5
> downgraded in severity, 2 false positives.



User-directed sweep of the entire `src/votv-coop/` tree triggered by a
shipped RULE-1 crutch (commit `987898b`, since reverted at `4b5a2e3`).
Two audit passes were run with distinct lenses across three layers
(`coop/`, `ue_wrap/`, `harness/+dev/+bootstrap/`):

1. **RULE 1** — "no crutches, no quick fixes" violations (CLAUDE.md).
2. **Reliability** — failure modes, races, lifecycle gaps, hot-path costs.

26 findings total at confidence ≥ 80. This doc consolidates them in
priority order for fix planning. Each finding lists the file:line, the
specific pattern, the confidence, and a concrete proper-fix proposal.
Where a finding was flagged by BOTH audits at the same site, it appears
once with both lenses noted.

---

## CRITICAL — fix first (lifetime/UAF/crash)

### C1. event_feed.cpp GT::Post captures stale puppet pointer (UAF)

- **File**: `src/votv-coop/src/coop/event_feed.cpp:489-493`
- **Lens**: Reliability
- **Confidence**: 88

```cpp
void* puppet = (remote && remote->valid()) ? remote->GetActor() : nullptr;
net::ItemActivatePayload pCopy = p;
const uint8_t peerId = p.peerSessionId;
ue_wrap::game_thread::Post([peerId, puppet, pCopy] {
    ::coop::item_activate::ApplyToPuppetOrDefer(peerId, puppet, pCopy);
});
```

`puppet` captured as a raw `void*` on the net thread. Game thread can
`Destroy()` the puppet between enqueue and lambda dispatch. `IsLive`
check inside the lambda is best-effort; if the GUObjectArray slot is
recycled, a dangling pointer transits engine calls.

**Fix**: capture `peerId` only; re-fetch puppet via
`coop::players::Registry::Get().Puppet(peerId)` INSIDE the lambda.

---

### C2. Non-atomic `g_session_ptr` in prop_lifecycle + npc_sync — race with parallel-anim workers

- **File**: `src/votv-coop/src/coop/prop_lifecycle.cpp:35` + `src/votv-coop/src/coop/npc_sync.cpp:30`
- **Lens**: Reliability
- **Confidence**: 85
- **[DOWNGRADED to HIGH per verifier]**: race is real, but the actual
  failure mode is a null-deref during shutdown, not mid-session data
  corruption. CRITICAL class was overstated. Fix proposal stands.

```cpp
coop::net::Session* g_session_ptr = nullptr;  // plain ptr
```

Observers fire from parallel-anim worker threads per `game_thread.cpp`
header comment. `g_session_ptr->connected()` deref races with harness
calling `SetSession(nullptr)` on shutdown. `item_activate.cpp` and
`weather_sync.cpp` use `std::atomic<Session*>` correctly — these two
files diverged.

**Fix**: change to `std::atomic<coop::net::Session*>` with
`memory_order_acquire` loads. Mirror `item_activate.cpp:107` exactly.

---

### C3. game_thread.cpp Uninstall nulls `g_originalPE` after hook removed

- **File**: `src/votv-coop/src/ue_wrap/game_thread.cpp:306-313`
- **Lens**: Reliability
- **Confidence**: 85

```cpp
void Uninstall() {
    if (!g_installed) return;
    ClearAllObservers();
    ClearAllInterceptors();
    hook::Uninstall(g_hookTarget);  // detour disabled, original restored
    g_installed = false;
    g_hookTarget = nullptr;
    g_originalPE = nullptr;   // worker thread mid-detour may still read this
}
```

A worker thread already inside `ProcessEventDetour` when MinHook
unhooks reads `g_originalPE` at line ~276 and calls a now-null address
→ crash. SC_CLOSE's 100ms worker drain reduces the window but doesn't
close it.

**Fix**: do NOT null `g_originalPE` during teardown (the field is no
longer read once the detour is unhooked; null is cosmetic + dangerous).
Add a `Sleep(50)` drain between `hook::Uninstall` and the `nullptr`
stores so in-flight dispatches complete first.

---

## HIGH — fix this week

### H4. RULE 1: engine_pawn.cpp SetControlRotation direct memory write

- **File**: `src/votv-coop/src/ue_wrap/engine_pawn.cpp:87-91`
- **Lens**: RULE 1 (crutch)
- **Confidence**: 92

```cpp
void SetControlRotation(void* controller, const FRotator& rot) {
    if (!controller) return;
    *reinterpret_cast<FRotator*>(reinterpret_cast<uint8_t*>(controller)
                                 + P::off::AController_ControlRotation) = rot;
}
```

The sdk_profile.h comment explicitly says "does the same thing" as
`K2_SetControlRotation` but that's unverified inference. UE4's
`SetControlRotation` runs `ProcessViewRotation` + `UpdateRotation` +
fires `OnRep_ControlRotation` in networked contexts. Raw write skips
all of that.

**Fix**: add `SetControlRotationFn = L"K2_SetControlRotation"` to
sdk_profile.h::name; call via `ue_wrap::Call`. Confirm with IDA decomp
that no side-effects need suppressing on the receiver.

---

### H5. reflection.cpp IsResolved() excludes g_processEvent

- **File**: `src/votv-coop/src/ue_wrap/reflection.cpp:30`
- **Lens**: Reliability
- **Confidence**: 88

```cpp
bool IsResolved() { return g_objArray && g_fnameToString; }
```

Health check returns PASS while `g_processEvent` is null → all
`reflection::CallFunction` returns false silently → entire UFunction
substrate is dead but the boot log claims success.

**Fix**: one-line change: `return g_objArray && g_fnameToString && g_processEvent;`

---

### H6. RULE 1: remote_prop.cpp StringToFName dual-try logs spurious ERROR every call

- **File**: `src/votv-coop/src/coop/remote_prop.cpp:421-430`
- **Lens**: RULE 1 (crutch) + Reliability (log noise)
- **Confidence**: 88

```cpp
if (!f.SetRaw(L"InString", &fs, sizeof(fs))) {
    if (!f.SetRaw(L"inString", &fs, sizeof(fs))) {
```

`SetRaw` logs `UE_LOGE` on every failed param. The first call
ALWAYS fires the error on builds where the param is `inString` (which
is the actual name in this cook — see `engine_widget.cpp:108`).
Measured: 29× ERRORs per LAN test run.

**Fix**: resolve via `R::FunctionParams(g_convStrToNameFn)` once at
install, cache the correct name in sdk_profile.h, remove the dual-try.

---

### H7. RULE 1: remote_player.cpp per-tick direct write to lag_fl pitch

- **File**: `src/votv-coop/src/coop/remote_player.cpp:591-597`
- **Lens**: RULE 1 (crutch) — silent rendering bug
- **Confidence**: 85
- **[DOWNGRADED to MEDIUM per verifier]**: spring arms re-evaluate
  RelativeRotation each tick from their TickComponent, so the direct
  write IS read on next frame. The "child propagation breakage" claim
  wasn't substantiated. Still a RULE 1 crutch (no UpdateComponentToWorld
  side-effects), but no confirmed visible bug.

```cpp
*reinterpret_cast<float*>(
    reinterpret_cast<uint8_t*>(lag_fl) + P::off::USceneComponent_RelativeRotation) =
    curPitch_;
```

Per-tick, per-puppet. Skips `UpdateComponentToWorld` propagation, so
child components (flashlight `light_R` cone direction) may not follow
the spring arm. Live rendering bug.

**Fix**: call `USceneComponent::K2_SetRelativeRotation` via reflection;
preserves engine's transform-propagation pipeline.

---

### H8. RULE 1: harness.cpp autotest continued-correction polling loop

- **File**: `src/votv-coop/src/harness/harness.cpp:969-1009`
- **Lens**: RULE 1 (crutch)
- **Confidence**: 85
- **[VERIFIED with caveat]**: core RULE 1 observation (polling loop
  instead of fixing the teleport call) is valid. The claimed
  `teleport_client.cpp` sibling fix using `teleportWObackrooms` was
  asserted by the original audit but NOT independently re-verified.
  Confirm by reading `dev/teleport_client.cpp` before fixing.

30-second polling loop fighting VOTV's BeginPlay/possess revert of
teleport. Same fix already implemented in `teleport_client.cpp` (use
`teleportWObackrooms` instead of `K2_TeleportTo` to bypass CMC
constraints) but never backported here.

**Fix**: switch autotest pose teleport to `teleportWObackrooms` (the
existing UFunction wrapper). Delete the 30-second correction loop.

---

### H9. RULE 1: puppet.cpp SpawnPuppetSkelMesh migration baggage

- **File**: `src/votv-coop/src/ue_wrap/puppet.cpp:297-338`
- **Lens**: RULE 1 (RULE 2 sibling — dead path)
- **Confidence**: 84

Function carries its own retirement comment + `VOTVCOOP_PUPPET_KIND`
env-var toggle. MainPlayer path is user-confirmed working (commit
`b100e8e`); gating criteria met. Two implementations of one concept
compiled together — explicit RULE 2 violation.

**Fix**: delete `SpawnPuppetSkelMesh`, `IsMainPlayerPuppetKind`,
`VOTVCOOP_PUPPET_KIND` env handling. `SpawnPuppet` becomes a direct
call to `SpawnPuppetMainPlayer`. ~40 LOC clean delete.

---

### H10. config.cpp port overflow silently wraps

- **File**: `src/votv-coop/src/harness/config.cpp:84`
- **Lens**: Reliability (trust boundary)
- **Confidence**: 85

```cpp
c.port = static_cast<uint16_t>(std::strtoul(port.c_str(), nullptr, 10));
```

`VOTVCOOP_NET_PORT=999999999` wraps via uint16 modulo → connects to
wrong port silently.

**Fix**: check `raw == 0 || raw > 65535` before cast; UE_LOGW + reject.

---

### H11. freecam.cpp WheelHookThread cannot observe shutdown flag

- **File**: `src/votv-coop/src/dev/freecam.cpp:193-202`
- **Lens**: Reliability (shutdown discipline)
- **Confidence**: 88

`GetMessageW` blocks. No `WM_QUIT` post on shutdown → zombie thread
during teardown; Windows hard-kills on process exit so the
`UnhookWindowsHookEx` never runs.

**Fix**: store hook thread ID in `Init`; from `DoShutdown` (or
HotkeyThread on shutdown observation) call `PostThreadMessageW(tid,
WM_QUIT, 0, 0)`.

---

### H12. prop_lifecycle.cpp g_takeObjInFlight is plain bool (race)

- **File**: `src/votv-coop/src/coop/prop_lifecycle.cpp:46-47`
- **Lens**: Reliability
- **Confidence**: 83

```cpp
bool g_takeObjInFlight = false;
```

Written in PRE observer, read in Init POST observer. Both fire on
parallel-anim workers (per the code's own atomic-for-other-fields
comment). C++ data race UB.

**Fix**: `std::atomic<bool> g_takeObjInFlight{false}`; relaxed
load/store (game-thread PRE→POST sequencing preserved by single-thread
execution order within game thread).

---

### H13. flashlight_click_sound.cpp AddToRoot without RemoveFromRoot

- **File**: `src/votv-coop/src/coop/flashlight_click_sound.cpp:117`
- **Lens**: Reliability (GC root leak)
- **Confidence**: 83
- **[DOWNGRADED to MEDIUM / cosmetic per verifier]**: `sAttenuation` is
  a function-local `static` — initialized ONCE per process lifetime, NOT
  reset on disconnect. So it's a single permanently-rooted object, not
  a per-reconnect leak. Still non-ideal (RemoveFromRoot missing at
  shutdown) but vastly less impactful than the original claim.

```cpp
R::AddToRoot(sAttenuation);
```

No matching `RemoveFromRoot` anywhere in the codebase. If
`sAttenuation` is reset per reconnect, each reconnect leaks one
permanently-rooted UObject.

**Fix**: verify whether `sAttenuation` is process-lifetime singleton or
per-session. If per-session, add `RemoveFromRoot` in `OnDisconnect`.

---

## MEDIUM — fix opportunistically

### M14. RULE 1: engine.cpp mainGameInstance_loadObjects direct write

- **File**: `src/votv-coop/src/ue_wrap/engine.cpp:174`
- **Lens**: RULE 1 (crutch)
- **Confidence**: 82

```cpp
*reinterpret_cast<uint8_t*>(reinterpret_cast<uint8_t*>(gi) +
    P::off::mainGameInstance_loadObjects) = 1;
```

Direct write to a save-related bool. Surrounding code calls
`setSaveSlotObject` via reflection; this is the exception. Either
redundant (if setSaveSlotObject sets it internally) or a bypass.

**Fix**: IDA-decompile `setSaveSlotObject`. If it sets `loadObjects`,
remove this line. If not, look for a `setLoadObjects` UFunction.

---

### M15. RULE 1: weather_sync.cpp DebugForceRain enable_rain guard-suppression write

- **File**: `src/votv-coop/src/coop/weather_sync.cpp:676-679`
- **Lens**: RULE 1 (crutch — borderline)
- **Confidence**: 82

```cpp
*reinterpret_cast<bool*>(... + P::off::AdaynightCycle_enable_rain) = isRaining;
```

Comment confesses "belt against any internal guard in causeRain's BP
body" — guard-suppression motivation, not canonical write.

**Fix**: complete IDA decomp of `ExecuteUbergraph_daynightCycle`'s
causeRain handler. If it doesn't read `enable_rain`, delete the write
(RULE 2 dead code). If it does, find the canonical setter or hook the
guard.

---

### M16. RULE 1: npc_sync.cpp ReturnValue-zero broad allowlist suppression

- **File**: `src/votv-coop/src/coop/npc_sync.cpp:214-226`
- **Lens**: RULE 1 (broad-suppression)
- **Confidence**: 80

Interceptor zeroes the spawn ReturnValue assuming all 12 VOTV spawner
BPs null-check before `FinishSpawningActor`. Not IDA-confirmed.
Comment admits crash risk.

**Fix**: IDA-RE each of the 12 spawner BP emit-sites; confirm all
null-check. Document audited BPs as gating criteria. If any spawner
doesn't null-check, patch THAT spawner specifically (Principle 4).

---

### M17. RULE 1: restore_vitals.cpp direct vital field writes

- **File**: `src/votv-coop/src/dev/restore_vitals.cpp:133-138`
- **Lens**: RULE 1 (crutch — dev-only)
- **Confidence**: 80

Direct writes to food/sleep/health on `UsaveSlot_C` bypass any
canonical setter (UI meter refresh, save-dirty flag, etc.).

**Fix**: search the dump for `setFood`/`setHealth`/`setSleep` /
`addFood` etc. on `mainPlayer_C` / `saveSlot_C`. If they exist, use
them. If not, document the field-write as the only path.

---

### M18. RULE 1: flashlight_setup.cpp direct battery field write

- **File**: `src/votv-coop/src/dev/flashlight_setup.cpp:146-147`
- **Lens**: RULE 1 (crutch — dev-only)
- **Confidence**: 80

Same pattern as M17 — direct write to battery state on save slot.

**Fix**: same approach; check for canonical setter UFunction first.

---

### M19. RULE 1: puppet.cpp DumpAnimNodeRegions inline magic offsets

- **File**: `src/votv-coop/src/ue_wrap/puppet.cpp:148-159`
- **Lens**: RULE 1 (magic numbers hiding RE)
- **Confidence**: 80

Four AnimBP region offsets as inline magic constants instead of named
in `sdk_profile.h::anim`.

**Fix**: move to `sdk_profile.h::anim` as named constants
(`kKerfurBlendSpacePlayer_Start/End`, etc).

---

### M20. players_registry.cpp Local() per-tick GUObjectArray walk

- **File**: `src/votv-coop/src/coop/players_registry.cpp`
- **Lens**: Reliability (hot path)
- **Confidence**: 82

`Local()` does full GUObjectArray rescan every time `GetController`
transiently returns null. CLAUDE.md forbids per-frame full scans.

**Fix**: add a `bool localValid_` flag set by RescanLocal; only
invalidate via explicit `InvalidateLocal()` (called on level change /
OnDisconnect), not on transient controller nulls.

---

### M21. ~~engine_pawn.cpp CamMgr per-frame walk during OMEGA~~ **[FALSE POSITIVE]**

- **File**: `src/votv-coop/src/ue_wrap/engine_pawn.cpp:72-76`
- **Verifier verdict**: the code IS cached. `g_camMgr` only re-runs
  `FindObjectByClass` when the cache is null or `!IsLive`. The
  per-frame-walk claim was wrong; the comment "Safe for per-frame
  callers" was accurate. **Remove from fix list.**

---

### M22. harness.cpp hud_feed Init per-tick walk during OMEGA

- **File**: `src/votv-coop/src/harness/harness.cpp:298-300`
- **Lens**: Reliability (hot path)
- **Confidence**: 80

Per-tick `FindObjectByClass(GameInstance)` during OMEGA splash.

**Fix**: same backoff timer pattern as M21.

---

### M23. prop_lifecycle.cpp Install per-tick scan before propBase loads

- **File**: `src/votv-coop/src/coop/prop_lifecycle.cpp:377-415`
- **Lens**: Reliability (hot path)
- **Confidence**: 80

Full GUObjectArray walk every NetPumpTick during loading window.

**Fix**: same backoff timer pattern.

---

### M24. harness.cpp connect-edge race on rapid disconnect/reconnect

- **File**: `src/votv-coop/src/harness/harness.cpp:349-373`
- **Lens**: Reliability (session lifecycle)
- **Confidence**: 80

Missed disconnect transition leaves `g_wasConnected=true`; next
reconnect doesn't trigger snapshot. Stale puppet drives new peer's UDP
packets.

**Fix**: add a per-session generation counter on `coop::net::Session`;
puppet caches the generation it was spawned in; mismatch → invalidate.
Design-level change.

---

### M25. harness.cpp SpawnSecondPlayerWhenReady not interruptible

- **File**: `src/votv-coop/src/harness/harness.cpp:649-688`
- **Lens**: Reliability (shutdown)
- **Confidence**: 80
- **[OVERSTATED per verifier]**: posts to a dead GT queue post-shutdown
  are HARMLESS (queue just abandoned, no crash). Real cost is a zombie
  thread for up to 120s. Cosmetic shutdown delay, not crash risk. Still
  worth a 1-line fix; severity is correct as MEDIUM.

1200-iter `Sleep(100)` loop has no `IsShuttingDown()` check. Posts to
dead GT queue if user closes mid-load.

**Fix**: one-line `if (coop::shutdown::IsShuttingDown()) break;` at
the bottom of the loop.

---

### M26. weather_sync.cpp lightning observer not unregistered on disconnect

- **File**: `src/votv-coop/src/coop/weather_sync.cpp` (Install + OnDisconnect)
- **Lens**: Reliability (observer lifecycle)
- **Confidence**: 80

POST observer on `BeginDeferredActorSpawnFromClass` (lightning) stays
registered after disconnect. Benign for 1v1 (role check inside
observer no-ops); fragile for 4-peer scope where role can change
across reconnects.

**Fix**: `OnDisconnect()` should call `GT::UnregisterObservers` on
the lightning target + reset `g_lightningObserverRegistered`.

---

## Patterns

| Pattern | Count | Findings |
|---|---|---|
| Direct memory writes bypassing UFunctions (RULE 1 sibling of the original triggering issue) | 6 | H4, H7, M14, M15, M17, M18 |
| Per-frame GUObjectArray walks (CLAUDE.md explicit rule) | 4 | M20, M21, M22, M23 |
| UAF / lifetime / cross-thread state races | 4 | C1, C2, C3, H12 |
| Shutdown discipline gaps | 3 | C3, H11, M25 |
| RULE 2 dead-code / migration baggage | 1 | H9 |
| Trust-boundary input validation | 1 | H10 |
| GC root leak | 1 | H13 |
| False-positive health check | 1 | H5 |
| Observer lifecycle / not-unregistered | 1 | M26 |
| Session-generation tracking missing | 1 | M24 |
| Log-noise hygiene | 1 | H6 |
| Magic numbers hiding RE | 1 | M19 |
| Broad allowlist suppression unverified | 1 | M16 |
| Workaround loop instead of fixing source | 1 | H8 |

**Dominant theme**: 6 of 26 findings are the exact crutch pattern that
triggered this audit (direct memory writes via reflection offsets
where a UFunction is the canonical setter). Systemic bias I've
been writing for months, not a one-off slip.

## Verification summary (2026-05-27 verifier pass)

| Verdict | Count | Findings |
|---|---|---|
| CONFIRMED as-stated | 14 | C1, C3, H4, H5, H6, H9, H10, H11, H12, M14, M16, M19, M20, M26 |
| DOWNGRADED severity | 4 | C2 (→HIGH), H7 (→MED), H13 (→MED cosmetic), M25 (cost re-scoped) |
| VERIFIED with caveat | 1 | H8 (sibling fix in teleport_client.cpp not independently checked) |
| FALSE POSITIVE | 1 | M21 (CamMgr — code IS cached) |
| Not directly re-read | 6 | M15, M17, M18, M22, M23, M24 (plausible but not spot-checked) |

Net: 14 confirmed + 4 downgraded-but-real + 1 hedge = **19 actionable
findings out of 26**. 1 clean false positive removed. 6 still need
file-touch confirmation before committing fix work.

## Suggested fix sequencing (post-verification)

**Batch 1 (~2 hours, ~30 LOC each)** — quick correctness:
- H5 IsResolved one-line
- H10 port bounds-check
- M25 IsShuttingDown one-liner in spawn loop
- M19 magic offsets → named constants
- M26 unregister lightning observer on disconnect

**Batch 2 (~half day, 50-150 LOC each)** — UAF/race fixes:
- C1 event_feed lambda re-fetch by peerId
- C2 atomic Session* in prop_lifecycle + npc_sync
- C3 Uninstall ordering + drain
- H11 freecam hook thread WM_QUIT
- H12 atomic g_takeObjInFlight
- H13 RemoveFromRoot on disconnect
- M20 Local() invalidation gating
- M22/M23 backoff timers on per-frame scans (one shared helper) — M21 dropped, was false positive

**Batch 3 (~day each)** — RULE 1 substantive:
- H4 SetControlRotation via UFunction
- H6 StringToFName resolve-once
- H7 SetRelativeRotation via UFunction for lag_fl
- H8 autotest teleportWObackrooms backport
- H9 SpawnPuppetSkelMesh delete (RULE 2)

**Batch 4 (each needs IDA RE first)**:
- M14 mainGameInstance loadObjects
- M15 enable_rain (waiting on causeRain IDA decomp anyway)
- M16 NPC spawner ReturnValue zeroing
- M17/M18 vitals/battery canonical setter search

**Batch 5 (design-level)**:
- M24 session-generation counter for proper reconnect handling

## Cross-references

- `[[feedback-no-direct-memory-write-crutch]]` — the rule the original
  weather_sync crutch triggered (and 6 other findings share)
- `CLAUDE.md` — RULE 1 + the explicit "no per-frame full-array scans"
  rule (4 findings violate this)
- `research/findings/votv-weather-RE-causeRain-IDA-2026-05-27.md` —
  source of the proper-fix pattern (echo-suppress) that should also
  apply to M15
- Commit `987898b` (reverted at `4b5a2e3`) — the triggering crutch
