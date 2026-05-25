# MULTIPLAYER_UI — coop menu design

**Living document.** Captures the user's vision for the in-game multiplayer
UI. The networking behind it is gated (methodology Phase 3+); this doc is
the design + the approach decision so we build the right thing when we get
there. Status: **design only, not yet built.**

## User vision (2026-05-22)

A multiplayer option surfaced **in VOTV's own main menu** (native, not a
debug overlay), with:

1. **Play as Host** → choose a save (new or existing) → load → start
   hosting (listen for one client; LAN-first).
2. **Connect** → type an IP → join the host's session.
3. **Server browser** (future) → list discoverable LAN/known hosts → join.

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
    └── Server browser  (future: LAN broadcast discovery; later a registry)
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

## Gating

**Status 2026-05-25:** the Phase 2 orphan + Phase 3 transport + session
API are all SHIPPED. The menu's prerequisites are met. Build is
deferred because the env-var `.bat` launchers
(`mp_host_game.bat` / `mp_client_connect.bat`) cover the hands-on
workflow adequately while feature work on replication (5N* / 5S* / 5T /
5D) is the active priority. The menu shell will land once the
per-feature replication surface stabilises; until then, see the
launchers + `docs/AUTONOMOUS_TESTING.md` for the runtime entry points.
