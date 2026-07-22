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
| BP mod via UE4SS BPModLoader (cooked widget .pak loaded through UE4SS's BPModLoader Lua mod) | yes | adds a new asset (allowed) but needs UE4.27 editor + cook | yes (BPModLoader) | rejected — ties us to UE4SS |
| Sibling-pak hybrid (cooked widget .pak mounted via UE4's NATIVE auto-mount of `Content/Paks/`) | yes | adds a new asset | **no** (revisited 2026-05-25 — see note below) | deferred to the public-server phase — toolchain cost not justified for current scope |
| ImGui overlay | no (debug look) | no | UE4SS's ImGui (or our own) | rejected for the menu — fine for dev/debug overlays only |

**2026-05-25 update (revisited after the user pushed back on "VT mod looks natural; we look dirty"):** the original rejection conflated BPModLoader-dependent paks (which DO require UE4SS) with all paks. UE4 itself auto-mounts every `.pak` it finds under `Content/Paks/` at engine startup — independently of UE4SS. So a sibling `votv-coop-content.pak` we author and ship alongside our DLL would mount cleanly without ANY mod-framework dependency (RULE 3 preserved). VOTV also ships `UPakLoaderLibrary` (Rama's PakLoader) + `URyRuntimePakHelpers` as Blueprint libraries already callable through our existing `ParamFrame` infrastructure, providing explicit `MountPakFile` if we want non-auto-mounted paks. See:

- [research/findings/architecture-audits/votv-mp-pak-mount-feasibility-2026-05-25.md](../research/findings/architecture-audits/votv-mp-pak-mount-feasibility-2026-05-25.md) — implementation feasibility (FEASIBLE without UE4SS via auto-mount or UPakLoaderLibrary).
- Architectural + reality-check verdicts (stay all-DLL for now, revisit the public-server phase; the perceived polish gap closes with programmatic outline + shadow + UBorder): recorded in this section — the planned standalone `hybrid-pak-architecture` / `hybrid-pak-reality-check` finding files were never filed (dead links removed 2026-07-12).

The "chosen" row remains correct for the current scope. The "rejected" row's reasoning is preserved (BPModLoader specifically does tie us to UE4SS). A new "deferred" row replaces a small fraction of the rejected row's blast radius — the sibling-pak path is technically clean per RULE 3 but the 80 GB UE4.27-editor toolchain cost isn't justified until a the public-server phase widget (server browser with sortable rows, etc.) actually needs it. For now, polish via programmatic UMG (text outline + drop shadow already shipped 2026-05-25; UBorder background panel queued).

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
`/etc/coop-master.env` — a release bump is an env edit + restart, no rebuild; docs/RELEASE.md step 5).
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

## Master / signaling server — Rust, on the NEW coop VPS `172.86.94.3` (migrated 2026-07-16)

The master + signaling servers are **Rust** (`tools/coop-server-rs/`, static musl), wire-compatible
with the retired Python (byte-exact TURN cred), with the 4-agent security audit's Tier A hardening
built in (relay-OOM cap, atomic admission, IPv6 /64 rate-keying, panic-isolation, coturn
quotas/denies; client JSON-depth crash-fix + parse clamps).

**2026-07-16 (evening): the whole stack MIGRATED to a new VPS** (`172.86.94.3`; the old box is
repurposed for unrelated services, its coop stack wiped per RULE 2 and verified dead). The new box was provisioned
Rust-native by the reworked `tools/vps_provision.sh` (Rust ExecStart — no Python ever landed there;
ufw allows incl. 80/tcp for Let's Encrypt; `curl -4` public-IP fix) and **functionally verified from
outside**: healthz, `/v1/latest`→111, host→join with full ICE, signaling relay A→B, leave + TTL=90
reaper, TURN-cred HMAC match vs the box's coturn secret. Compiled official endpoints flipped
(`protocol.h` `kOfficialMasterUrl/kOfficialSignalingUrl` → `172.86.94.3`, commit `cd6faf81`, DLL
`AFBF5728` x4); **2026-07-19 s29d: compiled endpoints CUTOVER to the hostname `master.multivoid.dev`**
(`dcc988c7`, DLL `9370C1C1` — resolution measured native on both consumers: WinHttpConnect for the
master HTTP + getaddrinfo in signaling_client; the proxied ROOT `multivoid.dev` must never be used
for custom-port traffic). All four installs' `multivoid.ini` carry explicit `net.*` fallback rows,
rewritten to the hostname in the same sweep. Later the
same day the box was apt full-upgraded + rebooted (docker + WireGuard removed) and the whole stack
re-verified from outside (healthz, `/v1/latest`, signaling TCP, STUN). (`/v1/latest` answered proto 111
at that time; since v122 the compiled default is **0** = "no released record" and the env override is
left unset — zero releases exist, and a client treats proto<=0 as no verdict.)

**Domain: `multivoid.dev`** (LIVE 2026-07-19: root proxied via Cloudflare → 172.86.94.3; `master.multivoid.dev`
unproxied/grey-cloud → the same box, for direct client traffic on our custom ports. The earlier `votv.mp` zone
never delegated and is retired).

**Tier B TLS — arcs 1-2 AS-BUILT + LIVE (2026-07-20), arcs 3-5 open.** A real Let's Encrypt certificate
on `master.multivoid.dev` (HTTP-01 via :80) terminated by `tokio-rustls` **inside our own bins** on NEW
ports **10443 (master) / 10442 (signaling)**, running beside the plaintext pair through the cutover
(`7aff6b73`); the client's master traffic moved to TLS with WinHTTP's default chain+hostname validation
(`87e66bce`). The client URL grammar is **SCHEMELESS = SECURE** — a bare `host:port` means TLS, so
`net.master` needs **no** `https://` prefix (an explicit `http://` is the deliberate downgrade for a
self-hoster without a certificate).
**Still cleartext:** the **signaling** channel. **Arcs 3 / 3b / 4 / 5 are ON HOLD as of 2026-07-20
(same day)** — a threat model was written and reordered the work: GNS is encrypted but
**peer-UNAUTHENTICATED**, so the real gap is peer identity rather than transport encryption, and
**Tier C dissolves into peer certificates** instead of shipping as per-session tokens. The
`net.master.insecure` flag discussed in that window was **never built and should not be**; the
`http://` downgrade grammar above is still what ships today, but it is queued for RULE-2 retirement.
**Read `docs/security/` (README = threat model, TRACKER = 20 OPEN findings) before touching any of
this.** The arc-1/2 as-built record + drills stay in
`research/findings/network/votv-tls-tier-b-c-DESIGN-2026-07-20.md` (superseded as a *plan*); the older
`votv-master-server-RE-and-rust-port-scope-2026-07-16.md` remains the server RE/port scope.
