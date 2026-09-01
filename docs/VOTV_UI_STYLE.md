# VOTV native widget style — the house style every Multivoid screen must match

**USER DIRECTIVE 2026-08-26:** *"Need to research this, write down, and strive fully towards this
native votv widget style"*, against eleven reference captures of the game's own menus
(`ignore_folder/votv_widgets_style/`, local-only). This file is the written result and the
**binding style contract** for anything Multivoid draws inside the game's UI.

**Read this before building or restyling ANY native screen** — and before picking a colour, a
border, or a hover treatment by eye. Every value below was **sampled from the reference PNGs**, not
judged by looking at them; see §7 for two places where looking at them gave the wrong answer.

**Scope.** This governs the NATIVE (UMG) surfaces — the server browser, the menu inject, anything
built into `ui_menu_C`'s switcher. It does **not** govern the Dear ImGui overlay (F1), which is a
developer surface with its own conventions and is not pretending to be part of the game.

---

## 1. The one-sentence version

VOTV's menus are **bordered charcoal boxes with monospace text, where colour carries meaning**:
white says *content*, orange says *this is a thing you interact with*, yellow says *your cursor is
here*, red says *this is destructive*, and a purple fill says *this row is selected*.

If a screen of ours is flat, borderless, and cyan, it is wrong — which is exactly what the native
server browser looked like on 2026-08-26, before this file existed.

---

## 2. The measured palette `[V]`

Sampled with a dominant-colour histogram over 6×6 patches (fills) and over glyph bodies (text), so
the values are the colours the game actually paints and not antialiasing between two of them.

### Surfaces

| role | value | where it was measured |
|---|---|---|
| Window fill | `#1A1A1A` | Settings window body, `SERVER_BROWSER_0` |
| Window / panel border | `#646464` | Settings window frame — **SUPERSEDED, see section 9**: the frame is the material `inst_uiBorder`, not a flat colour, and each edge has its own band pair |
| Row fill (normal) | `#313131` | section rows, `SERVER_BROWSER_0`; unselected save rows, `_9` |
| Row fill (alternate / inset cell) | `#404040` | setting rows and keybind value cells, `_2` / `_5` |
| Row fill **SELECTED** | `#400040` | the expanded save slot, `_9` |
| Destructive control fill | `#400000` | the keybind reset buttons, `_5` |
| Text input fill | `#000000` | the "search setting by name" box, `_0` |
| Scrollbar thumb | `#D4D4D4` | Settings scrollbar |
| Checkbox fill when ON | `#C66200` | "Enable disclaimer warning message", `_0` |

Note the deliberate arithmetic: `#400040` and `#400000` are the same `0x40` component moved between
channels, and `#1A1A1A` / `#313131` / `#404040` are a clean three-step grey ramp. This is a designed
palette, not an accumulation — treat a value that is not on this list as a mistake.

### Text

| role | value | notes |
|---|---|---|
| Body / label, idle | `#FFFFFF` | the default. Most text is white. |
| Interactive label, section header, value | `#FF7C00` | orange. **The load-bearing accent.** |
| Value emphasis (numbers left of a slider) | `#FFBC00` | amber, a lighter sibling of the orange |
| **HOVER** | `#FFFF00` | pure yellow. See §4 — this is the whole hover treatment. |
| Caution action | `#FFFF00` | e.g. "Open save data reset menu" |
| Destructive action | `#FF0000` | e.g. "Delete save slot", "Hard reset", "Convert Save System" |

**We currently use cyan (`#00FFFF`) as the coop accent.** It appears nowhere in the game's own menus.
See §6 for the decision that owes.

---

## 3. Structure

Every panel in every reference shot is the same object:

```
+--------------------------------------------------+   <- 2 px #646464 border, SHARP corners
| Title, centred, white, larger                    |   <- header strip, own border, #1A1A1A
+--------------------------------------------------+
| row  ................................. control   |   <- #313131 / #404040, own thin border
| row  ................................. control   |
| ...                                            ||<- scrollbar INSIDE the border, #D4D4D4 thumb
+--------------------------------------------------+
| Back                            Reset all | Apply |  <- footer: back LEFT, actions RIGHT
+--------------------------------------------------+
```

