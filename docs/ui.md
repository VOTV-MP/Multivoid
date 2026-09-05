# The user interface

## Purpose

Everything the mod draws: the MULTIPLAYER button and the native screens behind it (the server
browser, the two hosting windows, the address and name entry), the loading and failure surfaces,
the version line on the main menu, the F1 overlay and its panels, the in-game HUD (nameplates,
chat, the scoreboard), and the game's own notifications and what the mod does about them. The
game's widget style that the native screens must match is measured in
[VOTV_UI_STYLE.md](VOTV_UI_STYLE.md).

## How it works

### Two surfaces, one rule

Player-facing menus are native: hand-built Unreal widgets, constructed through reflection with
no Blueprint, no editor and no pak, added as children of the game's own menu switcher and shown
by writing the switcher's active index, exactly as the game shows its own sub-screens
(`ui/native_screen` is the kit: the measured palette, a framed box, a styled text block, a real
button cloned from a game donor). Developer and in-session surfaces are the mod's own Dear ImGui
overlay. A native screen is judged against the game's style; the overlay is a developer surface
and is not pretending to be part of the game.

### The main menu

A native MULTIPLAYER button is injected into the game's main menu above NEW GAME
(`ui/multiplayer_menu`); its click is detected by polling the button's hovered state on the menu's
own tick, because binding a delegate was not available when it was built. The button opens the
native server browser (`ui/server_browser_native`): the master server's lobby list with name,
players, version, world and age, an amber mark on a version pair that differs from ours, which
always means the connect will be refused, a Host button and a Join. The address and the nickname
are typed in their own small windows rather than inline (`ui/browser_input_screens`). The old
ImGui browser stays as a fallback chosen by an ini row and needing a restart
(`ui/server_browser`, `ui/server_browser_surface`).

Hosting is two windows. The first chooses the world (a save, or a new game through the game's
own save creation) and how the session is reachable, direct or brokered through the master
(`ui/host_window_native`, `ui/host_save_picker`); the second chooses who may join and who may
find it: a password, and whether the lobby is listed (`ui/host_session_settings`). Only then is
Host offered.

The top row of the game's own build labels gets one more native label: the mod's game target and
build, cyan like the button, amber when the master reports a newer release, and silent while the
master has no released record.

### Joining, and when it fails

While a client joins, the menu's widgets are hidden so only the 3D backdrop remains, and the
loading screen draws a centred progress bar with a Cancel button over it, in four stages
(`ui/loading_screen`); the console auto-shows beside it so the connect log is visible
(`ui/console`). A full-screen curtain hides the world's assembly and fades out when the host's
snapshot has landed (`ui/join_curtain`). A join that cannot be established shows a "could not
connect" modal with the reason over the reopened browser; a cancel is silent
(`ui/connect_failed_dialog`). A boot-time install problem (the game updated and the mod needs a
new release) shows its own modal (`ui/boot_warning_dialog`).

### The overlay

The overlay is one Dear ImGui host over the game's swap chain: it hooks the present and resize
calls and the window procedure through the mod's own detour substrate, detects the graphics API
at the swap chain, and draws on DirectX 11 and 12 alike (`ui/imgui_overlay`,
`ui/overlay_backend`). It survives other overlays and capture software on the same swap chain.
Fonts are vendored families embedded in the DLL with Cyrillic ranges, rasterised at the real
size for the current resolution; the whole overlay scales with the window height and a user size
preference (`ui/fonts`, `ui/scale`, `ui/style`). While any interactive surface is up the overlay
captures input, suppresses the game's cursor recentring and hands the pointer back on close
(`ui/input_focus`, `ui/overlay_cursor`); a keystroke typed into a text field never also fires a
game bind.

F1 opens the menu: a category tree whose developer categories are hidden unless the developer
switch is on (`ui/dev_menu`). For every player it holds Cosmetics (skins on a live mannequin,
the nameplate preference, the nickname colour, the font and size), Network (the live statistics
panel), World (the rules this peer runs under), Voice, and on the host Administration (online,
offline and banned players with teleport, kick, ban and unban). The config review panel shows the
boot sweep's findings on the ini before any session exists.

