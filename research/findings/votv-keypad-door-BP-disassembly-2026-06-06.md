# VOTV keypad + door — BP DISASSEMBLY ground truth (2026-06-06)

**Method (reusable):** `tools/bp_reflect.py` — extracts a cooked BP from the game's
unencrypted v11 `.pak` with **repak**, disassembles its kismet bytecode to structured JSON
with **kismet-analyzer** (both trumank, MIT, auto-downloaded to `research/pak_re/tools/`).
Output in `research/bp_reflection/<name>.json` (+ `.functions.txt`). The interactables are
**pure Blueprint** (no native code in the .exe — IDA can't read them; our C++ reflection sees
only field/function *signatures*), so this disassembly is the ONLY ground truth for their LOGIC.
Two independent agents reconstructed the bytecode byte-exact (computed `ScriptBytecodeSize` ==
declared, every jump target lands on a statement boundary) — so the below is decoded, not guessed.

> **This overturns the entire prior keypad/door coop model.** The earlier RE
> (`votv-keypad-passwordlock-accept-RE-and-coop-sync-2026-06-04.md`) inferred the logic from
> field names + probes and got the core facts WRONG. Corrections below.

---

## passwordLock_C (the keypad)

- **`isAcc` / `isDeny` are CROSSHAIR-HOVER flags, NOT accept/deny state.** `lookAt` sets them
  every frame from what the crosshair points at: `isAcc = (HitComponent == self.key_acc)`,
  `isDeny = (HitComponent == self.key_deny)`, `isCard = (HitComponent == self.card)`. They only
  pick the interaction-prompt text ("Enter"/"Cancel"/"Use keycard"). **`isAcc==0` is the normal
  resting state and says nothing about lock/accept.** (This is why the user saw a GREEN keypad
  with `isAcc==0` — green has nothing to do with isAcc.) → Mirroring isAcc/isDeny is meaningless.
- **The green/red VISUAL is the `ParticleSystem3` component**, not isAcc. `powerChanged(active_light)`
  = `ParticleSystem3.SetVisibility(active_light)` (power gates whether the light shows). `upd`
  selects the particle TEMPLATE via a Select keyed on **`isReset` + `self.active`** (the
  accepted/reset glow). Driven by **power/active**, never isAcc.