Rules that hold across all eleven captures:

1. **Everything is boxed.** Windows, header strips, rows, value cells, buttons, the description
   pane. There is no free-floating text anywhere in the game's menus.
2. **Sharp corners.** No rounding, anywhere.
3. **Monospace throughout** — the game's `font_ui`. No proportional text in a menu.
   **`[V]` 2026-08-30, read off the game's own widget** rather than sampled from a
   render: the label on `ui_saveSlots_C.button_back` carries `font='font_ui'
   size=20 outline=0` (`native_screen[fontprobe]`). One asset, and a button's
   label size is **20** — ours were 18, which is now the measured constant
   `native_screen::kBtnFontPx`.
   **`[?]` The one thing this does NOT settle:** a 2026-08-30 user report reads the
   game's OUTER buttons (`Back`) and its INNER ones (`Save`/`Reset`, inside the
   rules panel) as two different faces. The probe read the outer donor only. If
   they do differ it is size or outline, not asset — but that is unmeasured, and
   the inner kind has no consumer in our chrome yet.
4. **NEVER ALL-CAPS on a button.** `[V]` across the whole capture corpus VOTV
   uppercases no button label anywhere: *Play game*, *Delete save slot*, *Create
   new save slot*, *Open save data reset menu*, *Update list*, *Duplicate save
   slot*, *Open save directory*, *Convert Save System*, *Back*, *Save*, *Reset*.
   Sentence case, always.
   This is worth its own numbered rule because breaking it is not a subtle miss:
   ours shipped `BACK` / `REFRESH` / `HOST` / `CONNECT`, and combined with the
   two-point size error above it read to the user as *a different, pixelated
   font* — caps thickens a monospace block and the smaller size coarsens it.
   Two wrong constants presented as a wrong typeface. Fixed `7d0fb7df`.
5. **The title is centred and white**, on its own bordered strip, larger than body text.
6. **`Back` is bottom-LEFT. Confirming/destructive actions are bottom-RIGHT.** (`Back` /
   `Reset all` `Apply`; `Back` / `Hard reset`.) This is consistent in every window that has both.
7. **Collapsible sections** use a small triangle `▷` collapsed, `▾` expanded, at the LEFT of an
   orange header, with children indented one step.
8. **Two-column rows**: white label at the left, the interactive part right-aligned in its own
   inset cell. The keybinds window (`_5`) is the clearest case — label `#FFFFFF`, bind value
   `#FF7C00` inside a `#404040` cell.
9. **The scrollbar lives inside the panel border**, not outside it.

---

## 4. State, which is the part we are missing

This is the section that answers the user's 2026-08-26 report — *"выделения нету у позиций в списке
как у imgui браузера"* (the list rows have no highlight).

| state | native treatment | evidence |
|---|---|---|
| **Idle** | label `#FFFFFF`, row fill `#313131` | `_2`, "Mirror mode" / "Game resolution" |
| **Hover** | **the label turns `#FFFF00`** | `_2`, "VSync" under the cursor: 82 glyph px of pure yellow against `#FFFFFF` on every unhovered neighbour in the same list |
| **Selected** | **row fill becomes `#400040`** (purple), content stays white | `_9`, the expanded save slot |
| **Disabled** | label greys out, row fill lightens | `_2`, "Permanent season" while permanent-season is off |

**The hover treatment is a TEXT COLOUR CHANGE, not a background change.** Both the hovered and
unhovered rows in `_2` sample `#404040` for their fill — identical. That is the opposite of the
ImGui incumbent, which paints `HeaderHovered` behind the row and leaves the text alone, and it is
why porting the incumbent's look would be visibly foreign here.

**Selection is the reverse**: the fill changes to purple and the text does not.

So a Multivoid list row owes **two independent visual channels**, not one:
per-cell text colour for hover, and row fill for selection.

---

## 5. What this means for the native server browser, concretely

As of `95d18cc5` the browser is structurally right and stylistically foreign. The gap, in the order
it should close:

