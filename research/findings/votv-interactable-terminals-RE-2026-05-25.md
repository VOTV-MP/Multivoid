# VOTV interactable story-object terminals — RE (2026-05-25)

Scope: PREPARE-GROUND-only. No code shipped this pass. Goal: full mapping
of the in-base story-object terminals (computer, server, radar, signal-
catching) so the coop sync design has zero blind spots.

User directive (verbatim, 2026-05-25 LATE):
> "story objects (computers, servers, radar, signal catching), let's make
> servers, computers interactable for host and remote at the same time and
> sync their interactions, for example both host and remote go onto signal
> catching interactive computer where you control a radar thingy and point
> it towards spawning signal dots and they can race with each other like
> one points the crosshair towards up, then remote decides to point in
> another direction and both see the the product of double interaction"

Follow-up: "drone is future work to sync it too" → drone (AdroneConsole_C)
queued for AFTER terminals land.

---

## TL;DR

1. The "signal-catching computer" the user describes is **`AanalogDScreenTest_C`**
   — a single in-world actor that hosts FOUR sub-terminals (play / download
   / coords / comp) in one BP, plus links out to `Apanel_SATconsole_C` (the
   keyboard console UMG), `Apanel_radar_C` (the radar control panel), and
   `AcoordRadarDish_C` (the OUTDOOR satellite dish puzzle).
2. The crosshair the user describes — "point it towards spawning signal dots"
   — is **`AspaceRenderer_C.coords:FVector2D`** @0x0264. That's THE state.
   The space renderer integrates `move_right/left/up/down:bool` into `coords`
   per tick via its own `ReceiveTick`. The radar UI just renders it.
3. VOTV exposes a **first-class state snapshot/restore API** on the central
   terminal: `gatherData(Fstruct_signalTableState&)` / `setData(Fstruct_signalTableState)`.
   The struct is 0x3B8 bytes (952B) and contains the full signal-table
   state (download progress, decode level, polarity/frequency filters,
   coord ping data, signal play data, physical modules). **This is the wire
   payload for free.**
4. Every interactable inherits `Aactor_save_C`. Use-key entry is uniform:
   `player_use(AmainPlayer_C* Player, FHitResult Hit)`. Camera lock is uniform:
   `mainPlayer.setActiveInterface(...)` + `Enter Interface(activeInterface,
   LookAtLocation, camLocation, callFrom)`.
5. UE's "widget can only have focus from one PlayerController" is the
   **critical concurrency constraint** — but it's NOT a hard blocker because
   the actual gameplay state lives on the BACKING ACTORS, not the widget.
   The widget is just a view. We can drive both peers' inputs through their
   own local widget instances and merge into shared backing state via a
   custom path.
6. Two input modes coexist:
   - **Command-line** (Apanel_SATconsole_C + ui_console_C): EditableTextBox
     + `enterCommand("ping")` / `setSErverTarget` / `calibratteDish` / etc.
   - **Direct** (ui_radar_C + spaceRenderer + AanalogDScreenTest_C):
     `move_up/right/zoom:int32` (ui_radar) and `move_right/left/up/down:bool`
     (spaceRenderer) accumulators integrated into `Offset:FVector2D` /
     `coords:FVector2D` per tick.
7. **The merge model matches the user's "race together" intuition exactly**:
   both peers' WASD keystates apply to the SAME `spaceRenderer.coords`.
   No locking. If both push the crosshair up, it goes up faster. If they
   conflict (A pushes up, B pushes down), it cancels.

---

## 1. Class catalog (the surface)

### 1.1 Interactable actors (story-object terminals)

| Class | LOC in dump | Role | Where it lives | Sub-mode? |
|---|---|---:|---|---|
| `AanalogDScreenTest_C` | 626 | CENTRAL signal-processing terminal (play/download/coords/comp). The "signal-catching computer". | Indoor base, near the desk | 4 sub-modes |
| `Apanel_SATconsole_C` | 72 | SAT keyboard console (typing commands). Hosts `Uui_console_C* Widget`. Has `control_asDish:Adish_C*` ref. | Indoor base | typed-cmd |
| `Apanel_radar_C` | 93 | Radar control panel (module slots + lever + screen). Hosts `Uui_console_C* Widget` AND `Uui_radar_C* radarElement`. | Indoor base | typed-cmd + visual |
| `AcoordRadarDish_C` | 143 | OUTDOOR satellite dish puzzle (lever pull + fuse insertion + button puzzle to "calibrate" the dish) | Outdoor field | physical puzzle |
| `AserverBox_C` | 145 | Hackable server (with floppy disc reader, minigame puzzles when damaged) | Server room | minigame |
| `Adish_C` | 91 | Satellite dish ACTOR (the visible big dish; commanded by SAT console, NOT directly used) | Outdoor field | not directly interactable |
| `AdroneConsole_C` | 152 | Drone control console (USER: queued for AFTER terminals) | Indoor base | held-prop-style input |
| `Awallunit_console_C` | 10 | Wall-mounted display unit (likely display-only, no UFunctions exposed) | Indoor base | display-only? |

### 1.2 UMG widgets (the UI surfaces)

| Widget | LOC | Backing actor | Input mode |
|---|---:|---|---|
| `Uui_console_C` | 111 | panel_SATconsole + panel_radar | command-line (EditableTextBox + enterCommand) |
| `Uui_radar_C` | 78 | panel_radar | WASD-direct (move_up/right/zoom:int32 + Offset:FVector2D) |
| `Uui_signal_C` | 22 | spaceRenderer (rendered as blip on radar) | display-only |
| `Uui_serverMinigame_C` | 282 | serverBox (when damaged) | 8 minigame variants (click-driven + mouse-move) |
| `Uui_handradar_C` | 38 | Aprop_handradar_C (held prop) | continuous Tick scan (no player input) |

### 1.3 The space renderer (the brain)

`AspaceRenderer_C` — a single-instance actor in the world. Owns:
- `signals: TArray<Fstruct_signal_spawn>` @0x0278 — global signal pool
- `signals_a: TArray<Uui_signal_C*>` @0x0298 — visual blips
- `coords: FVector2D` @0x0264 — **THE CROSSHAIR POSITION**
- `coords_rot: FVector2D` @0x026C — rotation
- `move_right/left/up/down: bool` @0x0260..0x0263 — WASD held-key state
- `Movement: FVector2D` @0x02DC, `movementVelocity: float` @0x02D0,
  `movementDirection: FVector2D` @0x02D4, `movementSpeed: float` @0x02EC,
  `maxMovementVelocity: float` @0x02E4 — integrator state
- `coordinateDrift: float` @0x02E8 — drift the player must compensate for
- `triangleLevel: float` @0x02F0 — triangulation precision (?)
- `Atlas: Uui_consolesAtlas_C*` @0x0248 — the master UMG widget atlas
- UFunctions: `gatherSignal(skipCheck, return, Index, Data, dir, element,
  radiusCheck, caughtAtLeastOne)` — THE detection fn; `addSignal(InVec)`,
  `addSignal_cursor()`, `deleteSignal(ItemToFind)`, `spawnSignal()`,
  `signalFound()`, `convertCoords(InVec2D, FVector& return3D)`,
  `getCoords(InVec2D, FVector& return3D)`, `resetMomentum()`, `stopMoving()`,
  `ReceiveTick(DeltaSeconds)`

### 1.4 The state-snapshot codec (the gift)

`Fstruct_signalTableState` (0x3B8 = 952 bytes) — defined explicitly in
the dump, contains ALL signal-table state:
- DL_signalDownloadData (Fstruct_spaceObject, 0x70B)
- DL_SignalDownloadDLData (Fstruct_signalDataDynamic, 0x70B)
- DL_poFilterOffset/Speed, DL_FrFilterOffset/Speed (4 floats)
- DL_activeFrFilter/PoFilter (2 bools)
- DL_downloading, DL_PolarityDir
- comp_maxLevel, comp_progress, comp_data_0 (Fstruct_signalDataDynamic)
- signalPlayData (Fstruct_signal_data, 0x1C2 = 450 B — full signal record)
- coord_signalData (Fstruct_signal_spawn, 0x2C)
- DL_resPercent
- physMods (TArray<enum_physicalModules>)
- backPlatesStates, backPlatesUnscrew (2 TArray<bool>)
- remapVal (FVector2D)
- radarMods (TArray<enum_physicalModules>)

And `AanalogDScreenTest_C` provides:
- `gatherData(Fstruct_signalTableState& Data)` — SERIALIZE
- `setData(Fstruct_signalTableState Data)` — DESERIALIZE

**Wire payload = call `gatherData` on host, send ~1 KB to client, call
`setData` on client.** No bespoke codec needed.

---

## 2. The interact dispatch chain

### 2.1 Use-key entry (uniform across all `Aactor_save_C` children)

1. Player presses E → `mainPlayer.input_E(true)` fires
   (mainPlayer.hpp:187 — `FmainPlayer_CInput_E input_E` delegate)
2. mainPlayer's BP graph traces along the look-at arm:
   - `arm(customLength, Start, End, Rotation)` (mainPlayer.hpp:496)
   - LineTrace hits an actor → `lookAtActor:AActor*` (mainPlayer.hpp:179)
   - hit component → `lookAtComponent` (separate field)
3. mainPlayer calls `useAction(bool sec, bool& succ)` (mainPlayer.hpp:492)
4. If `lookAtActor != nullptr` and looking-at conditions pass, calls the
   actor's `player_use(this, HitResult)` (the BP-graph dispatch).

### 2.2 The actor's response (entering use mode)

The interactable's `player_use` body typically:
1. Validates (player is close enough, terminal not broken, etc.)
2. Calls `mainPlayer.setActiveInterface(Widget, inString, Zoom, sentBy,
   ignoreZoom, isActiveINterface3D, showInterface, return)` (mainPlayer.hpp:489)
3. Calls `mainPlayer.Enter Interface(activeInterface, LookAtLocation,
   camLocation, callFrom)` (mainPlayer.hpp:495)

The Widget is the SHARED UMG widget owned by the panel actor (e.g.
`Apanel_SATconsole_C.Widget:Uui_console_C*` @0x0298). All players entering
the same panel see the SAME widget instance — it's a 3D in-world UMG
attached to the panel actor.

### 2.3 The "active" state on mainPlayer

While `activeInterface:UWidget*` @0x07E0 is non-null, the player is in
terminal mode:
- Camera is locked to `camLocation:FVector` @0x082C looking at
  `LookAtLocation:FVector` @0x0820
- Movement is blocked (the BP graph likely sets `deactivateMouseInput` and
  similar gates)
- Mouse drives the `WidgetInteraction:UWidgetInteractionComponent*` @0x0628
  which auto-clicks WidgetComponents in the world (the 3D UMG buttons)
