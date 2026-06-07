# Authoritative Interactable Migration — "the mod IS the engine"

**Status:** PHASE A (`coop::Door`) + PHASE B (`coop::Keypad`) IMPLEMENTED 2026-06-06
(uncommitted; build+audit+smoke clean, hands-on-pending). Enabled by the new BP-disassembly
capability (`tools/bp_reflect.py`, see [[project-bp-reflection-capability]]).

> **The §4 inventory below is SUPERSEDED + EXPANDED by the recon roadmap:**
> `research/findings/votv-coop-class-clone-migration-roadmap-2026-06-06.md` — it covers ALL the
> fragile-BP-mirror mechanics (door✓/keypad✓ done; NPC, time-of-day/dark-world, trash-clump dup,
> grab/throw, flashlight, weather, drone) with per-mechanic disassembly verdicts + a prioritized
> order. Read the roadmap for the current plan; this doc remains the door/keypad design record.

> **The decision in one line.** Stop *observing and mirroring* VOTV's pure-Blueprint
> interactables (fragile: their verbs are unobservable and unforceable). Instead
> **reimplement the Blueprint's decision logic in our own C++ layer, own it
> authoritatively, and drive the engine Blueprint as a puppet** — exactly the MTA
> "be the engine" principle and our own Principle 3 (parallel class hierarchy).
> `RemotePlayer` already works this way; this plan generalizes it to every fragile
> interactable.

---

## 0.5 — AUDIT CORRECTIONS (2026-06-06; two independent byte-exact audits)

Two audits (architecture/MTA-fidelity + correctness-vs-disassembly) checked this plan against
the kismet bytecode. The keypad analysis HELD. The **Door design had a load-bearing factual
error** — corrected here; apply before coding. (The body sections below are marked where they
are superseded.)

**CRITICAL (Door): the E-press IS power-gated — "the door has no lock" was WRONG.**
- `door.player_use` is an EMPTY STUB. The real E-press path is `actionOptionIndex`, which at
  door-ubergraph offset 3533 checks the door's **`active` (power)** BEFORE toggling:
  `active==true` → toggle (`doorOpen(true)`/`doorClose(true)`); `active==false` → the
  `snd()`/ignoreBlackout branch, **NO open.** SP does NOT open an unpowered door on E.
- The keypad controls it: `passwordLock.setActive` runs **`door.active = self.active`**. A
  "keypad-locked" door is held `active=false`; the keypad accept (`open2`/`setActive`) sets
  `door.active=true`. `active` is also driven by the gamemode power trigger (`door.runTrigger`
  index 2/3) and persisted in the save.
- **The REAL lock = the door's own `active` (power) + `superClosed`/`jammed` — NOT `passlock.isAcc`.**
  Our old `IsLocked = passlock.Active && !passlock.isAcc` was wrong in the `!isAcc` term (a hover
  flag) but the `Active`/power HALF pointed at the right concept.
- **FIX (supersedes §1 + §5.1):** the host must NOT make the door a power-blind
  `doorOpen(bypassCheck=true)` toggle — that opens doors SP keeps shut. The host applies the
  toggle the way SP's `actionOptionIndex` does: **gate on the door's own `active`
  (+ `superClosed`/`jammed`), then toggle** — i.e. call `doorOpen(/*bypass=*/false)` (the
  door's engine gates it), or check `active` in C++ then `doorOpen(true)`. We still DELETE the
  fictional `passlock.isAcc` lock; we REPLACE it with the door's real `active` gate (field
  changes, gate concept stays). The keypad accept is what sets `door.active=true` to unlock.
- `DoorState` should carry `{isOpened, active, superClosed}` (or keep the client a pure render
  slave that never gates locally, with the host gating on its own fields). jammed/superClosed
  rejection is IMPLICIT (host calls the gated open, engine no-ops, host broadcasts nothing,
  client converges) — no explicit DENY packet. The hold register tracks CLIENT holds only.

**Keypad holes to resolve in Phase B (verified real):**
- **`isReset` branch:** if `isReset==true`, `open` WRITES `password = inPassword` (set-new-code).
  The host must read `isReset` and, when set, write the new password — NOT trigger an accept.
