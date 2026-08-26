# MULTIPLAYER_UI — coop menu design

**Living document.** Captures the user's vision + the approach decision for the
in-game multiplayer UI.

**Status (updated 2026-07-04): BUILT.** The menu shell + flows have shipped as
runtime-UMG built by our C++ mod (the "chosen" approach below) — see the `ui/`
modules. **PARTLY: corrected 2026-08-25 — the MULTIPLAYER button is native UMG, but
`server_browser.cpp` is an ImGui modal, which is the row the table below REJECTED for
player-facing menus. See "Native server browser — RESEARCH 2026-08-25".**
`server_browser.cpp` (the "future" browser below SHIPPED long since:
master-server lobby list + Direct Connect), `host_save_picker.cpp`,
`roster.cpp`, `scoreboard.cpp`, `dev_menu.cpp`, `moderation.cpp`, `hud.cpp`,
`skins_panel.cpp` (2026-07-02: the F1 > Cosmetics > Skins model browser —
gmod-style preview tiles from the LogicMods pak catalog + the v94 builtin
kerfur bodies, AS-BUILT; see docs/COOP_CLIENT_MODEL.md §3 for the skins
runtime). F1 > Cosmetics > Nameplate (v94, AS-BUILT 2026-07-02): the "show my
nameplate to other players" checkbox — a SYNCED per-peer pref (NameplateChange
+ the Join prefs byte; persists in multivoid.ini `nameplate=`).
**Overlay typography + chat (AS-BUILT 2026-07-04, `684f6670`+`1e6c86ea`;
hands-on = runbook 0j):** vendored TTFs EMBEDDED in the DLL as RCDATA
(`ui/fonts.cpp`) — Regular 16px is the default font of the WHOLE overlay,
Bold 18px is the chat face; Cyrillic glyph ranges on both, so chat is UTF-8
end-to-end (the deliberate ASCII '?'-squash is retired). Chat feed: 220ms
fade-in + fade-out tail, per-slot colored nick, 4-way outline, word-wrap;
chat input: Up/Down send-history; console: focus-on-open + command history;
the save-name + direct-IP fields submit on Enter. MP-created saves stamp
`Version` at create (`c81f1c2d` — the red "unk!" fix; runbook 0f).
**Resolution scale + font families (AS-BUILT 2026-07-04 late):** the whole
overlay is resolution-PROPORTIONAL — `ui/scale.h` owns ONE factor
(clientHeight/1080, quantized to sixths), fonts re-bake at `px * scale`
through imgui_freetype (vendored FreeType 2.13.3; sharper hinting than
stb_truetype), the style rescales (reset + ScaleAllSizes), and every pixel
constant in `ui/` goes through `S()`; a live resize/res change re-bakes on
the next frame. THREE embedded families — Roboto (default; user verdict
2026-07-04 after comparing), JetBrains Mono, Cascadia Code (all
Cyrillic-cmap-verified; OFL/Apache licenses in assets/fonts) — switchable
live in F1 > Cosmetics > Interface, persisted as multivoid.ini `ui.font`;
plus a user size pref (`ui.scale`, default 1.25x, F1 slider 0.75–1.75x)
multiplied into the resolution factor. The T-chat input bar matches the
chat column width; T-chat is available for the whole HOST session (a
zero-client lobby included), wire send best-effort.
**Nameplate occlusion (AS-BUILT 2026-07-04 `f8185847`; refined 2026-07-05
`4011fc73` + `76ce8c58`, verdict = runbook 0w-a):** a peer behind world
geometry (walls/closed doors/props — pawns never block) keeps a readable
plate, half-transparent as a unit (x0.5): the NICK renders GRAY; the HEALTH
BAR stays RED but darker + more translucent than the normal fill (user
2026-07-05: not gray — «hp красный, но потемнее и полупрозрачный»).
Hurt-flash red keeps priority on both. Line trace via `ue_wrap/trace.cpp`
per visible plate on the game thread.
**F1 > Cosmetics > Nameplate — nickname color (v103, 12f, AS-BUILT 2026-07-05
`76ce8c58`; hands-on = runbook 0z):** a per-player custom nick color — SYNCED
(live NickColorChange=88 + a `[has][r][g][b]` field in Join/PlayerJoined for
late joiners) and persisted (multivoid.ini `nick_color=RRGGBB`). ONE owner:
`coop/player/nick_color` (atomic per-slot store; 0 = surface default).
Consumers: nameplate nick (default white), chat nick prefix (default per-slot
palette), scoreboard row (default role gold/white — role stays readable via
the Link column). flash/occluded signal colors keep priority on the
nameplate. UI = inline hue-wheel picker, commit DEBOUNCED ~0.35 s after the
last edit (user bug 2026-07-05: deactivate-after-edit on the composite picker
never fired — the color only applied on a checkbox re-toggle). A NEW identity
(no `nick_color=` key) defaults to custom WHITE (user 2026-07-05); an
explicitly empty value (unchecked box) = the per-surface defaults.
**Overhead chat bubbles (12g, AS-BUILT 2026-07-05; hands-on = runbook 0aa):**
MTA/SAMP-style — a player's last chat message renders word-wrapped (max 5
rows) above their nameplate, 8 s hold + 0.7 s fade, outlined text without a
backing box. Display-only (`coop/comms/chat_bubbles`, fed by chat_sync next
to its PushChat calls — nothing new on the wire); rides the nameplate anchor,
so distance/occlusion fade applies and a v94-hidden plate hides the bubble
too. The on-screen clamp reserves the bubble height (no off-screen-top).
**F1 > Administration > Players (AS-BUILT 2026-07-05 `f66d2c7f`, verdict =
runbook 0w-b):** HOST-role-gated F1 category (dev_menu Cat/Sub `host` flag on
`roster::LocalIsHost` — clients/solo never see it): Online (roster rows +
Teleport/Kick/Ban), Offline (`coop/moderation/seen_players` — the persistent
GUID-keyed seen-players registry, multivoid-players.txt `guid|nick|lastSeen|ip`,
written at the host Join seam + disconnect edge), Banned (ban_list rows now
with REASON + Unban; file format `ip|nick|unixtime|reason`, lenient back-compat
parse). Ban modal takes a reason; offline ban uses the last known IP (P2P
records without one get a disabled button + tooltip). Smoke e2e: the registry
recorded the joining client + file round-trip; audit PASS 0 CRITICAL.
**Network stats overlay (AS-BUILT 2026-07-05, user ask; verdict = runbook 0t):**
`ui/net_stats_panel.cpp` — a passive top-right panel for host AND clients, OFF
by default (F1 > Network > Stats, persisted `ui.netstats`): live receive/send
rate (GNS wire-level, ~1 s window), session totals down/up, packets/s, peers +
worst ping, 60 s rate sparkline (rx sky filled area / tx amber line — the slot
palette). Data = `coop/net/net_stats` (the ONE owner of the wire counters —
Session's packet counters moved there; bytes counted at every GNS accept;
rates published by Session's existing 1 Hz net-thread telemetry sample). MTA
precedent: CNetworkStats. The F1 page doubles as a live readout while the
overlay is off.

**MULTIPLAYER menu button — native-parity polish (AS-BUILT 2026-07-15;
user hands-on iterated, appearance/behavior VERIFIED, font DEFERRED):** the
injected `UButton` (`engine::InjectCanvasButton` + `coop::multiplayer_menu`)
now matches native menu items: label "Multiplayer" + CYAN, flush-left
(content slot forced `HAlign_Fill` — a spawned `UButtonSlot` defaults to
Center), native press dip+springback (click fires on the mouse-RELEASE edge so
the real UButton finishes its own press animation), non-interactive while the
server browser is up (HitTestInvisible), a modal ImGui backdrop dims the menu
behind the browser, and the native menu SOUNDS play — `buttonclick` +
`buttonrollover` on the menu button (Slate, via the restored FSlateSound
ResourceObject in the style clone) AND on all browser buttons + the title-bar X
(click only) via `ui/menu_sfx.cpp` (`PlaySoundAtLocation` 2D → honors the
game's sound settings). The old `tex_btnStart` style-clone was retired (RULE 2):
its pointer is null at inject time, which silently fell back to Roboto/Center/
white. DEFERRED: `font_ui` (Share Tech Mono, the true native menu font) doesn't
resolve via `FindObject` yet — Roboto fallback stands. Full detail +
gotchas: `memory/lesson_umg_injected_menu_button_native_parity.md`.

This doc is kept for the **design rationale** (why runtime UMG, not
BPModLoader/paks); the code is the truth for the as-built UI.

## User vision (2026-05-22)

A multiplayer option surfaced **in VOTV's own main menu** (native, not a
debug overlay), with:

1. **Play as Host** → choose a save (new or existing) → load → start
   hosting (listen for one client; LAN-first).
2. **Connect** → type an IP → join the host's session.
3. **Server browser** (SHIPPED — master-server lobby list, see Status) → join.

## Flows

```
Main menu
├── [Singleplayer]            (VOTV's existing buttons, untouched)
└── [Multiplayer]  (ours)
    ├── Host
    │   ├── pick save slot (existing) OR New Game
    │   ├── -> load world (host-authoritative; reuses VOTV's load path)
    │   └── -> open UDP listen socket, wait for client
    ├── Connect
    │   ├── enter host IP (+ port; default fixed)
    │   └── -> handshake -> client loads host's world state -> in game
    └── Server browser  (SHIPPED: master-server registry list + Direct Connect)
```

Host = authoritative (owns save/world/progression). Client sends input +
receives state. Never the reverse. (Methodology Phase 3.1.)

## Approach decision — native UMG, built at runtime by our C++ mod

The menu must (a) look native/integrated, (b) not edit VOTV's menu asset
(principle 1 / anti-pattern A6), and (c) not bind us to UE4SS long-term
(see `ARCHITECTURE.md` "Substrate"). Options weighed:

| Approach | Native look | Edits assets? | UE4SS dependency | Verdict |
|---|---|---|---|---|
| **Runtime UMG widget via reflection** (our C++ mod constructs a `UUserWidget`, adds a Multiplayer button + panels, adds to the menu/viewport) | yes | no | none (works with or without UE4SS) | **chosen** |
| BP mod via UE4SS BPModLoader (cooked widget .pak loaded through UE4SS's BPModLoader Lua mod) | yes | adds a new asset (allowed) but needs UE4.27 editor + cook | yes (BPModLoader) | ~~rejected — ties us to UE4SS~~ **VERDICT VOID 2026-08-25: the premise died on 2026-08-21.** F2/D-3 makes Multivoid a UE4SS mod, and `BPModLoaderMod` is already in the r2modman profile (measured). The only remaining cost is AUTHORING — see "Native server browser" below |
| Sibling-pak hybrid (cooked widget .pak mounted via UE4's NATIVE auto-mount of `Content/Paks/`) | yes | adds a new asset | **no** (revisited 2026-05-25 — see note below) | ~~deferred — toolchain cost not justified~~ ~~**COST PREMISE VOID 2026-08-25: the user HAS UE 4.27 installed.**~~ **RE-DECLINED 2026-08-25 (post-`/qf`), on NEED rather than cost — see §8.** The editor being installed is no longer the question: the palette is **cloned from resident native widgets at runtime** (§7b/§8), so there is no art to author, cook or ship. **This decline is CONDITIONAL and names its trigger: if the probe's donor-residency check (§8 step 2, O7) comes back donor-null, this reason is VOID and this row re-opens.** Recorded that way so it cannot rot the way "ties us to UE4SS" did |
| ImGui overlay | no (debug look) | no | OUR vendored ImGui (RULE 3 — the "UE4SS's ImGui" option is retired; DX11 + DX12 backends as of 2026-07-26) | rejected for the menu — fine for dev/debug overlays only |

**2026-05-25 update (revisited after the user pushed back on "VT mod looks natural; we look dirty"):** the original rejection conflated BPModLoader-dependent paks (which DO require UE4SS) with all paks. UE4 itself auto-mounts every `.pak` it finds under `Content/Paks/` at engine startup — independently of UE4SS. So a sibling `votv-coop-content.pak` we author and ship alongside our DLL would mount cleanly without ANY mod-framework dependency (RULE 3 preserved). VOTV also ships `UPakLoaderLibrary` (Rama's PakLoader) + `URyRuntimePakHelpers` as Blueprint libraries already callable through our existing `ParamFrame` infrastructure, providing explicit `MountPakFile` if we want non-auto-mounted paks. See:

- `research/findings/architecture-audits/votv-mp-pak-mount-feasibility-2026-05-25.md` — implementation feasibility (FEASIBLE without UE4SS via auto-mount or UPakLoaderLibrary). *(In the local-only research corpus since 2026-08-23 — the local-only docs-arc note. Left as a path, not a link, so it does not read as a broken one.)*
- Architectural + reality-check verdicts (stay all-DLL for now, revisit the public-server phase; the perceived polish gap closes with programmatic outline + shadow + UBorder): recorded in this section — the planned standalone `hybrid-pak-architecture` / `hybrid-pak-reality-check` finding files were never filed (dead links removed 2026-07-12).

~~The "chosen" row remains correct for the current scope. The "rejected" row's reasoning is preserved (BPModLoader specifically does tie us to UE4SS).~~ **RETIRED 2026-08-25 — both halves are false now: D-3 ties us to UE4SS deliberately, and the user has the editor.** A new "deferred" row replaces a small fraction of the rejected row's blast radius — the sibling-pak path is technically clean per RULE 3. ~~but the 80 GB UE4.27-editor toolchain cost isn't justified until a the public-server phase widget (server browser with sortable rows, etc.) actually needs it.~~ ~~**2026-08-25: that moment ARRIVED and the cost is spent — the user asked for the sortable-row browser and has the editor.**~~ **SUPERSEDED THE SAME DAY by the USER's decision (§5 box) and by the converged `/qf` (§8): the editor route is CANCELLED and everything is built in C++.** The sortable-row widget the old text was waiting for is exactly what §8 now builds — natively, with its style **cloned from resident widgets** rather than authored, so arriving at that moment removed the need for the pak instead of triggering it. **§5 and §6's decision boxes GOVERN this table; where a cell and a box disagree, the box wins.** Polish via programmatic UMG (text outline + drop shadow shipped 2026-05-25; a `UBorder` background panel is still the right shape for a framed panel — one content child — though NOT for a list ROW, which stacks three children and needs a `UOverlay`, §8).

So: our C++ mod hooks the menu's construction (`ui_menu_C` BeginPlay /
construct), creates our own widget tree at runtime via reflection, and
wires its buttons to the `coop/` session API. No VOTV asset is modified; we
add our widget alongside. Styling is hand-built in code (more effort than a
designer asset, but substrate-agnostic and asset-clean).

Dev/debug overlays (connection stats, entity inspector) stay ImGui — that's
tooling, not the player-facing menu.

## Native server browser — RESEARCH 2026-08-25 (USER: "very very soon"), and one rejection reason has EXPIRED

The user asked for a **native** server-browser window and pointed at
`modestimpala/VotVMods` (SmartTV et al.) as the look to study. Three measurements, then the fork.

### 1. What SmartTV actually is `[V]`

`SmartTV.pak` (2,994,989 B, 238 entries, listed with `repak`) is **pure Blueprint — no DLL at all**:

```
VotV/Content/Mods/SmartTV/
    ModActor.uasset            <- the BPModLoader entry point (spawned into the world)
    smarttvmap.umap
    Assets/Widgets/**          <- 18 WidgetBlueprints, authored in the UE EDITOR
        ui_smartTV, ui_mediaPlayer, ui_Settings, ui_LogViewer, ui_projector,
        listEntry_mediaPlayer, listEntry_ytRequest, screen_smartTV, screen_chatMonitor, ...
    Assets/Textures/UI/**      <- cog, x, trashcan, eyeopen/closed, media_buttons sprites
    Assets/{Actors,Enums,Functions,Materials,Meshes,Sounds,Structs}
```

`AntiRagdoll` in the same repo ships its editor sources (`src/ue/Content/Mods/AntiRagdoll/*.uasset`
+ `.umap`), which confirms the workflow: **author UMG in the editor -> cook -> pak ->
`Content/Mods/<Name>/ModActor.uasset` -> UE4SS's BPModLoader spawns it.** Note the `listEntry_*`
widgets — that is exactly the sortable-row browser shape this doc's decision table deferred.

### 2. **The "rejected — ties us to UE4SS" verdict has EXPIRED** `[V]`

The table above rejected the BPModLoader route on 2026-05-22 for one reason: it would tie us to
UE4SS. **The F2/D-3 decision of 2026-08-21 makes Multivoid a UE4SS mod** (`docs/UE4SS_ARC.md`), and
`BPModLoaderMod` + `BPML_GenericFunctions` are already installed in the r2modman profile — measured.
So that rejection's premise is gone, and the cost of the route collapses to **authoring** only.
What is still real, and unchanged: our pak toolchain (`tools/client_model/`, `ue_cook.py`) cooks
**skeletal meshes** in Python; it cannot emit a compiled Blueprint graph. Producing WidgetBlueprints
means the UE4.27 editor — **which the user HAS installed as of 2026-08-25**, so the ~80 GB objection recorded in `docs/COOP_CLIENT_MODEL.md` §9 is spent. What is NOT solved is the authoring channel: no MCP supports 4.27 (UE 5.5+ minimum on every implementation checked), so see §6 below.

### 3. What we can ALREADY do, and it is more than the doc implies `[V]`

The interactive-native-widget mechanism is **shipping today**, in the live main menu:

- We inject a real `UButton` at the top of `ui_menu_C`'s VerticalBox, **cloning `button_start`'s
  style** (`sdk_profile.h:162-190`: `UButtonSlot` padding/HAlign/VAlign, `FButtonStyle` @ `0x0128`,
  `ColorAndOpacity`, `BackgroundColor`), injected by a POST observer on `ui_menu_C::Tick` and
  re-injected on the death-menu (`net_pump.cpp:140-154`).
- **Clicks are POLLED, not delegate-bound** — `UWidget::IsHovered()` plus our own mouse state
  (`engine.h:540,566`; `engine_widget.cpp:492-495` forces Visible so the hover/click poll sees it).
  **This is the fact that makes the runtime route viable**: binding a UMG multicast delegate from
  raw reflection needs a UFunction on a UObject we own, and we never solved that — we sidestepped it.
- We build widget trees at runtime with no cooked asset: `SpawnObject` -> `UUserWidget` ->
  `WidgetTree` -> `UTextBlock` / `UVerticalBox` (`AddChildToVerticalBox`), plus `AddToViewport`,
  `SetPositionInViewport`, `SetAlignmentInViewport`, `SetVisibility`, `SetIsEnabled`,
  `SetRenderOpacity` (`sdk_profile_names.h:396-450`, `engine_widget.cpp` 604 LOC).
- We know the game's own menu font: **`font_ui`** (Share Tech Mono, `/Game/main/fonts/font_ui`),
  which `ui_menu` labels use at size 16.
- We already drive a cooked VOTV widget: `spawn_menu.cpp` finds the live `ui_spawnmenu_C` and opens
  it via `ExecuteUbergraph`.

### 4. Correcting this doc about itself

This file's header says the menu shipped as runtime UMG *"see the `ui/` modules:
`server_browser.cpp`"*. **`server_browser.cpp:3` says it is "rendered as an ImGui modal over VOTV's
main menu"** — i.e. the browser landed on the row the table marked *"rejected for the menu — fine for
dev/debug overlays only"*. The MULTIPLAYER **button** is native; the **panel it opens is not**. The
user's request is therefore not a new direction — it is this document's own decision, un-executed for
the one surface that matters most.

### 5. The fork, with the honest costs

| route | look | new toolchain | reaches "very soon"? |
|---|---|---|---|
| **B — extend the runtime-UMG we already ship** (scroll box + per-row `UButton`s, styled from `button_start`, clicks polled) | good, hand-styled; same font/style as the game | **none** | **yes** — the gap is composition, in our own code, not mechanism |
| **A — cooked WidgetBlueprints in a pak** (SmartTV's way) | best; designer-authored | editor: **HAVE IT**; cook path our Python chain does not have; authoring channel unsolved (§6) | **maybe — see §6** |
| C — keep ImGui, restyle | foreign either way | none | yes, but it does not answer the ask |

> **DECIDED 2026-08-25 (USER): route B — all of it in C++, no editor, no authored widget.**
> *"Тогда нафиг этот редактор, сделаем всё сами в c++."* Route A is a recorded-and-declined branch
> (§6's superseded box). The table below is kept because the COSTS in it are measured and answer
> "why not the editor" without re-digging — it is no longer a live fork.

**Recommendation (pre-decision, now the decision): B, or the B+art hybrid in §6.** A's loading half became free with D-3 and its
80 GB half is now paid, so the only thing still standing between us and A is HOW a widget gets
authored — §6. ~~The missing pieces for B are a scroll container and per-row buttons~~ — **counted
precisely 2026-08-25: SEVEN new UMG classes** (`ScrollBox`, `Overlay`, `SizeBox`, `Image`,
`HorizontalBox`, `EditableTextBox`, `CanvasPanel`) on top of the five the mod resolves today
(`TextBlock`, `UserWidget`, `WidgetTree`, `VerticalBox`, `Button`); none of it needs a new substrate.
**DESIGN, not built** — nothing here has been implemented. ~~and the `/qf` this project requires
before non-trivial implementation has not been run.~~ **The `/qf` RAN to convergence on 2026-08-25 —
six rounds, every one of which removed or corrected something. Its output is §7b (the style
reference), §7c (the look) and §8 (the build plan).**

### 6. The authoring channel — USER 2026-08-25: "UE 4.27 is downloaded, but there is no official MCP; maybe I get UE5/UE6, we build widgets over MCP, and you convert them down to 4.27"

Three things measured in reply, because the third leg of that plan is the one that breaks.

**(a) No MCP supports UE 4.27.** Every Unreal MCP implementation found targets UE5 — the most
complete one documents **UE 5.5+** as its minimum. The gap the user named is real.

**(b) The "convert down" leg is the wrong bet FOR WIDGETS, and this is the load-bearing finding.**
Downgrading assets is not impossible in general: UE refuses to load a package saved by a newer engine
(a deliberate `FPackageFileSummary` version check), but third-party *Asset Downgrader* tooling exists
that patches `.uasset` headers down to 4.27, and hand hex-editing the version block is a known trick.
**Both target static meshes, materials and textures — data assets.** A `WidgetBlueprint` is the worst
possible case for that path: it is a **compiled `UBlueprintGeneratedClass` carrying version-tied
bytecode**, wrapped around a `UWidgetTree` of UMG objects whose class layouts and property sets
changed across 4.27 -> 5.x. The downgrade tools' own copy says newer-version features "can't be
ported". Betting the server browser on round-tripping a compiled BP graph across a major version is
the kind of dependency this project should not take, and it would be **untestable until the very end**
— the failure shows up as an asset the shipped 4.27 game silently refuses to mount.

**(c) The premise the plan was built on has already dissolved.** The reason to reach for UE5 at all
was that 4.27 has no MCP. But **MCP is not the only authoring channel — UE 4.27 ships Python editor
scripting**, and a script is strictly better here than an MCP session: I can write it, it lives in
the repo, it is reviewable, re-runnable and diffable, and it produces the asset in the RIGHT engine
version with no conversion step at all. Creating the asset is documented for 4.27:

```python
tools = unreal.AssetToolsHelpers.get_asset_tools()
wb = tools.create_asset("ui_serverBrowser", "/Game/Mods/Multivoid",
                        unreal.WidgetBlueprint, unreal.WidgetBlueprintFactory())
```

`[?] UNVERIFIED and it is the crux:` **populating the `WidgetTree` from Python in 4.27** is reported
as not straightforward — `UWidgetTree` is poorly exposed to the 4.27 Python API. That needs a spike
before anything is planned on it. Do not treat this route as costed until it is run.

**(d) The route that needs none of the above — and its tooling is not a plan, it SHIPS `[V]`.** Our tree is already built at runtime by shipping code
(§3), so the pak does not have to carry a widget at all — it can carry **only art**: textures for the
row/scrollbar/button brushes, a `UBorder` background, an icon set, and VOTV's `font_ui` is already on
disk. No Blueprint graph means **no compiled class, so no downgrade problem, no Python `WidgetTree`
problem, and no MCP need** — and the art half is not a capability we would have to build.
**`tools/client_model/ue_tex.py` cooks a PNG into a UE 4.27 cooked `UTexture2D` package in pure
Python with NO EDITOR**, its byte layout validated against the game's own `tex_kel3_skin`
(*"19/19 both slots vs the template"*), it carries a `selftest`, and it is **already in production**
— the portable skin pipeline calls it on every build (`tools/client_model/portable/build/stage/__main__.py:234`).
The whole pak chain is the same shape: **we have never used the UE editor to ship an asset**;
`ue_cook.py` splices the game's own cooked template byte-for-byte and hands off to UnrealPak.
**Consequence for the ordering below: the editor/Python spike is blocking for route (a) ONLY. It
blocks nothing on the path we would actually walk.**
That is route B plus art, and it is the cheapest thing that closes the "we look dirty" gap the user
raised back on 2026-05-25.

> **SUPERSEDED THE SAME DAY — 2026-08-25 evening. The editor is OUT. Do not restart the spike.**
> An earlier decision this day read *"USER DECISION 2026-08-25: build via UE 4.27 Python — route (c),
> so spike `WidgetTree` population before designing anything on it."* **USER, later the same day:**
> *"Тогда нафиг этот редактор, сделаем всё сами в c++."* Everything in §6(a)/(b)/(c) is kept as a
> **recorded-and-declined branch** — the measurements are durable and answering "why not the editor"
> should never need re-digging — but **nothing below is a live plan.** What replaced it: §6e (the
> delegate finding, which removed the reason to want an authored widget at all) and the MTA
> precedent in §8. The written spike script was never run; it is not needed and is not in the repo.
> **`[V]` decision recorded from the user's own words, this session.**

**The engine is located `[V]` 2026-08-25** — the earlier "not found" note is retired. From the Epic
launcher manifests (`C:\ProgramData\Epic\EpicGamesLauncher\Data\Manifests`) and
`HKLM:\SOFTWARE\EpicGames\Unreal Engine`:

| build | path |
|---|---|
| **UE 4.27** | **`H:\UE_4.27`** — the target |
| UE 4.25 | `F:\Games\UE_4.25` — present, not ours |

Two things there matter more than the path. `H:\UE_4.27\Engine\Binaries\Win64\` holds
**`UE4Editor-Cmd.exe`** (the commandlet host), so the spike can run **headless** — scriptable and
repeatable instead of a hand session in the GUI. And `Engine\Plugins\Experimental\PythonScriptPlugin`
**is present** in this install, which is the precondition for any of it. Do not assume the invocation
form: 4.27's Python commandlet switch is itself worth confirming in the same spike.

### 6e. DELEGATE BINDING IS AVAILABLE TO US — the "we cannot bind, so we poll" premise was a MISSING PIECE, not a limit (2026-08-25)

**Origin: a USER question, and it is the single most consequential finding of this arc.** Told the
editor route's appeal was that an authored widget could wire its own clicks, the user asked
*"а может графы тоже сделаем?"* — then, when the answer went too broad, narrowed it precisely:
*"я про графы для биндинга делегатов."* The proposal was: ship a Blueprint purely to OWN a UFunction, so
there is something to point a delegate at. Chasing it down showed the Blueprint is not needed either.

**The premise, and it is real `[V]`.** `include/ui/multiplayer_menu.h:13` records why every click in
this codebase is polled: *"A reflection-only DLL cannot bind the `UButton::OnClicked`
FMulticastScriptDelegate (no UObject+UFunction to point it at)"*. Censused this session:
**zero occurrences of `FScriptDelegate` / `InvocationList` / any binding code in the entire tree.**
We have never bound one. So the comment is an accurate account of what was tried — but *"no UObject+
UFunction to point it at"* is a statement about **what we had built**, not about the substrate.

**Every piece the bind needs already exists, and each was verified this session:**

| piece | where | tag |
|---|---|---|
| the delegate is a plain `TArray<FScriptDelegate>` at a known offset | `UButton::OnClicked` **@ 0x03C8, size 0x10**; also `OnPressed` 0x03D8, `OnReleased` 0x03E8, `OnHovered` 0x03F8, `OnUnhovered` 0x0408 | `[V]` CXXHeaderDump `UMG.hpp:284` |
| a delegate-dispatched BP event **reaches our ProcessEvent hook** | `docs/COOP_DISPATCH_VISIBILITY.md:81` — the game's OWN inventory buttons (`ui_playerInventory.BndEvt__Button_a_drop`): *widget delegate → PE* = **VISIBLE**, called "expected" there; :210 — `delegate → BP` is an OUTER door | `[V]` |
| weak-pointer construction for the target | `reflection.h` — `InternalIndexOf()` + `SlotSerial()`, both already public | `[V]` |
| engine-side allocation for the array | `reflection.h` — `EngineAlloc()` / `EngineFree()` | `[V]` |
| the FName half | `fname_utils.h:24` — `StringToFName()`; or simply `NameOf()` on the resolved UFunction | `[V]` |
| observe the resulting call, and CANCEL it if its body matters | `game_thread.h:86,124` — `RegisterInterceptor(targetUFunction, cb)`, *"returning true cancels the original"*; 17 live of `kMaxInterceptors=40`. Also `RegisterPreObserver` / `RegisterPostObserver` | `[V]` |
| `FScriptDelegate` = `{TWeakObjectPtr, FName}` = 16 B | UE engine layout; the dump does not export engine struct internals, and the 0x10 multicast size is consistent with it | **`[RD]` — NOT dump-verified. Measure in the spike.** |

**The shape that falls out — and it needs no asset, no editor, no pak.** A delegate target only has to
be *some* UObject carrying a void/no-param UFunction. So: spawn our own sink object, point the
delegate at a no-param UFunction the engine already provides, register an interceptor on that
UFunction, and **discriminate by the `self` pointer in the callback** — interceptors are keyed on the
UFunction, and the callback receives `self`, so **N sink objects share ONE function name and stay
distinguishable**. Cancel in the callback if the borrowed body would do anything.

~~**Why this matters more than the click itself:** the same mechanism reaches
`UEditableTextBox::OnTextChanged` / `OnTextCommitted`, which **dissolves the input half's risk**
(§8 step 2b) — the one part of the runtime route that was measured-hard, via the
`HasKeyboardFocus`-is-an-exact-widget-test trap in `coop/input/input_owner.h:64-69`.~~
**That risk turned out not to need this.** Measured 2026-08-25: `input_owner.cpp:379` filters its
focus walk by `DerivesFromUserWidget` **only**, not by "is a game class" — so our own `UUserWidget`
taking user-0 focus already flips `GameOwnsText` and the WndProc swallow already backs off.
**Typing into a native surface works with no new input code and no delegate.** `UEditableTextBox` is
a real Slate widget that owns keyboard, caret and IME itself; its value is read with `GetText()`.

**STATUS: `[RD]`, NOT `[V]`. Every LINK is measured; the COMPOSITION has never run.**

> **SCOPE, decided by the converged `/qf` (round 3, 2026-08-25): the bind is OUT of v1.**
> This section proves binding is **possible**; nothing makes it **necessary**. The shipped
> release-edge poll (`multiplayer_menu.cpp:244-262`) already reads a real `UButton` with no new
> primitives, and OPUS §7 endorses poll-and-diff where dispatch is in doubt. Dropping it removed the
> design's **last unmeasured leap** — the shape above needs "a no-param UFunction the engine already
> provides", and *which* one was never answered: interceptors key on the **UFunction**, so borrowing
> any existing one routes every other caller in the game through our callback, while minting our own
> means building a `UFunction` from a DLL that owns no `UClass`.
> **§8's probe still measures it** (on a throwaway button we spawn and never attach — a bind is a
> WRITE, not a read), so the v2 decision to retire the poll is made on evidence rather than by
> default. The arc this browser serves retires the **ImGui substrate**, not polling.

### 7. Should the EXISTING main-menu elements move to the pak too? NO — measured, and a pak would make them WORSE

USER 2026-08-25: *"кнопки в главном меню (MULTIPLAYER, Multivoid in left upper corner) будет лучше
заменить на нативный из нашего будущего pak? Или не стоит? Они хуже чем нативные через .pak?"*

**They are not worse. They are already the maximally-native form, and the pak version would be a
downgrade.** Both existing elements do not merely *look* like the game — they are the game's own
widget classes carrying the game's own style, **read off the live menu at runtime**:

> **CORRECTED within the hour, 2026-08-25 — the first version of this section overstated the
> mechanism, and the correction weakens one of its three arguments to nothing. Read this table, not
> the earlier claim that both elements "clone the live style".** The two are NOT built the same way:

| element | how it is built | what it actually takes from the game |
|---|---|---|
| **MULTIPLAYER button** | `engine::InjectCanvasButton(refButton, label, &out)` (`engine_widget.cpp:373-500`) | **not a style clone.** It walks `refButton`'s slot -> parent `UVerticalBox` and adds our `UButton` as a SIBLING there, so **slot + layout parity is structural**. The label's text style is **hardcoded from a MEASUREMENT** — `font_ui`, Size 16, ShadowOffset (2,2), black shadow, *"verified: `bp_reflection/ui_menu.json` tex_btnStart/tex_btnstat"* — and the label is then **deliberately tinted CYAN** to mark the coop entry. The source comment ends *"(Font parity is deferred.)"* |
| **the version / update line** (the upper-left "Multivoid …" text) | `engine::InjectTextRowAbove(refText, ...)` (`engine.h:551-564`) | **a real clone**: VOTV's own `txt_version` text style (font/colour/shadow/justification) **and** its row's slot layout (padding/alignment), inserted as one more row in the same `UVerticalBox` |

So the arguments, re-stated honestly after reading the implementation:

1. ~~**A clone tracks the game; an authored asset freezes it.**~~ **RETRACTED — this was wrong for the
   button.** `InjectCanvasButton` writes **frozen constants** measured once from `ui_menu.json`; it
   does not re-read `button_start`. A pak asset would freeze values too, so on this axis the two are
   **equivalent, not favourable to us**. (It remains true for the version LINE, which does clone at
   runtime — but that is one element, not the pair.)
2. **The version line and the button are CHILDREN of the menu, so lifetime and layout are free.**
   Both are inserted into the menu's own `UVerticalBox`. `InjectTextRowAbove`'s header states the
   consequence: *"a child of the menu, so it auto show/hides with the menu (no viewport add/remove,
   no per-frame gating)"*. A pak widget added to the viewport needs that gating **written and
   maintained by us**, plus its own position/DPI handling. **This is the strong argument and it
   survives intact.**
3. **A pak buys custom art, and custom art is not the goal here.** We want to be indistinguishable
   from `button_start` and `txt_version`. That is a parity problem, and parity is cheaper to reach by
   measuring the reference widget than by drawing a new one that must then be kept matching.

**Residual this correction exposes — and the obvious fix for it is FORBIDDEN by a measurement,
which is the part not to re-derive.** Our own source says *"Font parity is deferred"*, so
button-label parity is **incomplete today by admission**, and nobody has compared our button to
`button_start` on screen. The reflex fix is *"re-read the reference widget's style at inject time
instead of using frozen constants"* — **that was TRIED and REVERTED, and the frozen constants ARE
the fix.** `engine_widget.cpp:403-409` records why, verbatim: *"We do NOT clone a reference
UTextBlock (tex_btnStart): its pointer is null at some inject timings, which fell through to a
Roboto / centered / white default — the 'wrong font, indented, wrong colour' bug."* So:

- **Do not propose runtime style-cloning for the BUTTON.** It is not an un-done improvement, it is a
  rejected design with a named failure. (`InjectTextRowAbove` clones safely only because
  `txt_version` is a menu property that is always live at ITS inject timing — the difference is the
  reference pointer's lifetime, not the technique.)
- **What "font parity is deferred" actually leaves open** is narrower: `TypefaceFontName` is left
  None (correct for font_ui, a one-face font) and the constants were read from `ui_menu.json` once,
  in 2026-07. If the labels look wrong TODAY, that is a **stale-measurement** question — re-dump
  `tex_btnStart`'s style and compare it to the six constants — not a technique question.
- Either way the answer to *"should it move to a pak?"* is unchanged: **no**. A pak asset freezes the
  same values with none of argument 2's free lifetime.

**Reality check before spending anything here: the CYAN tint is deliberate**, so our button is
*supposed* to differ from `button_start`. If "хуже" meant the colour, nothing is broken at all.

**So the rule this section establishes, and it is the one to build by:**

> **CLONE anything the game already has an equivalent of. Use the PAK only for art the game has no
> equivalent of.**

The main menu is entirely the first case. ~~The browser's interior is entirely the second: a row
background with zebra/hover/selected states, a scrollbar, a column header, and status icons
(locked / version-mismatch / mic / ping) have **no counterpart in `ui_menu_C` to clone**. That is
precisely where hand-built UMG looks thin — which is the user's own 2026-05-25 complaint (*"VT mod
looks natural; we look dirty"*) — and precisely where authored art earns the toolchain.~~

> **CORRECTED 2026-08-25 by §7b's measurement — and this correction is why the pak was re-declined.**
> The browser's interior is ALSO the first case. The claim above scoped its search to **`ui_menu_C`**
> and concluded "no counterpart exists"; the counterparts were one level down, in the sub-screens
> `ui_menu_C` owns. Measured: the row background + its hover/selected states are
> `uicomp_saveSlot.Image_background` (the row's own `UButton` is *invisible* — `DrawAs: NoDrawType`
> on all three states — so the tint IS the state); the scrollbar is `ui_settings.scrollboxRoot`'s
> `WidgetBarStyle` on `inst_uiScroll`; the column-header bar is `inst_uiButtonDown` (what the
> settings `UExpandableArea` headers use); and the status icons have slots waiting for them —
> `uicomp_saveSlot` carries `Image_img` and `img_mid` beside its five text blocks.
> **Everything the browser's interior needs already exists in the game and can be cloned.**
> A search that stops at the parent widget is not a search for "does the game have this".

### 7b. The two native menus, dissected — the STYLE REFERENCE (`[V]` 2026-08-25)

The user asked for this directly: *"Полезно разобрать нативное меню настроек игры и меню создания
игры. Стиль такой же будет и у нас."* Both were dissected from the CXXHeaderDump **and** from the
cooked assets (`tools/bp_reflect.py` -> UAssetAPI export JSON), because the header gives structure
and only the asset gives style values.

**Note on «меню создания игры»: it is TWO screens, and reading it as one was an error caught in
`/qf` round 3.** `Uui_saveSlots_C` is the new-slot FORM (name / days / map / rules);
`Uui_gamemode_C` is the mode PICKER. Both are dissected below.

#### The structural vocabulary

| screen | what it is |
|---|---|
| `Uui_menu_C` | owns **13** sub-screens as child `UUserWidget`s. Three switchers: `screenSwi @0x350` (2: screen/loading), `switcher_1 @0x358` (3: a slideshow), **`switcher_widgets @0x360` (11: every sub-screen)**. `[V]` measured from `WidgetSwitcherSlot` parentage in the cooked asset, not from the field name. |
| `Uui_saveSlots_C` | structurally a server browser already: `UScrollBox ScrollBox_list @0x380`, `TArray<Uuicomp_saveSlot_C*> Slots @0x400`, `selected @0x420`, `ETB_slotName @0x330`, `cbox_sboxLevel @0x318` (map), `checkbox_dontShow @0x320`, `Owner @0x5A8`, buttons play/launch/updatelist/back/delete/duplicate/createNew |
| `Uuicomp_saveSlot_C` | ONE ROW as its own `UUserWidget` (0x330): an **invisible** `UButton button_select` (`DrawAs: NoDrawType` on Normal AND Hovered AND Pressed), a separate `UImage Image_background` carrying the state tint, **5 `UTextBlock`s + `Image_img` + `img_mid`**, `Parent`, `ID`, `upd(int32)`. **Row height 64 px.** |
| `Uui_settings_C` | `UScrollBox scrollboxRoot` + 6 `UExpandableArea` + ~200 named `Uuicomp_settingsSlot_C*` + `URichTextBlock rtb_desc` (the hover-description sink) |
| `Uuicomp_settingsSlot_C` | ONE **polymorphic** row (0x34C): a `UWidgetSwitcher` picks checkbox / combo / 2 sliders / 2 textboxes / button by an int `variableType`; `Button_hover` is a full-row invisible button whose `OnButtonHoverEvent` pushes `Description` into the parent's description pane |
| `Uui_gamemode_C` | the create-a-game screen (0x580): 8 mode buttons, `UScrollBox helpbox_desc` + `tex_desc` driven by `setDesc(int32)` on hover, and **three `TArray<Uuicomp_settingsSlot_C*>` (`sliders`/`v_slots`/`s_slots`) + `Fstruct_settings1 settingsCopy`** — i.e. **the game reuses the settings ROW inside a non-settings screen.** That is the shape of our host-a-lobby form. |

Three idioms worth copying, each measured in more than one screen:

1. **Description-on-hover** — settings' `rtb_desc`, saveSlots' rule description, gamemode's `tex_desc`.
   A house idiom, not a one-off.
2. **The whole row is an invisible button over a tinted background image** — not a styled button.
   *(We copy the LOOK — a tinted background image carrying the state — but **not** the button: see
   §8's "Why no row button". A control that draws nothing in every state is a hit-target, and we
   already hit-test in C++ for hover.)*
3. **Every native interaction is a BOUND DELEGATE** (`BndEvt__..._OnButtonClickedEvent__DelegateSignature`);
   the assets carry a `ComponentDelegateBinding` export. See §6e — and see §8 for why we still poll in v1.

#### The palette `[V]` — 9 materials, 2 textures, 2 fonts, 2 sounds

All under `/Game/`. **This list is the union of BOTH menus; a census of `ui_saveSlots` alone
undercounts it by three assets and one font**, which is how it was first got wrong.

| asset | role | seen in |
|---|---|---|
| `textures/ui/inst_uiBackground` | panel fill | saveSlots + settings |
| `textures/ui/inst_uiBackgroundUp` | raised fill | saveSlots |
| `textures/ui/inst_uiBorder` | window border | saveSlots + settings |
| `textures/ui/inst_uiButton` | button, all 3 states | saveSlots + settings |
| `textures/ui/inst_uiButtonDown` | **ExpandableArea header**, tint 0.6 (29 uses) | settings |
| `textures/ui/inst_uiDrag` | **slider handle** | settingsSlot |
| `textures/ui/inst_uiScroll` | **scrollbar thumb**, 16x16 | settings |
| `textures/ui/inst_uiSelectbox` | combo box | saveSlots + settingsSlot |
| `textures/ui/inst_uiTextbox` | editable text box | saveSlots + settingsSlot |
| `textures/ui/ui_checkbox_check` / `_uncheck` | checkbox, 32x32 | settings |
| `main/fonts/font_ui` | **12** (settings rows) / 16 (labels) / 20 / 24 (slot name) | all |
| `main/fonts/font_terminal` | a **second** face | settingsSlot |
| `audio/ui/buttonclick`, `audio/ui/buttonrollover` | press + hover | all |

Brushes are `DrawAs = Box` with `Margin 0.5` (9-slice). State tints: hover **0.8** grey, pressed
**0.5**; checkbox hover 0.6 / press 0.3; ExpandableArea header 0.6. Colours: text-box foreground
**(1, 0.1, 0)**, heading yellow (1,1,0), destructive red (1,0,0), orange (1,0.5,0), scrim black
A=0.5. `ExpandableArea.Style.RolloutAnimationSeconds = 0` — instant, no animation.

**Scrollbar: the two menus differ, and the browser takes the settings treatment.**
`ui_saveSlots.scrollbox_list` sets no style at all; `ui_settings.scrollboxRoot` sets
`WidgetBarStyle` (Normal/Hovered/Dragged thumb -> `inst_uiScroll`, ImageSize 16x16),
`ScrollbarThickness (16,16)`, `AlwaysShowScrollbar true`. A server list is the long-list case.

> **A trap that cost real time here, recorded so it is not repeated.** Every `FSlateBrush` in the
> asset JSON carries BOTH `ResourceObject` (an import index) and `ResourceName` (a string). Several
> sub-brushes read `ResourceName = "../../../Engine/Content/Slate/Common/Button.png"` **while**
> `ResourceObject` points at `/Game/textures/ui/inst_uiButton`. Reading `ResourceName` concludes
> "VOTV uses stock engine Slate art" — **false**. `ResourceObject` wins at runtime.

### 7c. What it will LOOK like (`[V]` for the shape, DESIGN for the layout)

> **USER DECISION 2026-08-25, on being shown this gap: *"it's fine, will fix styles later — the main
> thing is to properly lay out foundation, build something per rule 1 properly first."* So the visual
> verification below is EXPLICITLY DEFERRED, and the FOUNDATION (§8's probe, placement, lifetime and
> input seams) outranks it.** The gap is left standing and named rather than quietly closed.
>
> **HONEST GAP, named by `/qf` round 11 and NOT yet closed: there are no screenshots here, and the
> palette below was read out of asset JSON — nobody has LOOKED at the two menus.** The standing rule
> is *where eyes are needed, screenshot and show*
> (`memory/feedback_show_screens.md`), the user's first verb was «как будет **выглядеть**», and
> `tools/mp.py` already ships window capture (`_capture_window`, used by the arc-B board shots). So
> the values in §7b — tints 0.8 / 0.5, `Margin 0.5`, `DrawAs=Box`, the (1, 0.1, 0) text-box
> foreground — are **structurally sourced and visually unverified**. Capturing `ui_settings` and
> `ui_saveSlots` and putting them beside this mockup is owed before the style is called agreed;
> it needs the game running, which is shared with a parallel session, so it is a scheduled step
> rather than an assumption.

The user's question was «как будет **выглядеть**». The answer is: **like `ui_saveSlots` with our
columns** — same window frame, same row metrics, same font, same scrollbar as the settings list.

```
+--------------------------------------------------------------------+  <- UCanvasPanel frame
|  MULTIPLAYER                                          [ Refresh ]  |     inst_uiBackground
|--------------------------------------------------------------------|     + inst_uiBorder
|  Name                   Players  Version        World      Age    ||  <- header row (sortable)
|--------------------------------------------------------------------||
| [#] Pelmentor's server     3/8    0.9.0n b143   Meadow      2s    |#|  <- 64 px rows
| [#] LAN test               1/4    0.9.0n b143   Meadow     11s    |#|     UScrollBox +
| [*] locked lobby           5/8    0.9.0n b143   Meadow      4s    |#|     inst_uiScroll thumb
| [!] old build              2/8    0.9.0n b122   Meadow      7s    | |     (16x16, always shown)
+--------------------------------------------------------------------+
|  Nickname [____________]              [ Host Game ]  [ Connect ]   |
|  Direct   [____________]                             [  Back   ]   |
+--------------------------------------------------------------------+
```

**Columns — the five `UTextBlock`s, mapped from `coop::net::lobby::LobbyRow`:**

| # | column | field | note |
|---|---|---|---|
| 1 | Name | `name` | widest, left-justified, `font_ui` 16 |
| 2 | Players | `playersCur` / `playersMax` | `3/8` |
| 3 | Version | `game` + `" b"` + `proto` | the Paper pair, e.g. `0.9.0n b143`. **`version` is a LEGACY fallback used only when `game` is empty** (a pre-field host) — the producer stopped sending it when the mod-semver axis was retired (`lobby_announcer.cpp:36-39`) |
| 4 | World | `world` | |
| 5 | Age | `ageSec` | **heartbeat age, not ping** — by design; ping is measured post-connect via GNS and MTA's ASE-UDP pre-query was deliberately dropped |

**Not columns.** `lobbyId` is internal and never rendered. `locked` and `direct` are **icons**, which
is why the native row carries `Image_img` and `img_mid` beside its five text blocks — the shape fits
without being bent. **`proto` is NOT demoted to a tint — it is IN column 3's text** (the `b143`). The version
cell is additionally highlighted on a **two-leg** mismatch — `game != GameTarget()` **OR**
`proto != kProtocolVersion` — drawn amber `(1.00, 0.78, 0.35)` with a `(!)` suffix, while matching
rows are dimmed. This is ported verbatim from `server_browser.cpp:218-231`, not re-derived.

**Sort** is a header click, resolved in pure C++ over our `std::vector<LobbyRow>` — zero engine cost.

**Search — DEFERRED to v2, on a named trigger (USER 2026-08-25: *"do what's best"*).** The first
version of this line deferred it *"because it needs the live-filter path through `UEditableTextBox`"*,
and `/qf` round 9 showed **that reason is empty**: v1 already carries the Nickname and Direct-IP
fields and already resolves `SetText`/`GetText`, so the path is paid for either way.

The honest reason is that **search has no denominator yet.** A filter over the handful of rows the
master currently returns costs vertical space in the chrome and buys nothing; sort is free by
comparison (pure C++ over the vector, zero engine cost) and covers the same need at this scale.
**Trigger to build it: when a full list no longer fits without scrolling as a matter of course** —
i.e. when the browser stops being a list you read and becomes one you hunt in. Recorded as a trigger
rather than a "no" so the next reader knows what would change the answer, per this doc's own
re-decline convention.

**The host form** follows `ui_gamemode`'s shape (§7b): mode/option rows plus description-on-hover,
built from **our** widgets — never the game's classes (§8).

### 8a. P1 HAS RUN — the measurement, and what it changed (`[V]` 2026-08-25, b143)

> **Read this before the plan below it.** §8 is still the design of record, but **five of its
> statements are now measured and two of them were WRONG**. Instrument:
> `src/coop/dev/native_ui_probe.cpp` (the game-thread census + RUNG 1) and
> `src/coop/dev/worldless_frames.cpp` (RUNG 0), armed by `[dev] native_ui_probe=1` /
> `native_ui_probe_write=1`, driven by `python tools/mp.py nativeui`. Solo, MENU scenario, no
> save, no session. DLL `multivoid-0.9.0n-143.dll`, **proto 143 unchanged** (no wire change).
> **This is a real log from a real run, not a smoke marker** — but it is a LAB run, not hands-on.
> The full measurement record, including the six instrument defects found and fixed during the pass,
> is `research/findings/tooling/votv-native-umg-p1-probe-MEASUREMENTS-2026-08-25.md` (local-only
> corpus, per `docs/DOCS_ARC.md`).

| # | question | answer | consequence |
|---|---|---|---|
| **O1** | do the UMG classes + UFunctions resolve, each on its OWNING class? | **44/44, 0 missing** — including `UPanelWidget::AddChild`, which resolved nowhere in the tree before | P2 is unblocked on reflection |
| **O5** | is a donor brush's `FSlateResourceHandle` (+0x70) populated? | **0/4 populated across 3/4 brushes that DO carry a `ResourceObject`** (`ui_saveSlots_C.button_back`) | **P0 IS DISARMED.** There is no refcount bug and the hands-on-verified inject is not touched |
| **O7** | are §8's style donors resident at menu time? | **every one RESIDENT** — but only when read off the switcher's own child (see the trap below) | the donor table works; **one row was wrong** |
| **O8** | `UButton::OnClicked` layout | **`+0x3C8`, `num=1`**, entry = `BndEvt__button_NewGame_K2Node_ComponentBoundEvent_0_OnButtonClickedEvent__DelegateSignature` | the v2 retire-the-poll decision now has evidence; v1 still polls |
| **A5** | which sub-screen sits at which switcher index? | measured, table below | placement indices are no longer guesses |
| **RUNG 1** | does a hand-wired, never-`Initialize()`d `UUserWidget` render inside `UWidgetSwitcher`? | **RENDERS.** `AddChild` 11→12 children, index 0→11, `GetDesiredSize` (0,0)→**(623,39)**, screenshot confirms the magenta text on screen, index restored to 0 and `RemoveChild` back to 11 | **the 12th-child placement HOLDS** |
| **RUNG 0** | frames presented while no world exists? | boot: **~540 frames / 11.4 s with our task pump frozen**; a level travel presents **1 frame**; `UNKNOWN-AND-FRESH=0` over 11,298 frames | **O4 IS ANSWERED: two substrates, permanently.** ImGui is not retirable — see below |

**`switcher_widgets` child map `[V]`** — 11 children, `ActiveWidgetIndex=0` at the main menu:

| 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 |
|---|---|---|---|---|---|---|---|---|---|---|
| `CanvasPanel` | `ui_settings_C` | `ui_help_C` | `ui_keybinds_C` | `ui_saveSlots_C` | `ui_credits_C` | `CanvasPanel` | `ui_langs_C` | `ui_achievements_C` | `ui_gamemode_C` | `ui_stats_C` |

**THE SWITCHER IS AN OVERLAY LAYER, NOT THE MENU — and nothing said so.** With
`ActiveWidgetIndex = 11` (our throwaway), the screenshot still shows the title, the button list,
the patron column and the credits. So index 0's `CanvasPanel` is the *empty* state, the main-menu
chrome lives **outside** this switcher, and swapping the index **does not hide the menu**. Every
sub-screen must therefore be painting its own opaque background — which is why §8's donor table
already leads with a panel fill and a border, and why **P2 must paint one too**. Recorded because
"the 12th child replaces the menu" was an unstated assumption in every version of this plan.

**CORRECTION 1 — the donor table names a field that does not exist.** `image_border_*` is **not a
member of `ui_saveSlots_C`** in this build (it exists on `ui_cheatMenu`, `ui_objectUpgrades`,
`ui_spawnmenu`, `uicomp_dishStatusSlot`, `uicomp_signalSlot`, none of them menu-resident). The
probe carries that row deliberately so the log says so out loud. The measured border candidate on
that screen is **`Image_6`** (`@+0x340`, resident); the panel fill is `Image_0` (`@+0x338`). The
table below is left as written with this correction stapled to it rather than silently edited,
because the *reason* the row was wrong — it came from a §7b prose sweep, not from the class — is
the thing worth keeping.

**CORRECTION 2 — `FindObjectByClass` does not answer "the live one", and it produced a false
finding for one whole round.** Read through `R::FindObjectByClass` alone, **every** widget field on
`ui_saveSlots_C` and `ui_settings_C` came back null, while both instances were alive as switcher
children. Read as a finding, that says the sub-screen donors do not exist until the screen is shown
— which would have forced this design to add a precondition it explicitly rejected when it dropped
the row-instance donor. Measured with **both** pointers side by side: `FindObjectByClass` returns a
**different non-CDO instance** (a `WidgetBlueprint` carries a tree template that is *not* named
`Default__`, so the CDO skip at `reflection.cpp:511` does not exclude it), and the switcher's own
child has every donor resident. **Any donor read in P2 must come from the switcher child list, never
from a class-wide lookup.**

**CORRECTION 3 — O5's own instrument nearly published a verdict stronger than its evidence.** The
first run read `ui_menu_C.button_start` and found 0/4 handles set, and said "there is no handle
bug". Its four brushes carry **no `ResourceObject` at all** — and a handle is a *cache of a
resource*, so zero from a brush with no resource says nothing about a brush with one. The verdict
is now gated on `ResourceObject` (`INCONCLUSIVE` when none of the four carry art) and the answer
above comes from `ui_saveSlots_C.button_back`, which does. **This is also a fact about the shipped
inject worth keeping: `ui_menu_C`'s own menu buttons carry no brush art**, which is why
`InjectCanvasButton`'s clone produces a native-looking button without ever loading a texture.

**RUNG 0 — O4 IS ANSWERED, AND THE ANSWER IS "TWO SUBSTRATES, PERMANENTLY" `[V]`.** Three runs
(`python tools/mp.py nativeui --travel`), the last of them `ALL PASS`, boot -> gameplay ->
`AmainGamemode_C::transition("/Game/menu")` -> menu, i.e. **two real level travels in one process**:

```
RUNG0 EDGE Unknown  -> Other    after  540 frames / 11391 ms  (pumpFrozen 539, stale 1, fresh 0)
RUNG0 EDGE Other    -> Gameplay after    1 frame  /  6687 ms  (pumpFrozen 0, stale 0, fresh 1)
RUNG0 EDGE Gameplay -> Other    after    1 frame  /  4625 ms  (pumpFrozen 0, stale 1, fresh 0)
RUNG0 periodic: frames=11298 | unknown=540 gameplay=1 other=10757
                | PUMP-FROZEN frames=539 (longest run 539 frames / 10297 ms) stale=3 fresh=10756
                | UNKNOWN-AND-FRESH=0 | tasksRun=3685 degraded=0
```

Three facts, and each one is load-bearing:

1. **A LEVEL TRANSITION PRESENTS ONE FRAME.** Measured in both directions — 1 frame across 6.7 s
   entering the map, 1 frame across 4.6 s leaving it. There is nothing to draw into during a
   travel, for **any** substrate, so the transition window — the thing this run was built to
   sample — turns out not to be the question at all.
2. **THE BOOT WINDOW IS THE QUESTION, AND IT IS ~540 FRAMES OVER ~11.4 s** (longest contiguous
   frozen run 539 frames / **10 297 ms**), reproducible at 478 / 527 / 540 frames across three
   runs. Throughout it the world reads `Unknown` **and** `GT::TasksRun()` does not advance:
   the game is presenting at ~46 fps while our game-thread task pump is not draining at all.
3. **`UNKNOWN-AND-FRESH = 0` across 11,298 frames.** Outside boot, the game never presented a
   single frame with a *current* reading of "no world". The whole phenomenon is the boot window.

**Why that closes O4 rather than merely describing it.** Every UFunction this mod calls —
`SpawnObject` included — reaches the engine through `GT::Post`, which drains inside our
ProcessEvent detour. In a pump-frozen window we therefore **cannot create or drive a UMG widget
at all**, and independently there is no world, so no `UGameViewportClient` to `AddToViewport`
into. For ~10.3 seconds and ~540 presented frames at every launch, a native UMG surface is
unreachable from our layer in two ways at once, while ImGui's Present hook — which lives on the
render thread and needs neither — drew every one of those frames. `boot_warning_dialog` is
exactly a surface that must appear there.

**So: the ~3,700 LOC of overlay substrate is NOT retirable, and "two substrates permanently" stops
being a live possibility and becomes the measured answer.** The browser can be native without that
being a step toward retiring ImGui, and the two facts are now independent.

**One nuance kept on purpose, because it bounds the claim.** The pump being frozen does not mean
the *engine's* game thread is blocked — it fed the render thread 540 frames, and VOTV's own
loading screen is UMG and animates. What is frozen is **ProcessEvent dispatch**, which is what our
substrate is parasitic on. So the honest statement is *"WE cannot reach UMG during boot"*, not
*"UMG cannot draw during boot"*. For the decision it makes no difference — we are the ones who
would have to build the widget — but a future attempt to shrink that window should aim at reaching
the game thread without ProcessEvent, not at UMG.

**A consequence outside this doc:** `docs/OVERLAY_CAPTURE_COEXIST.md` §6's arc cannot resolve for
free by moving the draw to a native UMG surface. That option is now measured shut, and the RTSS/OBS
fix needs its own `FD3D11Viewport::PresentChecked` seam as originally designed.

**What P1 did NOT answer — ANSWERED 2026-08-26 by RUNG 2 (`12f44b68`), before a line of P2 was
written.** Both were gating: a false answer to the first invalidates the whole hit-test design with
no plan B, and the second decides whether the subtree needs a GC pin.

| # | question | answer `[V]` |
|---|---|---|
| **HOVER** | does a bare `UImage` with `Visibility=Visible` answer `IsHovered()`? | **YES, and it DISCRIMINATES.** Cursor inside a bounded 400x64 rect -> hovered=**1**; cursor at the client centre, outside it -> hovered=**0**. The root tracked it 1->0; the text block the cursor never touched read 0 both times. RUNG 1's `rootHovered=1` was exactly the full-bleed artefact it was suspected to be. |
| **GC** | does a hand-built subtree survive a purge? | **SURVIVES.** `ForceGarbageCollection()` mid-hold: children still 12, root still (654,64). The UPROPERTY chain from the live switcher (`UUserWidget::WidgetTree` @0x1D8 -> `UWidgetTree::RootWidget` @0x28 -> `UPanelWidget::Slots` @0x108 -> `UPanelSlot::Content` @0x30, all reflected) roots it, so **`AddToRoot` is NOT used and would be wrong** -- it would outlive the menu and demand a paired un-root (`reflection.h:109`). |

**And the instrument was wrong first, in the exact way section 8 predicted for the WndProc.**
Sampling `IsHovered()` in the SAME tick that moved the cursor read the PREVIOUS position every
time -- hover-ON sampled 0 while the five following periodic samples all read 1; hover-OFF sampled
1 while the five following all read 0. Move and sample are now separate phases 600 ms apart. So
"evaluate hover on the next game-thread tick, never in the WndProc" is **measured**, not argued.

Still out of v1 by decision: the delegate BIND.

---

### 8b. P2 IS BUILT AND ON SCREEN — AS-BUILT (`[V]` 2026-08-26, `fe7e55eb`)

> **Read this and §8a before §8.** §8 is the plan; this is what exists. Nothing below is
> hands-on: every claim is a real log or a real screenshot from a lab run.

**Shipped dark.** `[dev] browser_native` defaults to **0**: with it off the screen is never
constructed and the ImGui browser (`ui/server_browser.cpp`) is still the ONLY browser and still
what the MULTIPLAYER button opens (`multiplayer_menu.cpp:273`). There is deliberately no moment
where two browsers serve one button.

**What is built.** `ui/server_browser_native.{h,cpp}` (564 LOC) + `ue_wrap/engine/umg_build.{h,cpp}`
(the panel/style primitives) + two `[dev]` rows + `python tools/mp.py browser` (the lab run).
Driven from `multiplayer_menu`'s existing `ui_menu_C::Tick` observer -- deliberately NOT a second
observer on the same UFunction; that inject is the one hands-on-verified native path we have and
one owner of the menu tick is the point.

**The evidence.** `mp.py browser` -> ALL PASS, and `research/browser_shots/browser_native.png`
shows the screen over the live menu with **two real lobbies fetched from the production master**:
name, `1/4` players, `0.9.0n b133 (!)` in amber (b133 != our b143 -- the two-leg mismatch ported
verbatim from `server_browser.cpp:218-231`), world, age, and the status line. font_ui throughout;
window fill cloned from `ui_saveSlots.Image_0`; scrollbar style cloned from
`ui_settings.scrollboxRoot` (nine brushes).

**The scrim is FUNCTIONAL and is measured as such.** It is what eats a click that misses the
window, so "it looks dim in the screenshot" is not evidence. A dev self-check moves the cursor
outside the window and asks Slate: hovered **1** there, **0** inside (the window's own widgets sit
above it). Two-sided, so a scrim that answered true everywhere would not pass.

**`[V]` THE NATIVE BACKDROP IS A SCRIM, NOT AN OPAQUE PANEL -- §8a's consequence line was
imprecise and is corrected here.** `ui_saveSlots_C`'s widget-tree root has FOUR children and child
0 is `Image_302`: a `CanvasPanelSlot` with `Anchors.Maximum (1,1)` and zero offsets (so
full-screen) carrying `Brush.TintColor.SpecifiedColor = (0, 0, 0, 0.5)` and **no `ResourceObject`
at all**. So every native sub-screen DIMS the menu rather than hiding it, a tint-only `UImage`
draws a solid rect (visible directly in the RUNG 2 screenshot as a cyan block), and **the browser's
backdrop needs no donor** -- which shrinks what a null donor can block.

**`[V]` The placement's layering, measured statically from the cooked asset**
(`research/bp_reflection/ui_menu_fixed.json`): `switcher_widgets` fills the screen, its container
`CanvasPanel_101` is index **8** of the `screen` canvas, `canvas_menu` (the title + button list,
including our injected MULTIPLAYER button) is index **5**, and `imgCur` -- VOTV's own cursor image
-- is index **12**. So we paint above the chrome and Slate hit-tests us first, while the cursor
still draws above us. That last fact is why the 12th-child placement beats `AddToViewport`: a
viewport widget with a Z high enough to clear the menu would draw over the game's own cursor.

**`[V]` The game's own navigation is `SetActiveWidgetIndex(N)` + a refresh call on the screen**
(`umg_saveSlots->gen()`, `->upd()`, `->start()`) -- decoded from `ExecuteUbergraph_ui_menu`.
`widgetEnter` is written at exactly FIVE sites (`umg_settings`, `umg_help`, `umg_credits`,
`umg_achievements`, `umg_stats`) and **never** for `umg_saveSlots` or `umg_gamemode`; `canvMenu` is
touched once in the whole ubergraph by a `SetRenderOpacity` fade and never by a navigation, and
`Hidden` never. So not writing `widgetEnter` is the native-faithful choice for our class of screen
-- **but `OnKeyDown` clears it only on its OWN ESC path**, so leaving Settings by its back button
leaves a stale pointer and ESC over our screen would run `umg_settings->resume()`. Hence the screen
RECONCILES against the live switcher index every tick instead of trusting its own flag.

**Two invariants the code enforces, both from `/qf` rounds:**

1. **Row identity is the `lobbyId`, never an index into the network list.** `master.rs:531-553`
   emits `state.lobbies.values()` from a `HashMap` and `lobby_client.cpp:57-75` never sorts, so one
   host leaving while another joins gives the SAME COUNT in a DIFFERENT ORDER -- positional
   identity is invalid by construction, not merely racy. A row remembers the id it was RENDERED
   with, captured in the same pass as its text by the single writer.
2. **Rows are never detached.** Nothing roots a detached row -- the panel's `Slots` is the only
   reference -- so shrinking COLLAPSES surplus rows, keeping them rooted and un-hit-testable.

**`Open()` is a deferred INTENT** with a 20 s TTL, consumed only inside a MAIN-menu tick
(`isPause == false`). That is a first-hand positive observation and is stronger than any memoised
world reading; it also cannot fire over gameplay, which matters because the harness reopens the
browser from four failure-recovery paths in `session_runtime.cpp`, sometimes before a menu exists.

**INPUT IS NOW MOSTLY WIRED (2026-08-26, `95d18cc5` + `ea378daf`).** This paragraph used to say the
opposite and is rewritten rather than patched:

- **Chrome: BUILT.** An `X` in the title strip and `BACK` at the footer's left, both real `UButton`s
  cloning `ui_saveSlots_C.button_back`'s style (so they carry the game's press/hover sounds). Driven
  proof, not assumption: the self-check re-opens the screen after its ESC phase and synthesizes a
  press-release over the X, asserting through the public `IsOpen()` -- real log,
  `CLOSE BUTTON PASS ... (hovered=1)`.
- **Hover: BUILT.** `UpdateHover()` recolours the hovered row's TEXT to `#FFFF00`
  (`VOTV_UI_STYLE.md` §4 -- the native treatment is a text change, not a background one).
- **Row click / selection: BUILT.** A click on the hovered row sets `g_selectedId` and the row's FILL
  becomes `#400040`. Keyed on the LOBBY ID, never the index -- the master returns lobbies from a
  `HashMap` and nothing sorts them, so an index-keyed selection silently follows whatever lands at
  that position.
- **`NotePointerMoved()` is GONE** (`ea378daf`, RULE 2). It was declared and defined and called from
  nowhere, with `g_pointerMoved` written and never read. `UpdateHover` uses a `GetCursorPos` delta
  instead: one syscall, no edit to the overlay's input path, and it sees movement regardless of how
  the message was routed. **Do not go looking for that symbol -- this doc named it as live until this
  sweep.**

**STILL OPEN, and named rather than implied:**
- **`Close()` has zero callers and has never run** -- re-verified against the code this sweep
  (`grep server_browser_native::Close` over `src/` returns nothing as of `23481e3c`; the definition
  stands at `server_browser_native.cpp:767`). ESC and the X both call the INTERNAL `Hide()`. So the
  cross-thread `g_wantClose` path is still unproven, and T1's "its first caller must not be its first
  proof" is still owed.
- **Connect / Host / Refresh are absent.** Selection exists and nothing consumes it yet; wiring
  Connect to `g_selectedId` is the next thing the screen needs to be usable end-to-end.
- **The retire is not done.** `ui/server_browser.{h,cpp}` still ships. Its seam census is bigger
  than a name grep found: besides the nine `server_browser::` call sites, `imgui_overlay.cpp:721-724`
  opens it from `VOTVCOOP_BROWSER_OPEN` and logs `"server browser starts visible"`, `:796` closes it
  in `Shutdown()`, `harness.cpp:37` includes it, and **`tools/cursor_probe.py:39,42` and
  `tools/master_fetch_probe.py:78,89` both drive that env var and BLOCK on that literal log line.**
  They must be retargeted in the same commit -- `cursor_probe`'s subject is ImGui's
  `RenderMouseCursor` under capture, which the native browser does not hold, so it moves to
  `VOTVCOOP_MENU_OPEN=1` rather than being renamed.
- **§7c's style gap is unchanged**: nobody has captured `ui_settings` / `ui_saveSlots` themselves.
- Cosmetic: the window has no border (only the fill donor is used), and column widths are fixed
  weights with an 18 px gutter + `ClipToBounds` rather than measured.

---

### 8c. The STRESS HARNESS — REQUESTED 2026-08-26, DESIGN, sequenced after C2

> **USER REQUEST, verbatim:** *"Need a stress test functionality for the native browser, where we
> test robustness in different areas, for example filling in the list with 100 random servers and me
> as a tester scrolling there, getting how it feels, then update, then add another 100 servers to
> the list. But after we build it properly (for now i dont even see the X to close server browser
> window)."*
>
> **Two things in that, and the order is the user's:** the harness is wanted, and it is explicitly
> gated behind the browser being finished — which the missing close control is the evidence for.

**A SCOPED EXCEPTION TO A STANDING RULE, recorded because it contradicts one.**
`memory/feedback_autonomous_evidence_is_the_ceiling.md` says a plan that ends at "the user will
test it" is a shelf. Here the user has **volunteered as the tester for FEEL** — *"me as a tester
scrolling there, getting how it feels"* — and feel is the one axis an autonomous run genuinely
cannot judge. So this lane has two halves and they are not interchangeable: the ROBUSTNESS half is
machine-asserted and must stand on its own, and the FEEL half is the user's verdict. The exception
covers feel only; it does not license shipping the rest unmeasured.

#### 8c.-1 THE PERFORMANCE DESIGN — `/qf` RUN, 2026-08-26 `[V]` measurements / `[RD]` plan

> **USER:** *"How to do this properly and not eat performance too much deserve a qf in the next
> session."* Then, answering the one policy question this design put to them: *"5 seconds is good."*

**THIS SECTION REPLACES THE PRE-`/qf` COST MODEL WHOLE.** The text that stood here until 2026-08-26
said the per-sync cost was "≈9 reflected calls" per row and "a few percent of dispatch volume". Both
are **wrong**, and so was the arithmetic built on them. The `/qf` ran **twelve rounds** and every one of
them overturned something; the corrections are recorded below rather than quietly patched, because
**ten** of them were things this doc — or a message to the user — had already asserted as fact: the
"9 reflected calls" cost model, "200 rows / 1800 UWidgets", the "ms/s" framing, the 157 ns unit
cost, a players-desc sort default, deleting the background fetch, "mp.py builds and overwrites the
DLL", p99 as the metric, T0 as a zero-code probe, and the Rejected list's claim that a local test
master duplicated the harness seeder.

**STATUS, stated rather than implied:** the *measurements* below are `[V]` and cited. The *plan* is
`[RD]` — nothing in T1–T8 is built, and no number in those rows has been produced by a run. The loop
had not returned "that holds" when this was written; an earlier revision of this heading claimed it
had converged, which was a status label ahead of its evidence.

**T0 IS THE EXCEPTION: IT RAN 2026-08-26 AND THE ANSWER IS YES `[V]`.** Four synthesized wheel
notches moved the list's **view fraction 0.0000 → 0.0667** against a one-row threshold of 0.0460,
with the positive control passing first (a forced offset moved it **0.0000 → 0.7245** over a
1391-unit maximum). Screenshots corroborate — scrollbar thumb at the top before, at the bottom
after — and **the user confirmed it by hand during the run** (*"скроллится нормально"*), which is the
evidence class that outranks the instrument. Commit `a2879d7a`; instrument
`ui/server_browser_selftest.cpp`, driven by `python tools/mp.py browser --fake-master 30`.

**So T2b, T4a and T6 keep their premise, and the "no step prices making it scroll" hole never had to
be filled.** What T0 also produced is a UMG fact worth more than its own row — see the box below.

**THE UMG FACT T0 PRODUCED, and the reason it outlives this lane `[V]`.**
`UScrollBox::GetScrollOffset` **is an echo**: it returns Slate's `DesiredScrollOffset` — what was
last *asked for*, unclamped. Ask for 1 000 000 and it returns 1 000 000, measured on an empty box
*and* on 30 rows carrying 1391 units of real overflow. A Set/Get round-trip through it is a tautology
and **cannot fail**, which is the one property an instrument must not have; the first version of T0's
control failed the entire probe on exactly this, a true observation aimed at the wrong question.

Read these instead:

| call | what it actually reports | trust for |
|---|---|---|
| `GetViewOffsetFraction` | the scrollbar's own distance-from-top, 0..1 — physical post-layout state | **"did it move"** |
| `GetScrollOffsetOfEnd` | content extent minus viewport extent — real geometry (1391.0 against 30×64 rows in a ~529 px viewport; the arithmetic closes) | **"is there anywhere to go"** |
| `GetScrollOffset` | the last requested offset. Not a measurement of anything. | context only |

The generalisation, and it is not confined to scroll: **before reading a UMG getter to confirm a
write landed, check whether it is reading back your own request.** All three are wrapped in
`ue_wrap/engine/umg_build.{h,cpp}`.

**A LOOSE END, stated rather than hidden:** the passing run reported `hovered=0` — the wheel scrolled
the box while `IsHovered()` on it read false. The verdict is movement, so this does not touch the
YES, but the "the notches went somewhere else" branch that leans on that reading **would have
misfired had the answer been NO**. RUNG 2 measured `IsHovered` as answering on a bounded `UImage`;
whether a `UScrollBox` behaves the same is now an open question, and T7 depends on it.

**MEASUREMENT PROTOCOL, so a gate is a decision and not a coin flip.** Every threshold below is read
from **≥5 repeats**, reported as **median and max**, at each of 0 / 50 / 100 / 200 rows. A gate whose
threshold falls inside the run-to-run spread is **not decidable** and must be re-cut before it is
used — the first draft of T4b failed exactly this test, thresholding at 1 ms against its own
predicted 0.5–1.5 ms residue.

**Two properties of the metrics, each of which invalidated an earlier draft of this section:**

1. **The frame metric is cadence-SENSITIVE; the `Scope` metric is not.** `Scope` asks "does ONE sync
   push a frame over budget", which is independent of how often syncs happen. Stalls-per-second is
   not: 5x fewer syncs is 5x fewer stalls. So the cadence must be **fixed at its shipping value
   before the baseline is taken** (hence 5 s in T2b, not T4a) — otherwise the cadence change alone
   pays for T4b's gate and the diff looks unnecessary for a reason that has nothing to do with it.
2. **A percentile cannot see a rare stall.** One stalled frame per sync is 0.17% of frames at 5 s —
   roughly a SIXTH of the 1% p99 cut (not "an order of magnitude", as an earlier draft of this
   very sentence said), so p99 reports a normal frame either way. Use **max** and
   a **count of frames over 2x the median**. An earlier draft specified p99 and would have measured
   nothing.

##### The cost model, corrected `[V]`

**The dispatch count was never the problem. An uncached full-array walk was.**

`RowPartsAt` resolves `R::FindFunction(cw, L"GetContent")` **per row, per sync**
(`server_browser_native.cpp:209`), and `R::FindFunction` (`reflection.cpp:485-497`) has **no result
cache** — it walks the entire `GUObjectArray` on every call. Its sibling `R::FindClass` was *given*
that cache one day earlier (`ca1cd5e4`, 2026-08-25, the coin-gun fix); `FindFunction` never was.
`BuildRow` and the window build resolve `SetContent` the same uncached way (`:188`, `:361`) —
**once per row BUILT**, which is the growth burst.

One such walk is priced by the project's own instrument, in a real log from this build:

```
[WALK-TIME] sync:event_cue = 2317 us      <- a Tick whose own comment says its "only cost is
[WALK-TIME] sync:event_cue = 3220 us         the FindObjectsByClass GUObjectArray walk" (x2)
```

**≈1.1–1.6 ms per full walk** at `NumObjects=182767` (same log). For scale, the whole dispatch cost
this design was originally built around is ~0.6 ms **per second** (`detour self avg=157 ns`, and note
that figure is **detour self-time, engine-excluded** — the full cost of a `ProcessEvent` dispatch
remains **unmeasured**).

##### `kMaxRows = 64` bounds the whole loop, not just the display `[V]`

`want = min(rows, 64)`, the grow loop is bounded by `want`, and `total = ChildCount` therefore never
exceeds 64 (`:247-266`). Consequences, in order of how badly each was mis-stated before:

| condition | walks per sync | cost |
|---|---|---|
| today (~2 live lobbies) | ~2 | ~2–3 ms — **invisible, which is why nobody has seen this** |
| the current code's ceiling (64 rows) | 64 | **70–100 ms**, 576 UWidgets |
| the user's 100 / 200 | — | **cannot occur until the cap is raised** |

So the walk fix is a **prerequisite for raising the cap**, not an optimisation beside it. And
`kMaxRows`' own comment — *"the master caps its list well below this"* — is **FALSE**: `build_rows`
(`master.rs:531-553`) emits every listed lobby, unbounded. 100 servers would render 64, silently.

##### It is a SINGLE-FRAME STALL, not a background tax `[V]`

`SyncRows` runs inside one `ui_menu_C::Tick` post-observer, so every walk in a pass lands in **one
frame**. Quoting this as "70–100 ms/s" (as an earlier draft of this section and two messages to the
user did) understates it: it is **one frame at ~100 ms among neighbours at ~8.5 ms**. A visible
once-per-second hitch. **The deciding metric is therefore max frame interval + a STALL COUNT,
never mean FPS and never p99.** A percentile cannot see this: at 117 fps one stalled frame per sync
is **0.85% of frames at 1 Hz and 0.17% at the shipping 5 s cadence** — both below the p99 cut, so
p99 reports a normal frame whether or not the defect exists. A rare-but-severe event needs a **max**
and a **count of frames over 2x the median**; the percentile was chosen before that arithmetic was
done.

##### The instruments named in the old text do not work here `[V]`

- `perf_probe::Sample()` and `[HITCH]` both live in `net_pump::Tick` (`net_pump.cpp:437`, `:416`),
  which `session_runtime.cpp:651-653` gates on `running || idleInGameplay`. **Neither runs at the menu.**
  "Adding a `browserSync` bucket is a two-line change" yields a bucket nobody prints.
- `worldless_frames.cpp:136` stamps frames with `::GetTickCount64()` (**~15.6 ms granularity**) while
  `perf_probe.cpp:73` already uses QPC. At 117 fps a frame is ~8.5 ms — **below the clock's
  resolution**, so a p99 built on that counter is quantisation, not measurement.
- `pe_detour.cpp:104-110` (`RecordCbBodyNs`) *does* already bracket matched observer bodies and keep
  worst + `UFunction*`, and `SyncRows` runs inside one such body. The instrument is not missing; the
  **printer is unreachable at the menu**.

##### The list has no stable order, and that is a correctness defect `[V]`

`master.rs:534` iterates `state.lobbies` (a `HashMap`) with no sort, nothing sorts client-side, and
`SyncRows` writes **by position**. So "then add another 100 servers" **reshuffles the list under a
scrolling hand** — unusable at scale, independent of any performance question. The header's
invariant 1 (row identity is the `lobbyId`) protects the *click* from resolving to the wrong server;
it does nothing about the visual reshuffle.

**MTA's precedent is explicit** (`CServerBrowser.cpp:1402-1410`): every column carries a sort key,
each suffixed with `strTieBreakSortKey` — a deliberate **total** order so equal values cannot
reshuffle — and `:1411` comments *"The row index could change at any point here if list sorting is
enabled"*, writing by handle rather than index. `:2583` adds a persistent iterator + a 250/frame cap
+ a **33 ms frame budget**; `:2641` a per-server content revision.

**Sort on a STABLE key** (`name`, tie-broken by `lobbyId`) — **never** on mutable `playersCur` /
`ageSec`. A server gaining a player must not jump. A deterministic reshuffle is still a reshuffle,
and the invariant a user feels is *"nothing moves under my pointer"*. A "freeze reordering while
hovering" layer was designed and **dropped**: with a stable key it suppresses a symptom whose cause
is already fixed.

##### The native idiom, and where it does NOT transfer `[V]`

The game's own list screen (`research/pak_re/ui_saveSlots.json`) is a `ScrollBox` with one
`uicomp_saveSlot` widget per row (`AddChild` x9), refreshed by an explicit `button_updatelist` ->
`updateList`, **not a timer**. So **widget-per-row IS the native idiom** and our divergence is the
repaint, not the shape.

**But the precedent does not transfer to discovery.** A save list changes only by user action; a
server list changes externally, and seeing a server that came up while you are looking is what a
browser is *for*. An earlier draft deleted the background fetch entirely on this precedent and
rationalised it as idiom fidelity — that was an over-correction. **Cadence of record: fetch on open
+ explicit Refresh + a 5 s background refresh (USER-SET, 2026-08-26).** The 1 Hz full re-fetch goes:
an HTTP GET per second per client against the production master is indefensible on its own.

##### The plan, ordered, with pre-registered thresholds

Each step is gated on the previous. Thresholds are written down **before** the runs, so
"the complexity is not earned" stays a reachable outcome rather than a courtesy.

| # | step | gate |
|---|---|---|
| **T0** | **DONE 2026-08-26 — THE ANSWER IS YES `[V]`.** The wheel reaches this widget and scrolls it: view fraction 0.0000 → 0.0667 over four notches (one row = 0.0460), after a forced-offset control passed 0.0000 → 0.7245 against a 1391-unit maximum. User-confirmed hands-on. Two things this row got wrong before it ran, kept because both are traps: the probe was NOT zero-code (it needed the fixture AND a positive control), and its first PRECONDITION was `offsetOfEnd > 0`, which **an empty ScrollBox satisfies by reporting 1.0** — so the whole control ran against a box holding nothing. The gate now takes two terms, rows AND a full row of overflow, and neither is an epsilon test. | none |
| **T1** | **DONE 2026-08-26 (`95d18cc5`) -- the X and BACK exist and a driven click proves the X closes the screen** (`CLOSE BUTTON PASS`, hovered=1, real log). Real `UButton`s cloning `ui_saveSlots_C.button_back`, so they carry the game's press/hover sounds; the clone has ONE owner now (`umg::CloneButtonStyle`, extracted from `InjectCanvasButton` under RULE 2). BACK sits at the footer's LEFT -- it was first built bottom-RIGHT, which is the CONFIRM position in every native window. **`Close()`'s cross-thread test is NOT done**: ESC and the X both call the internal `Hide()`, so the public path still has no caller and no proof. | T0 |
| **T2a** | **A WALK COST WITHOUT `NumObjects()` ON THE SAME LINE IS NOT A MEASUREMENT (2026-08-26, measured).** `R::FindFunction` walks the whole `GUObjectArray`, so its cost is a function of the object count -- and `[V]` that count moves by an ORDER OF MAGNITUDE inside one boot: **106 469 -> 17 770 -> 182 767**, same process, same session (parallel-session dual-mod smoke, 2026-08-26). So *when* you sample dominates everything else, and two of your OWN runs are silently incomparable unless each cost line carries its denominator. A foreign pak is a rounding error beside it: `[V]` same run, same build, `NumObjects` 182 767 (no LogicMods pak) vs 182 768 (pak mounted) -- but treat that ONLY as *a mounted-but-IDLE pak does not visibly move the count at our sampling points*, because DebugMod's C++ half failed to load so nothing ever instantiated from the pak, and host-vs-client is two different code paths rather than a control. A real control is the same role with and without. **The instrument**: re-clock the menu frame counter to QPC; frame-interval **max + stall count** (NOT p99 — see above); call `perf_probe::Sample()` at the menu; a `Scope` around `SyncRows`; a MENU-TICK-vs-PRESENT counter. Behaviour-neutral. **Shown RED by an injected stall** before it is trusted. | none |
| **T2b** | **The rig**: raise `kMaxRows` + LOG truncation; the section-8c seeder; scroll drive; **and the 5 s cadence, moved here from T4a**. The frame statistic is cadence-SENSITIVE — 5x fewer syncs is 5x fewer stalls — so baselining at 1 Hz and re-reading at 5 s would let the cadence change alone pay for T4b's gate. Do not baseline a cadence you intend to discard. | none |
| **T2c** | **BASELINE** at 50 / 100 / 200 rows. | — |
| **T3** | **READS -> RAW** (`Slots@+0x0108` / `Content@+0x0030`) + **WRITES -> FnCache** (`GetContent`, `SetContent` into `umg_build.cpp`'s table). Re-measure. | **not earned if** at 50 rows the `SyncRows` Scope median < 4 ms AND the max frame interval exceeds the 0-row baseline max by < 8 ms |
| **T4a** | **Ungated**: stable order; scroll-offset preservation across structural change (`GetScrollOffset`/`SetScrollOffset`, both resolved, both unused today — section 8c.3 ceiling #4). Ship on their own merits. (The 5 s cadence was here; it moved to T2b as a measurement confound.) | none |
| **T4b** | **The content diff** keyed on `lobbyId` + locally-derived age (coarsened or visible-only). | **not earned if** after T3 the `SyncRows` Scope median at 50 rows < 4 ms — i.e. one sync cannot push a frame over budget. Read AFTER T4a so the cadence is already 5 s. |
| **T5** | **The full section-8c harness**: phases A–G incl. D (shrink), E (shuffle at constant count), G (GC), plus section 8c.4's un-gated id-reconcile selftest **shown RED first**. **BLOCKED ON the F1 hazard below** — the MANUAL feel phase hands the user a keyboard, and F1 kills scrolling TODAY. | — |
| **T6** | **Decide the row model.** Outcomes: keep widget-per-row (**live**), viewport pool (~9 widgets / 81 UWidgets vs 576), or `UListView` (a spike — see below). | T5's machine verdict |
| **T7** | **Row input and row FEEDBACK** against the decided model. **USER, 2026-08-26, after scrolling the native list by hand:** *"скроллится нормально, правда выделения нету у позиций в списке как у imgui браузера"* — it scrolls fine, but **rows have no highlight**, which the ImGui browser does have. That incumbent gets both halves free from one call: `ImGui::Selectable(label, g_selected == i, SpanAllColumns)` (`server_browser.cpp:193-196`) draws `HeaderHovered` under the cursor AND `Header` on the selected row. The native side already owns the surface to paint — every row carries a `bg` `UImage` at overlay child 0 (`RowPartsAt`), today a flat tint — so this is a tint-per-state, not new geometry. It needs a per-row hover read, which is exactly what the `hovered=0` loose end above puts in doubt: settle that FIRST, on a row box, before designing the feedback. | T6 |
| **T8** | **THE FLIP — AND THE DELETE IS CANCELLED. USER DECISION 2026-08-26**, verbatim: *"пусть imgui server browser пока живёт, даже когда он уйдёт на пенсию, пусть будет как запасной вариант"* (let the ImGui server browser live for now, and when it retires let it stay as the fallback). So T8 is: flip the MULTIPLAYER button to native BY DEFAULT and give the ImGui one a deliberate way to be reached — the `browser_native` config row inverts from a dev flag into a user-facing fallback switch. **This is an explicit RULE-2 exception on the user's own call, and it is a product decision about WHAT they want, not a HOW that RULE 1 could override.** **WHAT «запасной вариант» MEANS IS AN OPEN QUESTION AND IS THE USER'S TO ANSWER — not settled here.** An earlier draft of this row asserted a permanent both-surfaces capability obligation; that was INVENTED, not measured, and is withdrawn. Two facts bound the question and both are `[V]`: today the ImGui surface is the MORE capable one (it draws the `locked` "L" at `server_browser.cpp:186`; the native one writes five cells — name/players/version/world/age, `server_browser_native.cpp:521-527` — and no lock at all), and the ONLY way to reach it is `browser_native` in the ini plus a restart (`config_registry_rows.inc:345`, `CFG_FLAG(..., "dev", false, ...)`), so a player whose native browser breaks cannot fall back DURING the session that broke. Feature-equal and last-resort-connect are very different builds; pick one before T8. **What T8 now deletes: nothing.** `tools/cursor_probe.py` and `tools/master_fetch_probe.py` keep working unchanged; `BrowserOpen()` stays in `AnyOpen()`/`CaptureActive()`; `harness.cpp:37`'s include stays. `multiplayer_menu`'s dead `g_buttonInputBlocked` is dead on its own merits and is filed separately from this row. | T7 |

**Instrument-to-question mapping** (without this, a T4b re-measure passes on a broken build, because
the post-T3 residue of ~0.5–1.5 ms sits *under* what a frame statistic can resolve against 8.5 ms
frames):

| instrument | answers | gates |
|---|---|---|
| `SyncRows` Scope (QPC, µs) | is **our code** cheap | T3, T4b |
| frame-interval **max + stall count** | does the **list** cost frames | T6 |

**The verdict is MACHINE, and it is two-part.** Per `[[feedback-autonomous-evidence-is-the-ceiling]]`
(USER 2026-08-25: *"When im on pc i wont test it either"*), section 8c's scoped feel-exception does
**not** license making T6 *depend* on a hands-on. Correctness = the un-gated id-reconcile selftest +
phases D/E/G. Performance = max + stall count + the `SyncRows` attribution. **Feel corroborates; it never
gates.**

**The product requirement is "no perceptible cost at REALISTIC loads"** — tens of lobbies, which is
what the master will plausibly serve. The user's 100/200 is a **robustness assertion**, not a ship
gate. That is what keeps "keep widget-per-row" a live and possibly winning outcome of T6.

##### Open, and honestly so

- **Nobody has confirmed the mouse wheel scrolls this widget.** Nothing in the file touches a scroll
  API and no wheel event has ever reached it. `[V]` de-risked but not proven: `WM_MOUSEWHEEL` is
  swallowed **only** under `CaptureActive()` (`imgui_overlay.cpp:316-320`) and
  `server_browser_native` appears **nowhere** in that file, so the wheel reaches the game while our
  screen is the only surface up. **T5's first assertion.**
- **HAZARD, and it blocks T5, not T8.** Pressing F1 while the browser is open flips `CaptureActive()`
  true (`imgui_overlay.cpp:162` — the predicate includes `MenuOpen()`) and **the list stops
  scrolling, today**. This was first filed as a blocker on flip day; that was wrong by four steps —
  **T5's MANUAL phase hands the user a keyboard and asks them how scrolling feels**, so the hazard
  bites there first. And deleting the ImGui browser removes `BrowserOpen()` from that predicate but
  **not** `MenuOpen()`, so the hazard survives T8 and needs its own answer regardless.
- **Whether a viewport pool feels right while scrolling** is unmeasured. `SListView` re-points inside
  the layout pass; we would re-point from `ui_menu_C::Tick`. Note the pump rate (~0.34 tasks/frame at
  the menu, from the P1 probe data) is the **wrong proxy** — we run from a ProcessEvent post-observer,
  not a posted task — and **nothing measures menu-tick-vs-present**, which is why T2a adds that counter.
- **`USlateBlueprintLibrary` is unresolved.** `FGeometry` has no reflected members (empty, size 0x38),
  so click-by-arithmetic needs it; `IsHovered`-per-row is the measured alternative. Note the release-edge
  poll makes `IsHovered` trivial for **clicks** (once per click) — it is **hover highlight** (per frame,
  per row) that this actually gates.
- **`http_client.cpp:153` caps a response at 1 MiB**, logs on hit, and returns `ok=true` with a
  truncated body — surfacing as *"master sent a malformed list"* rather than "too large". At ~340 B/row
  worst case that is ~3000 rows, so **not a ceiling at 200**; the misleading surface is a separate
  small finding.

##### Filed separately, deliberately not smuggled into this lane

**`R::FindFunction` has no result cache, and 476 call sites depend on callers remembering to latch.**
This is the **second instance in two days** (`ca1cd5e4` cached `FindClass` on 2026-08-25), which per
`[[feedback-recurring-bug-is-architectural]]` means the level is wrong. **Precision, so the next
reader does not overclaim it:** `BeginClassWalk` memoises the class being *searched for*, not the
*result* — `FindObjectByClass` / `FindObjectsByClass` / `FindActorsByClass` all still walk, and
`FindClass` caches a result only because its result *is* a class. So a `FindFunction` result cache
would be a **new** cache, not parity with its siblings. It needs its own measurement and its own
commit.

##### Rejected, with reasons, so they are not re-derived

- **Filter or paginate instead of rendering N rows.** What real browsers do; dissolves the cost
  question. **Rejected on the ask** — the user explicitly wants to *scroll* 100 servers and judge feel.
- **Render the list as one multi-line text block.** O(1) widgets. **Rejected** — destroys per-row
  hit-testing and hover.
- **Cancel the native browser, keep ImGui** (which clips 1000 rows trivially). **Rejected** — reverses
  a decision settled and measured in sections 8 / 6e; the native route's justification was visual
  integration, not capability.
- **`UListView`** (the engine's virtualised list, `UMG.hpp:769`, `EntryWidgetPool` @ `UListViewBase`
  +0x0140). **`[V]` VOTV uses it NOWHERE** — zero hits across the whole reflected BP set — so there is
  no donor `EntryWidgetClass`, no in-game precedent, and whether `UListViewBase` accepts a foreign
  entry class at runtime without a `check()` is unknown. **A spike, not a plan**; T5's run probes it.
- **A second seeding mechanism.** An earlier draft rejected "a local test master" as duplicating the section-8c harness seeder. **That rejection was WRONG and is retracted: they are the SAME artifact.** The harness needs a synthetic row source; a local master IS that source; there was never a second implementation to reject. Shipped as `tools/fake_master.py`.
  **Why not `tools/coop_master_server.py`, which already serves `/v1/lobbies` and is already wired to the game by `master_fetch_probe.py:77`:** that one is a master EMULATOR carrying the production DoS semantics — `MAX_LOBBIES_PER_IP = 8` (`:103`) and `RL_CREATE` 10 per 60 s (`:108`), with every seed arriving from 127.0.0.1. Eight rows against a ~7.6-row viewport is **0.4 rows of scroll**, which reproduces the two-lobby problem one level down; and relaxing those caps would degrade the fidelity that is the emulator's whole point. A **fixture** (return N rows, mutate instantly, deterministic per seed) and an **emulator** (lobby lifecycle, heartbeats, TTL, rate limits) are different concepts, and section 8c.3 already required the fixture: *"the harness must seed synthetically and BYPASS the master"*.

#### 8c.0 The blocker the user named, and it is worse than a missing button `[V]`

**`server_browser_native::Close()` HAS NO CALLERS** (grep, 2026-08-26). With `[dev]
browser_native=1` + `browser_autoopen=1` there is no way to dismiss the screen from inside the
game: no X, no Back, and **ESC is a measured no-op at our switcher index** (§8's `OnKeyDown`
disassembly — the interface cast fails and the `ActiveWidgetIndex == 0` test fails). The
reconcile-on-index only fires if something ELSE moves the index, and nothing will.

So the screen **stranded the player at the menu**. This is precisely the hazard §8 wrote down for
the RUNG 1 probe — *"a probe that can trap the user is not read-only in any sense that matters"* —
which is why that probe got a hold deadline and an auto-restore. The browser got neither. It is
dev-gated and off by default so no shipping player could reach it, but anyone who turned the flag on
to look at the screen was trapped, which is exactly what happened.

> **FIXED the same day (2026-08-26). `[V]` ESC now closes the screen**, polled in the observer that
> already runs (`GetAsyncKeyState`, press edge, primed on show so a held key at open cannot close
> it). Deliberately not swallowed: the game's own handler still runs and is a no-op at our index, or
> navigates away on a stale `widgetEnter`, which the reconcile then observes — either path ends
> closed. Kept inside this TU: no edit to the overlay's input path or to the one hands-on-verified
> inject. Evidence: `hidden (ESC; index was 11, ours 11)` in a real `mp.py browser` run, with the
> index restored.
>
> **PRECISELY: the TRAP is fixed, `Close()` is NOT exercised.** ESC calls the internal `Hide()`
> directly. The PUBLIC `Close()` — and therefore the whole cross-thread path it exists for
> (`g_wantClose` atomic → consumed on the next menu tick → `Hide`) — **still has zero callers and
> has never run.** C2 wires it from `host_save_picker` and from the harness's four recovery paths,
> and must not assume it works because ESC does; they are different code paths.
>
> **AND THE FIRST VERSION OF THAT SELFTEST LIED ALL-PASS, twice over, which is the part worth
> keeping.** (1) It synthesized `keybd_event` down+up back-to-back in one tick — invisible to a
> per-tick poll, because the key is released before the next tick samples it. The hatch did nothing
> and the run was green. (2) The assert searched the log for `hidden (ESC` — and the selftest's own
> instruction line contained the literal string `'hidden (ESC...)'`, **so the assertion matched the
> sentence describing what it was looking for.** A predicate that can match its own instrumentation
> is not a predicate. Both fixed: the key is held across several ticks, the instruction line no
> longer quotes its expected output, and the assert keys on `hidden (ESC;`.

#### 8c.1 What must exist first (C2), because the harness cannot be driven without it

Chrome as real `UButton`s cloned from `ui_saveSlots.button_back` (they get both native sounds free
from the cloned `FButtonStyle`): **Back / close**, Refresh, Connect, Host. Plus hover highlight and
row click. Until a human can close the window and press Refresh, "scroll it and tell me how it
feels" is not a runnable instruction.

#### 8c.2 The phases — the user's sequence, plus what it should also cover

| # | phase | what it actually tests |
|---|---|---|
| **A** | seed **100** synthetic rows, user scrolls | build cost at scale; scroll smoothness; scrollbar thumb; wheel feel |
| **B** | let the **background refresh** run (5 s as of T2b — NOT the 1 Hz this row used to name), then press Refresh, **while scrolled down** | does the scroll POSITION survive a sync; does the row text flicker; stall count / max frame interval |
| **C** | grow **100 → 200** | pool growth mid-scroll; does the view jump; does the scrollbar rescale |
| **D** | shrink **200 → 50** | surplus rows are COLLAPSED, not detached — do they take zero space in the ScrollBox |
| **E** | **shuffle at CONSTANT count** | the invariant the whole design rests on: the id a row was RENDERED with, not its index. This is the phase that would have caught the `HashMap`-order defect, and it is the one the user's own sequence does not include |
| **F** | hostile strings: 64-byte names, non-ASCII (the repertoire/fold path), empty `game`/`world` | clipping + gutter; the UTF-8 boundary; the legacy `version` fallback |
| **G** | `ForceGarbageCollection()` mid-scroll | the subtree survives (RUNG 2 proved it for 12 children, not for 200 + collapsed) |

**Two modes, same seed.** MANUAL (the user drives; phases advance on a dev hotkey) for feel;
AUTONOMOUS (`python tools/mp.py browserstress`, timed phases, screenshots, log asserts) so the
robustness half is reproducible and survives without a human.

#### 8c.3 Four ceilings the harness will hit before it tests anything `[V]`

These are known defects in what is already committed, not predictions:

1. **`kMaxRows = 64`** (`server_browser_native.cpp:40`). The user's "100 random servers" would
   silently display **64**, with no log line. The cap must be raised and, more importantly, must
   LOG when it truncates — a silent ceiling in a stress harness invalidates the whole run.
   **Now step T2b**, and section 8c.-1 measured why it cannot be raised *first*: it bounds the whole
   sync loop, so raising it before the walk fix converts a latent defect into a 110–320 ms stall.
2. **`SyncRows()` is unconditional — there is no edge-apply.** **The cost model that stood here is
   SUPERSEDED by section 8c.-1 — read that, not this.** It said "≈9 reflected calls" and "thousands
   of ProcessEvent dispatches per second"; the `/qf` measured the real cost to be an **uncached
   `R::FindFunction` full `GUObjectArray` walk per row per sync** (`:209`), ≈1.1–1.6 ms each, landing
   in **one frame** — three orders of magnitude past the dispatch count, which is itself ~0.6 ms/s.
   Edge-apply is still wanted (step T4b) but it is not the root, and it is gated on a measurement.
3. **`sm::Refresh()` fires a real network fetch every second** (`:559`). The harness must seed
   synthetically and BYPASS the master — hammering the production master 200 rows deep for a UI
   test is not acceptable, and the numbers would be polluted by fetch latency anyway. The 1 Hz
   re-fetch itself is retired in **T2b** (fetch on open + Refresh + a 5 s background, USER-SET) —
   **it moved there from T4a** because the frame metric is cadence-sensitive and a baseline taken at
   1 Hz would let the cadence change alone pay for T4b's gate.
4. **Nothing preserves the scroll offset across a sync.** `GetScrollOffset`/`SetScrollOffset` are
   resolved (O1) and unused. **Now step T4a, ungated** — and note the 5 s cadence makes a reset
   RARER AND MORE STARTLING, not better; the content diff makes it rarer still but does **not**
   dissolve it for the GROWTH case, which is the user's exact scenario.

#### 8c.4 The half that needs no game at all

The id-reconcile invariant is pure logic over a `std::vector<LobbyRow>` and should get an
**un-gated selftest** in the project's own shape (`movement_ledger`'s 29-check selftest,
`drive_selftest`): feed it grow / shrink / shuffle sequences and assert that the id recorded for a
position is the id whose text was written there. Shown RED against a deliberately index-based
implementation, then GREEN. That makes the correctness half independent of anyone scrolling, and
leaves the interactive harness to do what only a human can: say whether it feels right.

#### 8c.5 Acceptance

- Machine: every phase asserts (rows built == rows requested; scroll offset preserved; the
  post-shuffle id matches the rendered text; no `[Error]`; RSS flat; GC survived) and the run ends
  ALL PASS with screenshots per phase.
- Human: the user's verdict on scroll feel at 100 and 200 rows, and on whether a refresh is
  visible as a flicker.

---

### 8. Build plan for the native server browser — CONVERGED `/qf`, 2026-08-25

**Status: DESIGN, with P1 MEASURED — read §8a first.** The plan below is unbuilt except for its
step 2 (the probe), which RAN on 2026-08-25 and is reported in §8a; **§8a corrects three statements
in this section and disarms step 3.** The `/qf` this project requires ran to convergence over
eleven rounds; **every round removed or corrected something**, so the rejections below are
load-bearing and should not be silently re-opened.

**What the `/qf` took OUT (each a concession, not a plan):**

- **A reusable native-widget LAYER — DROPPED.** `OPUS_48_DISCIPLINE.md:196-197` bars new frameworks
  before **N>=3** working cases. The browser is built **concretely**. Candidates #2/#3 are
  `host_save_picker` (227 LOC), `world_rules_panel` (109), `config_review_panel` (195) —
  **candidates, not commitments.**
- **The DELEGATE bind — DROPPED FROM v1** (see §6e: it is *possible*; nothing makes it *necessary*).
  `multiplayer_menu.cpp:244-262` already reads a real `UButton`'s release edge with no new
  primitives, and OPUS §7 endorses poll-and-diff. Dropping it removed the design's last unmeasured
  leap — minting a `UFunction` from a DLL that owns no `UClass`. **One probe line still measures it**
  so the v2 decision to retire the poll is made on evidence rather than by default; the bind is a
  WRITE, so it runs on a throwaway button we spawn and never attach, never on a live native one.
- **Reusing the game's row class — DEAD TWICE.** (a) `uicomp_saveSlot_C::upd()` (201 B) dereferences
  `parent->selected` typed `Uui_saveSlots_C*`, and its 2,813 B ubergraph is save semantics
  end-to-end (`getSaveObject`, `lastSavedDate`, `BytesToImage(save->preview)`). (b) `SpawnObject` is
  `UGameplayStatics::SpawnObject` = bare `NewObject`, so the row's members would be **null**; the only
  populate path is `UWidgetBlueprintLibrary::Create` (`UMG.hpp:1939`), which runs `Initialize` ->
  `Construct` -> the bound events = **a live BP brain on a mirror** (`OPUS_48_DISCIPLINE.md:56`).
  **We take the SHAPE, never the class.**
- **A new `input_owner` axis — DEMOTED to a comment.** It changes nothing observable:
  `input_owner.cpp:379` filters its focus walk by `DerivesFromUserWidget` **only**, so our own
  `UUserWidget` taking user-0 focus already flips `GameOwnsText` and the WndProc swallow already backs
  off — **typing works with no new input code.** Shipping an axis with one consumer would violate the
  same N>=3 bar that killed the layer. The comment records that the class-agnostic filter is
  deliberate and that our native widgets depend on it.
- **Loading the palette by `/Game` path — REPLACED by cloning resident brushes** (step 4).

**The order — and note the doc and the measurement both come before any edit.**

1. **The DOC (this section + §7b/§7c) — FIRST, and done.** `:151`/`:160` were live false statements
   ("the moment ARRIVED... the user has the editor") contradicting §5/§6's cancellation boxes ~100
   lines below; a parallel session sharing this repo could read them as a verdict. Fixed in place.
2. **P1 — one read-only MENU-TIME probe**, one log line per fact. **BUILT AND RUN 2026-08-25 —
   results in §8a.** **Not a boot probe** — corrected in
   `/qf` round 7: O5 asks about the inject, which fires from the `ui_menu` **Tick observer**
   (`multiplayer_menu.cpp:222-227`), and every O7 donor is a live-menu widget. **At boot there is no
   `ui_menu`**, so a boot-time run would read null for all of them, and that null is
   *indistinguishable* from "no handle, no bug, donors absent" — an instrument blind to the
   phenomenon always passes. It runs from the same observer as the inject, and records **which menu
   instance and tick it sampled**, so a null is attributable.
   - **RUNG 0 — one counter, and it settles O4's only real leg (`/qf` round 10).** The overlay frame
     loop already runs every Present, and `CurrentWorld()` already ships (reading-order 4j). So count
     **frames presented while `CurrentWorld()` is null**. That is the entire question behind "can UMG
     serve `join_curtain` / `loading_screen` / `boot_warning_dialog`": if the game never presents a
     frame without a world, UMG can serve them and the ~3,700 LOC of overlay substrate is bankable
     after all; if it does, ImGui's Present hook covers a window UMG structurally cannot, and two
     substrates is the measured answer. Banking "two substrates permanently" while the instrument
     sits in the same loop, unused, is the blind-instrument shape this doc keeps catching.
   - **RUNG 1 — the cheapest falsifier, and it can invalidate the placement.** `BuildTextWidget`
     (`engine_widget.cpp:157-170`) already hand-wires `UUserWidget -> UWidgetTree -> root` with bare
     `SpawnObject` + raw offsets, and ships through `pos_hud` — **into the VIEWPORT, which is the only
     path ever proven.** Whether a never-`Initialize`d `UUserWidget` renders **inside a
     `UWidgetSwitcher`** is unmeasured. So: `AddChild` that same throwaway as child 12,
     `SetActiveWidgetIndex(11)`, take **one screenshot**. That single step tests `AddChild`
     resolution, switcher rendering of a hand-wired widget, the ESC behaviour and `input_owner`'s
     focus term **at once** — before any of P2's ~700 LOC. **If it renders nowhere, the screen falls
     back to `AddToViewport` like every other surface we ship, and the 12th-child design goes with
     it.**

     > **This rung is a WRITE, and it must be flagged as one (`/qf` round 9).** O8's delegate bind was
     > carved onto a throwaway button *because a bind is a write*; this rung `AddChild`s into the
     > **live shipped `switcher_widgets`** and sets `ActiveWidgetIndex = 11` — into the one
     > **hands-on-verified** native inject — and I had not applied the same scruple. Worse: per §8's
     > ESC finding, **at index 11 ESC is a no-op and the throwaway has no `button_back`**, so the
     > spike as first written could strand the user in a menu with no way out. It therefore: runs
     > only behind a **dev env gate** (never a shipped path), **reads back the prior
     > `ActiveWidgetIndex` before clobbering it**, and **restores that index and `RemoveChild`s the
     > throwaway in the same tick**. A probe that can trap the user is not read-only in any sense
     > that matters.
   - **O1 — measure FUNCTION resolution, not class names.** Corrected in `/qf` round 8:
     `R::FindFunction` matches `OuterOf(fn) == owningClass` with **no super-walk**
     (`reflection.cpp:493`; `sdk_profile_names.h:414` says so in a comment). So resolving `ScrollBox`
     buys nothing by itself — `AddChild` lives on **`UPanelWidget`**, a class we already resolve, and
     **one** resolve there serves ScrollBox, Overlay, HorizontalBox and CanvasPanel alike. (The mod
     already declares **nine** UMG class constants, not five: `Widget`:387/:414 — a duplicated
     constant — `PanelWidget`:433, `ContentWidget`:434, `WidgetComponent`:376, `WidgetTree`:399,
     `UserWidget`:398, `TextBlock`:384, `VerticalBox`:405, `Button`:432.)
     **Classes to resolve** (instantiation only): `ScrollBox`, `Overlay`, `SizeBox`, `Image`,
     `HorizontalBox`, `EditableTextBox`, `CanvasPanel`.
     **Functions to resolve, each on its OWNING class** — the real risk: `AddChild` on
     `UPanelWidget` (**was resolved nowhere in the tree; §8a measured it RESOLVES**);
     `SetBrushFromMaterial`/`SetBrushTintColor` on
     `UImage`; `SetHeightOverride` on `USizeBox`; `SetText`/`GetText` on `UEditableTextBox`; the slot
     setters on each `U*Slot`; and `GetMousePositionOnViewport`/`GetViewportScale` on the **CDO** of
     `UWidgetLayoutLibrary` — a `UBlueprintFunctionLibrary` (`UMG.hpp:2067`) that appears **nowhere**
     in our tree, so it needs the `FindClassDefaultObject` pattern of `spawn_menu.cpp:129`.
     A miss must **fail loud at resolve time**, never draw a broken screen — names survive a recook,
     offsets do not (`docs/VERSION_MIGRATION.md`).
   - **O5**: is the donor's `FSlateResourceHandle` (the 16 unreflected bytes at `FSlateBrush`+0x70)
     **populated** at inject time? This gates step 3 and nothing else.
   - **O7**: donor residency per donor (step 4's table). All donors are now menu members — the row-instance donor was dropped in `/qf` round 7 (see step 4).
   - the delegate observation above, and the `input_owner` assertion.
3. **P0 — the brush-handle fix, GATED on O5. THE GATE CAME BACK CLOSED (§8a): 0/4 handles
   populated across 3/4 brushes that carry art, so there is no live handle bug on this build.
   SUPERSEDED 2026-08-26 (`fbc7be1c`): P0 is no longer a conditional fix at all.** A `/qf` critic
   asked the question that dissolves it -- if the browser's new clone helper must ALWAYS zero the
   handle, then `InjectCanvasButton`'s 0x278 memcpy is wrong by the same construction, since
   `FButtonStyle` embeds four `FSlateBrush`es and the shipped code zeroed only the two
   `FSlateSound` caches. So there is now exactly ONE brush clone in the tree
   (`ue_wrap::umg::CloneStyle`), it takes a brush-offset TABLE (`FScrollBarStyle` has NINE brushes,
   `FEditableTextBoxStyle` thirteen), and the shipped inject routes through it. Behaviour
   preservation measured, not asserted: the injected MULTIPLAYER label's cyan glyph mask in the
   menu screenshot is PIXEL-IDENTICAL before and after -- 489 pixels, same set, zero either side.
   The reasoning is kept because it is what the gate was measured against. `FSlateBrush` is 0x88: reflected fields end at
   `ImageType` @0x6F and the bitfield bools resume @0x80, so **the 16 bytes at +0x70 are an
   unreflected `FSlateResourceHandle` (a `TSharedPtr`)**. `FButtonStyle` is four brushes at
   0x08/0x90/0x118/0x1A0 (`SlateCore.hpp:12-15`), and `InjectCanvasButton`'s 0x278 `memcpy` covers all
   four while zeroing only the two `FSlateSound` tails (`engine_widget.cpp:463-466`). If the handle is
   populated we copy a refcounted pointer without `AddRef`. **The consequence is INFERRED, not
   measured — hence the gate.** If armed: zero the four handles exactly as `sdk_profile.h:193-196`
   already does for sound, then re-check visually that the button still draws its background (the same
   proof that settled the sound case). **If O5 returns null there is no bug and the one
   hands-on-verified inject is not touched at all.**
4. **P2 — build `ui/server_browser_native.{h,cpp}`, concretely.**
   - **Where it lives.** A screen `UUserWidget` added as the **12th child of `switcher_widgets`
     @0x360**.

     > **The safety argument, CORRECTED 2026-08-25 (`/qf` round 7).** The first version of this
     > paragraph said "exactly two `ui_menu` functions touch that switcher, and nothing iterates the
     > child list" — that was a census **inside `ui_menu` only**, sold as an invariant
     > (`lesson_a_leak_sweep_scoped_to_one_directory`). Measured across the siblings, in the
     > **bytecode** (the name `switcher_widgets` appears in all nine siblings' `.uasset` *name maps*,
     > which proves nothing): `ui_stats::ExecuteUbergraph` (3,379 B) touches `switcher_widgets`,
     > `SetActiveWidgetIndex` **and `GetAllChildren`**, and `ui_settings::ExecuteUbergraph`
     > (11,968 B) touches the first two. **Sibling screens do reach back into the switcher.**
     >
     > The placement still holds, for a reason that survives the wider sweep:
     > **(a)** `ui_stats`'s `GetAllChildren` is on its own **`statsList`**, not on the switcher —
     > *nobody iterates the switcher's children*; **(b)** what the siblings do is **write**
     > `ActiveWidgetIndex` to navigate; and **(c)** `AddChild` **appends**, so indices 0..10 keep
     > their meaning and a 12th child cannot renumber anything a hardcoded index refers to.
     > That is the invariant: **the child list is never iterated, and appending cannot renumber.**
   - **Back — and ESC does NOT work, on purpose, exactly as on the two model screens.**
     `int_widgets` is an *interface* (`Iint_widgets_C`: `triggerRandom`/`getSearchName`/`setIndex`/
     `resume`), and **`ui_saveSlots` and `ui_gamemode` do not implement it**; their cast fails today
     and the game is fine, so a failed cast is a normal exercised path. We need no interface, and
     could not implement one without owning a `UClass`.
     **Disassembled (`/qf` round 7) rather than assumed:** `OnKeyDown` is 538 B and its **only**
     integer constant is `0` — it casts `widgetEnter` to the interface, calls `resume()` if that
     succeeds, then tests `ActiveWidgetIndex == 0` and clears `widgetEnter`. There is **no
     comparison against 11 or any other index**. So at index 11 the cast fails and the `== 0` test
     fails: **ESC is a no-op on our screen — as it already is on `ui_saveSlots` and `ui_gamemode`.**
     Navigation is the explicit `button_back`. Stated rather than left to read as inherited.
   - **Lifecycle** exactly per `multiplayer_menu.cpp:222-227`: inject once per menu instance,
     self-heal on `!Alive()` (`CachedObjRef`, world-stamped), 1/s throttle, everything else
     edge-applied. That is the one **hands-on-verified** native inject we have.
   - **The row — and it has NO `UButton`.** `USizeBox(HeightOverride = 64)` -> **`UOverlay`**
     holding a `UImage` background + a `UHorizontalBox` of five `UTextBlock`s. **Not a `UBorder`** —
     that is a `UContentWidget` (`SetContent`/`GetContent`, single child) and a row stacks two things.

     > **Why no row button (`/qf` round 8 — it dissolved a problem rather than solving one).** The
     > native row's `button_select` draws nothing in all three states (`DrawAs: NoDrawType`), and the
     > release-edge poll's one surviving justification is *"let the `UButton` finish its own
     > press->release visual"* — **which is false for a button that has no visual.** And the hit
     > target does not need to be a button at all: `IsHovered()` is resolved on **`UWidget`**
     > (`engine_widget.cpp:214-217`), so it answers for the row's background `UImage` just as well —
     > see the hover step below. A per-row `UButton` therefore buys only the click **sound**, which
     > the screen can play directly.
     > *(Reason revised twice: the first version of this box said rect math replaced the button —
     > `/qf` round 10 reversed that, and the button stays gone for the reason above instead.)*
     > Removing it also removes any need to author `DrawAs = NoDrawType` into a cloned brush — which
     > would have been a carve-out in the clone-don't-author rule below, and which I had justified
     > with an asset-reference census that structurally cannot see `DrawAs` (a one-byte reflected
     > value, `SlateCore.hpp:293`). **Real `UButton`s remain only on the chrome** — Connect /
     > Refresh / Back / Host — where the press visual and the native sound are genuine and the framed
     > `button_*` donor is the right one.
     **Not a `UUserWidget`** either: rows would then land in `GUObjectArray` for `input_owner`'s 1 Hz
     focus walk and churn on every refresh. (A `UBorder` *is* right for a framed background panel —
     one content child. Choose by child count, not by habit.) The game uses `UCanvasPanel` + explicit
     offsets here; `UOverlay` is a deliberate divergence (alignment-driven, no offset math) and owes
     a one-line comment saying so.
   - **Style — CLONE, do not author.** Copy the 0x88 `FSlateBrush` from a resident native widget and
     zero the handle at +0x70. `DrawAs` and `Margin` live *inside* the brush, so a clone carries the
     9-slice for free and `SetBrushDrawAs`/`SetBrushMargin` — which do **not** exist in this build —
     never come up. **Donors are read FAIL-CLOSED.** `tex_btnStart` taught us a donor's *offset*
     resolves while "the pointed-to widget isn't bound yet"
     (`lesson_umg_injected_menu_button_native_parity.md` fact 1), so **any null donor means: do not
     build, retry on the 1/s self-heal — never fall back to a default style.** That fallback is
     exactly the Roboto/centered/white bug.

     | brush | donor |
     |---|---|
     | panel fill | `ui_saveSlots.Image_0` |
     | border | ~~`ui_saveSlots.image_border_*`~~ **-> `ui_saveSlots.Image_6`** (§8a correction 1: `image_border` is not a field on this class) |
     | button — 3 states **and both sounds** | any `ui_saveSlots.button_*` |
     | text box | `ui_saveSlots.ETB_slotName` |
     | scrollbar | `ui_settings.scrollboxRoot` |
     | row background | `ui_saveSlots.Image_6` **+ our own tint** — see the note below (was `image_border_*`; §8a correction 1) |

   - **Why the row background is NOT donated by `uicomp_saveSlot` (corrected, `/qf` round 7).** The
     first draft donated it from a live row instance. Measured: `uicomp_saveSlot` references **only**
     `inst_uiButton`, `inst_uiBorder`, `font_ui` and the two sounds — **every one already donated by a
     menu member** (`button_*` gives `inst_uiButton` and both sounds; `image_border_*` gives
     `inst_uiBorder`), and `Image_background` is simply `inst_uiBorder` tinted `(0.5,0.5,0.5)`. So the
     row-instance donor supplied **nothing unique** while imposing the design's strictest
     precondition — a populated `Slots`, i.e. *the user must first have opened the save-slot screen*.
     A fail-closed retry against a donor that may never exist would spin forever. **Dropped.**
   - **Bound the retry.** Fail-closed means retry, and retry means a genuinely-absent donor must be
     *diagnosable rather than silent*: log once after N attempts naming the missing donor. A silent
     forever-retry is the same defect class as a fallback style, one level quieter.
   - **Hover and click — `IsHovered()` on the row, edge-gated. Rect math was REVERSED in `/qf`
     round 10.**

     > This step used to say hit-testing would be *"pure C++ rect math, independent of N"* because
     > *"the row rects are ours"*. **Two measurements killed that.** (1) `IsHovered` is resolved on
     > **`UWidget`** — `engine_widget.cpp:214-217`, whose own comment says *"IsHovered + SetVisibility
     > are owned by UWidget"* — so it is callable on **any** `UWidget*`, a bare row `UImage`
     > included; the premise that we *needed* rect math was one grep from false. (2) Neither
     > `GetCachedGeometry` nor `GetScrollOffset` is resolved anywhere in our tree, and **the rows live
     > inside a `UScrollBox` that scrolls** — so "the rects are ours" quietly assumed a *static*
     > layout. Doing it by hand would mean reimplementing Slate's scroll transform, DPI scale and
     > clipping, and getting them wrong at the first scroll or non-1.0 UI scale.
     >
     > **`IsHovered()` is correct by construction — Slate already did the hit-test.** The cost worry
     > that motivated rect math is handled by edge-gating, but **not inside the WndProc** — corrected
     > in `/qf` round 11. `imgui_overlay.cpp:324` calls `CallWindowProcW(g_origWndProc, ...)` **last**,
     > so our detour runs *before* the engine sees the message: on a `WM_MOUSEMOVE`, Slate has not
     > processed that move yet and `IsHovered()` still answers for the **previous** position.
     > ("`WndProcDetour` runs on the game thread" was inferred to mean "sees the move"; those are
     > different claims.) **So the WndProc only sets a moved-flag, and hover is evaluated on the next
     > game-thread tick** in the observer that already runs — fresh, because Slate has processed the
     > move by then, and still not per-frame.
     >
     > **That also dissolves a re-entrancy risk that was never on the list:** issuing a `ProcessEvent`
     > UFunction call from inside the window procedure — while the engine is in its message pump, not
     > its tick — is a class the detour has never exercised (today it touches only ImGui and our own
     > state), and no existing site proves it safe. Evaluating on the observer tick means **no
     > `ProcessEvent` is ever issued from the message pump**, so the question does not arise.
     >
     > The row's hit target is its background **`UImage`**, which must be set explicitly to
     > `Visibility = Visible` (a `SelfHitTestInvisible` image answers `IsHovered() == false`) —
     > **a probe item**. This is also the third and final reason the row needs no `UButton`: a
     > `UImage` is a sufficient hit target, while a `UButton` would add a press visual we do not want
     > and a style we would then have to suppress.
   - **Clicks**: the release-edge poll — **re-derived, not inherited.** Its comment gives two reasons;
     the "ImGui swallows `WM_LBUTTONUP`" reason **dies with the ImGui browser**, and only "let the
     `UButton` finish its own press->release visual" survives. Rewrite the comment to say so — a
     comment citing a dead cause is how a false comment is born. `multiplayer_menu.cpp:228-241`'s
     `HitTestInvisible` block **dies entirely**; its only cause was ImGui capture.
   - **Row lifetime and GC — POOL, do not rebuild (`/qf` round 10; I had not considered this).**
     `BuildTextWidget` widgets come from bare `SpawnObject`, and the only thing that keeps a row
     reachable is the panel's `Slots` — a UPROPERTY `TArray` — so `RemoveChild`/`ClearChildren`
     (already resolved, `engine_widget.cpp:212`) is what makes one collectable. Two consequences:
     **(a)** **our C++ side must hold NO row pointers at all** — corrected in `/qf` round 11, because
     the previous version of this line said to hold them "through `CachedObjRef` (world-stamped)" and
     **`cached_obj_ref.h:34-46` is a KNOWN GAP box that denies exactly that**: *"UMG widgets are NOT
     covered … `WorldOf()` answers nullptr for the whole widget surface and the term is silently
     inert there"*, naming `multiplayer_menu`'s `g_button` and `pos_hud`'s `g_root` as caches that
     rely on liveness alone; and `:47-51` adds that UE assigns serials **lazily**, so a
     hand-`SpawnObject`ed row captures serial 0 and *"no caller may assume"* the ABA residual is
     closed. **So the pool lives in the PANEL, not in our globals**: `GetChildrenCount()` +
     `GetChildAt(i)` on demand, since the panel's `Slots` is already the authority. No row pointer
     survives a tick, so there is no ABA surface. One long-lived reference remains — the screen —
     carrying the *same* residual the shipped `g_button` already carries, i.e. no new exposure.
     **(b)** a 1 Hz refresh that
     *rebuilds* N rows would churn GC for nothing. **The game already shows the answer**: it keeps
     `TArray<Uuicomp_saveSlot_C*> Slots` and calls `upd(int32)` — **reuse the widgets, update their
     text, and add/remove only when the row COUNT changes.** That is the bounded teardown, and it is
     one more thing to copy rather than invent.
   - **Unchanged**: `coop::net::lobby::LobbyRow` and every `coop::session_manager` call. Only the
     renderer is replaced.
   - **PORT THE INCUMBENT'S CORRECTNESS, do not re-derive it.** The 277 LOC being retired encodes
     details the struct's field names do not: the version cell is `game` + `" b" + proto` with
     `version` as a legacy-only fallback, and the mismatch has **two** legs. Diff the new cells
     against `server_browser.cpp:218-231` **before** deleting it. (Caught in `/qf` round 6 — the first
     draft of §7c got this wrong by reading `lobby_client.h`'s field names instead of the producer
     `lobby_announcer.cpp:36-39` and the incumbent renderer.)
5. **P4 — the host form** follows `ui_gamemode`'s shape (§7b): mode/option rows plus
   description-on-hover, built from **our** widgets.
6. **P5 — retire the ImGui browser. `menu_sfx` SURVIVES.**

   > **CORRECTED (`/qf` round 9).** This step used to say `menu_sfx` was *"a deletion, not a port"*,
   > on the strength of its own header. **I never censused its callers.** There are **four** —
   > `boot_warning_dialog`, `config_review_panel`, `connect_failed_dialog`, `server_browser` — and P5
   > retires exactly **one**. The header sentence I quoted as proof (*"ImGui buttons have no native
   > audio, so this supplies the same two sounds for our server browser"*) continues **"(and any
   > future ImGui menu surface)"** — I stopped at the comma. That is `docs/LESSONS.md`'s *read the
   > clause after the claim*, firing on text I committed the same day I wrote that lesson down.
   >
   > So: **`server_browser`'s `menu_sfx` calls go with the browser; the module stays** for the other
   > three surfaces. It becomes a deletion only if the ImGui substrate itself is retired — exactly
   > the question this design refuses to bank. A native `UButton` still gets both sounds free from
   > its cloned `FButtonStyle`; that part was right, and it is why the *chrome* buttons need no
   > `menu_sfx` equivalent.

**What this does NOT promise — and it is now MEASURED rather than deferred (see §8a).** ImGui is
**not** being retired as the mod's UI substrate, and after the 2026-08-25 RUNG 0 travel runs that is
a finding rather than a caution: the boot window is ~540 presented frames over ~11.4 s in which our
task pump does not advance and no world exists, so a native UMG surface is unreachable there in two
independent ways. The paragraph below is the pre-measurement reasoning, kept because it is what the
measurement was run against. Doing so would bank ~3,700 LOC of overlay substrate (`imgui_overlay` 815,
`overlay_backend_dx12` 792, `_dx12_capture` 405, `fonts` 492, `atlas_watch` 386, `overlay_diag` 209,
`_dx11` 157, `overlay_backend` 155, `overlay_cursor` 70, `scale` 127, `style` 80), and that is
**unbankable** until someone measures whether UMG can cover `join_curtain`, `loading_screen` and
`boot_warning_dialog`. ~~Two substrates permanently is a live possible outcome.~~ **-> MEASURED
2026-08-25: two substrates permanently is the ANSWER, not a possibility (§8a).**

> **ANSWERED 2026-08-25 — see §8a. The framing below was right and is kept as the question the
> measurement was aimed at.** Sharpened (`/qf` round 9): O4 is a question about DRAW time, not ARM time. This paragraph used
> to lean on `boot_warning_dialog` being *"armed from the boot thread before any world exists"*. That
> is true and **irrelevant**: `Arm()` (`boot_warning_dialog.cpp:29-34`) takes a mutex, stores a
> `std::string` and sets an atomic — nothing world-related — while `Render()` runs from the overlay
> frame loop (`imgui_overlay.cpp:449`), i.e. at Present time, and at the main menu a world exists.
> **The real question is whether the game presents frames before the menu world exists** (startup /
> level transitions), because that is the window UMG structurally cannot draw in and an ImGui
> Present hook can. **That window has never been measured**, and it — not the arm thread — is what
> makes the substrate retirement unbankable. The browser is the experiment that
informs the decision; it is not the decision.

**Worth knowing when that question is taken up:** `docs/OVERLAY_CAPTURE_COEXIST.md` §6 states the root
of the whole RTSS/OBS arc as *"we draw from an inline hook ON `IDXGISwapChain::Present`"*, and its fix
as moving the draw *"into the engine's own present path ... exactly where the engine's own UI draws."*
**A native UMG surface IS that destination**, with no hook at all — so if the substrate question ever
resolves toward UMG, that arc resolves with it rather than needing its own AOB.

**Two things that must not be re-derived:**

- **Do not touch the main-menu button or the version line** (§7). They are done, and correctly.
- **MTA decides the shape**: `CServerBrowser.cpp` is **3,400 LOC of C++** and the MTA tree contains
  **zero `.layout` files** — the browser is built in code, not authored. The one divergence (they bind
  handlers, we poll) owes a one-line citation comment at the site, per the MTA rule.

## Anchors (from reflection dump, game 0.9.0-n)

- Menu widget: `ui_menu_C` (buttons: NewGame, Resume, Save, Settings, Exit,
  …). We attach a Multiplayer entry near these without touching the asset.
- Save selection UI: `ui_saveSlots_C` / `uicomp_saveSlot*` (reuse for the
  Host "choose save" step).
- Load path: `UmainGameInstance_C::setSaveSlotObject` + open `untitled_1`;
  GameMode applies world state (`loadObjects`/`loadTriggers`).

## Gating — RESOLVED (built)

**Historical (2026-05-25):** the menu build was originally deferred while the
env-var `.bat` launchers (`mp_host_game.bat` / `mp_client_connect.bat`) covered
the hands-on workflow and replication feature-work (5N*/5S*/5T/5D) was the
priority. **That deferral is over — the menu/flows shipped (see the Status
banner + `ui/` modules).** The `.bat` launchers remain the autonomous-test entry
points (see `docs/AUTONOMOUS_TESTING.md`).

## Connect / disconnect UX (AS-BUILT 2026-07-16, NOT hands-on)

Two coupled fixes to the join/leave surface — shipped (DLL `a760f9f51bec2f07`, deployed x4),
**not yet hands-on** (take pending). No wire change / no `kProtocolVersion` bump (both local-only).

- **Connect-failed dialog.** A failed browser join (dead/ghost host timeout, master unreachable,
  bad address) now shows a `COULD NOT CONNECT — <reason>` modal over the reopened browser, with an
  OK button, instead of silently reopening the browser (or leaking a stray toast). A user CANCEL is
  silent (no dialog). The reason is owned by `coop::join_progress` (`Fail(reason)` stashes it ONLY
  when it wins the abort exchange + not shutting down; `RequestCancel` clears it) and rendered by the
  new `ui/connect_failed_dialog.{h,cpp}` (dependency ui->session, like `loading_screen`). Per-frame
  gate is lock-free (`join_progress::FailPending()` atomic mirror); the mutex is taken only in Render.
- **Leave-toast false-positive fix.** The `"<X> left the game"` feed edge in `event_feed.cpp` now
  gates on `IsSlotReady` (the present edge), not `IsSlotConnected` (a transport handle held during a
  doomed connect) — so a timed-out connect no longer emits a false `"Remote player left the game"`
  that leaked into the menu. See `[[lesson-departure-toast-gates-on-ready-edge-not-transport]]`.

The **ghost lobby** a killed host left in the browser is **FIXED (2026-07-16, deployed live)**: the Rust
master's `LOBBY_TTL` is **90s** (3 missed 30s heartbeats; was 300s — commit `6d640679`, binary
`ad9844b6` live on the VPS). See
`research/findings/network/votv-master-server-RE-and-rust-port-scope-2026-07-16.md`.

## Player list / scoreboard (arc A, AS-BUILT 2026-07-27 `89620d59`, screenshot-verified NOT hands-on)

Opened with **TILDE** (`VK_OEM_3`, the physical key above TAB -- NOT Tab; `imgui_overlay.cpp:162-171`).
**Hold-to-peek on a client, toggle on the host**; the host's board is interactive (the clickable
Teleport/Kick/Ban action popup), a client's is a passive peek with no input capture.

Arc A made a CLIENT list every peer for the first time -- before it, `roster.cpp` skipped rows whose
slot was not `IsSlotConnected`, and a client only ever fills `peerConns_[0]`, so a client's board
showed only itself and the host. Columns, left to right: Player, [Mic], Link, Ping, **ID**.

Verified by capture, not by eye: `tools/net/roster_shot.ps1` brings up four peers, waits until the
HOST has seen a pose from every client slot (a peer's own "ready" log lines fire while it is still on
the loading screen -- see LESSONS), PostMessages the real tilde key per window, and photographs all
four. Evidence in `research/roster_shots/`.

Three user verdicts on the first such screenshot were fixed the same day (`89620d59`): the ID column
moved to the FAR RIGHT (right-aligned); the rows got a visible zebra + separator + cell padding (the
default `RowBg` stripe is invisible over a dark 3D backdrop, and a first pass at 0.055 alpha was still
unreadable in the re-shot picture); and a CLIENT's OWN row no longer renders a blank Link cell.

### The connection columns: host-measured, host-published (v131, 2026-07-28)

**Every row on every board answers ONE question: "how is THIS PLAYER connected to the session?"**
The peer that MEASURES a fact publishes it; nobody synthesises a substitute for a fact it cannot see.

Until v131 the Link column was computed per viewer, from *what I can measure about you* -- so it
answered **transport** on the rows whose connection this peer happened to own and **route**
(`VIA HOST`) on the rest, two axes in one column, side by side on one board. The user read it
immediately: *"why it says via host on 2 clients and lan on one client. It should be all the same, no
special treatment."* The Ping column had the same split (`<1ms` / blank / `--` / `--` down a single
client board), and so did the world nameplate, where a client's peer plates carried no ms at all.

The fix: the HOST measures every link and publishes `[u8 linkKind][i16 pingMs]` on `RosterRow`
(fixed prefix -- the tail's offsets live inside the `applyDeclared` block, which is skipped for
exactly the host row and the receiver's own row). `roster::Refresh` has **no role branching left**.

- **Link** -- `LAN` / `DIRECT` / `RELAY`, or `n/a` on the host's row (their traffic never crosses a
  socket). Every kind is measured FROM THE CONNECTION -- the GNS `Relayed` flag and the remote
  address -- never asserted from `cfg_.topology`, which used to label a port-forwarded WAN peer `LAN`.
  The kinds deliberately do not distinguish LanDirect from P2P: that is how a connection was
  *established*, not how a player is *connected*.
- **Ping** -- RTT to the SESSION, rendered on **every** row including your own. `n/a` means there is
  nothing to measure (the host); `--` means no sample has landed yet. Two distinct tokens on purpose,
  both ASCII (our fonts fall back to `?` on missing glyphs).
- **HOST** is a tag beside the NAME, not a word in the Link column. It says who the player is; the
  Link column says how their traffic arrives. `LAN HOST` fused the two -- and without the tag the word
  would have vanished from the UI entirely.
- **Header row**: `TableHeadersRow()` is now called (user: *"the rows are not even named properly"*) --
  on the scoreboard and on all three admin-panel tables.
- One renderer for all three surfaces: `ui/link_format.{h,cpp}`. The scoreboard, the admin panel and
  the nameplate each used to hand-copy the same `>0 / ==0 / else` cascade.

Freshness: the host fills the ledger **immediately before each `RosterRow` send**, never on a clock of
its own -- so the bytes are exactly fresh and there is no second cadence to drift. End-to-end age on a
client board is the RTT sampler (<=1 s) + the pulse (<=5 s, `kPulseSlowMs`). **That constant now has
two consumers** -- roster repair AND every board's ping freshness.

### The name a row shows is the HOST's to assign (arc B, AS-BUILT 2026-07-28 `8592fb5e`, proto 132)

Two players typing the same name each believe they are unique, and neither can see the other's choice
at the moment it is made -- so **uniqueness is not the name-owner's call**. The host, the one peer that
sees every name at once, ASSIGNS the display name at the Join seam and every peer including the named
one adopts it. Four `Pelmentor`s read `Pelmentor (host) / Pelmentor2 / Pelmentor3 / Pelmentor4`,
identically on all four boards.

**The assignment is KEPT, not borrowed** (USER 2026-07-28): it is written back to `multivoid.ini`, so
the next session asks to be called `Pelmentor2` and keeps it unless someone else already is. A second
`Pelmentor2` is the one that moves -- to `Pelmentor22`, because the whole requested name is the stem.
Deciding otherwise would mean guessing whether a trailing number is a suffix we once added or part of a
name its owner chose, and nothing in the string says which.

No new wire kind: the assignment rides the nick already in `RosterRow`'s FIXED PREFIX. On the receiving
side a row about ME writes `g_localNick`, not the ledger row -- that store is what chat authorship, the
action feed, the nameplate, the Join payload and both of the roster's local-row reads derive from, so
writing the row alone would have left five surfaces showing the name we asked for.

### Non-ASCII names render (arc D1, AS-BUILT 2026-07-28 in TWO parts: `9ae83454` entry, `2c3975d2`+`f19eedac` egress)

`Пельменьмень / …2 / …3 / …4` renders on the board AND on the floating nameplate AND in chat -- 13
characters, 26 UTF-8 bytes. The entry root was never the ASCII allowlist everyone blamed: nothing
decoded UTF-8 at entry, so a Cyrillic name arrived at the sanitizer as mojibake and was stripped
whole. Cyrillic costs **zero new font bytes** (all seven embedded families were cmap-measured to
cover U+0400-04FF).

**The first cut fixed only entry, and its evidence overstated the result.** Four EGRESS narrows
survived it, because a census of widens cannot see a narrow: `WideCharToMultiByte` into the 23-byte
`char nick[24]` buffers returns **0** on overflow rather than truncating, so the scoreboard row and
the ban record went BLANK past 12 Cyrillic characters, while the **floating nameplate** -- the
surface the request literally named -- squashed every non-ASCII character to `'?'`. The drill that
"proved" Cyrillic used `Пельмень`: 8 characters, 16 bytes, under the cliff, photographed on the
board with no plate in frame. Now: one egress owner (`coop::text::CopyUtf8ToBuffer`, truncates on a
codepoint boundary and never blanks) and every `char nick[]` declared with `kNickBufBytes`.

**The plate centres the NAME, not the name+ping composite** (`eb0bcdf5`, user-reported off a drill
screenshot). Centring `"<nick> (<ping>)"` as one string put the name's centre half the ping suffix's
width left of the health bar -- and would have slid the name sideways whenever the ping gained a
digit. The identity is centred on the anchor; the annotation hangs off its right edge and the
on-screen clamp is asymmetric to match. Measured 0.0 px offset at 8x on shipped bytes.

**Emoji render in colour; CJK renders as U+FFFD and is still UNIQUE** -- arc D2 is **BUILT**
(2026-07-28, `5947d391`, DLL `241fddcf95ad6f09`, drilled on DX11 and DX12, NOT hands-on). Decision
of record `votv-nickname-arbitration-roster-id-DESIGN-2026-07-27.md` **§9d**, as-built **§9e**
(which corrects three of §9d's mechanism claims), fact base
`votv-arc-d-gate-measurements-2026-07-28.md`.

That single `FallbackGlyph` is why arc D2 was never a font question. Two distinct CJK names had
distinct fold keys, so arc B assigned neither of them a suffix -- and they then drew as **the same
nameplate**. The originating ask ("просто чтобы у всех был уникальный Nameplate") was therefore false
on screen for exactly the players the donors were meant to serve, and no donor budget closes it for
every script. So uniqueness moved OUT of the pixels: `FoldKey` maps every out-of-repertoire
codepoint to one sentinel, the two names collide, and one takes the numeric suffix that already
ships. Uniqueness is font-independent; the donor set is a pure legibility knob. Drilled: `张伟` alone
keeps a bare name, `李明` meeting it becomes `李明2`, and `Anna中` sentinels only the hanzi.

**What changed on screen:** single-codepoint emoji render in colour (Twemoji Mozilla, the only donor
embedded, **+689 KB** -- the whole cmap, since the layout tables it drops could never have composed
without shaping); U+FFFD is now BAKED, so the fallback glyph is the replacement character instead of
`'?'` (the fix was one glyph RANGE -- all seven faces always had U+FFFD, nothing ever asked the atlas
for it); JetBrains Mono stops missing four Cyrillic letters (U+0400/040D/0450/045D) because the other
families are cross-merged in behind it; and CJK / Hangul / Thai names render as the sentinel glyph
but are never confusable with each other. A LONE such peer keeps a bare name -- the digit appears
only when two coexist. Cost: the atlas is RGBA32 1024x2048 and re-bakes in **58-80 ms** (was ~16), so
a windowed drag-resize is the place to watch for hitching.

## Version identity surfaces (b122, AS-BUILT 2026-07-19 `5246844a`, drill-verified NOT hands-on)

The Paper-pair identity (game target + build number; docs/RELEASE.md + the
`votv-version-identity-v122-DESIGN-2026-07-19.md` design doc) touches four UI surfaces:
- **Browser Version column** = `0.9.0n b122` per row (game falls back to the legacy version tag for
  pre-field hosts); amber `(!)` on a game/build mismatch — amber ALWAYS means "Connect will be refused
  with a popup" (no amber-but-joinable state). Browser header shows `DisplayVersion()`.
- **Pre-flight refusal popup**: a version-mismatched Connect click never raises the loading cover —
  `join_progress::RefuseJoin` (no Active gate) feeds the same connect-failed modal with the
  which-axis/who-updates wording (`VersionMismatchVerdict`, session_manager.cpp).
- **Host feed line** `"<nick> was turned away: <reason>"` (deduped 30 s per nick+reason) when the wire
  gate refuses a joiner at the Join seam (`player_handshake_version.cpp`).
- **Boot-warning modal** `MOD INSTALL PROBLEM` (`ui/boot_warning_dialog.{h,cpp}`, connect_failed's
  ownership shape + the SEH re-fault guard): armed at boot from `MULTIVOID_DUP_FILES` when the xinput
  proxy found several `multivoid-*.dll` (or a legacy `votv-coop.dll`) beside the exe; names the loaded
  file + the leftovers. Dup drill screenshot-proven (s29 `dup_popup.png`).

## Main-menu version / update line (NATIVE UMG — VERIFIED hands-on 2026-07-16)

The old v59 **launch UPDATE toast** is RETIRED (RULE 2; `ed009c0d`), and so is its first replacement,
the ImGui-drawn corner string (`DrawVersionCorner` + `IsMainMenuOpen` — never showed: the overlay's
render gate needs a surface/HUD open, and the bare main menu has none; retired in `fd50f127`). The
shipped form is a **native UMG `UTextBlock`** injected as the TOP row of the VerticalBox holding VOTV's
own build labels ("Alpha 0.9.0" / "Build a090n"), so the coop line reads as one more native label and
auto show/hides with the menu — **cyan** (the coop accent, matching the MULTIPLAYER button), amber when
an update is available. Verdict formats (b122 Paper-pair identity, 2026-07-19; AS-BUILT, label itself
hands-on-verified 2026-07-16 in the old format): `Multivoid 0.9.0n b122 (latest)` /
`... -- UPDATE <tag> AVAILABLE: <url>` / `... (dev; latest released bN)`; plain
`Multivoid 0.9.0n b122` (`session_manager::DisplayVersion`) until the check lands — and PERMANENTLY
while the master has no released record (`/v1/latest` proto 0 = NO VERDICT, the client stays silent).

Mechanics: `engine::InjectTextRowAbove` (clones `txt_version`'s text style + the row slot layout;
`InsertAtTopOfVBox` is the shared snapshot→Clear→re-add reorder, now save/restoring every native row's
slot layout) + `engine::SetTextBlockColorDispatch` (post-attach colour MUST be the
`UTextBlock::SetColorAndOpacity` setter dispatch — a raw write never repaints; see
`[[lesson-umg-runtime-inject-traps]]`). Driven from `coop::multiplayer_menu::UpdateVersionLabel` (the
`ui_menu_C::Tick` observer): inject once per menu instance (self-heals), text/colour edge-applied.
**Re-polled on every main-menu entrance** (a >500 ms tick gap = a fresh entrance →
`session_manager::RefreshLatestVersion`, DoS-safe: one worker in flight + an 8 s min-interval floor).
The master's `/v1/latest` answer is env-overridable on the VPS (`COOP_LATEST_PROTO` in
the master service env file (path in the local-only deploy notes) — a release bump is an env edit + restart, no rebuild; docs/RELEASE.md step 5).
Since 2026-07-19 (b122) the compiled default AND the box env are proto 0 = "no released record" — the
informational line stays silent until the FIRST real release sets the env (a stale record can never
fake "(latest)": equality required; the old stale-66 bug class is closed by the 0-default + the
client's proto<=0 no-verdict guard). USER hands-on confirmed (2026-07-16, pre-b122 format): cyan,
above the game labels, correct "(latest)" verdict.

**Paper-pair format status, reconciled 2026-07-22 — still NOT verified.** "The b122 format rides take
4" was stale-open: take 4 ran 2026-07-21. But it does NOT promote this row. The version label was not
among the 21 reported symptoms, and *absence from a bug report about the workstation/disc/drone is not
evidence the label rendered correctly* — nobody was looking at it. The format stays **AS-BUILT,
drill-verified, NOT hands-on**; promoting it on silence is exactly the unnamed-boundary green the
ladder forbids. (The build number in these examples is b122 as shipped then; current is b125.)

## Master / signaling server — Rust, on the NEW coop VPS `<coop-vps>` (migrated 2026-07-16)

The master + signaling servers are **Rust** (`tools/coop-server-rs/`, static musl), wire-compatible
with the retired Python (byte-exact TURN cred), with the 4-agent security audit's Tier A hardening
built in (relay-OOM cap, atomic admission, IPv6 /64 rate-keying, panic-isolation, coturn
quotas/denies; client JSON-depth crash-fix + parse clamps).

**2026-07-16 (evening): the whole stack MIGRATED to a new VPS** (`<coop-vps>`; the old box is
repurposed for unrelated services, its coop stack wiped per RULE 2 and verified dead). The new box was provisioned
Rust-native by the reworked `tools/vps_provision.sh` (Rust ExecStart — no Python ever landed there;
ufw allows incl. 80/tcp for Let's Encrypt; `curl -4` public-IP fix) and **functionally verified from
outside**: healthz, `/v1/latest`→111, host→join with full ICE, signaling relay A→B, leave + TTL=90
reaper, TURN-cred HMAC match vs the box's coturn secret. Compiled official endpoints flipped
(`protocol.h` `kOfficialMasterUrl/kOfficialSignalingUrl` → `<coop-vps>`, commit `cd6faf81`, DLL
`AFBF5728` x4); **2026-07-19 s29d: compiled endpoints CUTOVER to the hostname `master.multivoid.dev`**
(`dcc988c7`, DLL `9370C1C1` — resolution measured native on both consumers: WinHttpConnect for the
master HTTP + getaddrinfo in signaling_client; the proxied ROOT `multivoid.dev` must never be used
for custom-port traffic). All four installs' `multivoid.ini` carry explicit `net.*` fallback rows,
rewritten to the hostname in the same sweep. Later the
same day the box was apt full-upgraded + rebooted (docker + WireGuard removed) and the whole stack
re-verified from outside (healthz, `/v1/latest`, signaling TCP, STUN). (`/v1/latest` answered proto 111
at that time; since v122 the compiled default is **0** = "no released record" and the env override is
left unset — zero releases exist, and a client treats proto<=0 as no verdict.)

**Domain: `multivoid.dev`** (LIVE 2026-07-19: root proxied via Cloudflare → <coop-vps>; `master.multivoid.dev`
unproxied/grey-cloud → the same box, for direct client traffic on our custom ports. The earlier `votv.mp` zone
never delegated and is retired).

**Tier B TLS — arcs 1-2 AS-BUILT + LIVE (2026-07-20), arcs 3-5 open.** A real Let's Encrypt certificate
on `master.multivoid.dev` (HTTP-01 via :80) terminated by `tokio-rustls` **inside our own bins** on NEW
ports **10443 (master) / 10442 (signaling)**, running beside the plaintext pair through the cutover
(`7aff6b73`); the client's master traffic moved to TLS with WinHTTP's default chain+hostname validation
(`87e66bce`). The client URL grammar is **SCHEMELESS = SECURE** — a bare `host:port` means TLS, so
`net.master` needs **no** `https://` prefix (an explicit `http://` is the deliberate downgrade for a
self-hoster without a certificate).
**Arcs 3 / 3b / 4 / 5 are ON HOLD as of 2026-07-20 (same day)** — a threat model was written and
reordered the work: the transport is already encrypted, so the next thing worth building is **peer
IDENTITY**, and **Tier C dissolves into peer certificates** instead of shipping as per-session
tokens. The
`net.master.insecure` flag discussed in that window was **never built and should not be**; the
`http://` downgrade grammar above is still what ships today, but it is queued for RULE-2 retirement.
**Read the local-only security register (`docs/security/`, untracked since 2026-08-23 — see
the local-only docs-arc note) before touching any of this**, so a validation lands at the right layer. The arc-1/2 as-built record + drills stay in
`research/findings/network/votv-tls-tier-b-c-DESIGN-2026-07-20.md` (superseded as a *plan*); the older
`votv-master-server-RE-and-rust-port-scope-2026-07-16.md` remains the server RE/port scope.
