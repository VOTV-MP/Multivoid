# VOTV dirt / grime / window-cleaning coop sync — IMPLEMENTATION record (2026-06-08)

Implements the user request (Request 1): "sync + mirror the dirt on the base's main huge WINDOW
and on the other surfaces — walls, ceiling, floor; and if a peer cleans the dirt with a sponge,
sync + mirror that too, in real time."

RE authority (byte-exact, 2 converging agents): `votv-dirt-window-cleaning-RE-and-coop-sync-design-
2026-06-07a.md` + `...-2026-06-07b.md`. This doc records what was BUILT on top of that RE, the
design decisions (including a deliberate divergence from the pre-compaction eid blueprint), the
bugs the smoke/audits caught, and the deferred work.

Status: **Parts A + B SHIPPED** (built, deployed to all 4 folders hash-verified, 4 audit agents
0-CRITICAL, LAN smoke PASS x5). **HANDS-ON PENDING:** the static smokes prove identity + plumbing +
connect-snapshot but never actually *wiped*, so the wipe→mirror VALUE path is unexercised
autonomously (it is the proven min-wins poll+apply shared with the doors/keypad sync). Protocol
`v40 → v41` (window) → `v42` (grime).

---

## The 3 substrates (from the RE) and what each became

| Substrate | Class | Dirt state | Coop model SHIPPED |
|---|---|---|---|
| **Main huge window** | `AbaseWindow_C` | whole-surface scalar `clean`@0x0260 | **Part B** — keyed by `Aactor_save_C::Key`; symmetric min-wins |
| **Walls/ceiling/floor grime** | `Agrime_C` (+~20 subclasses) | per-decal scalar `process`@0x0250 | **Part A** — keyed by QUANTIZED WORLD POSITION; symmetric min-wins |
| **Radio-signal decode panel** | `Ad_window_C` | render-target pixels | **DEFERRED** (not requested; RT stroke-replay) |

---

## The unifying model: a keyed monotone-decreasing scalar, MIN-wins