**STATUS 2026-08-26: S1-S7 and S9 are BUILT** (`mp.py browser --fake-master 30` ALL PASS, and the
capture shows the frames). S8 was already correct. What remains open is listed under the table.

| # | gap | target | state |
|---|---|---|---|
| S1 | No panel border. The window is a cloned 9-slice from `ui_saveSlots.Image_0` with no frame. | ~~2 px `#646464` frame~~ **SUPERSEDED by section 9** — the target was wrong: a flat rectangle is what the user rejected twice; the frame is a cloned 9-slice of `inst_uiBorder`, ONE ring per box, and the stepped look comes from NESTING | **DONE, then REDONE 2026-08-31, then corrected 2026-09-01** — `AddFramedBox` |
| S2 | Title is cyan and left-aligned, on no strip. | white, centred, own bordered header strip | **DONE** |
| S3 | Rows have a flat `rgba(1,1,1,0.05)` tint and no border. | `#313131` fill, thin border, per-row box | **DONE** — fill `95d18cc5`, the per-row FRAME 2026-08-30 (`AddFramedBox`) |
| S4 | **No hover.** The user's report. | label cells → `#FFFF00` on hover | **DONE** — `UpdateHover`; 2026-08-30 the row's FRAME turns `#FFFF00` too, see 5b |
| S5 | **No selection.** No selected row exists yet at all. | row fill → `#400040` | **DONE** — keyed on `lobbyId`; 2026-08-30 it OUTRANKS hover, see 5b |
| S6 | Column headers are dim grey. | orange `#FF7C00`, matching a section header | **DONE** |
| S7 | ~~The X /~~ The BACK button carries a cloned `button_back` style but authored white labels. (**The X is GONE from both native windows since 2026-08-30, `b7b11978`** — USER: no native VOTV window has one; the exits are Back and ESC and all three are `[V]` proven. This row's X half is history.) | orange `#FF7C00` labels; BACK belongs bottom-**LEFT**, not bottom-right | **DONE 2026-08-30** — both halves. BACK moved LEFT on 2026-08-26; the ORANGE half did not ship until now, while this row said DONE. The user found the residue by eye — *"кнопки какие-то жирные, совсем не как нативный votv"* — and it was never weight: `measured` from the captures, every native button label is the accent (`Hide all`, `Language`, `Binds`, `Back`, `Reset all`, `Apply`, `Fix mailbox`, every gamemode tab) and WHITE is reserved for the window title and body text. Scaled for capture size our glyphs carry LESS ink than the game's (114 vs 144 lit px); white on near-black is simply maximum contrast, so it reads bold. |
| S8 | Version-mismatch tint is amber `#FFBC00`-ish by accident. | keep, but say so deliberately — amber is in the palette | **DONE** — kept, deliberately |
| S9 | Status line is dim grey, free-floating. | white, in a bordered footer strip | **DONE** |

**S7 contained a real inversion** and is fixed: BACK was bottom-RIGHT, the CONFIRM position in
every native window, for a cancel action. It is now bottom-LEFT; Connect/Join takes the right when
T7 adds it.

**Still open after S1-S9:**

* ~~**Per-row borders.**~~ **DONE 2026-08-30**, on the user's direct instruction — *"чтобы из списка
  серверы не сливались, у каждого свои границы в стиле votv нативный визуал максимально"*. Every row
  is now a `native_screen::AddFramedBox` (the same primitive as the panel, the header strip and the
  footer), INSIDE the existing 64 px row, so no layout arithmetic moved. **Its edge is no longer a
  flat `#646464`** (2026-09-01): once the frame donor resolves, every framed box including a row
  clones the game's `inst_uiBorder`, and `[V]` a row's edge over the `#313131` face samples
  `353535 -> 222222` where the window's over `#1A1A1A` samples `919191 -> 646464` -- the same
  material reading differently against what is under it. The 2 px slot gap **stays** — two adjacent frames with no gap read as one
  4 px rule, which is the blending the change exists to end. `[V]` `research/browser_shots/browser_native.png`.

  The deferral this replaces was priced on the widget budget (one more `UImage` per row against the
  open perf lane, `MULTIPLAYER_UI.md` §8c.-1). That reasoning was not wrong and is not resolved — T2a
  and T2c still have not run — it was **outranked**: a list whose rows blend into one slab is a
  defect the player sees, and an unmeasured cost is not a reason to ship one. The row's per-paint
  cost is now 4 dispatches for the skin (was 3) and the same 21 for a full paint.
* **The list's first row sits tight under the column header** — visible in the capture. Cosmetic,
  measured, not yet chased.
* **The expander / two-column idiom** (§6) — not adopted, on purpose.

### 5b. The one place we deliberately exceed the measurement `[V]` measured, `[A]` extended

§4's table is what the captures show. The browser's rows do **one thing more than that**, added
2026-08-30 on the user's instruction, and it is recorded here rather than folded into §4 so the
measurement and the extension never blur:

| channel | native (measured) | the browser's rows | why |
|---|---|---|---|
| hover | label → `#FFFF00` | label → `#FFFF00` **and the row's frame → `#FFFF00`** | native hover was measured on SETTINGS rows — one label, one value. A server row is five columns across ~640 px, and recolouring the glyphs alone is a change the eye does not find. The colour is the measured one, applied at row scale; nothing new enters the palette. |
| selection | row fill → `#400040` | unchanged | |
| **both at once** | not observable in any capture (native lists have no persistent selection) | **selection wins outright: a selected row ignores hover in every channel** | the user's rule, verbatim: *"если сервер из списка кликнут, то выделение держится только на нем, а hover игнорится"*. Implemented as one predicate (`PointerLit`), so the frame painter and the text painter cannot disagree about it. |

The structural consequence is worth keeping: hover owns the **frame and the text**, selection owns
the **fill**. They are writes to different widgets, so they can never fight over a pixel, and the
precedence is enforced by suppressing hover at the source rather than by painting over it.

---

## 6. Decisions this file does NOT make

* **The coop cyan.** `#00FFFF` is Multivoid's identity colour — it is the MULTIPLAYER menu item, the
  title, the accent everywhere. It appears in **zero** native VOTV menus. Dropping it makes us look
  native; keeping it makes us look like *us*. That is a product call, not a style measurement, and it
  goes to the user. **PARTLY ANSWERED 2026-08-26, and only for one surface.** The user settled the
  BROWSER's title directly -- *"The windows title should say something like Multivoid - Server
  Browser and be in the style of votv, not the current colors"* -- so that title is WHITE
  (`ui/server_browser_native.cpp:250`, `kText`), and the sentence that stood here saying "the
  browser keeps cyan for the title" was already false when it was written. **What is still open is
  everything OUTSIDE the browser**: the injected MULTIPLAYER menu item and the version/update line
  are still cyan (`ui/multiplayer_menu.cpp:81` `kVersionCyan`), and those live on the game's own
  main menu rather than inside a window of ours. Do not silently drop or keep them; that half is
  still the user's call.
* **Whether to match the section/expander idiom.** The server list is flat; VOTV's lists are
  collapsible trees. Adopting `▷`/`▾` for a flat list would be cargo-culting the shape without the
  content.
* **The font.** We already draw with the game's `font_ui` via `StyleTextBlock`; nothing to change,
  but it is listed so nobody "improves" it.

---

## 7. Three places where looking gave the WRONG answer `[V]`

Recorded because it is the reason this file insists on sampling.

1. I read the `_9` right-panel header **"Game rules for the slot:"** as green. Sampled, its glyph
   body is `#FFFFFF` — 650 px of white and no green in the top three colours. There is no green in
   this palette.
2. I read **"14,183KB"** in the same shot as cyan. Sampled: `#A5A5A5`, plain grey.

Both misreadings came from viewing a downscaled render. A palette taken by eye off a scaled
screenshot invents colours the game does not use, and then those colours get *shipped*. Sample the
source PNG at full size, over glyph bodies, with a histogram.

3. **2026-08-30, and this one was OUR screen, not the game's.** I looked at the browser's own
   capture and read it as a screen whose colours work. Sampled, **no runtime text colour on it had
   ever applied**: the hovered row's frame was `#FFFF00` (77 px) while its glyphs were `#FFFFFF`
   (216 px), and the World/Age cells specified `#A5A5A5` were `#FFFFFF` too — so hover yellow, the
   dim secondary columns, the green "your server" name and the **amber version-mismatch cue** were
   all dead. `ApplyRowTextColors` was calling the RAW `SetTextBlockColor`, which `engine.h:516-531`
   states in capitals does not propagate to a constructed UMG tree.

   What makes this the sharpest of the three: **the screen looked right because the part that works
   is the part set at BUILD time.** The column headers are `#FF7C00` and always were — coloured
   before the widget is attached, where a raw write does land. A surface can be half-dead in a way
   that is invisible precisely because its other half is correct, so "the capture looks right" is
   not evidence about any *particular* colour. Sample the one you claim.

   The same rule the first two teach, one level up: it is not enough to sample instead of squinting
   at a game's screenshot — sample your own output too, per property, against the value you wrote.

---

## 8. Provenance

* Reference: `ignore_folder/votv_widgets_style/` — eleven captures made by the user 2026-08-26,
  covering Settings (idle / graphics / display+hover), Language, Keybinds, the coordinate-panel bind
  list, Start game, Story, the save-slot list, a selected save slot with its rules panel, and
  Credits.
* Sampling: `PIL`, dominant-colour histogram; 6×6 patches for fills, glyph-body histograms with a
  background reject for text.
* This file is DESIGN + measurement. **§5's S1-S9 and its per-row-border item are BUILT**
  (2026-08-30); §5b is built; §6's cyan question outside the browser is still open.

---

## 9. THE FRAME IS A MATERIAL, AND THE LADDER IS NESTING (AS-BUILT 2026-09-01, `c026720c` + `045ce285`)

**This section SUPERSEDES the "Window / panel border `#646464`" row in section 2 and the S1 row in
section 5, both of which specify a flat 2 px rectangle. Building that rectangle is what the user
rejected, twice.**

Verbatim: *"У нас рамка сервер браузера и наших нативных окон - неправильная, не соответствует
стилю рамок VOTV"*, then *"Всё еще говно ебаное"*, then, with zoom crops, *"видишь же что у votv
много скосов на рамке"*, and finally *"еще такие комбинации лесенок должны быть у нас"*.

**Why a colour could never have worked.** `[V]` The native frame's edges each carry their own pair
of 2 px bands, and the horizontal pair is not the vertical one:

| edge | outer band | inner band |
|---|---|---|
| top / bottom | `#A5A5A5` | `#585858` |
| left / right | `#919191` | `#646464` |

A raised bevel lit from above. No single `#646464` rectangle produces that, in any thickness.
`[V]` It is not drawn at all -- it is the MATERIAL **`inst_uiBorder`**, a 9-slice Box brush with
`Margin 0.5`, carried by `ui_settings`'s `image_border`. That material is the game's shared UI
border; many widget assets reference it.

> **CORRECTION 2026-09-01, and this file predicted it.** An earlier version of this section listed
> eight DARKER values here (top `#838383 -> #606060`, left `#5F5F5F -> #515151`, bottom
> `#333333 -> #202020`, right `#383838 -> #292929`). Those came from a dimmed / downscaled capture
> and did not survive re-sampling at full resolution against
> `SERVER_BROWSER_4_keybinds_tab_opens_new_window.png` (1885x1046). The warning at the foot of this
> section is the reason they were doubted, and it caught its own author.

### THE LADDER IS NESTING, NOT A DOUBLED BORDER `[V]`

The "много скосов" the user pointed at is what **nested framed boxes** produce when their rings
abut. It is NOT one box wearing two rings, and an earlier as-built said it was.

The measurement that settles it -- the same edge of the same native window, at two heights:

```
left edge, y=500 (across the LIST) : 919191x2 646464x2 919191x2 646464x2 1A1A1A
left edge, y=208 (across the TITLE): 919191x2 646464x2 1A1A1A
```

A window wearing two rings would show two pairs at BOTH heights. It shows one where no inner panel
sits, so the second pair belongs to the panel. **One ring per box; stack them by nesting.**

**As-built.** `native_screen::AddFramedBox` clones `image_border`'s brush through `CloneStyle`
(0x88 bytes, the unreflected `FSlateResourceHandle` at +0x70 zeroed) onto ONE border image and puts
the fill UNDERNEATH at full size, no inset. A box's content is inset by exactly the ring's rendered
width -- `native_screen.h`'s `kNativeRingPx = 4` -- so a child's ring starts where its parent's
ends. `[V]` our window's right edge now samples `919191x2 646464x2 919191x2 646464x2`,
byte-identical to the game's left edge at y=500, and the list's left edge across a row reads three
clean steps: `919191x2 646464x2 | 919191x2 646464x2 | 353535x2 222222x2 | 313131` -- window ring,
list ring, row bevel. `[V]` autonomous (`mp.py browser --fake-master 12`, ZERO absorbed AVs, eight
screen tests PASS), **NOT hands-on**.

**Both neighbouring insets are wrong, and both were built and measured.** At 6 the child's ring
lands ON the parent's inner band and merges with it (`919191x4` -- a four-pixel light run the game
never produces). At 0 the child's ring is drawn UNDER the parent's border image, which is painted
last at full size, and disappears entirely.

