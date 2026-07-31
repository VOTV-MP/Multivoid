# Input ownership: keys, binds and the cursor — MEASURED fact base (2026-07-31)

The trigger is **GitHub issue #5, "Unable to sv.request"** (filed by a real player, not by us):

> "Due to the T button being bound to the chat button with no way of seemingly changing it,
> I am incapable of completing the daily task."

plus the user's own 2026-07-27 report *"clicking multiplayer shows multiplayer pop up but no
CURSOR showing"* (reproduced 2026-07-28, root NOT found until today).

> **STATUS BOX, updated 2026-07-31 evening.** Two halves, very different states:
> * **The CURSOR bug is FIXED** (`b8c34198`) and **§7 of this document was WRONG** until that
>   commit — the root is `ScaleAllSizes` truncating `MouseCursorScale` to 0, not cursor
>   ownership. §7 is rewritten; do not read any older copy of it.
> * **The KEY-BINDING half is DESIGNED, NOT BUILT.** Design of record:
>   `votv-input-bindings-cursor-DESIGN-2026-07-31.md` (20-round `/qf`).
> * **GATE 0 RAN (2026-07-31, the user, at the SAT console) and `f03c04f0` FAILED it.** Their
>   words: *"I'm in the IN-GAME server console - I type sv.request - upon hitting V - voice
>   settings imgui window pops up, then I press V again to close it, same with T."*
>   **The root is now MEASURED and it is not what §8/M1 implied — see §4b, which supersedes
>   the "so the thing we must detect is Slate keyboard focus" conclusion at the end of §4.**
>   The game consumes a typed key iff `IsValid(mainPlayer.activeInterface)`, and delivers it
>   through a **virtual user**, so *every* user-0 focus accessor is structurally blind to the
>   in-world screens. Fixed by deleting the focus term, not by adding accessors.

Everything below is `measured` unless tagged otherwise. Sources: the CXXHeaderDump
(`Game_0.9.0n_HOST/.../CXXHeaderDump/`, 2645 classes), the BP bytecode dumps
(`research/bp_reflection/`), the cooked paks, and a live instrumented run
(`tools/cursor_probe.py` + `VOTVCOOP_CURSOR_PROBE=1`).

---

## 1. What our overlay takes from the keyboard today

`src/votv-coop/src/ui/imgui_overlay.cpp` `WndProcDetour`:

| key | line | condition | swallowed? |
|---|---|---|---|
| `VK_F1` | :159 | **none** | always |
| `VK_OEM_3` (tilde) down+up | :175/:184 | **none** | always |
| `'T'` | :192 | `!CaptureActive() && !PauseMenuOpen() && chat_sync::SessionActive()` | on open |
| `'V'` | :204 | `!CaptureActive() && voice_chat::Enabled()` | on toggle |

`CaptureActive()` (:130-139) enumerates **our own** surfaces only. There is no term in it,
and no term anywhere in the mod, that asks whether **the GAME** currently owns the keyboard.
`ui/input_focus.h` is close in name but is strictly about (a) our own process being foreground
and (b) *our* ImGui text fields — `SetOverlayCapturingText(io.WantTextInput || chat open)`.

## 2. We are stealing two of VOTV's OWN default binds

From the cooked `VotV/Config/DefaultInput.ini` (there is no `VotV/Config/` on disk; extracted
from the pak):

| our key | VOTV's default binding | verdict |
|---|---|---|
| **F1** | `lockmouse` — "Toggle mouse lock" (`DefaultInput.ini:134`) | **collision** |
| **V** | `noclip` (`:137`) | **collision** |
| tilde | `ConsoleKeys=Tilde` + `ConsoleKeys=ё` (`:167-168`) — the UE4 console, not an ActionMapping | engine-level |
| **T** | no ActionMapping, no `list_keybinds` row, no BP `InputKeyEvent` | **genuinely free** |

