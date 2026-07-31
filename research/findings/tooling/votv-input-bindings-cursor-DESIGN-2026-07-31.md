# Input bindings + cursor ownership — DESIGN OF RECORD (2026-07-31)

**Status: DESIGN, converged over a 20-round `/qf` pass. NOT built.** Fact base:
`votv-input-ownership-FACTS-2026-07-31.md` (read it first; §7 is under re-measurement, see
STEP 0 below). Thread transcript: `<scratchpad>/qf_thread.md`.

## 0. The ask, in the user's words

> *"Need to tackle **the cursor bug** + key bindings. There's a lot of places in game that require
> typing, server consoles and might be other game systems (typing in notepad in game) —
> [issue #5]. Take As many qf sessions as needed."*
> — then *"Run until converging"*, *"full autonomy… per rule 1"*, *"Take as many rounds as needed
> for per rule 1 solutions."*

Cursor symptom, verbatim (2026-07-27): *"clicking multiplayer shows multiplayer pop up but no
CURSOR showing."*

GitHub issue #5, filed by a real player (decodinatorX): *"Due to the T button being bound to the
chat button with no way of seemingly changing it, I am incapable of completing the daily task."*
(The daily task requires typing `sv.request` into VOTV's in-game console.)

**Already shipped, `f03c04f0`:** `coop/input/input_owner` — the arbiter with three terms
(`gameOwnsText` / `overlayOwnsText` / `foreground`). That closed the *"I cannot type"* half.
**Whether it closed it for the reporter's actual surface is UNVERIFIED** — see G0.

---

## 1. The six stores that bind a key in VOTV (measured, complete)

| # | store | offset | lifetime | how read |
|---|---|---|---|---|
| 1 | `UInputSettings.ActionMappings` | 0x0080 | player-writable | reflection |
| 2 | `UInputSettings.AxisMappings` | 0x0090 | player-writable | reflection |
| 3 | `UInputSettings.ConsoleKeys` (+ `ConsoleKey` @0x0118) | 0x0130 | player-writable | reflection |
| 4 | `bF11TogglesFullscreen` / `bAltEnterTogglesFullscreen` | 0x0038 bits | player-writable | reflection |
| 5 | `UPlayerInput.DebugExecBindings` | 0x0120 | **runtime-only, never persisted** | reflection |
| 6 | raw BP key events, 5 cooked assets | — | **cook-time, unrebindable** | generated table |

`FInputActionKeyMapping` = `{FName ActionName@0x00, flags@0x08, FKey Key@0x10}`, size 0x28.
`FKey.KeyName` = `FName @0x00` (`InputCore.hpp:6`). `FKeyBind` = `{FKey@0x00, FString
Command@0x18, flags@0x28}`, size 0x30. `UInputSettings` is a CDO —
`reflection::FindClassDefaultObject(L"InputSettings")`.

`UPlayerInput` carries **no** own `ActionMappings` (only `DebugExecBindings` and `InvertedAxis`),
and `ForceRebuildKeymaps()` existing is the engine's own statement that any non-reflected cache is
rebuilt *from* `UInputSettings`. There is no seventh store.

**`save_main.keybinds` is NOT the authority** — it is VOTV's settings-UI mirror of a subset. This
was the round-1 reframe; the design had been built on it for ten rounds of prior thinking.

## 2. The measured facts the design rests on

| id | fact |
|---|---|
| F2 | `ParseKey` (`voice_chat.cpp:71-75`) = `size()==1 ? toupper : strtol` — `"F2"` → **0**, any Cyrillic → **0**, both SILENTLY |
| F4 | cooked exclusion: **F1 collides `lockmouse`, V collides `noclip`, Q collides `spawnmenu`/`down`/`radar_zoomOut`**; `T` free; `Tilde` free of ActionMappings but **is** a ConsoleKey |
| F6 | an incomplete name table **silently reports "free"** — burned this pass three times |
| F7 | probed on this RU box: `VkKeyScanExW('ё',RU)=0xC0`, `('ё',US)=-1`, `` ('`',US)=0xC0 ``, `('t',US)=0x54`, **`('t',RU)=-1`**, `('е',RU)=0x54` |
| F9 | 3050-asset scan: raw BP key events — `mainPlayer`(AnyKey, Escape, LMB, MouseWheelAxis), `car`(Escape), `car1_witch`(LMB, LeftShift), `car2`(Escape, SpaceBar), **`mg_invaders`(Enter, Left, Right, Up)**. `Left`/`Right`/`Up` had been in the "free" set |
| F11 | the live `%LOCALAPPDATA%/VotV/Saved/Config/WindowsNoEditor/Input.ini` has **`ConsoleKeys=F10`**, not `Tilde`; it **replaces** rather than appends; UE **case-normalises** ActionNames on save (`lockmouse`→`LockMouse`); `throwPath` unbound by the player |
| F13 | 42 distinct live key names across stores 1-3: **12 single-char ASCII**, **exactly one non-ASCII** (`ё`), **29 multi-char** |
| F14 | **zero** of 78 `ActionMappings` rows carry any of `bShift/bCtrl/bAlt/bCmd` — the per-VK bitset loses nothing |
| F15 | the arbiter's "fast path" does a `FindObjectByClass` **walk** every 100 ms (`input_owner.cpp:92`); the header's *"a stale predicate can never cost a character"* is **overstated** |
| F16/F18 | VOTV's **console** and the user's **"notepad"** are both `activeInterface`-driven (`panel_SATconsole` and `ui_notebook` are measured callers) — neither is one of the 8 census outliers |
| F17 | `input_owner.h:64`'s *"read from the WndProc + poller threads"* is **false today**: all five `MayTakeKey()` sites are in the WndProc |

## 3. STEP 0 — measurements, on the CURRENT tree, gated behind nothing

`CaptureActive()` is *also* the cursor path's only gate (`SetCursorPosDetour`, `MouseDrawCursor`,
the `SetCursor(nullptr)`, and `CursorFrame`'s own argument), so commit B would move the cursor
arc's ground truth **and its instrument's own input** before it is read. Measure first.

- **G3 — is `WndProcDetour` the game thread?** One `UE_LOGI` behind a once-latch;
  `game_thread::IsGameThread()` exists (`game_thread.h:62`) and nothing calls it there. **If yes,
  focus is evaluated SYNCHRONOUSLY at the keydown** and both staleness windows vanish on the path
  where characters are lost; the published atomic survives only for the poller threads.
- **G3b — re-entrancy.** `HasKeyboardFocus` is a ProcessEvent dispatch and the WndProc also fires
  from modal move/resize loops and `WM_KILLFOCUS`. Drag/resize, alt-tab repeatedly, sustained run.
  Fallback is graceful: the tick-side O(1) refresh still removes the 1 Hz component.
- **G0 — type `sv.request` into VOTV's console and see it EXECUTE.** The acceptance test for the
  already-shipped `f03c04f0` **and for issue #5 itself**. Our own key trace is corroboration, not
  the verdict (a KEYDOWN that passes can still be followed by a CHAR eaten one gate down).
  M1 only ever measured `textbox_search` inside `ui_playerInventory_C`.
- **G0b — close the console, press `T` immediately;** chat must open.
- **C0 — reproduce the USER's cursor sequence.** Pointer onto the Multiplayer button with the real
  `SetCursorPos` **before** capture (setup, not the input under test), click, observe. Per
  `docs/LESSONS.md` "AN INSTRUMENT BLIND TO THE PHENOMENON ALWAYS REPORTS NOT PRESENT": use
  **`GetCursorInfo`** (`flags` separates hidden from re-centred; `ptScreenPos` gives position),
  **assert `GetForegroundWindow() == g_hwnd` in the run**, and carry a **known-positive frame**.
- **C1 — re-run the cursor probe** printing `MousePos`, the box, `vp`, **`FramebufferScale`** and
  **`MouseCursorScale`** (the 0.83 UI scale was in none of the §7 arithmetic); detour `ClipCursor`
  and log its rect + frequency; log the pointer **every frame** across the capture transition.
  Answers: *what re-parks the pointer, how often, and which term of the visibility predicate fires.*
- **measure `mainPlayer`'s `AnyKey` handler** before excluding it.

## 4. STEP 1 — COMMIT B (messages)

`CaptureActive()` is **one name answering three questions**: owns cursor / owns typed text / is
modal. `LoadingOpen()` and the passive `ScoreOpen()` answer *yes* to the first and *no* to the
other two, which is why `T` is swallowed whole mid-join — a defect the shipped code labels STILL
BROKEN at `imgui_overlay.cpp:216-221`, and a principle-8 violation (chat unreachable mid-join).

- Split the three. Surface state reachable **only** through a table with modality as a required
  column; `AnyOpen()`/`CaptureActive()` iterate the table, so a surface absent from it cannot be
  queried.
- **Enforcement is a gate script, not the schema.** A required column enforces *rows*, not
  *readers*. `tools/input/wndproc_gate.ps1`, by **operation kind** (the `peerconn_gate.ps1` /
  `nick_gate.ps1` / `registry_gate.ps1` shape): a `Surfaces::IsOpen` is a **state read**, not
  asking permission — the gate requires a **decision call**. Its subject is every site that
  **acts**: returns early **or** mutates a surface (including the ESC path that mutates and falls
  through, and the unconditional `WM_CHAR` feed). **Shown RED against the current tree first** —
  it must flag the ungated voice-close at `imgui_overlay.cpp:241`.
- **B must be behaviour-preserving for the cursor path** — the *owns-cursor* term must be exactly
  today's `CaptureActive()` at all four cursor sites, proven with the body-diff + mutate recipe, or
  C1 re-runs.
- Fix three stale `docs/LESSONS.md:794-798` citations (two of them in shipped code) — that range is
  the roster-screenshot lesson; the CaptureActive one is ~800-814. **Cite by title, not by line.**
- Delete the false comment at `imgui_overlay.cpp:196` (*"Tilde is free in VOTV"* — the cooked
  default is `ConsoleKeys=Tilde`).

**Not in commit B:** a `WM_CHAR` latch on the last keydown's verdict. It was designed and dropped —
walking the measured trace, the CHAR path behaved correctly, and for a key taken without opening a
capturing surface `MayTakeKey` already guarantees nothing has focus. No message it changes could be
named, so adding it would be a second compensation layer.

## 5. STEP 2 — COMMIT C (keys)

**The invariant: an explicit player choice beats a default; between two defaults, the GAME's wins.**
Not a veto — vetoing an explicit `keys.chat = F1` would *be* the reporter's sentence reinstated.
It governs **ambient acquisition only**: a mode the player explicitly entered (freecam, our capture
surfaces, the rebind-capture panel) owns its keys by the same rule.

- **D1 — two resolution rules, not one table.** *Our* config values: a **positional** UE-name↔VK
  table; ASCII letters/digits resolve positionally and **never** through `VkKeyScanW` (F7 —
  otherwise `keys.chat = T` resolves to nothing on a Russian layout). *The game's* entries: table
  first, then `VkKeyScanExW` on the current layout for character names, where `-1` is a legitimate
  answer ("not on this keyboard"). **Assertion:** the table covers all `A-Z`/`0-9` and the 29
  measured multi-char names; a single-char **ASCII** name that misses it is a table **bug** (loud),
  never a layout question. An unknown game name is **loud + treated as bound**.
- **D2 — every key the mod takes is a registry row** (`keys.chat/menu/playerlist/voice_panel/
  spawnmenu/screenshot`, plus the three voice rows re-pointed at the new parser). Freecam's WASD
  stay unbound-by-design as a mode's keys, stated.
- **`MayTakeKey(vk)` = `foreground && everTicked && !gameOwnsText && !overlayOwnsText &&
  !gameBinds(vk)`.** The overlay-text term moves **inside** (leaving it beside as a second
  predicate every caller must AND would re-form the same fusion defect one level up).
  `AnyModalOpen()` stays orthogonal. `OverlayOwnsText()` retires as a public accessor unless the
  Keys panel displays it. This replaces `IsOurWindowForeground() && !IsOverlayCapturingText()` at
  all 14 sites, paying the RULE-2 debt `input_owner.h:79-84` names.
- **Provenance is applied at the 1 Hz RESOLVE, not at press time.** Explicit-vs-default comes from
  the registry, the bitset answers only bound-vs-free, and the two combine **once** into a resolved
  VK (0 = yielded). Press time is one comparison.
- **The bitset**: 256 bits, built from stores 1-5 plus store 6's generated table, rebuilt **every**
  full tick (the layout is an *additional* invalidation input, not the only one), names compared
  **case-insensitively** (F11). Guarded by a **`valid` latch** — a zero-initialised bitset is
  indistinguishable from a measured-empty one, which is F6 promoted to a race.
- **One fail rule at every level: unknown ⇒ do NOT take the key**, matching the contract already
  written at `input_owner.h:24-30`. Scope: **unknown, not universal** — `mainPlayer`'s `AnyKey` is
  a *known* universal consumer with a stated exclusion and its own reason, and the CI gate cannot
  see an `AnyKey` collision by construction.
- **Edges:** down edges gate; the **UP** edge of a key we already took is always honoured (mirrors
  the ImGui release-pairing invariant at `imgui_overlay.cpp:274-289`).
- **The O(1) synchronous predicate:**
  `gameOwnsText(now) = fastPath() || (remembered && IsLiveByIndex(remembered, idx) &&
  HasKeyboardFocus(remembered))`. Conditional on first caching `{pawn, InternalIndexOf(pawn)}`
  (`reflection.h:84/90`) — the shipped fast path opens with a `FindObjectByClass` **walk** (F15).
  The 1 Hz GUObjectArray walk is then needed only to *discover* a new outlier owner, so the
  stale-TRUE component vanishes.
- **Defaults move: menu F1→F2, playerlist Tilde→F3, voice panel V→F4; chat stays T.** All three
  free in both mechanisms, contiguous, and **F-keys can never be typed into a text field** — which
  is the issue-#5 class. (They collide as easily as any key; that is the runtime term's job, not
  the default's.) Asked in plain text at round 5; answered by two blanket autonomy grants. It is a
  one-line default in the registry row and stays trivially flippable.
- **Failure is LOUD, never silent** (as first written, this design rebuilt issue #5 in mirror): an
  unparseable value is a config error in the log **and** the config-review panel; a **yield emits a
  local chat/feed line at the moment it happens** (the feed renders with no key, so it survives the
  very yield it reports — the boot-time surfaces cannot cover a mid-session rebind).
- **Keypress-capture rebinding is IN this commit,** not deferred. A GitHub reporter who never opens
  `multivoid.ini` gains nothing from a read-only panel. Capture is a **mode**; binding `Q` is
  **allowed** and annotated *"VOTV binds this to spawnmenu / down / radar_zoomOut"* — refusing
  there would resurrect the veto.
- **The CI gate proves exactly one thing: a fresh install has no collision.** It reads the cooked
  `DefaultInput.ini` **and** the generated BP-key table. It may claim nothing about a live machine
  (F11). On the current tree it must go RED in **four** places — F1, V, Q **and the scoreboard**
  (fresh installs have `ConsoleKeys=Tilde`).
- **Store 6's table carries a cook-identity stamp** checked at boot against `kGameTarget`; a
  mismatch means unknown ⇒ do not take.
- **G1 — six negative controls, each shown RED before the term is trusted.**
  `VOTVCOOP_KEYBIND_PROBE_SKIP=<store>` for stores 1-5 (skip ConsoleKeys → `F10` flips free; skip
  `DebugExecBindings` → a `bind`-created key flips free; skip the bools → `F11` flips free), plus
  store 6's control one level up in the **generator's drill** (regenerate excluding `mg_invaders` →
  `Left`/`Right`/`Up` flip free).

**Keys at acquisition, messages at swallow — that is the commit boundary**, not packaging. The
measured loss (`CHAR 0x435 SWALLOWED by CaptureActive`) lives entirely on the message side, which
no bitset or registry reaches.

## 6. STEP 3 — COMMIT A (cursor), shaped by C0/C1

**Ownership model:** while captured the **overlay** owns the pointer position; otherwise the game
does. The existing ~120 Hz suppression **is** how ownership transfers — it is not a compensation,
and it is load-bearing (without it the pointer is pinned to the centre and cannot track the mouse).

**Candidate fix, conditional on C1:** the invariant *while captured, the pointer must be somewhere
the cursor is visible*. Per frame at `CursorFrame`'s existing position (immediately before
`Render()`), check the measured failure and re-seed **only then** — a per-frame *check* with a rare
write. It never fights legitimate motion, a mid-capture re-park heals on the next frame, and
**there is no edge for anyone to own** (`CaptureActive()` polled on the render thread is not
reliably an edge when two surfaces hand off within a frame). Seed target computed from the
**window** (`cOrg + cr/2`) — no history, no init hole. Write-then-verify, mismatch logged.
**C2: it ships in SHADOW first** — count would-fire frames, write nothing.

**If C1 shows `ClipCursor` is the actor, the fix is at the clip and NO re-seed is written.**

### What is honestly NOT established about the cursor

- **The stated root explains the PROBE's frozen run, not the USER's symptom.** `mm=1` says the
  mouse may never have entered the bat-launched window; the user's pointer was demonstrably on the
  Multiplayer button one frame before the popup, so "it stays where it was left" predicts it stays
  *visible*. Something re-parks it, and that mechanism is unrooted. **No run has ever followed the
  user's sequence.**
- **§7's "the arrow drew off-viewport" rests on an arithmetic error of mine.** Pinned v1.92.9
  `imgui_draw.cpp:2650` gives `ImGuiMouseCursor_Arrow` offset `(0,0)`; with it, box `(0,0)-(14,21)`
  vs viewport `(0,0)-(853,640)` passes all four `ImRect::Overlaps` inequalities and should print
  `overlaps=1`. It printed **0**. The raw line is unrecoverable (later smokes overwrote the log).
- `clip=(0,24)-(853,664)` **is** measured, but the pointer at screen `(0,24)` is equally *the
  clip's corner* and *client `(0,0)`* (`cliOrg=(0,24)`). One sample cannot separate clamp from
  parked.
- Whether VOTV registers raw input for the mouse is **inferred absent** (one registered device,
  not usage page 1 / usage 2; `WM_INPUT` 0-1 times per run), corroborated by M5.

### What IS established

Drawing is healthy every run (`draw=1 texOk=1 atlasFlags=0`). VOTV writes `SetCursorPos` ~120×/s at
the window centre and every one is no-oped while a surface is up — **including with no world**
(the probe ran at the main-menu server browser). **M5, 123 samples: the camera does NOT spin** —
yaw held `-61.13` under ~118 suppressed recentres/second, so mouselook rides `WM_MOUSEMOVE`/raw
input and **our own write cannot inject camera motion**. The M2 negative control passed (the freeze
reproduces with the probe's own write removed).

## 7. Residuals

- `UPlayerInput` is per-instance; on a coop peer there may be more than one, and which instance's
  `DebugExecBindings` is authoritative is undecided.
- UE's own `ё`-to-VK resolution is inferred. It governs exactly one name in the live set, and its
  failure mode is a *missed warning*, not a stolen key.
- Commit C is large (parser + table + generator + rows + arbiter term + 14 call sites + capture
  panel + CI gate + probe + the O(1) predicate) and is not yet split.
- `imgui_overlay.cpp`'s `release` feed sends `WM_CHAR` to ImGui unconditionally while the shipped
  comment claims the keydown swallow is what stops the chat bar starting with a `t`. The two are in
  tension; the gate covers the site.