Both window `clean` and grime `process` are dirt amounts that only ever DECREASE (a sponge/rain
lowers them; nothing re-raises them mid-session) and are otherwise INERT (no autonomy re-drives
them, unlike a door's autoclose). That makes a **SYMMETRIC min-register** the root-cause-correct
shape (an MTA-style monotone CRDT — no central authority needed):

- Each peer POLLS every instance's scalar and broadcasts on a DECREASE (a wipe), keyed by the
  instance's cross-peer-stable identity.
- The receiver applies `MIN(local, wire)` — a wire update can ONLY make an instance cleaner, never
  dirtier → two peers wiping concurrently CONVERGE to the lowest value with NO oscillation and NO
  regression/flicker. (Contrast doors, which toggle both ways → oscillate → needed HostAuth + a
  hold register; dirt needs none of that.)
- The host RELAYS a client's wipe to the other clients (in `IsClientRelayableReliableKind`).
- On a new client's connect edge the host snapshots each instance's current scalar with `adopt=1`
  (apply VERBATIM, so the joiner adopts the host's world even if its own save was cleaner). `adopt`
  is **trust-gated to senderPeerSlot==0** at the receiver — a client edge is always forced to a
  min-wins live wipe so it can never force-DIRTY another peer's surface.
- SENDER is a POLL, not a UFunction observer: the cleanSponge / clean() verbs are BP-internal
  (CallFunction→ProcessInternal, bypass our ProcessEvent detour — the same finding as doors/keypad),
  so a POST observer never fires; polling the resulting STATE field catches every writer.

One wire payload serves both (RULE 2, mirroring how `KeyedTogglePayload` serves 3 bool kinds):
`KeyedScalarPayload { WireKey key; float value; uint8 adopt; uint8 _pad[3]; }` == 40 B. (Renamed
from the v41 `WindowCleanPayload`.) `ReliableKind::WindowCleanState=30` (v41), `GrimeState=31` (v42).

---

## Part B — `AbaseWindow_C` (the main huge window)

- `ue_wrap/base_window.{h,cpp}` — resolves `baseWindow_C`, `clean`@0x0260, `Key`@0x0230 (NOTE: Key
  is on the `Aactor_save_C` parent, so it is resolved against `actor_save_C` — `FindPropertyOffset`
  does NOT climb to parents, same gotcha as the door's `triggerBase_C::Key`), and the zero-arg
  `setClean()` UFunction. `WriteCleanAndApply` writes the field then calls `setClean()` (a pure
  `SetCustomPrimitiveDataFloat(0, clean)` setter that reads `this->clean`).
- `coop/window_sync.{h,cpp}` — the small symmetric keyed-float channel: index by Key, poll `clean`,
  broadcast on decrease, min-apply, connect-snapshot (adopt=1), deferred-apply retry.
- **VERIFIED at smoke:** both peers resolve + index **4 windows** with an IDENTICAL `keysHash`
  (`0x9FA54F41F7D98FE1`) → the window Keys are cross-peer STABLE (the whole identity model rests on
  this). Connect-snapshot fires.

## Part A — `Agrime_C` (walls/ceiling/floor grime)

### The key design decision: POSITION-keying, NOT eid (divergence from the blueprint)

The RE agents (and a pre-compaction code-architect) grouped grime with NPCs — "unkeyed,
runtime-spawnable, self-destructs" → a host-allocated eid + Element registry + a
BeginDeferredActorSpawnFromClass spawn-interceptor (the NPC model). That WORKS but is heavy. The
decisive observation they missed: **grime decals are STATIC** (a level-placed decal with a saved
transform that never moves). So both peers load the same save and place each decal at an IDENTICAL
world position — that position IS a free, deterministic, cross-peer identity. No eid, no Element,
no spawn-interceptor, no connect-time destroy/respawn. grime then rides the EXACT same proven
window/interactable pattern, keyed by a quantized world-position string.

> `PosKey = "g_<round(X/2)>_<round(Y/2)>_<round(Z/2)>_<Type>"` (2 cm grid + the Type disambiguator).
> Determinism: both peers read the same saved float → `std::lround((double)coord/2)` → same key.
> The correctness audit confirmed: the `double` cast removes rounding-mode sensitivity; at VOTV's
> ±40,000 cm base the key is ~24 chars (< the 31-char WireKey cap); no same-cell collision at the
> 2 cm grid. **Smoke PROVED it: host & client index the SAME 936 (then 1006) decals with an
> IDENTICAL posHash** (`0xFDABE7E45C34431E` / `0x27075C55E21ED4A6`).

- `ue_wrap/grime.{h,cpp}` — resolves `grime_C`, `process`@0x0250, `Type`@0x024C, `applyMaterial()`.
  `WriteProcessAndApply` writes `process` then calls the zero-arg `applyMaterial()` (which repaints
  the decal at `process/maxProcess`; maxProcess is a per-instance constant identical across peers,
  so the wire only carries `process`).
- `coop/grime_sync.{h,cpp}` — same channel shape as window_sync, keyed by PosKey, plus the
  grime-specific apply (applyMaterial) and the PosKey cache (below).

---

## Bugs the smoke + audits CAUGHT (and the root-cause fixes)

1. **Death-watch streaming flood (smoke-caught, REMOVED).** My first grime build used an
   IsLiveByIndex death-watch: a decal that vanished from the index was assumed wiped-to-destruction
   → broadcast a destroy. The first smoke flooded HUNDREDS of false destroys on the connect edge —
   the connect-teleport moves the client, its spawn-area sublevel STREAMS OUT, and a streamed-out
   decal goes IsLiveByIndex-false. The host then destroyed its (still-present) matching decals.
   **Root-cause fix (RULE 1, not a heuristic): removed the death-watch entirely.** The `process`
   min-wins stream is inherently streaming-safe (a stream-out doesn't change `process` → broadcasts
   nothing; only a live wipe decreases it). A wiped decal's mirror is driven to `process≈0`
   (invisible). See the KNOWN GAP below.

2. **~1000-decal poll heap-churn (self-review + perf audit, FIXED).** The poll snapshotted every
   ~20-char position key into a scratch vector each tick → ~1000 heap allocs/tick (keys exceed the
   MSVC SSO limit). **Fix: iterate the index IN PLACE** (only the rare wipe key is copied for the
   deferred broadcast) + **throttle the poll to 20 Hz** (a wipe lasts ~1-2 s; the window's 4-window
   poll didn't need this, grime's ~1000 does).

3. **~1000× `GetActorLocation` (ProcessEvent) per 2 s rebuild (perf audit H-1, FIXED).** `PosKey`
   dispatched `GetActorLocation` (a UFunction) for every decal on every rebuild → a recurring ~3 ms
   frame hitch. But a static decal's PosKey never changes. **Fix: cache PosKey per actor pointer** —
   only a newly-streamed decal computes it; the cache rebuilds from the live set each pass (dead
   actors drop, re-streamed decals get a fresh pointer → recompute). Verified the cache PRESERVED
   the keys (posHash still matches host↔client post-fix). [Audit's ideal — a direct
   RootComponent→location field read with no UFunction — is queued.]

4. **NaN guard + adopt range guard (audits, ADDED).** The inbound scalar drives
   `SetCustomPrimitiveDataFloat` / `applyMaterial` (a shader uniform) — a NaN/Inf there is undefined
   and can trip a device-removed crash. event_feed now drops a GrimeState/WindowCleanState with a
   non-finite or negative value (and an out-of-range `adopt`), matching every other float payload.

### RULE-2 cleanups done in passing
- Generalized `WindowCleanPayload` → shared `KeyedScalarPayload` (window clean + grime process).
- Extracted the triple-copied `WireKey`↔wstring + FNV helpers (`interactable_sync` + `keypad_sync`
  + the new `window_sync`) into `coop/net/wire_key_util.h`.

---

## Audit results (4 agents: perf + correctness on each part)

**0 CRITICAL on all four.** Window: NaN guard added, deferred-apply slot logging fixed, one false
positive dismissed (`setClean` confirmed zero-arg). Grime: PosKey cache (H-1), in-place
connect-snapshot (M-2), adopt guard (M-1 correctness) applied. Everything else verified clean:
PosKey determinism + range, MIN-wins convergence, the adopt trust gate, echo suppression,
`g_pending` bounded by a 25 s TTL, lock ordering, iterator safety, `applyMaterial` zero-arg
correctness, sizeof asserts.

---

## DEFERRED / KNOWN GAPS (flagged for follow-up)

1. **Fast-wipe ghost (correctness audit H-1, grime).** A GRADUAL wipe (multiple sponge hits over
   ~1-2 s) is fine: the 20 Hz poll captures the decreasing `process` and drives the mirror to
   invisible. But a SINGLE high-strength hit that drives `process` from a high value straight past 0
   in one poll interval is never captured (the poll's prior value was high, then the actor is gone)
   → that decal's MIRROR stays VISIBLY DIRTY permanently. Root-cause fix: a `K2_DestroyActor` PRE
   edge that reads `process<0` just before the actor dies and broadcasts `value=0` (drive the mirror
   invisible). UNCERTAIN whether that observer fires for the BP-internal `clean()`→`K2_DestroyActor`
   (needs probing). **Verify hands-on whether a one-shot wipe is even achievable in-game** before
   prioritizing.
2. **`applyMaterial` re-creates the dynmat per apply (perf audit M-1).** Cosmetic flicker on a
   remote wipe (infrequent). The native `clean()` fast-path instead does
   `dynmat.SetScalarParameterValue(cleanParameter, process/maxProcess)` directly on the existing
   instance — switch to that if hands-on shows flicker. (dynmat@0x0258, cleanParameter@0x0260,
   maxProcess@0x0268 all known.)
3. **Shared GUObjectArray scanner (perf audit H-2).** grime is now the ~7th independent throttled
   (2 s) full-array walk (door/light/container/keypad/window/grime). The root-cause fix is one
   shared world-object scanner that all keyed-by-class channels subscribe to as filters. Structural,
   not grime-specific.
4. **Runtime projectile-splatter grime** (`grimeProjectile` blood/oil/wine). Non-deterministic spawn
   position → not position-identifiable → would need the eid/spawn path. Not in the stated ask.
5. **`Ad_window_C` signal-decode panel** (the 3-pane radio panel) — render-target stroke replay; not
   requested.

---

## Files

NEW: `ue_wrap/base_window.{h,cpp}`, `ue_wrap/grime.{h,cpp}`, `coop/window_sync.{h,cpp}`,
`coop/grime_sync.{h,cpp}`, `coop/net/wire_key_util.h`.
MODIFIED: `coop/net/protocol.h` (v42, KeyedScalarPayload, WindowCleanState=30, GrimeState=31),
`coop/net/session_lanes.h` (relay whitelist), `coop/event_feed.cpp` (dispatch),
`coop/net_pump.cpp` (Install/Tick/connect-snapshot/OnDisconnect wiring),
`coop/interactable_sync.cpp` + `coop/keypad_sync.cpp` (migrated to wire_key_util.h), `CMakeLists.txt`.

Diagnostics: `[dev] window_log=1` / `grime_log=1` enable per-key dumps. The `keysHash`/`posHash`
lines (logged on a change) are the cross-peer identity-stability check — compare host vs client.