So the reporter's problem is NOT that T is a bad choice of key. **T is the one key we took that
was actually free.** The problem is that we take it *unconditionally with respect to the game's
own text entry*.

Free by default: F2 F3 F4 F7 F8 F9 F12 G H J K L B N M Y U I O P.
Reserved: F5 quicksave, F6 backupsave, F11 + Alt+Enter engine fullscreen, NumPad0 cheatmenu,
NumPad-Decimal debugtp, Escape `pause` (**not rebindable** — absent from `list_keybinds`).

## 3. The game's text-entry surfaces: 26 classes, 73 fields

`pcui_file`, `ui_bufferDatablock`, `ui_cheatMenu`, `ui_colorPicker`, **`ui_console`**,
`ui_floppyDatablock`, `ui_help`, `ui_laptop`, **`ui_notebook`**, `ui_playerInventory`,
`ui_printer`, `ui_radioInterface`, `ui_resetSave`, `ui_saveSlots`, `ui_settings`,
`ui_signalName`, `ui_spawnmenu`, `ui_toolParameter`, `ui_tvInterface`, `ui_vcam`,
`uicomp_camSlot`, `uicomp_settingsSlot`, `uicomp_settingsSlot_screenResolution`,
`uicomp_signalSlot`, `uicomp_videoSlotEncrypted`, `uiwindow_craftingBook`.

Breakdown: `UEditableTextBox` x66, `UMultiLineEditableText` x5 (all `ui_notebook`),
`UMultiLineEditableTextBox` x2 (both `ui_saveSlots`), zero bare `UEditableText`.
The user named two of these unprompted ("server consoles", "typing in notepad in game") —
they are `ui_console` and `ui_notebook`. **A per-surface filter would be a 26-row site list.**

## 4. How typed characters actually reach the console — and it is NOT BP key capture

`Uui_console_C` types through **Slate keyboard focus on `UEditableTextBox* EditableTextBox`
(0x0268)**. The blueprint's own key-capture path is present in the asset but **DEAD**:

- `text_command` (the TextBlock the BP path renders into) is `ESlateVisibility::Collapsed`
  in the widget tree; `upd()` writes a fake caret `Command + "|"` into a collapsed widget.
- `EditableTextBox.SetFocus()` is really called on entry: ubergraph `@4137`, reached from
  `adrwewr()` -> `Enter()`, called by the owner `panel_SATconsole` ubergraph [5].
- The command string comes from the **commit delegate**, not keystrokes:
  `OnEditableTextBoxCommitted` -> `@10384 Command := Conv_TextToString(Text)` ->
  `@10497 enterCommand(Command)`.
- `OnKeyDown_0` / `typeSymbol` have **zero callers**: an exact FName scan over all **3050**
  cooked `.uasset` finds `OnKeyDown_0` 0 times and `typeSymbol` only in `ui_console.uasset`'s
  own declaration.