**The 4 is VOTV's own, not a pixel guess** -- `[V]` seven of eleven border slots across
`ui_settings` and `ui_saveSlots` carry a slot offset of 4, and `ui_saveSlots.image_border_6` /
`image_border_7` are a genuine nested pair on parent-and-child canvases exactly 4 apart. Being a
slot offset (which scales with the layout transform) rather than a screen-pixel constant, our frame
drifts under DPI as the game's does. **That number was measured correctly the first time and spent
on the wrong distance** -- an inset between a box and a second ring of its own, rather than between
a box and its PARENT. When a measured constant moves into code, name the distance it measures.

**FOUR TRAPS, each of which cost a full cycle:**

1. `[V]` `image_border` has **`bIsVariable = False`** -- no UPROPERTY -- so a property read
   (`DonorField`) returns null FOREVER, silently. Its sibling `scrollboxRoot` is `True`, which is
   why every other donor resolved and hid the asymmetry. Walk the `WidgetTree` by Outer instead
   (`native_screen::DonorChild`).
2. `[V]` `UUserWidget::GetWidgetFromName` is **not a resolvable UFunction in this build**. It was
   tried, measured absent (`fn=0`), and DELETED rather than kept as a fallback.
3. Cloning alone still gave ONE band: the fill was being added on TOP with an inset and painted
   over the brush's inner bands.