- **`active` = building POWER** (driven by the gamemode's `powerChanged` event), NOT "unlocked".
  `setActive` propagates power to the `pair` keypad AND to `self.door` (`door.active = self.active`).
- **`open(bool active)`** is the submit handler (called by the auto-validator at `inputNumber`
  when `Len(inPassword)>=5 && inPassword==password`). It sets `self.active` momentarily + plays a
  sound, then clears `inPassword` + `setActive(false)`. **`open()` NEVER calls `doorOpen` and never
  sets a persistent "accepted" flag.** (If `isReset` mode: `open` instead WRITES `password = inPassword`
  — that's the "set a new code" path.)
- **`open2`** is the actual "access-granted" cascade (beep chain → after ~4 s → `self.door.doorOpen(false)`).
  It is invoked EXTERNALLY (keypad `del` dispatcher event #12, via the generic action-menu delegate) —
  the trigger condition is not inside passwordLock.json/door.json. `open2` is the ONLY keypad path
  that opens the door.
- **Save persistence (`getTriggerData`/`loadTriggerData`):** persists `active`, `protected`,
  `pair_key`, `door_key`, `password`. **NOT** persisted: `isAcc`, `isDeny`, `inPassword`, `Num`
  (all transient). So an "unlocked" keypad does NOT persist isAcc — its door's open-ness persists on
  the DOOR (see below).

## door_C (the door)

> **CORRECTION 2026-06-06 PM (full offset-aware CFG trace, kismet-analyzer `gen-cfg`):** the first
> pass below the line got the E-press WRONG — it read `player_use` and concluded "E ignores power."
> `player_use` is a NO-OP. The real E-press IS power-gated. Corrected facts first; the superseded
> claims are struck through.

- **`door.player_use` is a NO-OP.** Its thunk jumps to `ExecuteUbergraph_door(375)`, and ubergraph
  offset 375 is a bare `EX_PopExecutionFlow` → returns. (The events at ubergraph 374–391 — player_use,
  kicked, thrown, playerStepped, … — are all single-pop trampolines, i.e. unimplemented in this door.)
  So the manual door interaction does NOT go through `player_use`.
- **The real manual toggle is power-gated** (byte-exact, blocks verified in `door_cfg/door.txt`):
  - The interaction reaches an `Active` (power) check at **ubergraph offset 3533**:
    `3533: EX_JumpIfNot 3552` with condition the door's **`Active`** instance var (read at 3538).
    `Active==true` → fall to `3547: EX_Jump 438` (the toggle). `Active==false` → 3552: if the gamemode
    `usesp_light` (story power system, the normal case) → return (NO open); else gated by `ignoreBlackout`.
  - The toggle (block 392, reached from 438 when `!isMoving`): `IF isOpened → doorClose(TRUE) ELSE
    doorOpen(TRUE)`. It is preceded by `GetPlayerCameraManager(0)` look-at math (swing-away-from-player)
    — confirming this is the player-interaction path, not a sensor/script path.
- **Exact `doorOpen(bypassCheck)` open condition** (byte-exact, blocks 2797→2908→2922→2937 and
  3354→3369→3384 verified):
  `Open ⇔ ¬jammed ∧ ¬superClosed ∧ ( bypassCheck ∨ (¬isMoving ∧ ¬isOpened ∧ Active) )`
  - The toggle uses `bypassCheck=TRUE` for ACTUATION — so once you're past the 3533 power gate, the
    open itself only needs `¬jammed ∧ ¬superClosed`. The `bypassCheck=FALSE` path (keypad `open2`, the
    sensor) re-checks `Active` itself.
- **NET: a player's E-press opens a door iff `Active ∧ ¬jammed ∧ ¬superClosed`** (in the normal story
  gamemode, `usesp_light==true`). `Active` is the DOOR's own power flag (`Adoor_C::Active` @ 0x0352),
  driven by the gamemode power trigger (`runTrigger` index 2→Active=false, 3→Active=true) + the keypad's
  `setActive` (`door.Active = keypad.Active`) + the save (`loadTriggerData` persists Active/isOpened/
  superClosed/jammed/...). `superClosed`/`jammed` are the manual hard-blocks even when powered.

~~The door has NO keypad/passlock/isAcc/lock logic~~ / ~~`player_use` toggles via doorOpen(true)~~ /
~~E opens the door ignoring power~~ — **all SUPERSEDED by the corrected trace above.** (The door indeed
carries no isAcc logic, but it IS power-gated via its own `Active`, which the keypad/gamemode drive.)

---

## Implications for the coop fix (CORRECTED 2026-06-06 PM, IMPLEMENTED)

1. **WRONG (removed):** the door HostAuth lock `IsLocked = passlock.Active && !passlock.isAcc`
   (`ue_wrap/door.cpp::IsLocked` + the gate in `interactable_sync.cpp Channel::OnRequest`). It read the
   PASSLOCK's `isAcc` — a crosshair-HOVER flag — so a POWERED door (door.Active=1) with isAcc=0 (not
   hovering the accept button) was reported LOCKED and the host DENIED the client's open. THAT is the
   user's "client's first E-press doesn't work, host doesn't see the door open." → **Replaced with
   `ue_wrap::door::CanOpen = Active && !jammed && !superClosed`** (the door's OWN fields, byte-exact
   E-press gate). The host applies a client's open only if `CanOpen`. So coop never opens a door SP
   keeps shut, and a powered door opens immediately (bug fixed).
2. **WRONG (removed):** the keypad `isAcc`/`isDeny` mirror (`coop/keypad_sync` broadcast+apply + the
   mutual-exclusivity/poll-repair/`ReadActive` purple machinery + `passwordlock::WriteAccepted/
   WriteDenied/ReadActive`). isAcc/isDeny are hover flags; writing both onto a mirror was the "PURPLE."
   → **Removed entirely (RULE 2).** KeypadSyncPayload is now buffer-only (protocol v35).
3. **CORRECT model (kept):** the keypad `inPassword` digit mirror stays — replaying `inputNumber` on
   the receiver drives the keypad's OWN native validator, so on the HOST a client's correct code is
   accepted natively (MTA input-replication). The door then opens via its own logic; the host gates the
   open on the door's `Active` (`CanOpen`). coop = SP by construction, for any door, regardless of
   whether its lock is `Active`-based or `superClosed`-based (CanOpen checks both).
4. **"Green" = powered**, not accepted — the LED is the keypad's power-driven `ParticleSystem3`, already
   equal across peers from the same world; we do NOT drive it.

**DONE — Phase A (door gate + purple removal) AND Phase B (keypad accept).**

**Phase B keypad accept (verified via `gen-cfg`, research/bp_reflection/passwordLock_cfg/passwordLock.txt):**
the auto-validator is `inputNumber` -> grow `inPassword` -> when `Len(inPassword)>=5` (block 2398->2479)
-> `open(password==inPassword)`; a correct `open(true)` chain reaches `door.doorOpen(false)` (block 2360
calls `door->doorOpen(False)`), which needs the door's `Active` (power) on + the door near the player to
animate. Live keypad offsets (CXXHeaderDump/passwordLock.hpp): `password`@0x0350, `door`(Adoor_C*)@0x0338,
`isReset`@0x0360, `inPassword`@0x0380, `Pair`@0x0320, `Active`(keypad's own)@0x0330. (All confirmed by
live reflection in the smoke: `passwordlock: resolved ... inPassword@0x0380 password@0x0350 door@0x0338
isReset@0x0360`.)

## CANCEL button + the `active` LED state (2026-06-07, two converging agents, byte-exact)

User report: "client pressing the keypad CANCEL button -> keypad goes RED, not mirrored to host."
Two independent BP-disassembly agents traced it; CFG offsets cited (passwordLock_cfg/passwordLock.txt):

- **CANCEL = `open(false)`.** `lookAt` (block 804) sets `isDeny = crosshair-over-key_deny` AND `num=-1`
  (off 1491). A click calls `inputNumber(self.num=-1)` -> ubergraph 5068 -> the `num<0` "button" branch
  (3672): `isAcc?` -> `isDeny?` (3691) -> isDeny==true -> **`open(false)`** (block 3705). (accept = isAcc
  -> 2479 -> `open(password==inPassword)`.) `actionOptionIndex` IGNORES its `Action` param -- it just
  calls `inputNumber(self.num)`; the button identity is the `isAcc`/`isDeny`/`num` that `lookAt` set.
- **`open(arg)` WRITES `self.active = arg`** (CFG 848: `self.active = EX_SwitchValue(self.false){case0:
  arg}`; `false`@0x034A is 0 -> case 0 -> arg). So `open(false)` sets **`active=false`**, `open(true)`
  sets `active=true`. Then `inPassword=""`, `setActive(arg)` (-> `upd` + propagates `door.active=active`
  + `pair.active`), and `SpawnSoundAttached(active ? button_keypad_success : button_keypad_deny)`.
- **RED = `ParticleSystem3` template `eff_glow_red`, selected by `upd` ONLY from `(isReset, active)`:**
  `eff = isReset ? blue : (active ? green : red)`. So **RED <=> `!isReset && !active`** -- PERSISTENT
  (no timer; stays until `active` is set true again by a correct `open(true)` / `open2` / save load).
  The LED is NEVER purple (only green/red/blue) -- the historical "purple" was a separate LIGHT actor
  driven by `powerChanged` (a different bug), NOT this LED.
- **`enterFalse`@0x0349 is NOT the red** -- it is an input-LOCKOUT flag (written by `open2`/`falseEnter
  Event`, read only at the 2945 `actionOptionIndex` gate to suppress presses during a beep cascade).
  The cancel path does not touch it. (The earlier 2026-06-07 hypothesis that enterFalse=red was REFUTED.)
- **Wrong-CODE red ALREADY mirrors:** the client's 5 wrong digits replay via `inputNumber` onto the host,
  whose native validator fires `open(false)` -> host `active=false`. **Only the EXPLICIT cancel button is
  unmirrored** (it types no digit, so the inPassword buffer-mirror alone misses it).

**FIX (IMPLEMENTED 2026-06-07, protocol v38):** add the keypad's own `active`@0x0330 (bool) to the
keypad state mirror (`PL::State.active`, `ReadState`/`WriteActive`, `KeypadSyncPayload.active`). The
receiver `ApplyState`, AFTER the buffer reconcile (re-reads `active` so a wrong-code red the digit replay
already produced is NOT double-applied), DIRECT-writes `active` (never the `setActive` verb -> never fires
`powerChanged`/the LIGHT "purple") + propagates the same value to the gated door via `ue_wrap::door::
SetActive` (keeps `keypad.active==door.active` like SP) + `CallUpd` to repaint. Closes the cancel-button
gap on every peer; the connect-snapshot carries `active` so late joiners converge.

## (Phase B accept -- prior) IMPLEMENTED host-authoritatively (MTA "be the engine", not the fragile native latent chain): the HOST
poll (`coop::keypad_sync::HostAcceptPoll`) evaluates the SAME condition against its OWN `password` (never
replicated) and on the accept edge drives the keypad's gated door itself -- `door::SetActive(true)` (the
unlock CanOpen gates on) + `door::ForceOpen` (snaps isOpened reliably at any distance; the native
`doorOpen` freezes far away) + `door::SuppressHostHeldDoor` (the proven anti-oscillation recipe -- the
host has no player at the door, so its checkSensor would auto-close it). The door channel broadcasts the
open to every peer. Latched per keypad (fires once per code-entry). The CLIENT only sends input (the
inPassword digit mirror, replayed via inputNumber). DEFERRED (Phase B.1, documented in code): registering
the keypad door in the door channel's hold register so it is E-closable / auto-closes after walk-through
(today it stays open + suppressed, like SP's persistent unlock); the `pair`-keypad + `isReset` set-new-
code propagation (isReset entries are correctly NOT auto-opened).