- Both key handlers are gated on `Active` (0x0360), which has **no writer anywhere in the
  dumped corpus** and is absent from the CDO (=> false). [`inferred` that no undumped class
  writes it; cheap settle: read the live instance's byte while the console is open.]

`sv.request` is confirmed a real command: `enterCommand` [54]
`NotEqual_StriStri(inputCommand, 'sv.request')`.

`Uui_notebook_C` is the same shape — Slate focus on `UMultiLineEditableText`, `OnKeyDown`
handles only Escape. It never calls `SetFocus`, so the player must click into it.

~~**Consequence: the thing we must detect is Slate keyboard focus, not a game flag**~~ -- **THIS
CONCLUSION IS WRONG AND WAS THE BUG. SUPERSEDED BY §4b (2026-07-31 evening).** It is right about
how the *characters* reach the text box and wrong about what *we* can observe, because it never
asked which Slate USER holds that focus.

## 4b. What actually decides it: the game's own guard, and a virtual user

Decoded from `mainPlayer`'s ubergraph (`research/bp_reflection`, `kdec.py`) -- the block that runs
for every key, at the `AnyKey` entry points:

```
CallFunc_IsValid_ReturnValue_99 = IsValid(activeInterface)
POP_FLOW_IF_NOT(CallFunc_IsValid_ReturnValue_99)        <-- the guard
WidgetInteraction.SetFocus(activeInterface)
WidgetInteraction.PressPointerKey[virt](key)
CallFunc_PressKey_ReturnValue = WidgetInteraction.PressKey[virt](key, false)
```

Three facts follow, and together they are the whole answer to issue #5:

1. **The game consumes a typed key IFF `IsValid(mainPlayer.activeInterface)`.** One pointer read,
   on a field we already resolve. It is the game's own guard, not our model of one.
2. **Delivery is through a VIRTUAL USER.** `UWidgetInteractionComponent` carries its own
   `VirtualUserIndex` (`UMG.hpp:2031`). `UWidget::HasKeyboardFocus()` asks about **user 0**, so it
   is structurally blind to this path -- and the in-world screens (SAT console, laptop, arcade, TV,
   radar, portable PC) are UMG hosted in a `UWidgetComponent` and driven exactly this way.
   `panel_SATconsole` itself owns no `UWidgetComponent` (`panel_SATconsole.hpp:7-16`: its `Widget`
   is a bare `Uui_console_C*`); the screens are rendered by `ticker_widgetRender` /
   `analogDScreenTest`, which do.
3. **Why §8/M1 looked like it worked.** M1 measured `ui_playerInventory_C`, and
   `setActiveInterface` *also* calls `SetInputMode_GameAndUIEx(PC, activeInterface)`, which focuses
   the same widget for **user 0**. So the one surface that was measured is the one surface where
   the wrong predicate answers correctly. Two delivery paths; only one was ever modelled.

A *second*, independent defect lives in the same accessor and is worth keeping straight from the
above: `HasKeyboardFocus()` is an **exact-widget** test, so even on the user-0 path it reads true
only while focus sits on the user widget itself, and goes **false the moment a field inside it is
clicked** -- i.e. it fails exactly when typing is happening. That is why the 1 Hz backstop scan now
ORs `HasFocusedDescendants()`. It is real, and it is *not* what issue #5 was.

## 5. The single game-side "a UI owns input" state — and we already resolve it

**`AmainPlayer_C::activeInterface`, `class UWidget*` @ 0x07E0** (`mainPlayer.hpp:103`).

Sole write site in the class: `setActiveInterface` stmt [35]. That function also does
`[43] newWidget->bIsFocusable = true`, `[48] SetInputMode_GameAndUIEx(PC, activeInterface, ..)`
or `[62] SetInputMode_UIOnlyEx(...)`, `[50] PC->bShowMouseCursor = true`; the null path does
`[65] SetInputMode_GameOnly(PC)` + `[67] bShowMouseCursor = false`.

**Our codebase already has this offset**: `src/votv-coop/src/ue_wrap/core/reflected_offset.cpp:102`
— `VC_DEFINE_OFFSET(MainPlayer_activeInterface, L"mainPlayer_C", L"activeInterface")`, read at
`src/votv-coop/src/ue_wrap/desk/device_screen.cpp:224-230`. It is described there as "THE
inside-a-device discriminator (v63 occupancy)".

Public entry `Enter Interface(UWidget*, ...)` -> `setActiveInterface`. Callers of
`setActiveInterface`: `atm`, `laptop`, `mainGamemode`, `mainPlayer`, `ui_laptop`, `ui_notebook`,
`ui_playerInventory`. Callers of `Enter Interface`: `analogDScreenTest`, `laptop`, `mainPlayer`,
`panel_SATconsole`, `panel_radar`, `panel_reactor`, `prop_arcade`, `prop_portablePc`,
`transformerMGPanel`.

Caveats that matter for the design:
- `activeInterface != null` means **a UI owns input**, which is broader than "a text field is
  focused" (e.g. `panel_radar` is an interface with no text field).
- `SetInputMode*` store an `FInputModeDataBase` that is **not a UPROPERTY** — not readable.
- VOTV has **no Blueprint `APlayerController` subclass**; it uses the stock engine class.
- ~~The finer predicate IS reachable via `UWidget::HasKeyboardFocus()` etc.~~ **MEASURED FALSE
  2026-07-31 -- see §8/M1.** Those accessors exist (`UMG.hpp:1798-1804`) and are callable, but on a
  `UEditableTextBox` every one of them reads false even with the field on screen and focused,
  because UMG tests the cached `SObjectWidget` wrapper rather than the inner `SEditableTextBox`.
  They read TRUE on the owning USER WIDGET. The per-field predicate does not exist.

## 6. VOTV has its own rebinding system, and it is readable at runtime

- UI `Uui_keybinds_C` (from `ui_settings.button_binds`), row widget `Uuicomp_keybindSlot_C`.
- Struct `Fstruct_keybind`: `FText displayName` @0x00, **`FKey key` @0x18**, category @0x30.
- Storage: defaults in DataTable `/Game/main/datatables/list_keybinds`; the user's bindings in
  **`Usave_main_C::keybinds` `TArray<Fstruct_keybind>` @0x0468** with parallel
  `keybindsNames` `TArray<FName>` @0x0488; mirrored into `UInputSettings` and flushed by
  `save_main::saveKeybinds()` -> `SaveKeyMappings` -> `%LOCALAPPDATA%/VotV/Saved/Config/
  WindowsNoEditor/Input.ini`.
- Read path (this is exactly what `lib_C::getKeybindFromName(FName, WCO, FKey&)` does, 3 stmts):
  `mainGamemode.save_main (0x0508) -> keybindsNames (0x0488) Array_Find(name) = i ->
  keybinds (0x0468)[i].key (+0x18)`.
- **Hazard**: `saveKeybinds()` does `RemoveActionMapping` + `AddActionMapping` +
  `ForceRebuildKeymaps` + `SaveKeyMappings` on the global `UInputSettings` at every gamemode
  boot and every applied bind — so any ActionMapping a mod injects can be clobbered **or leak
  into the player's `Input.ini`**. (The removal predicate inside `saveKeybinds` is
  `inferred`; `save_main` is not among the dumped assets.)
