# MULTIPLAYER_UI — coop menu design

**Living document.** Captures the user's vision + the approach decision for the
in-game multiplayer UI.

**Status (updated 2026-07-04): BUILT.** The menu shell + flows have shipped as
runtime-UMG built by our C++ mod (the "chosen" approach below) — see the `ui/`
modules: `server_browser.cpp` (the "future" browser below SHIPPED long since:
master-server lobby list + Direct Connect), `host_save_picker.cpp`,
`roster.cpp`, `scoreboard.cpp`, `dev_menu.cpp`, `moderation.cpp`, `hud.cpp`,
`skins_panel.cpp` (2026-07-02: the F1 > Cosmetics > Skins model browser —
gmod-style preview tiles from the LogicMods pak catalog + the v94 builtin
kerfur bodies, AS-BUILT; see docs/COOP_CLIENT_MODEL.md §3 for the skins
runtime). F1 > Cosmetics > Nameplate (v94, AS-BUILT 2026-07-02): the "show my
nameplate to other players" checkbox — a SYNCED per-peer pref (NameplateChange
+ the Join prefs byte; persists in votv-coop.ini `nameplate=`).
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
live in F1 > Cosmetics > Interface, persisted as votv-coop.ini `ui.font`;
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
late joiners) and persisted (votv-coop.ini `nick_color=RRGGBB`). ONE owner:
`coop/player/nick_color` (atomic per-slot store; 0 = surface default).
Consumers: nameplate nick (default white), chat nick prefix (default per-slot
palette), scoreboard row (default role gold/white — role stays readable via
the Link column). flash/occluded signal colors keep priority on the
nameplate. UI = inline hue-wheel picker, commit-on-release.
**F1 > Administration > Players (AS-BUILT 2026-07-05 `f66d2c7f`, verdict =
runbook 0w-b):** HOST-role-gated F1 category (dev_menu Cat/Sub `host` flag on
`roster::LocalIsHost` — clients/solo never see it): Online (roster rows +
Teleport/Kick/Ban), Offline (`coop/session/seen_players` — the persistent
GUID-keyed seen-players registry, votv-coop-players.txt `guid|nick|lastSeen|ip`,
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
| BP mod via UE4SS BPModLoader (cooked widget .pak loaded through UE4SS's BPModLoader Lua mod) | yes | adds a new asset (allowed) but needs UE4.27 editor + cook | yes (BPModLoader) | rejected — ties us to UE4SS |
| Sibling-pak hybrid (cooked widget .pak mounted via UE4's NATIVE auto-mount of `Content/Paks/`) | yes | adds a new asset | **no** (revisited 2026-05-25 — see note below) | deferred to Phase 7+ — toolchain cost not justified for current scope |
| ImGui overlay | no (debug look) | no | UE4SS's ImGui (or our own) | rejected for the menu — fine for dev/debug overlays only |

**2026-05-25 update (revisited after the user pushed back on "VT mod looks natural; we look dirty"):** the original rejection conflated BPModLoader-dependent paks (which DO require UE4SS) with all paks. UE4 itself auto-mounts every `.pak` it finds under `Content/Paks/` at engine startup — independently of UE4SS. So a sibling `votv-coop-content.pak` we author and ship alongside our DLL would mount cleanly without ANY mod-framework dependency (RULE 3 preserved). VOTV also ships `UPakLoaderLibrary` (Rama's PakLoader) + `URyRuntimePakHelpers` as Blueprint libraries already callable through our existing `ParamFrame` infrastructure, providing explicit `MountPakFile` if we want non-auto-mounted paks. See:

- [research/findings/votv-mp-hybrid-pak-architecture-2026-05-25.md](../research/findings/votv-mp-hybrid-pak-architecture-2026-05-25.md) — architectural verdict (stay all-DLL for now; revisit Phase 7+).
- [research/findings/votv-mp-pak-mount-feasibility-2026-05-25.md](../research/findings/votv-mp-pak-mount-feasibility-2026-05-25.md) — implementation feasibility (FEASIBLE without UE4SS via auto-mount or UPakLoaderLibrary).
- [research/findings/votv-mp-hybrid-pak-reality-check-2026-05-25.md](../research/findings/votv-mp-hybrid-pak-reality-check-2026-05-25.md) — reality-check verdict (stay all-DLL; the perceived gap closes with programmatic outline + shadow + UBorder polish).

The "chosen" row remains correct for the current scope. The "rejected" row's reasoning is preserved (BPModLoader specifically does tie us to UE4SS). A new "deferred" row replaces a small fraction of the rejected row's blast radius — the sibling-pak path is technically clean per RULE 3 but the 80 GB UE4.27-editor toolchain cost isn't justified until a Phase 7+ widget (server browser with sortable rows, etc.) actually needs it. For now, polish via programmatic UMG (text outline + drop shadow already shipped 2026-05-25; UBorder background panel queued).

So: our C++ mod hooks the menu's construction (`ui_menu_C` BeginPlay /
construct), creates our own widget tree at runtime via reflection, and
wires its buttons to the `coop/` session API. No VOTV asset is modified; we
add our widget alongside. Styling is hand-built in code (more effort than a
designer asset, but substrate-agnostic and asset-clean).

Dev/debug overlays (connection stats, entity inspector) stay ImGui — that's
tooling, not the player-facing menu.

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
