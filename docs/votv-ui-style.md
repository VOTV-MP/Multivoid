# The game's widget style

The house style every native Multivoid screen must match, measured from eleven captures of the
game's own menus rather than judged by eye. This governs the native UMG surfaces (the server
browser, the hosting windows, the menu inject, anything built into the game's menu switcher). It
does not govern the Dear ImGui overlay, which is a developer surface with its own conventions and
is not pretending to be part of the game. Read it before building or restyling any native screen,
and before picking a colour, a border or a hover treatment by eye.

## In one sentence

The game's menus are bordered charcoal boxes with monospace text, where colour carries meaning:
white says content, orange says this is a thing you interact with, yellow says your cursor is
here, red says this is destructive, and a purple fill says this row is selected. A screen of ours
that is flat, borderless and cyan is wrong.

## The measured palette `[V]`

Sampled with a dominant-colour histogram over small patches for fills and over glyph bodies for
text, so the values are the colours the game paints and not the antialiasing between two of them.

### Surfaces

| Role | Value | Where measured |
|---|---|---|
| window fill | `#1A1A1A` | the settings window body |
| window and panel frame | not a colour: the material `inst_uiBorder`, see the frame section | the settings window frame |
| row fill, normal | `#313131` | section rows; unselected save rows |
| row fill, alternate or inset cell | `#404040` | setting rows and keybind value cells |
| row fill, selected | `#400040` | the expanded save slot |
| destructive control fill | `#400000` | the keybind reset buttons |
| text input fill | `#000000` | the search box |
| scrollbar thumb | `#D4D4D4` | the settings scrollbar |
| checkbox fill when on | `#C66200` | a settings checkbox |

The arithmetic is deliberate: `#400040` and `#400000` are one component moved between channels,
and the three greys are a clean ramp. This is a designed palette, not an accumulation; a value
not on this list is a mistake.

### Text

| Role | Value |
|---|---|
| body and labels, idle | `#FFFFFF` |
| interactive label, section header, value | `#FF7C00`, the load-bearing accent |
| value emphasis (a number beside a slider) | `#FFBC00` |
| hover | `#FFFF00`, the whole hover treatment |
| a caution action | `#FFFF00` |
| a destructive action | `#FF0000` |

The mod's coop accent, cyan `#00FFFF`, appears nowhere in the game's menus; see the decisions
below.

## Structure `[V]`

Every panel in every capture is the same object: a framed box with sharp corners; a centred white
title, larger than body text, on its own framed strip; rows of white label on the left and the
interactive part right-aligned in its own inset cell; the scrollbar inside the frame; a footer
with Back at the bottom left and the confirming or destructive actions at the bottom right.

1. Everything is boxed: windows, header strips, rows, value cells, buttons, description panes.
   No free-floating text anywhere.
2. Sharp corners, no rounding.
3. Monospace throughout, the game's `font_ui`. Read off a game button, its label is size 20 with no
   outline; the constant is `native_screen::kBtnFontPx`. Whether the game's outer buttons and the
   inner ones inside a panel differ in size or outline is unmeasured.
4. Never all-caps on a button. The game uppercases no label anywhere: *Play game*, *Delete save
   slot*, *Back*, *Save*, *Reset*. Caps thicken a monospace block and a smaller size coarsens it;
   the two together read as a different, pixelated font.
5. The title is centred and white on its own framed strip.
6. Back is bottom-left; confirming and destructive actions are bottom-right, in every window that
   has both.
7. Collapsible sections use a small triangle at the left of an orange header, children indented
   one step.
8. Two-column rows: white label on the left, the interactive part right-aligned in a `#404040`
   cell with an orange value.
9. The scrollbar lives inside the panel frame.

## State `[V]`

| State | Native treatment |
|---|---|
| idle | label `#FFFFFF`, row fill `#313131` |
| hover | the label turns `#FFFF00`; the fill does not change |
| selected | the row fill becomes `#400040`; the text does not change |
| disabled | the label greys, the fill lightens |

Hover is a text-colour change, not a background change, and selection is the reverse. So a list
row owes two independent visual channels: per-cell text colour for hover, and row fill for
selection. The ImGui overlay does the opposite, painting a hovered background behind unchanged
text, which is why porting its look would be visibly foreign.

### Where the browser deliberately exceeds the measurement