### In the game

The HUD is passive and always on: nameplates projected over remote players with nickname, ping
and a health bar, greyed and half-transparent behind geometry; the chat feed with a fading tail;
the voice indicators (`ui/hud`, `ui/voice_icons`). T opens the chat bar, Enter sends, Escape
closes without sending and falls through to the game's pause menu (`ui/chat_input`). The tilde
key shows the player list: a hold-to-peek on a client, a toggle on the host with the per-player
action menu (`ui/scoreboard`). The mod's own surfaces play the game's own menu sounds through the
game's audio engine so they honour its volume settings (`ui/menu_sfx`).

### The game's notifications

The game's corner toasts, emails, console lines and alarms are never networked: each machine's
own game evaluates its local state and paints its own widget through one static function that
no hook sees. The mod does not touch the widget. Where a client would show a false notice from
diverged state, the mod drives the state that produces it instead: the signal servers' broken
set is host-authored and driven into each client's server boxes, so a client never authors a
false "server down". The purely local notices (an item that cannot be used while held) are left
alone.

## Who owns what

| State | Owner | Shape |
|---|---|---|
| the browser's list | the master server | fetched on open and refresh |
| the hosting choices | the host, at creation | the reachability and the listing are announced once |
| the loading state | the joining client | a snapshot the screen renders |
| the version line's verdict | the master's latest record | silent without one |
| nameplates, the scoreboard | game-thread snapshots | render-thread draws |
| the notifications | each machine's game | the state behind them is synced, not the toast |

## Wire messages

None of its own: every surface renders a snapshot owned by a lane on another page. The preferences
it edits (skin, nameplate, colour) travel on the players page's kinds; the lobby list and the
update check are the master server's HTTP.

## Late join

The loading screen, the curtain and the console are the joiner's own view of the join; the
scoreboard fills as roster rows arrive; nameplates appear with each puppet's first pose.

## Known limits

| Limit | Evidence |
|---|---|
| The native screens' frames do not yet reproduce the game's bevelled, nested border material; the flat border was measured wrong | `[V]` the style doc's frame section |
| The game's own toasts a client self-generates from diverged world state are not suppressed or mirrored; only the server state behind one family is driven | `[V]` the notification catalog |
| The scoreboard and the connect dialogs are verified by screenshot and the rig, not by hand | `[V]` the capture scripts |
| The old ImGui browser is kept as a fallback, a deliberate exception to retiring replaced code | `[V]` the surface chooser |

## Code map

| Concept | Files |
|---|---|
| the main menu and the native screens | `ui/multiplayer_menu`, `ui/server_browser_native` with `ui/server_browser_rows`, `ui/server_browser_actions`, `ui/server_browser_panels`, `ui/server_browser_surface`, `ui/host_window_native`, `ui/host_save_picker`, `ui/host_session_settings`, `ui/host_session_choices`, `ui/browser_input_screens`, `ui/native_screen`, `ui/native_text_field`, `ue_wrap/engine/umg_build` |
| the fallback browser | `ui/server_browser` |
| joining and failing | `ui/loading_screen`, `ui/join_curtain`, `ui/connect_failed_dialog`, `ui/boot_warning_dialog`, `ui/console` |
| the overlay host | `ui/imgui_overlay`, `ui/overlay_backend`, `ui/overlay_backend_dx11`, `ui/overlay_backend_dx12`, `ui/overlay_cursor`, `ui/input_focus`, `ui/fonts`, `ui/atlas_watch`, `ui/scale`, `ui/style` |
| the F1 panels | `ui/dev_menu`, `ui/skins_panel`, `ui/voice_panel`, `ui/world_rules_panel`, `ui/net_stats_panel`, `ui/admin_panel`, `ui/config_review_panel` |
| the HUD | `ui/hud`, `ui/chat_input`, `ui/chat_view`, `ui/scoreboard`, `ui/voice_icons`, `ui/menu_sfx`, `ui/link_format` |
| tests | `python tools/mp.py browser` (the browser lab run), `tools/net/roster_shot.ps1` (the four-peer scoreboard capture) |