- **Pair keypad:** `setActive`/`reset`/set-code propagate to `self.pair`
  (`pair.active`/`pair.password`/`pair.upd()`). `coop::Keypad` must propagate accept/power/
  password to the keypad's `pair`, or paired panels desync.
- **Accept side-effects:** on `inPassword==password`, set `door.active=true` (unlock) + open the
  gated door via `coop::Door`, clear the authoritative buffer + broadcast a clear so client
  displays wipe. Open-timing decision: SP delays ~4 s via `open2`; recommend **instant + document
  the delta** (the host's local `open2` fires ~4 s later as a no-op since the door is already open).
- **PREREQ:** verify the live `password` field offset with `keypad_probe` before coding accept
  (don't trust the disassembly offset blindly). `password`/`active` are host-authoritative — the
  host evaluates accept against its OWN `password`; clients never validate locally; do NOT
  replicate `password` across peers.

**MTA citation fix (§2):** `CObjectSync` is `#ifdef WITH_OBJECT_SYNC` (disabled in the live
build) — cite **`CUnoccupiedVehicleSync.cpp`** (the live single-syncer election:
STARTSYNC/STOPSYNC) for the authority model. Confirm the reliable ARQ lane is ordered within a
connection (if yes, no stale-drop/`SyncTimeContext` needed; if not, add a per-door monotonic seq
to `DoorState`).

**Deletions to add (§7):** also delete `ue_wrap::door::IsLocked` AND `ue_wrap::door::DumpLockStates`
(both read the now-fictional passlock `isAcc`). File-size: Phase A leaves `interactable_sync.cpp`
~775 LOC (under the 800 cap); `coop/keypad_sync.cpp` (315) fully deleted; new `coop/door.{h,cpp}`
+ `coop/keypad.{h,cpp}` target < 400 LOC each.

**Confirmed SOLID (do not second-guess):** the Authoritative-Interactable pattern; keeping the
door Channel machinery (hold register, settling bridge, sensor suppression, connect-snapshot); the
RemotePlayer parallel; deleting the `isAcc/isDeny` mirror + the `!isAcc` term; NOT invoking
`open2`; the Phase A→B→C order; `bp_reflect` as a permanent ladder rung. The keypad accept model
(`inPassword==password`, host-authoritative) is verified correct.

---

## 1. Why — the field-mirror dead end

VOTV's gameplay objects (keypad, door, lightswitch, container, flashlight, …) are
**pure Blueprint**. Two hard facts, now disassembly-proven:

- **Unobservable.** A BP verb invoked from BP dispatches `CallFunction →
  ProcessInternal`, *bypassing* our `ProcessEvent` detour. A POST observer on
  `doorOpen` / the keypad accept / `updateFlashlight` never fires (the original
  "doors `sent=0`" bug; `interactable_sync.cpp:652-656`, IDA-confirmed).
- **Unforceable.** Calling the accept/submit verb via reflection is inert
  (`passwordLock` accept does nothing even with the buffer filled;
  `passwordlock.h` "the NATIVE ACCEPT is unreachable by us").

So the pre-disassembly subsystems coped by **OBSERVE** (hook a UFunction we hoped
was observable) or **FIELD-MIRROR** (poll fields each tick and write them on the
mirror). Both are fragile *because we were syncing a black box we couldn't read*.
The cost surfaced as real bugs:

- `keypad_sync` mirrored `isAcc`/`isDeny` — which the disassembly proved are
  **crosshair-hover flags**, not accept/deny state. The "purple" was us writing
  both hover flags onto a mirror. We synced noise.
- The door's coop lock `IsLocked = Active && !isAcc` was wrong in the `!isAcc` term
  (a crosshair-hover flag) — which is why the host wrongly *denied* a client's
  E-press. **(CORRECTION, see §0.5: the door is NOT lock-free — its E-press is gated
  on the door's own `active`/power at offset 3533; the `Active`/power half of the old
  check pointed at the right concept. We replace the fiction with the real `active`
  gate, not remove the gate.)**

We could not have known any of this without reading the BP. Now we can
(`tools/bp_reflect.py`). The dead end ends here.

Full ground truth: `research/findings/votv-keypad-door-BP-disassembly-2026-06-06.md`.

---

## 2. The principle (MTA fidelity)

MTA does not ask GTA what happened — **MTA reimplements the gameplay and drives the
engine.** `CClientVehicle` / `CClientObject` own the authoritative state and push it
into GTA every frame; `CObjectSync` runs a single-syncer authority model;
**keysync replicates INPUT** and lets each client's deterministic sim resolve the
*output* (never replicate a derived output by replaying the verb that derives it).

Our analog already exists and is our reference: **`RemotePlayer`** owns the pose
state machine and `ApplyToEngine()` drives a `mainPlayer_C` *puppet* (transform +
CMC velocity) each tick. No per-frame BP observe, no verb forcing. The engine actor
is a render/physics slave; our C++ owns the decisions and the network.

**This plan applies that same shape to interactables.** The engine Blueprint becomes
the puppet; a small authoritative C++ class becomes the brain.

MTA precedent (vendored, `reference/mtasa-blue/`): single-syncer authority
(`CObjectSync`), client-state-ownership-and-drive (`CClientVehicle`,
`CClientPed::m_pPlayerPed -> CPlayerPed*`), input-replication
(`CKeySyncPacket`/`CNetAPI`). Catalog in `memory/feedback_follow_mta_architecture.md`.

---

## 3. The target pattern — "Authoritative Interactable"

A migrated subsystem is a C++ class in `coop/` (Principle 7's gameplay/network
layer) with four responsibilities:

1. **Own the authoritative state** — the *minimal* real state the BP computes (e.g.
   a door's `isOpened`; a keypad's typed buffer + a derived `unlocked` bool). Never
   trust the engine's copy; ours is the source of truth.
2. **Reimplement the decision logic in C++** — transcribed from the disassembly
   (e.g. keypad accept = `inPassword == password`; door open rule = `¬jammed ∧
   ¬superClosed ∧ (manual ∨ powered)`).
3. **Drive the engine puppet** via the existing `ue_wrap/` "drive" primitives
   (`SetActorLocation`, `CallDoorOpen`, `WriteField`, `CallInputNumber`, …) so the
   visual/physics match our state. The engine BP renders; it does not decide.
4. **Network host-authoritatively** (MTA `CObjectSync` single syncer = the host for
   non-owned world objects): a client's *interaction* becomes a **request** to the
   host; the host runs the authoritative logic and **broadcasts the resulting
   state**; every peer drives its puppet from that state. We replicate *intent +
   authoritative state*, never a derived field we don't understand.

`ue_wrap/` stays exactly as it is — it is already the rich "drive the engine" half
(see the primitive inventory in §6). We are moving the *decisions* out of the engine
BP and into `coop/`.

---

## 4. Migration inventory (from the 2026-06-06 code audit)

| Subsystem | Approach today | Fragility | Verdict |
|---|---|---|---|
| **keypad_sync** (`coop/keypad_sync` + `ue_wrap/passwordlock`) | FIELD-MIRROR `isAcc/isDeny/inPassword` | **HIGH** — accept verb unreachable; mirrored hover flags; "purple" | **MIGRATE → `coop::Keypad`** |
| **doors** (`coop/interactable_sync` door channel + `ue_wrap/door`) | OBSERVE→fell back to FIELD-MIRROR `isOpened`; fake `IsLocked` | **HIGH** — `doorOpen` unobservable; lock was fiction | **MIGRATE → `coop::Door`** |
| **flashlight** (`coop/item_activate`) | OBSERVE (5-UFunction hedge); receiver avoids the BP update-graph | **MED-HIGH** — `updateFlashlight` BP-inlined | **MIGRATE (later) → `coop::Flashlight`** |
| **weather** (`coop/weather_sync` + `_fog/_lightning/_redsky`) | HOST-OBSERVE+BROADCAST; fog already a host-auth flag-poll | **MED** — 3-way split; observers may degrade | **MONITOR** — migrate lightning/redsky to flag-poll if they degrade |
| lightswitch / container (`interactable_sync`) | FIELD-MIRROR toggle (Symmetric) | LOW — no auto-revert fight | Fold into the `coop::Door` pattern opportunistically |
| **remote_player** | **OWN+DRIVE** | LOW | **REFERENCE PATTERN** (the template) |
| props (`remote_prop`/`prop_lifecycle`), trash clump, npc, grab, balance, player_damage, sounds | OWN+DRIVE or one-time OBSERVE (spawn/destroy is ProcessEvent-observable) | LOW | **Fine as-is** |

The HIGH-fragility set — **keypad + doors** — is the whole reason this plan exists
and is done first. Flashlight follows. Everything else is already correct or a
softer pattern that can wait.

---

## 5. Per-subsystem designs (the HIGH-fragility migrations)

### 5.1 `coop::Door` — FIRST

**Disassembled truth:** the door has no lock. `player_use` toggles via
`doorOpen(bypassCheck=true)`; open ⇔ `¬jammed ∧ ¬superClosed ∧ (bypassCheck ∨
(¬isMoving ∧ ¬isOpened ∧ active))`. The E-press path ignores power and the keypad.
The real manual blockers are `jammed`/`superClosed`. Save persists the door's own
`isOpened`.

**Authoritative class:**
- Owns `isOpened` per door (keyed by the door's `Key`) + a per-peer "holding open"
  register (MTA `CObjectSync` door-hold shape; we already have this in the channel).
- **Host:** on a client `DoorToggleRequest` (or the host's own E-press), run the SP
  rule **as `actionOptionIndex` does (CORRECTION per §0.5 — NOT a power-blind
  `bypass=true` toggle):** gate on the door's own `active`/`superClosed`/`jammed`,
  then toggle — `CallDoorOpen(door, /*bypass=*/false)` (the engine gates it) or check
  `active` in C++ then `CallDoorOpen(door,true)`. Then broadcast
  `DoorState{key, isOpened, active, superClosed}`. (The keypad accept sets
  `door.active=true` to unlock — §5.2.)
- **Client:** drives its puppet door to the broadcast `isOpened` (animate if near
  via the existing SmartApply, snap if far). Client's E-press → `DoorToggleRequest`
  to the host; no local lock check, no fictional gate.
- **DELETE:** `ue_wrap::door::IsLocked` and the `keypad-locked` DENY in
  `interactable_sync.cpp Channel::OnRequest`. (RULE 2 — the fiction goes.)
- **Keep:** the client-autonomy suppression (sensor/autoclose) is still correct —
  it stops the engine door's own logic from fighting our authoritative state.

### 5.2 `coop::Keypad` — SECOND

**Disassembled truth:** `isAcc/isDeny` are crosshair-hover flags (ignore). Typing
fills `inPassword`; when `inPassword == password` the BP runs `open`; the door is
opened by `open2 → door.doorOpen(false)` (needs power). The keypad light is
`ParticleSystem3` driven by power/`active`, not isAcc. Save persists
`password`+`active`, not isAcc/inPassword.

**Authoritative class:**
- Owns, per keypad (keyed by `Key`): the **typed buffer** and a derived
  **`unlocked`** bool. We can READ `password` (offset known) and compare ourselves —
  we no longer need the BP's inert accept verb.
- **Input:** a player's digit/clear on a keypad → `KeypadInput{key, digit|clear}`
  request to the host (MTA keysync: replicate the INPUT). The host appends to the
  authoritative buffer.
- **Accept (host, authoritative):** when `buffer == password`, set `unlocked=true`
  and tell `coop::Door` (the gated door, read from the keypad's `door` field) to
  open. We *decide* accept in C++; we do not call the inert BP verb.
- **Drive (all peers, display only):** mirror the buffer to the engine keypad's
  `inPassword` (via `CallInputNumber`/`CallReset`, already built) so the panel shows
  the digits; the light is power-driven and already equal across peers (don't drive
  it from a mirror — that was the original purple).
- **DELETE:** the entire `isAcc/isDeny` broadcast+apply, the 2026-06-06
  mutual-exclusivity/poll-repair/`upd`-gating/`ReadActive`-diag machinery (all built
  on the hover-flag misunderstanding). (RULE 2.)

### 5.3 `coop::Flashlight` — THIRD (later phase)

Own on/off + mode + intensities in C++; drive the puppet's `light_R` directly via
the *engine* UFunctions (`SetIntensity`/`SetVisibility`/cone angles — already not
BP), dropping the 5-observer hedge and the BP update-graph avoidance.

---

## 6. `ue_wrap/` "drive" primitives — already available

No new wrapper work is needed up front; the drive half exists:

- `ue_wrap/engine`: `SetActorLocation/Rotation`, velocity, `GetActor*`.
- `ue_wrap/engine_component`: component physics (`SetSimulatePhysics`, linear/ang vel).
- `ue_wrap/call` (`ParamFrame` + `Call`): dispatch ANY UFunction by name.
- `ue_wrap/door`: `CallDoorOpen/Close`, `SmartApply`, `ForceOpen/Close`,
  `SuppressClientAutonomy`.
- `ue_wrap/passwordlock`: `CallInputNumber`, `CallReset`, read `inPassword`/
  `password`/`active`.
- `ue_wrap/puppet`: `WriteAt<T>/ReadAt<T>` raw-offset access.

The migration *removes* code (the field-mirror brains) far more than it adds; the
new C++ classes are thin and call existing primitives.

---

## 7. Deletions (RULE 2 — no parallel old + new paths)

When each class lands, the corresponding fragile code is removed in the SAME change:

- `coop::Door`: delete `ue_wrap::door::IsLocked` + the keypad-locked DENY gate; the
  door channel's symmetric/fake-lock branches.
- `coop::Keypad`: delete `coop/keypad_sync`'s isAcc/isDeny mirror + the purple
  machinery + `passwordlock::WriteAccepted/WriteDenied/ReadActive` (the hover-flag
  writes); keep `CallInputNumber`/`CallReset` (display drive).
- `coop::Flashlight`: delete `item_activate`'s 5-observer hedge.

---

## 8. Phasing & verification

Each phase: **`bp_reflect` the BP → transcribe the logic → build the authoritative
class → drive the puppet → network host-auth → delete the old path → hands-on
verify**, then an MTA-fidelity audit agent (methodology WP2).

- **Phase A — `coop::Door`.** Reimplement the toggle; delete the fake lock. Verify
  doors open/close identically in BOTH directions (client→host AND host→client),
  near and far, no desync. This alone fixes the user's #1 door bug.
- **Phase B — `coop::Keypad`.** Own the buffer + accept; on accept open the gated
  door via `coop::Door`. Verify: a client entering the code opens the host's door
  for everyone; wrong code does not; no purple; digits wipe after submit.
- **Phase C — `coop::Flashlight`**, then weather monitor (migrate lightning/redsky
  to flag-poll only if they degrade).

Open dependency to resolve in Phase B: the keypad's `open2` (the BP's own
door-opener) is triggered *externally* (an action-menu delegate not in the two BPs
we dumped) — but it is irrelevant to our design, because **we** decide accept and
open the door directly; we never invoke `open2`. (Noted so a future reader doesn't
hunt for it.)

---

## 9. What this buys us

- **Correctness:** the gameplay matches single-player because we run the real,
  disassembled logic — not a guessed mirror.
- **Robustness:** zero dependence on observing or forcing BP verbs (the entire class
  of bugs that ate this session).
- **Generality:** every future interactable is a small authoritative class driving a
  puppet — the same shape as `RemotePlayer`, the same shape as MTA. No more
  one-off field-mirror fragility.
- **Less code:** the migrations net-delete the brittle mirror brains.

## 10. The enabler

`tools/bp_reflect.py` (repak + kismet-analyzer) is now a permanent rung on the
escalation ladder: **reflection → BP disassembly → IDA → UE4SS**. Any interactable
we migrate starts by dumping its BP and transcribing the real logic. Dev/RE only;
nothing ships (RULE 3); we read cooked assets, never modify them (RULE 1).