- `GetAllActionMappings` does not exist anywhere in the pak (0 hits over 8.17 GB).

## 7. The cursor bug — ROOT FOUND 2026-07-31, and it is NOT what this section said

**SUPERSEDED IN FULL.** Everything this section previously concluded — "the root is cursor
OWNERSHIP, not cursor drawing", "the OS pointer does not move", "the arrow drew off-viewport" —
is RETRACTED by measurement. The real root, fixed in commit `b8c34198`:

```
imgui.cpp:1649  ImGuiStyle::ScaleAllSizes():  MouseCursorScale = ImTrunc(MouseCursorScale * f);
imgui_overlay                                 st.ScaleAllSizes(ui::scale::Ui());   f = 0.833
                                              ImTrunc(1.0f * 0.833f) == 0.0f
imgui.cpp:4131  RenderMouseCursor():          AddImage(tex, pos, pos + size * scale, ...)
                                              scale == 0  ->  a ZERO-AREA quad. Nothing drawn.
```

Every other style field is a **pixel size**, where truncating toward zero is correct.
`MouseCursorScale` is a unitless **multiplier**, and truncating a multiplier below 1 to 0 deletes
the thing it scales. Present since **v1.91.5** (`git log -S"MouseCursorScale = ImTrunc"` ->
`f401021d5 Version 1.91.5`), so NOT a regression from the 1.92.9 upgrade. With the OS cursor
hidden by our own `WM_SETCURSOR` handler (measured: `cursorInfo showing=0 hCursor=NULL`), a
zero-size software cursor means **no cursor at all** — the user's exact words, *"clicking
multiplayer shows multiplayer pop up but no CURSOR showing"*.