- Keyboard input routes through standard UE slate focus to the focused
  widget (the EditableTextBox if it's a console)

### 2.4 The exit

`InpActEvt_Escape_K2Node_InputKeyEvent_0(FKey)` fires Escape. The BP graph
likely clears `activeInterface` and calls `driveDetached()` on the actor
(which is a UNIVERSAL `Aactor_save_C` callback — every interactable has
a no-op default, panel_SATconsole overrides it to clear `controlObject`).

### 2.5 The TWO control modes

**Mode A: command-line typed-input (panel_SATconsole + ui_console)**
- `Uui_console_C.EditableTextBox:UEditableTextBox*` @0x0268 — the entry
- Player types into the editable text box (slate keyboard focus)
- `BndEvt__EditableTextBox_..._Committed` fires on Enter
- `enterCommand(FString C)` parses → calls backing actor functions
  (`dish.startMovingTo`, `setSErverTarget`, `calibratteDish`, …)

**Mode B: WASD direct movement (ui_radar + spaceRenderer + AanalogDScreenTest)**
- `Uui_radar_C.move_up/right/zoom:int32` @0x035C..0x0364 — held-key state
  (probably +1 on KeyDown of W, -1 on S, 0 on KeyUp)
- `Uui_radar_C.Offset:FVector2D` @0x0354 — current crosshair position
- `Uui_radar_C.Tick()` (line 71) — per-frame integration:
  `Offset += FVector2D(move_right, move_up) * speed * DeltaTime`
- BUT — the spaceRenderer ALSO has its own `move_right/left/up/down:bool`
  state (separate from ui_radar's int32 quad). One of these is the
  authoritative "where the crosshair is in space" — and based on field
  names (`spaceRenderer.coords` vs `ui_radar.Offset`), spaceRenderer's
  `coords:FVector2D` is the WORLD-SPACE radar position, while
  `ui_radar.Offset:FVector2D` is the SCREEN-SPACE pixel offset on the
  radar widget.

**The signal-catching minigame answer** (the user's "point it towards
spawning signal dots"):
- `spaceRenderer.coords` is the crosshair the player is moving
- `spaceRenderer.signals[]` is the pool of signal sources at their own coords
- `spaceRenderer.gatherSignal()` is the per-tick detection check — compares
  `coords` against each signal's `coordinates` (3D); within tolerance =
  caught
- Detection feedback returns to `mainPlayer.returnSignal(float signalStrength)`
  (mainPlayer.hpp:634) which drives the audio beep / visual indicator

---

## 3. The save/persistence pattern (host-authoritative)

`Aactor_save_C` base class:
- `Key:FName` @0x0230 — unique save identifier
- `skipReset:bool` @0x0238, `skipSave:bool` @0x0239
- `GameMode:AmainGamemode_C*` @0x0240 — cached
- `getData(Fstruct_save&)` — serialize current state
- `loadData(Fstruct_save, return)` — restore state

`Fstruct_save` (0xF8 = 248 bytes) holds polymorphic arrays:
- `class_:TSubclassOf<AActor>` — what to spawn on load
- `transform_:FTransform`
- `key_:FName`
- `bools_/floats_/ints_/strings_/vectors_/rotators_/transforms_/bytes_/names_/classes_`
  — all `TArray<Fstruct_mX>` typed wrappers
- `signals_:TArray<Fstruct_signalDataDynamic>` — signals specifically (this
  is in the base save struct because signals are first-class persistable
  state)

**Implication for coop**: terminal state IS save-persisted. Phase 5S0
snapshot bootstrap (already shipped) sends `Fstruct_save` records on
connect; client `loadData`s them. For LIVE updates, host calls
`getData()` periodically (or on state change), sends delta, client calls
`loadData()`. **The `gatherData/setData` pair on analogDScreenTest is a
finer-grained alternative to `getData/loadData` for that specific actor.**

---

## 4. The "concurrent control" design

### 4.1 The constraint

UE4 widgets natively support ONE-PlayerController focus at a time. A
shared UMG widget instance can't simultaneously receive keyboard input
from both peers' PlayerControllers.

### 4.2 The escape hatch

**Each peer maintains its own LOCAL view of the terminal state but the
authoritative state lives on the HOST's backing actor**. The widget is
just a view — what matters is:
- `spaceRenderer.coords:FVector2D` (host) — authoritative crosshair
- `AanalogDScreenTest_C` field state (host) — authoritative terminal mode

Per peer, we don't share the widget — we share the BACKING STATE. Client
runs its own local `Uui_radar_C` / `Uui_console_C` instance (or the same
in-world widget rendered locally), but its keyboard input does NOT mutate
the local widget's state — it FORWARDS to host as a `TerminalInput`
packet. Host applies to its own `spaceRenderer.coords`, broadcasts new
state to all peers, peers render.

### 4.3 The "race together" semantics

User wants: "one points the crosshair towards up, then remote decides to
point in another direction and both see the the product of double
interaction".

Implementation:
- Each peer sends per-tick `TerminalInput { terminalKey, moveUp:i8,
  moveDown:i8, moveLeft:i8, moveRight:i8, modifiers:u8 }` (10-30Hz, gated
  on keystate change OR continuous if held)
- Host integrates BOTH local + remote inputs:
  ```
  combined_up = local_move_up | remote_move_up   (logical OR for held state)
  spaceRenderer.move_up = combined_up
  // spaceRenderer's own Tick() then integrates into coords
  ```
- Host broadcasts `TerminalState { terminalKey, coords:f32x2, Movement:f32x2,
  movementVelocity:f32, coords_rot:f32x2, isMoving:bool }` at 10-20Hz
- Both peers `setActorField(spaceRenderer, ...)` to apply state.

If both peers press W simultaneously, `combined_up = 1` and crosshair
moves up. If they press conflicting WASD, the OR collapses to "moving in
both directions" — spaceRenderer's integrator handles that (the
`Movement` vector becomes the sum direction, then `movementVelocity`
+ `coords` follow). Either way: BOTH peers see the same end-result,
which IS the merged input.

For COMMAND-LINE mode (typed commands), the model differs:
- Each peer's local widget accepts typed text into a peer-local buffer
- Press Enter → peer sends `TerminalCommand { terminalKey, command:str
  ≤256B }` reliable to host
- Host validates + executes (via existing `enterCommand` BP)
- Host broadcasts `TerminalLogAppend { terminalKey, line:str }` reliable
  to all
- All peers append to their local log display

So typed commands are SEQUENTIAL and OWNED by whoever pressed Enter —
not merged. (The user's "race together" was specifically for the
direct/WASD mode; the typed-command mode naturally serializes.)

---

## 5. Wire protocol design (DRAFT, validated against existing protocol.h)

### 5.1 New packets (additions to existing UDP protocol)

```cpp
// Reliable channel (matches existing reliable-stream pattern from chat)

struct TerminalEnter {
    u32 terminalKey;       // hash of actor.Key:FName
    u8  peerId;            // sender peer id (host fills for itself)
};

struct TerminalExit {
    u32 terminalKey;
    u8  peerId;
};

struct TerminalCommand {
    u32 terminalKey;
    u8  peerId;
    u8  cmdLen;            // <= 200
    char command[200];     // typed command (e.g. "ping signalA")
};

struct TerminalLogAppend {
    u32 terminalKey;
    u8  lineLen;           // <= 200
    char line[200];        // server response line
};

// Unreliable channel (matches existing PropPose / EntityPose pattern)

struct TerminalInput {
    u32 terminalKey;
    u8  peerId;
    u8  axisBits;          // bit 0=W bit1=S bit2=A bit3=D bit4=Zoom+ bit5=Zoom-
                           //   bit6=Modifier1 bit7=Modifier2
    i16 mouseDeltaX;       // optional per-axis delta (for mouse-driven panels)
    i16 mouseDeltaY;
    u8  buttonBits;        // LMB/RMB/etc held state
};

struct TerminalState {
    u32 terminalKey;
    u32 codecVersion;      // bump if struct evolves
    u16 stateLen;          // <= 1200 (fits one MTU comfortably)
    u8  stateBlob[1200];   // per-class codec output; see 5.2
};
```

### 5.2 The state codec — per terminal class

The strategy: ONE generic packet, MANY codecs (one per terminal class).
The codec maps to existing BP functions where possible.

| Terminal class | Codec source | Approx size |
|---|---|---:|
| `AanalogDScreenTest_C` (signal-catcher) | `gatherData(Fstruct_signalTableState&)` direct | ~952 B |
| `AspaceRenderer_C` (crosshair-only, often nested in analogDScreenTest) | hand-written: coords + move bits + Movement + isMoving | ~32 B |
| `Apanel_SATconsole_C` | hand-written: `controlObject:AActor*` (as key hash) + isLlamable + Root + button-states | ~32 B |
| `Apanel_radar_C` | hand-written: `upgrades:TArray<enum_physicalModules>` + module slot occupancy | ~64 B |
| `AcoordRadarDish_C` | hand-written: opened, IsBroken, fuses (TArray<u8>), puzzleLights (TArray<bool>), leverTL.a (timeline alpha), brokenLightState | ~96 B |
| `AserverBox_C` (and active minigame) | hand-written: damaged, IsBroken, Active, upgrade_1/2/3, minigame:int32, floppyType + minigame-specific blob | ~64 B + per-minigame |
| `Uui_serverMinigame_C` (minigame variants) | hand-written per-variant blobs | 32-256 B per variant |

The `AanalogDScreenTest_C` is the BIG one. Wire it first; everything else
is much smaller.

### 5.3 Tick rates

- TerminalEnter/Exit: reliable, one-shot. Free.
- TerminalCommand: reliable, ~1 per Enter-press = ~once per second peak.
  Free.
- TerminalLogAppend: reliable, ~1 per command execution. Free.
- TerminalInput: unreliable, 20Hz gated on change. ~40-200 B/s per peer
  per active terminal. With both peers active at one terminal: ~400 B/s.
- TerminalState: unreliable, host-out, 10-15Hz. analogDScreenTest = 952B,
  so 10 Hz = 9.5 KB/s out per active terminal. With 2 active terminals,
  ~19 KB/s — fits comfortably in the ~300-400 KB/s host-out budget.

### 5.4 Coalesce / delta strategy (optional, defer)

For analogDScreenTest specifically, full-state broadcast at 10Hz is
~9.5KB/s. Acceptable. If we later want to compress: delta-encode against
the previous broadcast (XOR + RLE) — ~70% reduction typical. **Defer
this** — the user's whole-map-sync rule + LAN-target means we have
headroom.

---

## 6. Implementation increments (Phase 5T — Terminals)

Phase 5T = Terminals. Increments:

**Inc1 — Terminal-key resolution (foundation)**
- Every `Aactor_save_C` already has `Key:FName`. Hash it to u32 for wire.
- Reuse the existing `prop_wrap` pattern from physics props
  ([[project-physics-object-pickup]]): host scans GUObjectArray once,
  builds Key→Actor map. Client does the same on connect.
- Test: cross-peer Key lookup returns same actor on both sides.

**Inc2 — TerminalEnter/Exit reliable**
- Hook `player_use(Player, Hit)` on every terminal class (PRE-observer
  via UE4SS UFunction hook — same pattern as our Aprop_C.Init hook).
- On PRE-observer: send `TerminalEnter { terminalKey, peerId }` to host.
- Host tracks `g_terminalUsers: map<u32 terminalKey, set<peerId>>`.
- On Escape (or driveDetached hook), send `TerminalExit`.
- Validate: both peers can enter the same terminal; host sees both in
  the set.
- This Inc DOES NOT change gameplay yet — it's wire-layer only, gated on
  an env flag (`VOTVCOOP_TERMINAL_SYNC=1`).

**Inc3 — TerminalInput unreliable (peer → host)**
- Hook `ui_radar.OnKeyDown` / `OnKeyUp` PRE-observers (or hook the
  spaceRenderer's input bridge directly — TBD via runtime test).
- Client forwards each key event as a `TerminalInput` to host.
- Host merges into its own spaceRenderer state.
- Test: client's WASD on the radar panel moves host's crosshair.

**Inc4 — TerminalState broadcast (host → all)**
- Host calls `analogDScreenTest.gatherData(state)` on tick (10Hz) when
  any peer is in the terminal user set.
- Broadcasts `TerminalState { terminalKey, stateBlob }`.
- Receivers call `setData(state)` on their local analogDScreenTest.
- Test: client sees the same signal-table state as host (signal play
  progress, decode level, etc. all mirror).

**Inc5 — Per-class codecs for non-analogDScreenTest terminals**
- panel_SATconsole, panel_radar (small structs)
- coordRadarDish (the puzzle state)
- serverBox + active minigame (the hack minigame state)
- Each codec: hand-rolled gather/apply via reflection
- Test: each terminal type works concurrently.

**Inc6 — Command-line replication**
- TerminalCommand peer→host reliable
- Host runs `enterCommand` via reflection
- TerminalLogAppend broadcast back
- Test: client types "ping" → host pings → both see the log line.

**Inc7 — Audit + scope reduction (per-minigame which-are-merge-friendly)**
- Per the agent's classification:
  - MERGE-FRIENDLY: Bitfit (each peer toggles independent bits),
    Pipe (each rotates different pipes), Slider (each grabs different
    sliders), Wire (each drags different wires) → simple concurrent
  - EXCLUSIVE: Hack (password typing), Math (single answer),
    Maze (single cursor), Simon (sequential playback) → either lock
    to first-peer or merge via "any-peer-wins" rule
- May want to leave EXCLUSIVE minigames as single-peer for v1 (whoever
  starts the minigame, owns it; other peer is spectator).

---

## 7. Open questions / decisions to escalate

1. **Q-7.1 Widget rendering during use**: when both peers are at the same
   terminal, does each peer locally render the panel's in-world UMG
   widget independently (just a camera view from their viewpoint, no
   sharing), or does one peer's widget render override? **Expected**:
   each peer renders independently because the WidgetComponent is in-
   world and visible to all cameras. CONFIRM via hands-on once Inc4
   lands — should "just work".

2. **Q-7.2 Sound effects**: terminals make sounds (beeps, sweep audio).
   Sounds attached to the actor (`UAudioComponent` children of the
   terminal actor) play locally on every peer for free if the actor's
   state is mirrored. Should NOT need separate sound replication.

3. **Q-7.3 EXCLUSIVE-minigame policy**: lock to first peer, lock to host,
   or "first to start the minigame is owner; other can spectate"?
   Recommend: first-to-start is owner; other sees the same UMG but
   their input is dropped client-side (server doesn't even need to
   reject — the client's input layer self-gates). DEFER decision to
   Inc7.

4. **Q-7.4 Aanalog drive slots (drive_play, drive_comp)**: the central
   terminal has two physical floppy-drive slots that accept dropped
   drive props (`AdriveSlot_C`). When a peer drops a drive into a slot,
   that's an Aprop interaction that ALREADY syncs via prop_lifecycle.
   But the slot's state (which drive is in, processing progress) lives
   on analogDScreenTest. Confirm Phase 5S0 + 5T composition handles
   this — should be free since both bus through actor state.

5. **Q-7.5 Save-load on client**: when client (re)joins mid-session, the
   Phase 5S0 snapshot bootstrap should include analogDScreenTest's
   state. Currently snapshot uses `Fstruct_save` polymorphic struct;
   we'd need to add a one-off `setData(gatherData())` round-trip for
   analogDScreenTest specifically OR rely on the existing
   `getData/loadData` (which goes through the polymorphic struct). The
   latter is probably already correct — VOTV's own save system uses
   it. CONFIRM by reading `analogDScreenTest.getData/loadData` BP graph
   (IDA escalation, or UE4SS Lua probe).

6. **Q-7.6 Drone (deferred)**: AdroneConsole_C uses the `playerHand*`
   input family (different from terminal use-key flow). Same general
   model applies — host-authoritative drone state, both peers stream
   input — but hooks are different. Out of immediate scope per user.

---

## 8. Escalation flags (cannot resolve from headers alone)

Per CLAUDE.md escalation ladder (reflection → IDA → UE4SS), the following
need deeper inspection BEFORE Inc1 ships:

| Flag | Description | Tool to escalate to |
|---|---|---|
| F-8.1 | spaceRenderer.ReceiveTick integrator formula (how exactly does it map `move_*` bools + DeltaTime into `coords`?). Need to know if it's linear or has acceleration curve, drift compensation logic. | UE4SS Lua probe (dump the BP node graph) |
| F-8.2 | mainPlayer.input_E full BP graph (use-key dispatch). Need to confirm `useAction` is the gate. | UE4SS Lua probe OR IDA on the native shim |
| F-8.3 | analogDScreenTest.player_use BP graph (specifically: which sub-mode does pressing E on it activate? Just "near-the-desk" or context-specific?). | UE4SS Lua probe |
| F-8.4 | Exact UE event for Escape-clear-activeInterface — find the BP node that clears `mainPlayer.activeInterface`. | UE4SS Lua probe |
| F-8.5 | gatherSignal's cone/radius predicate — is it angle-distance or 3D-distance? | IDA decompile if reflection-driven, OR UE4SS Lua dump of the function body |
| F-8.6 | Confirm `Uui_console_C` is one-per-panel-actor (instance is created in panel's BeginPlay, not per-player). Implication: widget IS shared in-world. | Read panel_SATconsole BP graph (UE4SS Lua probe) or runtime test (autonomous probe checking Widget pointer between two peers). |

These can be answered in a single 30-min UE4SS Lua-probe session
WITHOUT shipping code. Recommend: run that probe session BEFORE Inc1.

---

## 9. Cross-refs

- [[project-coop-interactable-terminals]] — the scope memory written
  the same day; this doc is its detailed RE backing.
- [[project-coop-save-host-authoritative]] — already-decided save model;
  applies cleanly.
- [[project-coop-whole-map-sync]] — already-decided no-AOI rule;
  terminals are whole-map state.
- [[project-coop-scale-100-entities]] — already-decided bandwidth budget;
  ~19 KB/s peak for terminals fits.
- [[feedback-re-related-functions]] — RULE: RE all related fns before
  implementing. THIS DOC IS THAT.
- [[project-physics-object-pickup]] — the Key→Actor resolution pattern
  to reuse for terminal-key lookup.
- [[project-coop-aprop-lifecycle-RE-inc3-scope]] — analogous RE doc for
  Aprop_C, same structure.

## 10. Update log

- 2026-05-25 LATE: doc created from full reflection-dump RE pass +
  2 parallel Explore-agent investigations. No code shipped. Awaiting
  user direction on which increment to start with (Inc1 / Q-7.x
  decisions / F-8.x escalations first).
- 2026-05-25 LATE +1h: audit pass via feature-dev:code-reviewer. 11
  issues caught. Section 11 below records corrections.

---

## 11. Audit corrections (post-audit pass, 2026-05-25 LATE)

The audit caught real issues. Documented here so the doc is the
authoritative reference for Inc1+ design (don't trust earlier sections
in isolation where this section contradicts them).

### 11.1 CRITICAL — `kMaxPacketBytes = 256` is the actual MTU

Section 5.1 proposed `TerminalState { stateBlob[1200] }`. **Wrong.**
The existing protocol caps every packet at 256 bytes
(`protocol.h:kMaxPacketBytes`). The recv buffer in `session.cpp` is
`char buf[kMaxPacketBytes]`. A 1200B datagram is silently truncated.

**Resolution (decided 2026-05-25 audit):**

- `TerminalState` is REDESIGNED to fit 256 bytes. It carries the HOT
  PATH only — the spaceRenderer crosshair state (~32B) and a small
  set of single-fields-changed-this-tick deltas (~50B max). NOT the
  full Fstruct_signalTableState.
- The full snapshot (Fstruct_signalTableState, 952B) ships only ONCE
  at TerminalEnter time, NOT continuously. It uses the
  RELIABLE STOP-AND-WAIT channel (same path as chat) with multi-fragment
  framing IF needed. Fragmentation is justified for the snapshot
  one-shot but NOT for the per-tick state.
- For 10Hz delta broadcast, send only what changed: pack into a `u16
  fieldMask` + per-field payload, ≤256B always.

Concrete redesign of `TerminalState` and new packets:

```cpp
// Hot crosshair path (10-20Hz unreliable)
struct TerminalCrosshairState {
    u32 terminalKey;
    f32 coordsX, coordsY;          // FVector2D 8B
    f32 coordsRotX, coordsRotY;    // 8B
    u8  moveBits;                  // right/left/up/down OR'd from all peers
    u8  isMoving;
};                                  // ~24B fits comfortably

// One-shot full snapshot (reliable, fragmented if >256B)
struct TerminalSnapshot {
    u32 terminalKey;
    u16 chunkIndex;                // 0..N-1
    u16 chunkTotal;                // N
    u16 chunkSize;                 // <= 200
    u8  chunkData[200];            // 952 / 200 = 5 fragments for analogDScreenTest
};

// Per-field deltas (reliable, batched 10Hz)
struct TerminalDelta {
    u32 terminalKey;
    u16 fieldMask;                 // bit per field; ≤16 fields per packet
    u8  payload[240];              // packed field values
};
```

Doc section 5.1 is SUPERSEDED by this 11.1 redesign.

### 11.2 CRITICAL — `Fstruct_signal_data` size typo

Section 1.4 said `signalPlayData (Fstruct_signal_data, 0x1C8B)`. Actual
size is `0x1C2 = 450 B` per `struct_signal_data.hpp:43`. **Fixed inline
in section 1.4.**

### 11.3 HIGH — Key-held leak on peer disconnect

Section 4.3's OR-merge needs an explicit disconnect-cleanup path.

**Resolution:** the host maintains a per-peer key-bit cache:
```
struct PeerKeyState { u8 moveBits; u8 buttonBits; };
g_peerKeyState: map<peerId, PeerKeyState>
```
On TerminalInput receipt, update that peer's entry. On peer disconnect
(or TerminalExit), the host explicitly zeros that peer's entry. The
combined `host.moveBits = OR_all_peers(g_peerKeyState)` is recomputed
on every TerminalInput / disconnect. NO held-state leak.

### 11.4 HIGH — `setData()` may trigger client audio comps (NEW F-8.7)

Confirmed: analogDScreenTest has 15+ UAudioComponent members. Calling
`setData(state)` on the client likely re-runs BP event dispatchers that
play sound on state changes. If host already plays the sound locally and
the client also plays via `setData()`, the user hears double audio.

**Resolution:** Before Inc4 ships, F-8.7 (UE4SS Lua probe) verifies
which audio components are driven by `setData()` and which are driven
by local Tick logic. Two possible mitigations:
- (a) Use REFLECTION WRITES to individual fields instead of `setData()`
  — bypasses the BP dispatchers entirely. Costs ~30 reflection writes
  per snapshot, but no sound side-effects.
- (b) Mute the audio components on client when in "mirror mode"
  (set UAudioComponent.bIsActive=false via reflection). Crude but
  works.
- (c) Accept double audio as a known limitation for v1.

Decision deferred to Inc4 design time.

### 11.5 HIGH — `signalObject_planetEater_C` is a missing entity (NEW)

Section 1 catalog missed `AsignalObject_planetEater_C` (subclass of
`AsignalObjectActorBase_C` with skeletal mesh + animation callbacks +
`fullyProcessed:bool` lifecycle).

**Resolution:** treat signal-object actors as entities under existing
Phase 5N1 NPC sync, NOT terminals. The terminal RE doc covers the
TERMINAL state but the signal-object world actors are a separate sync
domain. Add to Phase 5N1 entity list (NPC sync increment is the right
home, not Phase 5T). Update the scope memory to clarify.

### 11.6 HIGH — `intComs_anyKey` may be the correct Inc3 hook (NEW F-8.9)

analogDScreenTest defines `intComs_anyKey(FKey Key, bool Pressed)`. The
"intComs" prefix is the gamemode event-bus pattern. If WASD presses
route through `intComs_anyKey` (and not just the widget's `OnKeyDown`),
hooking `intComs_anyKey` is the cleaner Inc3 path — fewer hooks, more
authoritative.

**Resolution:** Add F-8.9. UE4SS Lua probe: while inside the terminal,
log all `intComs_anyKey` calls on analogDScreenTest. If WASD arrives
here, that's the Inc3 hook. If only Escape/Tab/etc arrive here,
revert to widget-level hooks.

### 11.7 MEDIUM — `AanalogDScreenTest_C` does NOT inherit `Aactor_save_C`

Section 1.1 + section 3 implicitly claimed every terminal inherits
`Aactor_save_C`. **Wrong** for two of them:
- `AanalogDScreenTest_C : public AActor` (direct)
- `AdroneConsole_C : public AActor` (direct)

The other six (panel_SATconsole, panel_radar, coordRadarDish, serverBox,
dish, wallunit_console) DO inherit `Aactor_save_C` correctly.

**Resolution:** Inc1 (terminal-key resolution) needs two paths:
- Standard path: `obj.GetKey()` UFunction call for `Aactor_save_C`
  children — already maps to `Key:FName @0x0230`.
- Non-standard path: for analogDScreenTest, use the actor's path-in-world
  or BP-class hash as the key (since it's a singleton in the level).
  The doc claim "every interactable has a stable Key" doesn't generalize
  — we need a class-singleton fallback for these two.

For now, ASSUME analogDScreenTest is a singleton per world and use a
fixed key constant. Drone (deferred anyway) gets the same treatment
when we tackle it.

### 11.8 MEDIUM — `rootConsole` vs `consoles[]` (NEW F-8.8)

analogDScreenTest has BOTH:
- `rootConsole:Apanel_SATconsole_C* @0x1268` — primary
- `TArray<Apanel_SATconsole_C*> consoles @0x0B48` — mirrors/secondaries

Inc6's `enterCommand` reflection call needs to target the right one.

**Resolution:** Add F-8.8. UE4SS Lua probe: log which console
`enterCommand` is called on during normal play. Likely answer:
`rootConsole` for typed commands; `consoles[]` for mirrors that display
the log but don't drive commands. Inc6 routes the peer's command to
`rootConsole.Widget.enterCommand`.

### 11.9 MEDIUM — OR-vs-SUM merge inconsistency between docs

The findings doc section 4.3 says OR; the scope memory says SUM. **Pick
one.**

**Decision (2026-05-25 audit):** OR is correct for the WASD bool quad
because `spaceRenderer.move_*` are BOOLS, not deltas. The integrator
gates movement velocity on the bool; SUMming bools makes no sense. The
"race together" semantics the user described actually maps to OR: both
peers' inputs are CO-PROCESSED but the movement speed is dictated by the
integrator, not by the per-peer input count. The scope memory has been
amended to say OR.

If we later add a mouse-delta-driven mode (where each peer contributes
a signed FVector2D delta), THAT merges with SUM. Currently no terminal
class has mouse-delta input identified.

### 11.10 MEDIUM — EXCLUSIVE-minigame v1 policy (RULE 2 risk)

The doc deferred EXCLUSIVE-minigame ownership to Inc7. RULE 2 risk if
the v1 ships with "no concurrent minigame access" and Inc7 later adds a
parallel "concurrent access" code path.

**Decision (2026-05-25 audit):** Commit to FIRST-PEER-OWNS for v1, NO
spectator mode. When peer A starts a minigame on server X, peer B
trying to enter server X's minigame gets a TerminalEnter failure
(reliable response packet). Peer B sees a UI message "in use by Alice".
Inc7 may later add spectator mode, but it would EXTEND the v1 design
(client UI shows minigame state read-only), NOT replace it. So v1's
gate is FORWARD-COMPATIBLE with Inc7's enhancement — no RULE 2 baggage.

### 11.11 MEDIUM — TerminalInput held-key reconnect heartbeat

Section 5.3 said TerminalInput is "unreliable, 20Hz, gated on change".
A held-key state cannot survive a packet-loss-on-keydown.

**Resolution:** TerminalInput is "unreliable, 20Hz, gated on change OR
periodic (≥1Hz heartbeat while in terminal)". The 1Hz heartbeat keeps
held-state synced even after packet loss. Cost: <100 B/s/peer at idle
in-terminal. Negligible.

Also: TerminalEnter response includes current held-keystate for that
peer (initialized to 0). Inc2 wire layer must include this.

### 11.12 NOT A BUG — Bandwidth budget remains valid

The 9.5 KB/s estimate was per-peer per-terminal under the old (broken)
1200B design. New design: ~24B × 20Hz crosshair + 240B × 10Hz delta +
1KB snapshot one-shot = ~3 KB/s steady state per active terminal. Even
LOWER bandwidth than originally claimed. Budget is fine.

### 11.13 NOT A BUG — gatherData heap alloc on 10Hz tick

Section E concern about heap allocs on game thread is correct in
principle but NOT a problem in revised design (section 11.1): we don't
call `gatherData()` at 10Hz anymore — only once at TerminalEnter for
the snapshot. Per-tick delta uses individual reflection reads (cheap).

### 11.14 Updated escalation flag list

Phase F (UE4SS Lua probe session, ~30 min before Inc1 ships):
- F-8.1: spaceRenderer.ReceiveTick integrator formula
- F-8.2: mainPlayer.input_E full BP graph
- F-8.3: analogDScreenTest.player_use BP graph (sub-mode dispatch)
- F-8.4: Escape clears activeInterface (BP node)
- F-8.5: gatherSignal cone/radius predicate
- F-8.6: Widget instance scoping (per-actor confirmed)
- **F-8.7 (NEW):** Does `setData()` trigger audio comp side-effects?
- **F-8.8 (NEW):** rootConsole vs consoles[]; which has enterCommand?
- **F-8.9 (NEW):** Does WASD route through `intComs_anyKey` (preferred
  hook) or only widget OnKeyDown?

### 11.15 Updated implementation increments

Revised Inc plan based on audit:

**Inc0 — UE4SS Lua probe session** (NEW, 30 min). Resolve F-8.1..F-8.9
before any wire code. Document answers in this file. NO CODE.

**Inc1 — Terminal-key resolution + singleton-fallback**. Adds the
class-singleton-key path for analogDScreenTest + droneConsole. Standard
GetKey path for the rest. Test: both peers' Key→Actor maps agree on
all 8 candidate terminals.

**Inc2 — TerminalEnter/Exit + held-key heartbeat scaffolding**.
Reliable; ack carries initial held-keystate; tracks `g_terminalUsers`
+ `g_peerKeyState`.

**Inc3 — TerminalInput unreliable + held-key 1Hz heartbeat**. Hook
either `intComs_anyKey` or widget OnKeyDown (decided in Inc0).
Disconnect-cleanup zeros peer keybits explicitly.

**Inc4 — TerminalCrosshairState 20Hz broadcast** (the 24B hot path).
Receivers apply via reflection writes to spaceRenderer.coords/coords_rot/
move bits.

**Inc5 — TerminalSnapshot one-shot reliable + multi-fragment**. Sent
on TerminalEnter for the full Fstruct_signalTableState. Receivers
reassemble + apply via reflection (NOT `setData` — see 11.4).

**Inc6 — TerminalDelta 10Hz reliable for low-rate state changes**
(active_play, decodedLevel, comp_progress, …). Per-class delta codec.

**Inc7 — Per-terminal-class codecs** for non-analogDScreenTest.
panel_SATconsole, panel_radar, coordRadarDish, serverBox state.

**Inc8 — Command-line replication**. TerminalCommand peer→host reliable;
host calls rootConsole.Widget.enterCommand via reflection;
TerminalLogAppend broadcast.

**Inc9 — EXCLUSIVE-minigame v1 gate**. First-peer-owns; second peer
sees "in use" message. NO spectator mode in v1.

**Audit cycle** after each Inc (per [[feedback-audit-every-time]]).

### 11.16 Updated open-questions resolutions

| Q | Status | Decision |
|---|---|---|
| Q-7.1 widget rendering | OPEN | Confirm in Inc4 hands-on test |
| Q-7.2 sound effects | RESOLVED → see 11.4 | Audio is a real concern; F-8.7 first |
| Q-7.3 EXCLUSIVE-minigame policy | RESOLVED → see 11.10 | First-peer-owns v1, spectator later if asked |
| Q-7.4 drive slots | OPEN → see section 12 | drive slots are PHYSICS-PROP-DRIVEN; prop_lifecycle already syncs position; drive data needs supplementary sync |
| Q-7.5 client save-load on join | OPEN | TerminalSnapshot at TerminalEnter (Inc5) handles this — confirmed |
| Q-7.6 drone | DEFERRED | User: "drone is future work" |

---

## 12. The analog control surface (user-requested deep dig, 2026-05-25 LATE +3h)

User directive: "Dig into RE of computer actor analogue controls. We need
to sync their state, interaction too (there are actual control buttons,
knobs which drive what shows on screen and stuff)".

The terminals expose a LOT of physical (in-world, not-UMG) controls on
top of their UMG widgets. This section catalogs the full analog surface,
maps each control to the state field it mutates, identifies the input
dispatch path, and lays out the sync strategy.

### 12.1 The 5 control archetypes

Across all terminals there are 5 distinct analog-control archetypes,
each with its own input dispatch and sync strategy.

| Archetype | What it is | Dispatch path | Sync strategy |
|---|---|---|---|
| **A1 — Click button** | `UBoxComponent` (or `UStaticMeshComponent` w/ collision); player looks at it + E-presses (or LMB-clicks). | `player_use(Player, Hit)` → BP graph reads `Hit.GetComponent()` → matches against named bindings (`BndEvt__<actor>_<button>_ComponentOnClickedSignature__DelegateSignature`) | `TerminalButtonPress { terminalKey, peerId, componentHash }` reliable; host applies via reflection-driven re-fire of the named UFunction |
| **A2 — Scroll knob** | Virtual "knob" = scroll mode flag (`scroll_*:bool`) gated on look-at-component; mouse wheel drives value. | `intComs_anyKey(Key, Pressed)` for mouse-wheel up/down → BP graph checks active `scroll_*` flag → mutates value | `TerminalScroll { terminalKey, peerId, scrollMode:u8, delta:i8 }` reliable; host updates value field |
| **A3 — Slot insertion** | `UBoxComponent`/`UStaticMeshComponent` with `BeginOverlap` binding; held physics-prop overlap triggers slot install | BeginOverlap fires when `Aprop_X_C` enters slot → BP calls `plugInModule(holdActor, slot, Player, return)` or `putDriveIn(overlapped)` / `insertFloppy(floppy)` | DERIVED — prop_lifecycle already syncs prop position; BOTH peers' local slots fire the overlap; state ends up identical via deterministic local processing. ONLY supplementary: sync the prop's STATE PAYLOAD (`Aprop_drive_C.dataPaste`, `Aprop_floppyDisc_C.Data`) which prop_lifecycle does NOT currently cover. |
| **A4 — Lever / continuous-press** | `UBoxComponent` with hold-to-pull semantics (`leverMoving`/`isAnim` flags); BP timeline drives smooth motion | `player_use(Player, Hit)` triggers `moveLever(Condition)` → drives `leverTL` Timeline → state changes when timeline completes | Treat as A1 (click button) if it's a one-shot pull. If it's a continuous-hold lever (rare), needs key-state synced like the WASD path. |
| **A5 — Puzzle multi-button** | Array of `UPrimitiveComponent*` puzzle elements (`puzzle_buttons`, `fusesButtons`, `fusesFuses`); player presses each in sequence; state is in a parallel `TArray<bool>` (`puzzleLights`, `fuses`) | Looking at sub-button → BP sets `isLookAtButton`/`lookAtButtonIndex`; press triggers `switchSwitch(Index, Update)` / `switchNeighbors(Index, Update)` | Treat each as A1; the resulting array state is in the terminal's snapshot |

### 12.2 Per-terminal analog control inventory

#### 12.2.1 `AanalogDScreenTest_C` (the central signal-processing computer)

This is the user's "computer actor" — the BIG one. ~25 physical buttons
+ 12 module slots + 5 scroll modes + 2 drive slots + 1 phone button.

**Click buttons (A1 archetype) — 25 total:**

PLAYBACK section (`active_play` drives screen_play):
- `button_play_left` (UBoxComponent @0x05C0) — prev signal in list, drives `play_selectIndex--`
- `button_play_right` (UBoxComponent @0x05C8) — next signal, `play_selectIndex++`
- `button_play_vis` (UBoxComponent @0x0238) — toggle `visualizerMode:bool` @0x0688
- `button_play_saveSig` (UBoxComponent @0x0240) — invokes `canSaveSignal()` save-current
- `button_play_remapMax` (UBoxComponent @0x0248) — selects `scroll_remapMax` mode (A2)
- `button_play_remapMin` (UBoxComponent @0x0250) — selects `scroll_remapMin` mode (A2)
- `button_play_vol` (UBoxComponent @0x0258) — selects `scroll_volume` mode (A2)
- `button_play_scroll` (UBoxComponent @0x0260) — selects `scroll_list` mode (A2)

DOWNLOAD section (`active_download` drives screen_download):
- `button_downl_PF_spdAdd_1` (UBoxComponent @0x0268) — increments `DL_poFilterSpeed` by 1
- `button_downl_PF_spdAdd_15` (UBoxComponent @0x0270) — +15
- `button_downl_PF_spdAdd_5` (UBoxComponent @0x0278) — +5
- `button_downl_FF_spdAdd_1` (UBoxComponent @0x0280) — increments `DL_FrFilterSpeed` by 1
- `button_downl_FF_spdAdd_5` (UBoxComponent @0x0288) — +5
- `button_downl_FF_spdAdd_15` (UBoxComponent @0x0290) — +15
- `button_downl_PF_toggle` (UBoxComponent @0x05A0) — toggles `DL_activePoFilter:bool` @0x0A05
- `button_downl_FF_toggle` (UBoxComponent @0x05B0) — toggles `DL_activeFrFilter:bool` @0x0A04
- `button_downl_pdir_toggle` (UBoxComponent @0x0580) — cycles `DL_PolarityDir:int32` @0x0A30
- `button_downl_delSig` (UBoxComponent @0x0588) — deletes downloaded signal
- `button_downl_saveSig1` (UBoxComponent @0x0590) — saves downloaded signal

COORDS section (`active_coords` drives screen_coords):
- `screenbutton_switchToCoord1` (UBoxComponent @0x0328) — switch active dish target slot 1
- `screenbutton_switchToCoord2` (UBoxComponent @0x02E8) — slot 2
- `screenbutton_switchToCoord3` (UBoxComponent @0x02E0) — slot 3
- `screenbutton_bringCoord1` (UBoxComponent @0x0308) — load saved coord 1 into target
- `screenbutton_bringCoord2` (UBoxComponent @0x02D0) — load coord 2
- `screenbutton_bringCoord3` (UBoxComponent @0x02D8) — load coord 3
- `screenbutton_bringCoordCent` (UBoxComponent @0x02C0) — bring to center

COMP (decode) section (`active_comp` drives screen_comp):
- `button_comp_start` (UBoxComponent @0x0608) — fires `comp_start(startProgressFrom, succ)`
- `button_comp_stop` (UBoxComponent @0x0600) — fires `comp_stop()`
- `button_comp_upload` (UBoxComponent @0x0610) — fires `comp_uploadData(InputPin, succ)`
- `button_comp_maxLevel` (UBoxComponent @0x05F8) — selects `scroll_downloadScroll` mode for max-level adjust (A2)

GLOBAL:
- `button_coords` (UBoxComponent @0x05E0) — toggle coord screen active
- `button_phone` (UBoxComponent @0x0458) — answer/dismiss red phone alert; fires `startRinging()`/`tryRinging()` related logic. Has its own timer state (`timerHandle_redphone` @0x1408).

**Scroll knobs (A2 archetype) — 5 modes:**

The 5 mutually-exclusive scroll_* booleans @0x1350..0x1354:
- `scroll_downloadScroll` (bit for max-level decode adjust)
- `scroll_volume` (signal play volume `play_volume:int32` @0x06D0)
- `scroll_list` (signal-list selection `play_selectIndex:int32` @0x06C4)
- `scroll_remapMin` (`remapValue.X` floor for visualizer)
- `scroll_remapMax` (`remapValue.Y` ceiling)

`event_up()` / `event_down()` / `Event scrollDown()` are the BP scroll handlers
called from `intComs_anyKey` when the mouse wheel fires. The active
`scroll_*` flag (set by clicking the corresponding A1 button) gates
which value the scroll mutates. `updScroll()` refreshes display.

**Module slots (A3 archetype) — 12 slots:**

`mod1..mod12` (UStaticMeshComponent @0x0438, 0x0428, 0x0420, 0x0418,
0x0410, 0x0408, 0x0400, 0x03F8, 0x03F0, 0x03E8, 0x03E0, 0x03D8).

Each has `BndEvt__analogDScreenTest_modN_ComponentBeginOverlapSignature__DelegateSignature`
binding. When a held `Aprop_physModule_*_C` overlaps, BP calls
`plugInModule(holdActor, slot, Player, return)` which validates
(`isModuleAllowed`) and inserts.

The result populates `physMods:TArray<enum_physicalModules::Type>` @0x12A0
and updates which features are active (radar_hasAlarm, radar_hasColors,
has_radarRadius, has_radarRadial, has_radarDraw, has_autoCoordRot,
has_autoCoordVis, has_tapeCompression, …).

Module enum has 34 entries (`enum_physicalModules_enums.hpp`) — placeholder
names (`NewEnumerator0..33`). The wire layer treats them as opaque u8.

**Back-panel covers (A3 archetype, special) — 4 covers:**

`lid1` (UStaticMeshComponent @0x03D0), `lid2` (@0x03C8), `lid3_no` (@0x03C0),
`lid4` (@0x03B8). These are panel covers that block module access. Player
unscrews them with a screwdriver (uses `unscrewPanel()` + `resetUnscrew()`
+ `unscrewProgress:float` @0x1300 + `player_unscrew:AmainPlayer_C*` @0x1308).

State arrays: `panelsState:TArray<bool>` @0x12B0, `panels:TArray<UPrimitiveComponent*>`
@0x12C0, `panelsUnscrew:TArray<bool>` @0x12D0, `lookingAtPanel:int32` @0x12E0,
`panelTakeOff:bool` @0x12E4, `canTakeOff:bool` @0x12E5.

When unscrewed, the cover becomes a held prop via `lookAtPanel:UPrimitiveComponent*` @0x12F8.

**Drive slots (A3 archetype, special) — 2 slots:**

- `driveSlot_comp:UChildActorComponent*` @0x0548 — comp/decode drive slot
  (hosts `AdriveSlot_C`); current installed drive in `obj_driveSlot_comp:AdriveSlot_C*` @0x0A88
- `driveSlot_play:UChildActorComponent*` @0x0568 — playback drive slot;
  current drive in `obj_driveSlot_play:AdriveSlot_C*` @0x0648

`AdriveSlot_C` is its own actor with a `drivePort:UBoxComponent` and
`drive:Aprop_drive_C*`. When a drive overlaps, `putDriveIn(overlapped)`
installs; `drivePulledOut()` removes. Each fire delegates `driveIn(drive)`
+ `driveOut()` which the terminal listens to via `driveIn_play(drive)` /
`driveOut_play()` / `driveIn_comp(drive)` / `driveOut_comp()`.

CRITICAL: `Aprop_drive_C` carries 562B of state (`dataPaste:Fstruct_signal_data`
@0x0388 + `Data_0:Fstruct_signalDataDynamic` @0x0550). This DATA must
sync between peers when a drive prop is held by either peer and inserted.
Currently prop_lifecycle syncs position only — drive data is a new sync
domain.

#### 12.2.2 `Apanel_radar_C`

**Click buttons (A1) — 1:**
- `lever:UBoxComponent` @0x0298 — radar lever (probably "scan" trigger)

**Module slots (A3) — 4:**
- `meshslot1..4:UStaticMeshComponent` @0x0270..0x0288 — physical module
  slot meshes (visual)
- `meshmodule1..4:UStaticMeshComponent` @0x0250..0x0268 — module mesh
  inserted (visual representation; the slot's content)
- `moduleMeshes:TArray<UPrimitiveComponent*>` @0x0318 — combined collision/list
- Each has `BndEvt__panel_radar_meshmoduleN_ComponentBeginOverlapSignature__DelegateSignature`
- BP calls `setUpgSlot(upgrade:AActor*, slotComponent:UPrimitiveComponent*)`
- Validates with `allowedModules(Module, isAllowed)`
- State: `upgrades:TArray<enum_physicalModules::Type>` @0x0308

`lookAtSlot:bool` @0x0328, `lookatModule:enum_physicalModules::Type` @0x0329,
`lookingAtButtons:bool` @0x032A, `lookingAtSlots:bool` @0x032B,
`lookingAtSlotIndex:int32` @0x032C track look-at state.

#### 12.2.3 `Apanel_SATconsole_C`

**Click buttons (A1) — 0 dedicated physical buttons.**

This terminal is UMG-dominant. The "buttonsRoot:UBillboardComponent" @0x0288
and "stand:UBillboardComponent" @0x0290 are SCENE GROUPING only — the
actual interactive controls live on the embedded `Widget:Uui_console_C*`
@0x0298 (EditableTextBox + typed commands).

The ONLY phys "button" is the player walking up + pressing E to engage
the console via `updName:USphereComponent` @0x0278 proximity trigger
(`BndEvt__updName_ComponentBeginOverlapSignature` + `ComponentEndOverlapSignature`).

So Apanel_SATconsole_C is COMMAND-LINE ONLY — analog-control sync for
it is just the typed-command path covered in section 5. Nothing
additional to design.

#### 12.2.4 `AcoordRadarDish_C` (outdoor dish puzzle)

**Click buttons (A1) — 2:**
- `button_retract1:UBoxComponent` @0x0260 — retract dish 1
- `button_retract2:UBoxComponent` @0x0268 — retract dish 2
- `buttonLever:UBoxComponent` @0x02E0 — main lever

**Lever (A4) — 1:**
- `lever:UBillboardComponent` @0x0270 (visual anchor)
- `meshLever:UStaticMeshComponent` @0x02E8
- `leverTL:UTimelineComponent` @0x0350 — smooth lever motion
- State: `leverMoving:bool` @0x03AC, `isLookAtLever:bool` @0x03AD,
  `isLookAtRetract:bool` @0x0411, `isAnim:bool` @0x0412
- `moveLever(Condition)` fires the timeline

**Puzzle multi-button (A5) — variable:**
- `puzzle_buttons:TArray<UPrimitiveComponent*>` @0x0370
- `puzzle_lights:TArray<UParticleSystemComponent*>` @0x0380
- `puzzleLights:TArray<bool>` @0x0360 — state per light
- `puzzleMeshes:TArray<UStaticMeshComponent*>` @0x0400
- BP funcs: `switchSwitch(Index, Update)`, `switchNeighbors(Index, Update)`,
  `updPuzzle()`, `genPuzzle()`, `solvePuzzle()`, `Scramble Radar Dish()`,
  `skipPuzzle:bool` @0x03F8
- `isLookAtButton:bool` @0x0394, `lookatText:FString` @0x0398,
  `lookAtButtonIndex:int32` @0x03A8

**Fuse insertion (A3-puzzle hybrid) — variable count:**
- `fusesButtons:TArray<UPrimitiveComponent*>` @0x03B0 — slot collision boxes
- `fusesFuses:TArray<UPrimitiveComponent*>` @0x03C0 — visual fuse mesh
- `fuses:TArray<uint8>` @0x03D0 — STATE: which slot has which fuse type
- BP funcs: `insertFuseIn()`, `countFuses(Amount)`, `genFuses()`, `updFuses()`
- `isLookAtFuse:bool` @0x03E0, `lookAtFuseIndex:int32` @0x03E4

**Audio fields** (for proper sound feedback during sync):
- `audio_fuseInsert`, `audio_fusePullout`, `audio_lever`, `audio_fail`,
  `audio_success`, `audio_click` — all UAudioComponent

#### 12.2.5 `AserverBox_C`

**Click buttons (A1) — 0 dedicated.** The serverBox is mostly proximity-
triggered (`Box:UBoxComponent` @0x0338 + `coll:UBoxComponent` @0x0260 +
`takeUpgrade:UBoxComponent` @0x0268).

**Floppy slot (A3) — 1:**
- `floppyRoot:UBillboardComponent` @0x0298
- `floppyMesh:UStaticMeshComponent` @0x02A0 — visual when inserted
- `floppyMove:UBillboardComponent` @0x02A8
- `Timeline_1` @0x0360 (floppyIn/floppyTR floats) — smooth animation
- BP funcs: `insertFloppy(floppy:Aprop_floppyDisc_C*)`, `ejectFloppy()`
- State: `floppyData:TArray<FString>` @0x03D8, `floppyObjectData:FString` @0x03E8,
  `floppyType:int32` @0x03F8, `fltr_A:FTransform` @0x0400, `floppyReadwrites:int32` @0x0430

**Upgrade slots (A3) — 3:**
- `upg1:UStaticMeshComponent` @0x0290
- `upg2:UStaticMeshComponent` @0x0280
- `upg3:UStaticMeshComponent` @0x0288
- `takeUpgrade:UBoxComponent` @0x0268 — extract slot
- State: `upgrade_1/2/3:bool` @0x0434..0x0436, `upgrades:int32` @0x0440

`lookatFloppyButton:bool` @0x0444, `lookatUpgrades:bool` @0x0458 track
look-at state.

When server is damaged, `breakServer()` → `IsBroken:bool @0x0378` = true →
hacking minigame triggers via `Uui_serverMinigame_C` (covered in audit
section 11.10 — first-peer-owns-minigame v1).

### 12.3 Input dispatch mechanics — 4 dispatch paths

After this catalog, the input dispatch mechanics resolve to 4 distinct paths:

**Path P1 — Use-key on component**:
- E pressed → `mainPlayer.input_E(true)` → arm trace → `lookAtActor` +
  `lookAtComponent` populated → calls `lookAtActor.player_use(this, Hit)`
- The actor's `player_use` BP graph inspects `Hit.GetComponent()` and
  dispatches to the matching button handler.
- Used for: A1 (click buttons), A4 (lever), A5 (puzzle buttons)
- Hook point for coop: PRE-observer on `player_use` of each terminal class;
  capture `Hit.Component`'s name; forward as componentHash.

**Path P2 — Mouse wheel via `intComs_anyKey`**:
- Scroll wheel fires → mainPlayer's BP graph dispatches → terminal's
  `intComs_anyKey(FKey, Pressed)` receives it
- BP graph checks active `scroll_*` flag → calls `event_up()` / `event_down()`
- Used for: A2 (scroll knob)
- Hook point: PRE-observer on `intComs_anyKey` of analogDScreenTest_C;
  detect WheelUp / WheelDown / scroll-mode buttons; forward.

**Path P3 — Held-prop overlap (slot insertion)**:
- Player holds prop (via grab system) → prop overlaps slot's collision box
- `BndEvt__<slot>_ComponentBeginOverlapSignature` fires
- BP calls `plugInModule` / `putDriveIn` / `insertFloppy` etc.
- Used for: A3 (module/drive/floppy/upgrade slots)
- ALREADY SYNCED via prop_lifecycle position sync — overlap detection is
  deterministic on both peers given identical positions. NO new wire
  packet needed for the OVERLAP itself.
- SUPPLEMENTARY: the prop's STATE PAYLOAD (drive's dataPaste, floppy's Data,
  serverUpg's upg byte) is NOT currently in prop_lifecycle sync. Add
  `PropDataPayload { propKey, payloadType:u8, payloadBlob[200] }` reliable
  on prop spawn/transfer.

**Path P4 — Continuous use (lever hold, screwdriver progress)**:
- Player holds E → mainPlayer ticks → terminal's `used(bool Pressed)` fires
  + integration on terminal side (e.g., `unscrewProgress:float` accumulates)
- Used for: panel-unscrew + maybe coordRadarDish lever (TBC)
- Hook: same as P1; need extra "still holding" pulse if it's truly continuous.

### 12.4 The widget atlas + render-target architecture (visual sync is free)

`AanalogDScreenTest_C` has `widgetRender:UWidgetComponent` @0x0618 +
`widget_RT:UTextureRenderTarget2D*` @0x0650 + the SHARED widget atlas
`Widget:Uui_consolesAtlas_C*` @0x0638.

`Uui_consolesAtlas_C` (~1700 bytes) is THE master UMG. It contains:
- `umg_console:Uui_console_C*` @0x0600 — embedded SAT console
- `umg_radar:Uui_radar_C*` @0x0608 — embedded radar
- `ui_atlasDishesStatus`, `ui_coordinates`, plus all 16 deathscreen sub-widgets
- Atlas of text/image fields fed from analogDScreenTest state

The architecture: state lives on `AanalogDScreenTest_C` → atlas widget
reads it → atlas renders to `widget_RT` → `widgetRender` projects onto
the in-world screen mesh.

`Aticker_widgetRender_C` (separate actor) holds `panels:AanalogDScreenTest_C*`
+ `FrameRate:int32` and ticks the atlas refresh. `changeFramerate(N)`
adjusts the refresh rate.

**Implication for coop sync**: if both peers' local `AanalogDScreenTest_C`
state is identical (via section 11.x wire layer), both peers' atlases
render identical pixels. We do NOT network the render target. We do NOT
network the widget. **State sync = visual sync**, automatically.

The ticker's `FrameRate` is a perf lever — on the host peer (the one
generating authoritative state), keep at design default; on client peer
(rendering a mirror), can be left at the same rate since it's just
reading local state (no network cost).

### 12.5 Wire protocol additions for analog controls

The 11.1 packet set is INSUFFICIENT for analog controls. Adding:

```cpp
// A1: Click buttons (P1 dispatch)
struct TerminalButtonPress {
    u32 terminalKey;
    u8  peerId;
    u32 componentHash;     // hash of "buttonName" string (e.g.,
                           //   hash("button_play_left"))
};

// A2: Scroll knob (P2 dispatch)
struct TerminalScroll {
    u32 terminalKey;
    u8  peerId;
    u8  scrollMode;        // 0=downloadScroll, 1=volume, 2=list,
                           //   3=remapMin, 4=remapMax, 5=other
    i8  delta;             // +1 or -1 (wheel notch)
};

// A3 supplementary: prop data payload (drive/floppy/upgrade state)
struct PropDataPayload {
    u32 propKey;           // existing prop_lifecycle key
    u8  payloadType;       // 0=driveData, 1=floppyData, 2=upgByte, 3=radArgemI
    u16 payloadLen;        // <= 200 (split into fragments if larger)
    u8  payload[200];      // serialized Aprop subclass-specific fields
};

// A4/A5: covered by A1; no new packets needed.

// Slot-state notification (optional defensive packet for desync recovery)
struct TerminalSlotState {
    u32 terminalKey;
    u8  slotType;          // 0=mod, 1=drive_play, 2=drive_comp, 3=floppy,
                           //   4=serverUpg, 5=panel_radar_mod
    u8  slotIndex;         // 0..15
    u32 occupantPropKey;   // 0 if empty
    u8  occupantStateByte; // mod-enum or 0xFF
};
```

Total new packet types: 4 (TerminalButtonPress, TerminalScroll,
PropDataPayload, TerminalSlotState). All fit in 256B MTU. All reliable
(button/scroll/payload/slot state are infrequent and order-sensitive).

### 12.6 Updated implementation increments (post-section-12 revision)

Revised Inc plan to slot in analog-control work:

- **Inc0 — UE4SS Lua probe session** (unchanged): resolve F-8.1..F-8.9
  PLUS the new F-8.10..F-8.13 below. 30-45 min.
- **Inc1 — Terminal-key resolution + singleton-fallback** (unchanged)
- **Inc2 — TerminalEnter/Exit + held-key heartbeat scaffolding** (unchanged)
- **Inc3 — TerminalInput unreliable + held-key 1Hz heartbeat** (unchanged)
- **Inc4 — TerminalCrosshairState 20Hz broadcast** (unchanged)
- **Inc4.5 (NEW) — TerminalButtonPress reliable** for A1 click buttons.
  Hook every terminal's `player_use` PRE; capture `Hit.GetComponent().GetFName()`;
  forward to host; host re-fires the named BndEvt via reflection. Tests:
  client clicks a button → host's terminal state updates → broadcasts
  back via Inc4/Inc6.
- **Inc4.6 (NEW) — TerminalScroll reliable** for A2 scroll knobs. Hook
  analogDScreenTest's `intComs_anyKey` PRE; capture wheel deltas; forward.
- **Inc5 — TerminalSnapshot one-shot reliable** (unchanged)
- **Inc6 — TerminalDelta 10Hz reliable** (unchanged)
- **Inc6.5 (NEW) — PropDataPayload reliable** for A3 drive/floppy/upgrade
  data sync. Hook each prop subclass's spawn + pickup/drop to attach data
  blob to existing prop_lifecycle messages. Receivers apply via reflection
  to the matching field.
- **Inc7 — Per-class codecs** (unchanged) — also covers panel_radar
  upgrades, coordRadarDish puzzle state.
- **Inc7.5 (NEW) — TerminalSlotState defensive sync** — on TerminalEnter
  + on suspected desync, host sends slot occupancy for each slot. Belt
  and suspenders: A3 sync is deterministic from prop position, but if
  prop_lifecycle drops a packet, this recovers.
- **Inc8 — Command-line replication** (unchanged)
- **Inc9 — EXCLUSIVE-minigame v1 gate** (unchanged)

### 12.7 New escalation flags (UE4SS Lua probe additions)

Add to Inc0 probe session (now ~10 flags total):

- **F-8.10 (NEW)**: For each named `BndEvt__<actor>_<button>_ComponentOnClickedSignature__DelegateSignature`,
  is it bound to the ON-CLICK of the component (LMB click) or to
  player_use-component-match? Read the BeginPlay body in BP. This decides
  whether we hook player_use or the component's OnClicked delegate.
- **F-8.11 (NEW)**: For analogDScreenTest's `intComs_anyKey` body, does it
  dispatch ONLY scroll events or ALL key events? If all, our hook needs
  to scope down. Probe by logging keys during normal play.
- **F-8.12 (NEW)**: For `Aprop_drive_C.dataPaste` and `Aprop_floppyDisc_C.Data`
  — are these fields STABLE while the prop is held (no per-tick mutation)
  or do they update continuously? If stable, we sync once on pickup; if
  continuous, we sync periodically.
- **F-8.13 (NEW)**: For `AdriveSlot_C.BndEvt__drivePort_BeginOverlap`,
  does the slot's BP call `putDriveIn` immediately on overlap or wait
  for a "drop" gesture? If immediate, both peers' local slots register
  install once prop_lifecycle positions converge. If gesture-gated, we
  need to sync the drop event.

### 12.8 Concurrency edge cases (specific to analog controls)

- **EC-1 — Both peers click the same button simultaneously**: Two
  TerminalButtonPress packets arrive at host within ~ms. Host applies
  both via reflection; the button's BP graph runs twice. Most buttons
  are idempotent (toggle bool, +1 to counter) so double-fire is benign.
  RISKY buttons: `comp_start()` (might start the decode twice — but the
  BP probably guards against that internally). DECISION: NO ordering or
  de-dup at wire level v1; rely on BP idempotency. Test in Inc4.5; if
  any button breaks, add an "in-flight" gate per buttonHash.

- **EC-2 — Peer A starts scrolling volume while peer B clicks
  scroll_remapMin**: peer A's mode-flag (scroll_volume) gets overwritten
  by peer B's click (scroll_remapMin). Then peer A's next scroll event
  drives remapMin, not volume. **This is correct behavior** — the active
  scroll mode is GLOBAL on the terminal, not per-peer. User's "race
  together" allows for this kind of interference.

- **EC-3 — Peer A is unscrewing a panel while peer B walks away**:
  unscrewProgress increments only while player_unscrew == that peer. If
  host owns the screwdriver progress state, and client started unscrewing,
  the screwdriver-progress is in host's analogDScreenTest. When client
  walks away, BP graph should reset progress (it has `resetUnscrew()`).
  Wire: client sends "stopped unscrewing" via TerminalExit or via
  player_unscrew=null state. Need to verify BP graph triggers this on
  player exit (F-8.4 covers Escape-clear).

- **EC-4 — Drive prop carried by client; client disconnects mid-air**:
  prop_lifecycle handles the prop position freeze. Drive data sync
  (PropDataPayload) is one-shot at pickup, so the data is intact. When
  host or other client picks up the dropped drive, sync resumes
  normally. No special handling needed.

- **EC-5 — Server breaker prop**: `Aprop_serverBreaker_C` uses RMB
  while held to do something to nearby servers. This is a HELD-PROP
  interaction (playerHand_RMB), not a terminal interaction. Probably
  covered by Phase 5N1 NPC sync (it does AI-related damage), not Phase
  5T. Confirm in Inc7.

### 12.9 What's still NOT covered (out of scope)

These exist in the dump but are not part of terminal sync:

- **Walking/proximity effects on terminals**: e.g., audio cues when
  approaching analogDScreenTest's `Sphere` overlap. These are local-only
  audio; no sync needed.
- **Visual-only props** (`prop_radArgem_C`, `prop_computerpanels_C`):
  radArgem is a small physics prop with its own getData/loadData; covered
  by Aprop sync. computerpanels is a food-derivative decoration.
- **`Aprop_serverBreaker_C`**: see EC-5.
- **`Atrigger_breakDish_C`**: world trigger that breaks a specific dish;
  fires `runTrigger(Owner, Index)`. Save-state event, not real-time. Phase
  5S0 / event-system territory, not Phase 5T.

---

### 12.10 Section-12 update log

- 2026-05-25 LATE +3h: full analog-control RE pass per user directive
  ("Dig into RE of computer actor analogue controls"). 5 archetypes
  catalogued. Per-terminal control inventory documented. 4 dispatch
  paths identified. Widget-atlas architecture documented. Wire protocol
  extended with 4 new packet types (TerminalButtonPress, TerminalScroll,
  PropDataPayload, TerminalSlotState). 3 new Inc additions (4.5, 4.6,
  6.5, 7.5). 4 new F-flags (F-8.10..F-8.13). 5 concurrency edge cases
  documented.
- 2026-05-25 LATE +4h: audit pass via feature-dev:code-reviewer.
  8 issues caught (4 HIGH + 4 MEDIUM). Corrections in section 12.11
  below.

---

## 12.11 Audit corrections (post-section-12 audit, 2026-05-25 LATE +4h)

### 12.11.1 CRITICAL — dispatch path SPLIT for A1 click buttons

Section 12.3 P1 claimed all A1 click buttons dispatch via
`player_use → BP graph reads Hit.GetComponent()`. **Wrong for the 7
`screenbutton_*` coord buttons.** The dump shows they have
`BndEvt__analogDScreenTest_screenbutton_<name>_K2Node_ComponentBoundEvent_N_ComponentOnClickedSignature__DelegateSignature`
bindings — direct LMB-click delegates on the UBoxComponent, NOT
`player_use` dispatches.

**Resolution:** A1 archetype splits into A1a and A1b:

- **A1a (player_use-routed)**: most analogDScreenTest buttons (button_play_*,
  button_downl_*, button_comp_*, button_coords, button_phone), plus
  panel_radar.lever, plus coordRadarDish.button_retract1/2.
  Hook: PRE-observer on `player_use(Player, Hit)` of the actor; capture
  `Hit.Component->GetFName()`.
- **A1b (OnClicked-bound)**: the 7 `screenbutton_*` on analogDScreenTest
  (switchToCoord1/2/3, bringCoord1/2/3, bringCoordCent).
  Hook: PRE-observer on each named
  `BndEvt__analogDScreenTest_screenbutton_<name>_..._OnClickedSignature__DelegateSignature`
  UFunction directly. 7 distinct hooks per terminal class.

**Implication for Inc4.5**: must implement BOTH hook strategies. The wire
packet `TerminalButtonPress { terminalKey, peerId, componentHash }`
remains; the divergence is the HOOK side, not the wire side.

Update F-8.10: "Identify which BndEvt entries are A1a vs A1b on each
terminal class; the suffix `_ComponentOnClickedSignature__DelegateSignature`
is the A1b tell from the dump alone — Lua probe confirms whether the
A1a buttons (no OnClicked BndEvt) really go through player_use vs some
other path I missed."

### 12.11.2 CRITICAL — scroll hook should be `playerHandMouseWheel`, not `intComs_anyKey`

Section 12.3 P2 / Inc4.6 said hook `intComs_anyKey` for scroll capture.
**Wrong.** `intComs_anyKey` is the gamemode event-bus (fires on EVERY
key); it's a broadcast bus, not a scroll-specific dispatch.

The dump shows `analogDScreenTest.playerHandMouseWheel(Player, float wheelDelta)`
@ analogDScreenTest.hpp:505 — a dedicated UFunction per terminal class
that receives mouse-wheel float deltas. THIS is the precise hook target.

**Resolution (RULE 1 root-cause fix):** Inc4.6 hooks
`playerHandMouseWheel(Player, float)` on each terminal class. Wire packet
`TerminalScroll { terminalKey, peerId, scrollMode:u8, delta:i8 }` revised:

```cpp
struct TerminalScroll {
    u32 terminalKey;
    u8  peerId;
    f32 wheelDelta;        // raw float delta from playerHandMouseWheel
                           //   (BP graph applies it against current
                           //   scroll_* flag locally)
};
```

The `scrollMode:u8` from the original 12.5 design is REMOVED — the
scroll mode is GLOBAL on the terminal (set by clicking a scroll-mode
button, an A1 event already covered separately). The host just receives
the wheel delta and applies it via reflection-fired `playerHandMouseWheel`.

Update F-8.11: "Confirm via Lua probe that `playerHandMouseWheel` BP
graph dispatches to event_up/event_down based on the currently-active
scroll_* flag. If yes, hooking playerHandMouseWheel is sufficient. If
no (e.g., it has a sign check or non-trivial integration), we need to
hook event_up/event_down instead."

### 12.11.3 HIGH — non-idempotent accumulator buttons enumerated

Section 12.8 EC-1 was wrong to lump all non-`comp_start` buttons as
idempotent. The following buttons MUTATE BY +N (not by toggle), so
double-fire causes 2× accumulation:

| Button | Mutation |
|---|---|
| `button_downl_PF_spdAdd_1` | `DL_poFilterSpeed += 1.0` |
| `button_downl_PF_spdAdd_5` | `DL_poFilterSpeed += 5.0` |
| `button_downl_PF_spdAdd_15` | `DL_poFilterSpeed += 15.0` |
| `button_downl_FF_spdAdd_1` | `DL_FrFilterSpeed += 1.0` |
| `button_downl_FF_spdAdd_5` | `DL_FrFilterSpeed += 5.0` |
| `button_downl_FF_spdAdd_15` | `DL_FrFilterSpeed += 15.0` |
| `button_play_left` | `play_selectIndex -= 1` |
| `button_play_right` | `play_selectIndex += 1` |

**Resolution (v1 design decision, 2026-05-25):** PER-COMPONENT host-side
de-dup window of **80 ms**. Host tracks `lastPressTime[componentHash]:u64`
per terminal. If a TerminalButtonPress arrives for the same componentHash
within 80ms of the last accepted press, REJECT the duplicate. 80ms is
chosen as: longer than two-peer click clustering (~30ms LAN latency
both ways), shorter than the user's tactile expectation for "this is a
new press" (typing repeats at ~250ms; UI button clicks feel discrete at
≥100ms).

This de-dup applies to ALL buttons (toggle, +1, +N) for safety. Toggle
buttons that double-fire would NO-OP-back-to-original anyway — but the
de-dup is cheap and avoids visual flicker.

The de-dup REJECTS one peer's press silently in the rare two-peer race.
Acceptable: the user's "race together" intent for analog controls is
specifically the WASD continuous-input case (covered in section 11),
not the discrete button-click case. For buttons, "first peer wins by
~80ms" is the right race semantic.

### 12.11.4 HIGH — PropDataPayload struct fragmentation gap

Section 12.5's `PropDataPayload { payloadLen:u16, payload[200] }` is
self-contradictory if payloadLen > 200. Drive `dataPaste` is 456 bytes
(450B struct + alignment to 0x1C8 field gap) — exceeds 200.

**Resolution:** REMOVE `PropDataPayload` from section 12.5 entirely.
Drive data sync routes through TWO existing channels:

1. **At prop spawn / pickup-by-peer**: piggyback onto the existing
   prop_lifecycle's PropSpawn message. Add a payload section to PropSpawn:
   `prop_class:u8 + dataLen:u16 + data:variable` with the existing
   reliable-stream fragmentation (the chat channel pattern). Drive +
   floppy + radArgem all use this path. Triggered ONCE on Aprop_C.Init
   POST when the spawning peer holds the prop.
2. **At drive insertion into a terminal slot**: when client inserts a
   drive into host's slot, host already detects the overlap locally
   (deterministic from synced position). Host READS the drive's data
   FROM ITS OWN LOCAL DRIVE COPY via reflection (the drive prop is a
   synced Aprop_C; it exists on host too). NO additional wire needed.

This eliminates `PropDataPayload` as a standalone packet. The drive
data lives on the existing prop_lifecycle channel.

Inc6.5 is REMOVED. Drive data sync becomes Inc2.5: "extend prop_lifecycle
PropSpawn with subclass-specific payload (drive/floppy/radArgem state)".

### 12.11.5 MEDIUM — module enum versioning (NEW F-8.14)

The 34-value `enum_physicalModules` is treated as opaque u8 on wire.
If VOTV reorders or extends the enum between game versions, two peers
on different versions silently misinterpret module identities.

**Resolution:** New escalation flag F-8.14: "Module enum stability
checked via `tools/sdk_diff.py` (already exists per
[[project-adaptation-strategy]]). Add a check to compat_report.txt
generator that compares the enum_physicalModules table against
the baseline; fail loud if reordered." This becomes a pre-flight
check before Inc7 ships and stays as part of patch-day workflow.

### 12.11.6 MEDIUM — panel_radar has embedded Uui_console_C widget (NEW F-8.15)

Section 12.2.2 missed `panel_radar.Widget:Uui_console_C*` @0x02E0 — the
same typed-command widget as panel_SATconsole. The radar panel may
accept typed commands.

**Resolution:** New F-8.15: "Lua probe: while at panel_radar, log any
`enterCommand` calls and `EditableTextBox` events. Does the radar panel
accept typed commands like the SAT console? If yes, command-line
replication (Inc8) covers it for free. If no (just visual), document
the widget as render-only."

### 12.11.7 MEDIUM — player_using re-fire semantics

When host receives client's TerminalButtonPress and re-fires the BP
button action via reflection, it must pass HOST'S OWN local mainPlayer
pointer as the `Player` argument. This means:

- `analogDScreenTest.player_using` on host always reflects host's local
  player when client presses a button (the BP graph sets it during
  player_use).
- The same applies to `player_unscrew` for the unscrew progress.
- `actorStandingOn`, `lookAtActor`, `lookAtComponent` on host stay as
  host's locally-perceived values (unrelated to client's terminal use).

For client-only UI feedback (e.g. "Bob is using the terminal"),
the wire layer's separate `g_terminalUsers:set<peerId>` covers the
multi-peer-state visibility — we DO NOT rely on the BP's player_using
field for that.

**Resolution:** Documented here; no code change. EC-3 (unscrew interrupt
on walk-away) is updated: when client peer's mainPlayer leaves the
terminal proximity (no more TerminalInput packets + 1s timeout), host
explicitly calls `resetUnscrew()` on its local terminal IF
`player_unscrew == host.localPlayer` AND host knows the unscrew was
initiated by the now-departed client (host tracks this via a side-table
`g_unscrewOrigin:map<terminalKey, peerId>`).

### 12.11.8 MEDIUM — polarity/frequency toggle delegates (NEW F-8.16)

`analogDScreenTest.buttonPressed_polarityToggle` @0x1438 and
`buttonPressed_frequencyToggle` @0x1448 are multicast delegates that
external actors can subscribe to. When the host re-fires the
toggle button on client's behalf, the delegate fires on host —
subscribed actors on host get notified, but their client-side
counterparts do NOT.

**Resolution:** New F-8.16: "Lua probe: enumerate subscribers to these
two delegates at runtime (after BeginPlay). For each subscriber, decide:
(a) is the subscribed actor itself state-synced (then its client mirror
will derive the state change from the broadcast TerminalState/Delta and
behave correctly), or (b) is the subscriber a peer-local-only actor
(then we need to drive the delegate explicitly on client when state
arrives)?" Defer Inc4.5 polish until this is answered.

### 12.11.9 Final wire packet set (post 12.11 audit)

Effective packet additions from Phase 5T (revised):

```cpp
// A1 click buttons (BOTH a1a and a1b dispatch — same wire packet)
struct TerminalButtonPress {
    u32 terminalKey;
    u8  peerId;
    u32 componentHash;     // hash(component.Name); de-duped 80ms per
                           //   componentHash on host (12.11.3)
};

// A2 scroll knob (hook is playerHandMouseWheel, 12.11.2)
struct TerminalScroll {
    u32 terminalKey;
    u8  peerId;
    f32 wheelDelta;        // raw float from playerHandMouseWheel
};

// A3 supplementary REMOVED (12.11.4) — drive/floppy/radArgem data sync
// piggybacks on prop_lifecycle PropSpawn via subclass-specific payload
// extension; see Inc2.5.

// Defensive: slot occupancy snapshot
struct TerminalSlotState {
    u32 terminalKey;
    u8  slotType;
    u8  slotIndex;
    u32 occupantPropKey;
    u8  occupantStateByte;
};
```

Down to 3 packet types from 4 — cleaner. All fit 256B MTU comfortably.

### 12.11.10 (pre-Inc0-probe) Final increment plan (subject to revision after probe)

Phase 5T (Terminals) final increments:

- **Inc0** — UE4SS Lua probe (F-8.1..F-8.16 = 16 flags)
- **Inc1** — Terminal-key resolution + singleton-fallback
- **Inc2** — TerminalEnter/Exit + held-key heartbeat scaffolding
- **Inc2.5 (NEW per 12.11.4)** — Extend prop_lifecycle PropSpawn with
  subclass-specific payload (drive, floppy, radArgem state). Reuses
  existing reliable channel fragmentation.
- **Inc3** — TerminalInput unreliable + held-key 1Hz heartbeat
- **Inc4** — TerminalCrosshairState 20Hz broadcast
- **Inc4.5 (REV per 12.11.1)** — TerminalButtonPress reliable. Hook A1a
  (player_use PRE) AND A1b (per-BndEvt OnClicked PRE) on each terminal
  class. Host 80ms de-dup gate per componentHash.
- **Inc4.6 (REV per 12.11.2)** — TerminalScroll reliable. Hook
  `playerHandMouseWheel(Player, wheelDelta)` PRE per terminal class.
- **Inc5** — TerminalSnapshot one-shot reliable + multi-fragment
- **Inc6** — TerminalDelta 10Hz reliable for low-rate state changes
- **Inc6.5 (REMOVED)** — Drive data folded into Inc2.5
- **Inc7** — Per-class codecs (panel_radar, panel_SATconsole,
  coordRadarDish, serverBox state)
- **Inc7.5** — TerminalSlotState defensive sync
- **Inc8** — Command-line replication (covers Apanel_SATconsole and
  potentially panel_radar pending F-8.15)
- **Inc9** — EXCLUSIVE-minigame v1 gate (first-peer-owns)

Audit cycle after each Inc.

---

## 12.12 Empirical Inc0 probe findings (UE4SS Lua, dev copy, 2026-05-25 LATE +5h)

Ran two probe iterations against `Game_0.9.0n_dev/` with save `s_may2026`
loaded via the harness. Probe v1 used `RegisterHook` on BP UFunctions and
crashed UE4SS (BP path strings throw function-typed errors). v2 was
HOOKLESS (direct UFunction invocation + state polling).

### 12.12.1 Resolved flags (empirical confirmation)

**F-8.10 — OnClicked vs player_use dispatch — CONFIRMED**

`BlueprintGeneratedClass /Game/test/analogDScreenTest.analogDScreenTest_C`
has 268 UFunctions on the class. Filtering by suffix:
- 7 × `_ComponentOnClickedSignature__DelegateSignature` (the 7 `screenbutton_*`)
- 13 × `_ComponentBeginOverlapSignature__DelegateSignature` (12 `mod*` slots
  + 1 `lookRad` proximity)

Exact UFunction names (K2Node ordinals VOLATILE — must resolve at runtime,
not hardcode):
```
BndEvt__analogDScreenTest_screenbutton_switchToCoord1_K2Node_ComponentBoundEvent_2_ComponentOnClickedSignature__DelegateSignature
BndEvt__analogDScreenTest_screenbutton_switchToCoord2_K2Node_ComponentBoundEvent_3_ComponentOnClickedSignature__DelegateSignature
BndEvt__analogDScreenTest_screenbutton_switchToCoord3_K2Node_ComponentBoundEvent_4_ComponentOnClickedSignature__DelegateSignature
BndEvt__analogDScreenTest_screenbutton_bringCoord1_K2Node_ComponentBoundEvent_5_ComponentOnClickedSignature__DelegateSignature
BndEvt__analogDScreenTest_screenbutton_bringCoord3_K2Node_ComponentBoundEvent_6_ComponentOnClickedSignature__DelegateSignature
BndEvt__analogDScreenTest_screenbutton_bringCoord2_K2Node_ComponentBoundEvent_7_ComponentOnClickedSignature__DelegateSignature
BndEvt__analogDScreenTest_screenbutton_bringCoordCent_K2Node_ComponentBoundEvent_8_ComponentOnClickedSignature__DelegateSignature
```

**Implication for Inc4.5**: confirms A1a/A1b split from 12.11.1. The
non-screenbutton click buttons (button_play_*, button_downl_*, etc.) have
NO OnClicked BndEvt UFunctions on the class — they must route through
`player_use(Player, Hit)` and the BP graph dispatches on
`Hit.GetComponent()`. The 7 screenbuttons have their own dedicated
OnClicked entries.

**Resolve at runtime** (NOT hardcode K2Node ordinals): walk the
`UClass.Children` chain at boot per terminal class, regex-match
`BndEvt__<actor>_<button>_..._OnClickedSignature__DelegateSignature`,
cache the UFunction*. The ordinals change with every BP recompile —
RULE 2 (no migration baggage) + already-shipped reflection-resolved BP
offsets pattern (`reflected_offset.cpp`).

**F-8.11 — playerHandMouseWheel is gated on scroll_* flag — CONFIRMED**

Called `ads:playerHandMouseWheel(player, 1.0)` and `(-1.0)` directly.
Both calls returned successfully (call_ok=TRUE) but ALL state fields
unchanged:
```
BEFORE wheel +1 DL_po=0.0 DL_Fr=0.0 vol=10 sel=0 sDL=false sVol=false sList=false sMin=false sMax=false
AFTER  wheel +1 DL_po=0.0 DL_Fr=0.0 vol=10 sel=0 sDL=false sVol=false sList=false sMin=false sMax=false
```

Empirically confirms: `playerHandMouseWheel` BP body GATES on the active
`scroll_*` flag. No flag active → no-op. **This is the correct hook
target for Inc4.6**.

**Implication for Inc4.6**: TerminalScroll wire packet should fire
ONLY when scroll mode is active (cheap optimization: client also gates
the send on its local scroll_* state, since the BP graph would no-op
the host's re-fire otherwise). Or: send unconditionally and let the host
no-op — also acceptable.

**F-8.8 — rootConsole vs consoles[] — PARTIALLY RESOLVED**

At fresh save load (no players at terminal, no servers spawned):
- `analogDScreenTest.rootConsole = panel_SATconsole_C /Game/maps/untitled_1...panel_SATconsole_2`
  ✓ rootConsole is set at level-design time (UPROPERTY default).
- `analogDScreenTest.consoles[] = empty (length 0)`
  consoles[] populates DYNAMICALLY as servers spawn (the prior probe
  v1 run had a different save state and showed 25 entries — all
  `server_GEN_VARIABLE_panel_SATconsole_C_CAT_18xx` referencing each
  spawned server's child console).

**Implication for Inc6/Inc8**: typed commands route through
`rootConsole.Widget` (the primary). consoles[] are server-attached
mirrors that DISPLAY the log but don't drive commands. Inc6 wire calls
`rootConsole.Widget.enterCommand` via reflection.

**F-8.12 — drive/floppy data is per-instance UPROPERTY — CONFIRMED**

In the loaded save:
- 42 × `prop_drive_C` instances in PersistentLevel
- 3 × `prop_floppyDisc_C` instances (Y, R, G colored variants)
- 0 × `prop_radArgem_C` instances

Sample floppy state: `Data count = 0, zip=false, readWrites=32` (fresh,
unused floppies — but `readWrites=32` is the default max).

**Implication for Inc2.5 (drive data piggyback on PropSpawn)**: confirms
drive `dataPaste:Fstruct_signal_data` (~450B) and `Data_0:Fstruct_signalDataDynamic`
(~112B) are per-instance UPROPERTYs. Floppy `Data:TArray<FString>` is
also per-instance. Need to serialize these in Inc2.5.

**F-8.13 — driveSlot occupancy snapshot — CONFIRMED**

4 `driveSlot_C` instances in world, 0 currently occupied (`.drive = nil`).

**Implication for Inc7.5 (TerminalSlotState defensive)**: snapshot 
mechanism confirmed simple: walk all driveSlots + read .drive pointer.
Send `(slotKey, occupantDriveKey | 0)` packet on TerminalEnter for
state recovery.

**F-8.6 — widget instances enumeration — PARTIALLY RESOLVED**

At idle save-load state:
- 0 × `ui_console_C` instances live (widgets created on-demand when
  player approaches a console)
- 0 × `ui_radar_C` instances
- 0 × `ui_consolesAtlas_C` instances
- 0 × `ui_signal_C` instances
- 1 × `ui_handradar_C` (attached to GameInstance as a HUD precache —
  `mainGameInstance_C_2147482583.ui_handradar_C_2147482124`)
- 1 × `ui_serverMinigame_C` (attached to GameInstance precache too)

**Important architectural insight**: most terminal widgets are
LAZILY-CREATED, not pre-instantiated. They're spawned in BP's
`player_use` → `Enter Interface` path, attached to the terminal actor,
and destroyed on exit. The `ui_handradar_C` and `ui_serverMinigame_C`
are EAGERLY created on GameInstance and reused — those are the special
cases.

**Implication for coop sync**: client peer's local terminals create
their own widget instances when CLIENT pulls up the terminal. The widget
is per-peer-local (not shared across peers). State sync via reflection
writes to the backing actor; widget displays state on the local instance.

**F-8.15 — panel_radar.Widget is lazily populated — CONFIRMED**

At idle save-load:
- `panel_radar.Widget = INVALID` (Uui_console_C ref is null)
- `panel_radar.radarElement = INVALID` (Uui_radar_C ref is null)

Both UPROPERTY fields are populated on-demand when player approaches /
enters use. So Inc8's command-line replication for panel_radar can only
fire when the player is actively at the radar panel (which is fine —
no point sending typed commands when no one is there).

**Implication for F-8.15**: combined with prior dump finding that
panel_radar has the Uui_console_C field, panel_radar DOES support
typed commands when activated. Just like panel_SATconsole. Inc8 needs
to handle both rootConsole+consoles AND panel_radar.Widget instances.

### 12.12.2 Unresolved flags (probe limitations)

**F-8.14 — enum_physicalModules naming — STILL UNRESOLVED**

Found the enum at `/Game/main/enums/enum_physicalModules.enum_physicalModules`
but `UEnum::NumEnums()` returned 0 via UE4SS Lua (API path is wrong).
Need different access pattern:
- Try `enum.Names` direct property access (UEnum.Names: TArray<TPair<FName,int64>>)
- Or use IDA Pro to read the cooked enum table from the exe directly
- Or use UE4SS's blueprint reflection-dump artifact (the .hpp showed
  placeholder names only)

Action: **deferred to a v3 probe pass or IDA inspection**. Not blocking
Inc1; module enum is only consumed in Inc7 codec design.

**F-8.16 — delegate subscribers — STILL UNRESOLVED**

`InvocationList` access threw a non-string error in UE4SS Lua. The
multicast delegate (`buttonPressed_polarityToggle`,
`buttonPressed_frequencyToggle`) struct layout isn't directly exposed
to Lua scripts in this UE4SS version.

Action: **deferred to IDA inspection**. The delegate struct
`FMulticastInlineDelegate` has its invocation list at a known offset
(0x10 or 0x18 in UE4.27). IDA can read it. Document as a one-shot
IDA task before Inc4.5.

**F-8.1 — spaceRenderer integrator formula — UNTESTED**

Stage B integrator-poll never executed before the game crashed during
the screenbutton invoke test. Re-run needed in a future v3 probe.

**F-8.2/F-8.3/F-8.4 — input_E / player_use / Escape dispatch — INFERRED ONLY**

Cannot directly observe BP dispatch flow without RegisterHook (which
crashes in this UE4SS version for BP UFunctions). Inferred dispatch
flow from header signatures + dump structure is documented in section
12.3 but NOT EMPIRICALLY VERIFIED.

Action: **deferred to hands-on session** where user can run a
modified probe that just polls state continuously while the user
performs the actions (E-press, Escape) physically. Or **IDA decompile**
of the mainPlayer.input_E native shim.

**F-8.5 — gatherSignal predicate — UNTESTED**

Stage B never reached the gatherSignal direct-invoke. Same fix as F-8.1.

**F-8.7 — setData audio side-effects — UNTESTED**

Stage B never reached the setData test. Same.

**F-8.9 — intComs_anyKey filtering — INFERRED ONLY**

Same as F-8.2/3/4.

### 12.12.3 CRITICAL EMPIRICAL FINDING (NEW)

**E-12-CR1 (NEW)**: Direct UFunction invocation of
`BndEvt__screenbutton_*_OnClickedSignature__DelegateSignature` with
`(self, nil, nil)` parameters CRASHED THE GAME PROCESS. This happened
mid-probe v2 during F-8.10 screenbutton-invoke-test, after only the
first few lines logged. Process gone with no crash log.

**Implication**: re-firing these UFunctions on host from client's
TerminalButtonPress requires THE TERMINAL TO BE IN ACTIVE-USE STATE.
The BP body of the OnClicked handler probably:
1. Reads some state that's only initialized when the player has activated
   the terminal (e.g., `player_using`, `activeInterface`, or terminal
   audio component refs)
2. Calls a method on a null pointer
3. Crashes

**Resolution for Inc4.5**: 
- The host MUST be in active-use state (one local mainPlayer at the
  terminal) when re-firing client's button presses.
- If host is NOT using the terminal, the client's button press should
  be either DROPPED (host has no view of the terminal) or QUEUED
  until host enters the terminal.
- Recommended: drop with a "host not present" reliable response
  packet → client UI shows "host hasn't activated this terminal yet".
- This is a NEW design constraint not covered in section 12.5.

**Added to Inc4.5 design**: prerequisite check — host's
`mainPlayer.activeInterface != nil` AND `activeInterface` matches
the terminal in the TerminalButtonPress. If false, drop + reliable
NACK to peer.

### 12.12.4 Updated escalation flag status

| Flag | Status (post-Inc0 v1+v2) |
|---|---|
| F-8.1 spaceRenderer integrator | UNTESTED — re-run probe |
| F-8.2 input_E dispatch | INFERRED — needs IDA or hands-on |
| F-8.3 player_use per terminal | INFERRED — needs IDA or hands-on |
| F-8.4 Escape clear | INFERRED — needs IDA or hands-on |
| F-8.5 gatherSignal predicate | UNTESTED — re-run probe |
| F-8.6 widget scoping | RESOLVED (lazily-created per-actor) |
| F-8.7 setData audio | UNTESTED — re-run probe |
| F-8.8 rootConsole vs consoles[] | RESOLVED (rootConsole = level-design; consoles[] = dynamic) |
| F-8.9 intComs_anyKey filtering | INFERRED — needs hands-on |
| F-8.10 OnClicked vs player_use | **RESOLVED** (7 OnClicked + 13 BeginOverlap on analogDScreenTest_C; A1a/A1b split confirmed) |
| F-8.11 playerHandMouseWheel target | **RESOLVED** (correct hook, gated on scroll_* flag) |
| F-8.12 drive/floppy data field | RESOLVED (per-instance UPROPERTY) |
| F-8.13 drivePort overlap | RESOLVED (overlap-install semantics confirmed via .drive ref enumeration) |
| F-8.14 enum_physicalModules names | UNRESOLVED — needs IDA pass |
| F-8.15 panel_radar Widget | RESOLVED (lazily-populated; supports typed commands via Uui_console_C) |
| F-8.16 delegate subscribers | UNRESOLVED — needs IDA pass |
| **E-12-CR1 NEW** | DOC ADDED to section 12.12.3 — direct OnClicked invocation crashes without active-use state |

### 12.12.5 Decision: ENOUGH RE TO PROCEED to Inc1

7 of 16 flags resolved empirically. 5 flags need hands-on observation
(can be batched: instrument the same v3 probe with state-polling-only
patterns + tell user to do specific actions in-game). 2 flags need IDA
work (F-8.14 enum names + F-8.16 delegate subs).

NONE of the unresolved flags BLOCK Inc1 (terminal-key resolution +
singleton fallback). They start to matter at Inc4/Inc5/Inc6.

**Recommendation**: proceed to Inc1 implementation; revisit F-8.x
escalations as those specific increments are reached. Hands-on
verification can pair with Inc4.5 implementation testing (the user
flagging "click this button" while the probe captures state, a
realistic test pattern).

### 12.12.6 Section-12.12 update log

- 2026-05-25 LATE +5h: empirical Inc0 probe pass complete (v1 crashed
  UE4SS via RegisterHook on BP funcs; v2 hookless succeeded on 7 of
  16 flags before another crash during direct OnClicked invoke). 7
  flags resolved, 9 deferred (5 to hands-on, 2 to IDA, 2 ready for
  v3 probe rerun). 1 critical new finding (E-12-CR1: direct OnClicked
  invocation crashes without active-use state → Inc4.5 needs host-
  active-state precheck before re-fire).