4. **A donor with no brush art must be REFUSED** (`SetBorderDonor`, `045ce285`). `AddFramedBox`
   picks its child order from "is there a donor" while `FramedBoxParts` works out which child is
   which by reading the border's `ResourceObject`; a donor that resolved with no art would build
   the framed order and be classified flat, swapping every row's fill and border silently. `[V]`
   not hypothetical -- on `ui_saveSlots_C.button_back` only three of four brushes carry art.

**AND A MEASUREMENT WARNING THAT APPLIES TO THIS WHOLE FILE.** Section 1 says every value here was
*"sampled from the reference PNGs"*. `ignore_folder/votv_widgets_style/` holds eleven captures
ranging from 546x570 to 1885x1046, **and the small ones cannot resolve a 4 px band**. Any
sub-4-px claim in this document inherits that doubt -- re-sample it against
`SERVER_BROWSER_4_keybinds_tab_opens_new_window.png` (1885x1046) before relying on it. And sample a
repeated pattern in TWO places before attributing it to an object: a single scanline is the SUM of
a widget stack and says nothing about which widget owns which pixel.
See `[[lesson-a-reference-capture-must-match-the-conditions-of-the-report]]` and
`[[lesson-sample-a-repeated-pattern-at-two-places-before-attributing-it]]`.

**The nesting comparison this section used to list as STILL OPEN is DONE** (2026-09-01) and is the
measurement above. What is open now is smaller and named in `docs/SERVER_BROWSER_ARC.md` section 9:
the `kPadPx` change is shared by all three shell screens and only the browser's edges were sampled.