It reads as intermittent only because the factor tracks client size: at `Ui() >= 1.0` the
truncation is harmless, below 1.0 the cursor vanishes.

**Evidence — RED then GREEN at the SAME factor, plus a real hands-on:**

```
drill (VOTVCOOP_CURSOR_SCALE_DRILL=1):
  [ERROR] MouseCursorScale=0.000 at uiScale=0.833          user, live: "cursor is invisible"
fixed:
  style rebuilt (uiScale=1.250, MouseCursorScale=1.000)    <- built-in control: benign at >=1.0
  style rebuilt (uiScale=0.833, MouseCursorScale=1.000)    user, live: "cursor was visible"
  19 probe samples, curScale=1.000 throughout
```

`VOTVCOOP_CURSOR_SCALE_DRILL=1` exists so the guard can be SHOWN failing — a guard never
observed failing passes by construction.

### Why the earlier conclusion was wrong — three separate errors, all mine

1. **The probe runs were not foreground.** The lines this section quoted as "the pointer is
   frozen" read `posValid=0`, `imguiPos=(-3.4e38,-3.4e38)` and **`fg != hwnd`**. ImGui's Win32
   backend only updates `io.MousePos` while the window is foreground, so `MousePos` was
   `-FLT_MAX` and `overlaps=0` followed mechanically. That is `docs/LESSONS.md`'s "AN INSTRUMENT
   BLIND TO THE PHENOMENON ALWAYS REPORTS NOT PRESENT" — written 2026-07-28 while chasing this
   very bug, and then walked into by the instrument built to chase it. Once `fg == hwnd`:
   `posValid=1`, `overlaps=1`, and the pointer tracks the mouse normally.
2. **"The arrow drew off-viewport" was an arithmetic error.** `imgui_draw.cpp:2650` (pinned
   v1.92.9) gives `ImGuiMouseCursor_Arrow` an offset of `(0,0)`; the write-up used `(0,24)`,
   mixing screen and client coordinates. With the real offset the four `ImRect::Overlaps`
   inequalities all pass.
3. **`osScreen=(0,24)` was read as a `ClipCursor` clamp signature.** It is equally the CLIENT
   ORIGIN (`cliOrg=(0,24)` on that run) — one sample cannot separate the two, and no run ever
   followed the user's actual sequence.

### What DOES survive from the original investigation

- VOTV calls `SetCursorPos` **~120x/s** at the window centre and every one is no-oped by
  `SetCursorPosDetour` while a surface is up — **including with no world** (the probe runs at the
  main-menu server browser).
- **M5, 123 samples: the camera does NOT spin.** `ControlRotation` yaw held `-61.13` under ~118
  suppressed recentres/second. If mouselook were `SetCursorPos(centre)` -> `GetCursorPos()` ->
  delta, a parked pointer under suppressed writes would give a constant non-zero delta and the
  camera would drift. It does not — so mouselook rides `WM_MOUSEMOVE`/raw input (both swallowed),
  the ~120 Hz write is a viewport **lock**, and **our own write cannot inject camera motion**.
  This is what licenses the transition in §7b.