The native hover was measured on settings rows of one label and one value. A server row is five
columns across a wide box, so on the browser's rows hover also turns the row's frame yellow, and
selection wins outright: a selected row ignores hover in every channel. Hover owns the frame and
the text, selection owns the fill; they write different widgets and can never fight over a pixel,
and the precedence is enforced by suppressing hover at the source.

## The frame is a material, and the ladder is nesting `[V]`

The native frame's edges each carry their own pair of two-pixel bands, and the horizontal pair is
not the vertical one:

| Edge | Outer band | Inner band |
|---|---|---|
| top and bottom | `#A5A5A5` | `#585858` |
| left and right | `#919191` | `#646464` |

A raised bevel lit from above, which no flat rectangle in any thickness produces. It is the
material `inst_uiBorder`, a nine-slice box brush the game's own settings window carries as its
border image and many widget assets share.

The stepped ladders in the game's frames are nested framed boxes whose rings abut, not one box
wearing two rings. The same edge of the same native window, sampled across the list and across the
title, shows two band pairs where an inner panel sits and one where none does: one ring per box,
stacked by nesting. As built, `native_screen::AddFramedBox` clones the border image's brush onto
one border image and puts the fill underneath at full size; a box's content is inset by exactly
the ring's rendered width, `kNativeRingPx`, so a child's ring starts where its parent's ends. Both
neighbouring insets were built and measured wrong: at six the child's ring lands on the parent's
inner band and merges into a four-pixel light run the game never produces; at zero the child's ring
is painted under the parent's border and vanishes. The four is the game's own slot offset, carried
by most of its border slots and by a genuine nested pair four apart, so it scales with the layout
transform as the game's does.

Four traps, each of which cost a full cycle:

1. The game's border image is not a reflected property, so a property read returns null forever
   while its sibling resolves; walk the widget tree by outer instead (`native_screen::DonorChild`).
2. The widget-by-name lookup is not a resolvable function in this build; it was measured absent
   and deleted rather than kept as a fallback.
3. Cloning alone gave one band: the fill was added on top with an inset and painted over the
   brush's inner bands.
4. A donor with no brush art must be refused: the box picks its child order from "is there a
   donor" and classifies the children by reading the border's resource, so a donor that resolved
   with no art would be built framed and classified flat, swapping every row's fill and border.
   On the game's own back button only three of four brushes carry art.

## Decisions this page does not make

- **The coop cyan.** It is the mod's identity colour, the MULTIPLAYER menu item and the version
  line, and it appears in no native menu. Dropping it looks native; keeping it looks like us. The
  browser's own title is white, in the game's style; what is open is everything on the game's own
  main menu, which is a product call, not a measurement.
- **The expander idiom.** The server list is flat and the game's lists are collapsible trees;
  adopting the triangle for a flat list would copy the shape without the content.
- **The font.** The screens already draw with the game's `font_ui`; listed so nobody improves it.

## Three places where looking gave the wrong answer `[V]`

1. A right-panel header read as green. Sampled, its glyph body is white; there is no green in
   this palette.
2. A file size read as cyan. Sampled, plain grey.
3. The browser's own capture read as a screen whose colours work. Sampled, no runtime text colour
   on it had ever applied: the hovered row's frame was yellow while its glyphs were white, and the
   dim secondary columns and the amber version cue were all dead, because the colour call used
   was the raw setter that does not propagate to a constructed widget tree. The screen looked
   right because the part that works is the part set at build time, the column headers coloured
   before the widget is attached. A surface can be half dead invisibly, so "the capture looks
   right" is not evidence about any particular colour: sample the one you claim, against the value
   you wrote.

## Open

- The frame inset is shared by the three shell screens, and only the browser's edges have been
  sampled against the game's.
- The list's first row sits tight under the column header. Cosmetic, measured, not yet chased.

## Provenance and a measurement warning

Eleven captures of the game's menus, sampled with a dominant-colour histogram: small patches for
fills, glyph-body histograms with a background reject for text. The captures range from small to
full resolution, and the small ones cannot resolve a four-pixel band; any sub-four-pixel claim on
this page inherits that doubt and is re-sampled against the full-resolution capture before it is
relied on. Sample a repeated pattern in two places before attributing it to an object: a single
scanline is the sum of a widget stack and says nothing about which widget owns which pixel.