- The M2 negative control (the freeze reproduces with the probe's own write removed) was
  methodologically right; it just controlled for the wrong hypothesis.

## 7b. The SECOND cursor symptom — ownership transition, adopted from MTA

Reported by the user mid-session 2026-07-31, after the scale fix was live: *"when I closed the
multiplayer screen and went into ESC the cursor was acting weird and not respecting my mouse
movements."* Different symptom, different root, also shipped in `b8c34198`.

MTA's `CLocalGUI::Draw` (`reference/mtasa-blue/Client/core/CGUI.cpp:800-848`) runs a per-frame
transition we were running only the middle of:

```
ENTER GUI mode:  SetCursorPos(m_StoredMousePosition);   // restore where the player left it
                 DisableSetCursorPos();                  // then suppress the game
LEAVE GUI mode:  GetCursorPos(&p); m_StoredMousePosition = p;
                 SetCursorPos(width/2, height/2);        // MTA's own comment:
                 EnableSetCursorPos();                   //   "to prevent the game from
                 pGUI->ClearSystemKeys();                //    reacting to its movement"
```

The **exit recentre** is what answers the report: leaving the pointer parked means the game
resumes mouselook with the whole accumulated offset. Our design had explicitly said *"on release,
do nothing — the game self-heals"*, which MTA's comment contradicts by name.

`ui/overlay_cursor.cpp` is that transition, with two divergences commented at their sites: the
enter-restore is gated on a validity latch (MTA restores from a zeroed `POINT`, which would park
the first surface's cursor in the screen corner — the exact artefact two probe runs mistook for a
root cause), and the exit recentres the **client rect** rather than the screen, because this game
runs windowed. The restore is write-then-verify, because `SetCursorPos` is silently clamped by
whatever `ClipCursor` rect is live and we do not own that rect.

**Status: BUILT + smoke PASS + the user's own hands-on for the SCALE half. The TRANSITION half
(§7b) is BUILT and smoke-clean but has NOT been hands-on-confirmed** — the user's ESC report is
what it was written against, and nobody has re-run that sequence since.

## 8. THE INSTRUMENTED RUN (M1-M5), 2026-07-31 -- what it settled

`tools/input_probe.py` + `coop/dev/input_focus_probe.cpp` (`VOTVCOOP_INPUT_PROBE=1`), one host
peer on s_1234, F1 menu forced open, then F1 to close it, TAB to open the inventory, T pressed.
Four iterations; the first three failures were all INSTRUMENT bugs caught by their own controls,
which is recorded here because each is a repeat of a named project lesson.

### M1 -- the focus predicate. ANSWERED, and it kills the fine-grained version.

```
FOCUS ACCESSORS after SetKeyboardFocus --
    target{kb=0 desc=0 anyUser=0}   iface{kb=1 desc=0 anyUser=1}
```
target = `textbox_search` inside the ON-SCREEN `ui_playerInventory_C`; iface = that user widget.

- **`UWidget::HasKeyboardFocus()` on a `UEditableTextBox` is ALWAYS false**, even with the field
  on screen and after calling the engine's own `SetKeyboardFocus()` on it. UMG wraps the field in
  an `SObjectWidget` around the real `SEditableTextBox`, and the accessor tests the wrapper
  exactly. `HasFocusedDescendants()` and `HasAnyUserFocus()` on the field are false too.
- **The owning USER WIDGET reports `HasKeyboardFocus()=1` and `HasAnyUserFocus()=1`.**
- => **"is a TEXT FIELD focused" is NOT reachable through UMG reflection.** The per-class scan over
  the three editable widget types -- the invariant this arc was going to be built on -- cannot be
  implemented. What IS readable is "a game UI owns keyboard focus".

### M1b -- `mainPlayer.activeInterface`. MEASURED WORKING.

`activeInterface = 0x0` while playing; `activeInterface = ui_playerInventory_C` the moment TAB
opens the inventory, across every subsequent sample. `hasFocusedDesc=0` throughout (the search box
was never clicked), consistent with M1: descendants are not the accessor that fires.

### M2 -- the negative control. THE FREEZE IS REAL.

With the probe's own write test REMOVED (`writeTest armed=0`): `osScreen=(0,24)` unchanged across
every sample, `after=(0,24)`, `msgs mm=1`, while `scpCalls` climbs 574 -> 810 -> 1046 (~118/s) with
`scpLast=(427,344)`. So the frozen pointer is not an artefact of the instrument that found it --
round 4's concession is discharged.

### M3 -- DPI. THE CROSS-PROCESS ANOMALY WAS AN ARTEFACT.

`dpiAware=0` (`DPI_AWARENESS_UNAWARE`) for the game process. PowerShell is per-monitor aware, so
its `GetCursorInfo` and the game's `GetCursorPos` were never in the same coordinate space and the
"(426,344) vs (0,24) at the same instant" disagreement is not evidence of anything. **Retracted.**

### M4 -- keydown vs char. THE MECHANISM IS CONFIRMED, AND IT IS BOTH.

```
KEYDOWN 0x54 ('T') -> SWALLOWED by the T-chat hotkey   [capture=0 chat=0]
KEYUP   0x54 ('T') -> SWALLOWED by CaptureActive       [capture=1 chat=1]
CHAR    0x435      -> SWALLOWED by CaptureActive       [capture=1 chat=1]
```
`TranslateMessage` queues the `WM_CHAR` before `DispatchMessage` reaches us, so the keydown swallow
cannot stop the char existing -- but by the time it arrives `chat_input::Open()` has made
`CaptureActive()` true and the char is swallowed too. **The character reaches nobody but ImGui.**

Two further findings from the same lines:
- **`wParam=0x435` is U+0435, Cyrillic `е`.** The WM_CHAR carries the LAYOUT-translated character
  while our hotkey matches the VK, so **the swallow is layout-blind**: on a Russian layout the key
  that opens chat is the key that types `е`. Same shape as `ConsoleKeys=Tilde`/`ё` on VK_OEM_3.
- **`KEYUP 0x70 (F1) -> passed to the GAME`.** We swallow F1's press but hand the game an unpaired
  release (the deliberate release-feeding rule at `:231`), so VOTV's `lockmouse` sees a release
  with no press.
- TAB pressed while our menu was open logged `SWALLOWED by CaptureActive` -- a live demonstration
  of the `LESSONS.md` lesson "A readiness ANNOUNCEMENT is not evidence of the VISIBLE state it
  precedes" (our non-text surfaces eat the game's keys). Cited by TITLE: `:794-798` is a
  DIFFERENT lesson (the roster screenshot one).

### M5 -- does the camera spin? NO.

`ControlRotation` yaw held `-61.13` across 123 samples with capture ON and ~118 suppressed
recentres/second. Mouselook is therefore NOT poll-from-centre; a frozen pointer produces no
rotation. The round-6 worry is closed, and `WM_MOUSEMOVE` swallowing at `:245ff` is what covers
the case where the pointer does move.

### The four instrument bugs, each a repeat of a named lesson

1. The round trip targeted `/Script/UMG/Default__EditableTextBox` -- **a class default object**.
   The walk never skipped CDOs. Reported "PREDICATE DEAD".
2. Filtering only the IMMEDIATE outer removed 3 of 400: a widget-tree TEMPLATE's immediate outer is
   a `UWidgetTree` just like a live instance's. The discriminator is one level further up.
3. Targeting the first live instance in object order picked `pcui_file`, then `ui_resetSave` --
   widgets that exist but are not in the viewport, where `SetKeyboardFocus` is a no-op.
4. `ProbeKeyMsg` fired on `WM_MOUSEMOVE`, because the swallow switch shares ONE body across the
   mouse and key cases.

Every one was caught by a control the probe printed about ITSELF (what it targeted, how many
objects it kept, which message kind it saw). A probe that had only printed `focused=0` would have
shipped "the predicate does not work" three times over.

### Still UNDETERMINED about the cursor
- Why run 3's pointer was parked at exactly client `(0,0)` rather than wherever the user last
  left it. It does not change the root (a stationary pointer under suppressed writes) but the
  corner is suspiciously exact; `ClipCursor` is `(0,24)-(853,664)` = the client rect, so a
  clamp against a stale rect is the leading `inferred` explanation.
- Whether VOTV registers raw input for the mouse. `GetRegisteredRawInputDevices` reports
  **one** registered device in-process and it is not usage page 1 / usage 2, and `WM_INPUT`
  reached our WndProc 0-1 times per run — so mouselook is very likely driven by the
  `SetCursorPos`-recentre-and-measure-delta scheme, not by raw input. Worth confirming before
  any design leans on it.
