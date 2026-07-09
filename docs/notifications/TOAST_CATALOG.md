# VOTV notification CATALOG — every on-screen notice, by system + coop verdict (RE 2026-07-09)

Cross-validated by two RE agents + a serverBox dig against `research/bp_reflection/*.json` +
`research/pak_re/cfg/*/*.txt`. Confidence: **[V]** code/reflection-verified · **[RD]** dump-derived ·
**[?]** needs a runtime probe. Companion: [TOAST_SYSTEM_RE.md](TOAST_SYSTEM_RE.md) (the `addHint`
mechanism + dispatch invisibility).

## 0. PREMISE CORRECTION — there are THREE notification systems, and "SERVER X is down" is NOT a toast
The user's example (`SERVER "X" is down`) is **not** an `addHint` HUD toast — it is an **email + a console
line + a physical alarm**. VOTV has no single "toast" API; the popups split into unrelated channels:

| System | API (owner) | Surfaces as | Sites |
|---|---|---|---|
| **Hint / toast** | `addHint(Text, byte type, bool debug, ctx)` — `lib_C`, via `mainGamemode."Add Hint from Gamemode"` | floating corner hint widget (`ui_hints_C`, see SYSTEM_RE) | 316 across ~65 BPs — **mostly LOCAL per-player interaction feedback** |
| **Email** | `addEmail(pfp, byte type, subject, body, self)` — `lib_C`→`mainGamemode.addEmail`→`laptop.addEmail` | **laptop inbox row + email-tab unread recolor** (no floating toast, no sound in this path) | 26 — **ALL host-authoritative** |
| **Console log** | `writeToLog(str, byte)` | text line on the in-world SAT/laptop console screen | panel_SATconsole, ui_console |
| **Alarm** (adjacent) | `serverDown` component `Activate()` | physical alarm on the SAT-console prop | panel_SATconsole |

`addHint` type byte (inferred **[?]**, not a decoded enum): 0 info · 1 warning · 2 error(red) · 3 Kel
inner-voice/sanity-hunger · 4 system. `addEmail` type: 0 normal · 2 alert/red · 8 anomaly/corrupted.

## 1. SERVER / SIGNAL family — the priority, fully expanded
**Authoritative state (all host-world-sim, NONE UE-replicated) [V serverBox.json]:**
- `mainGamemode.servers : array<serverBox_C>` (registry); `mainGamemode.brokenServers : int`
  (`breakServer` +1 / `fix` −1); `mainGamemode.serverEfficiency_calc` + `serverEfficiency_downl : float`
  (recomputed by `calcServerEff = lib.countServerEff(servers,0|1,self)` on every break/fix); per-server
  `serverBox_C.{isBroken, damaged, upgrades(0..3), health, minigame}`; `saveSlot.catchedSignals : name[]`.
- Broadcast via per-server multicast delegates `serverBox_C.serverBroke(self,bool)` + `fixed()`.
- **Break/fix [V]:** host-only `ticker_serverBreaker` (Tick, reads daytime/brokenServers/difficulty) picks a
  random `servers[]` → `breakServer` → gate `canBreak = RandomBoolWithWeight(Lerp(1,0.1,(upgrades/3)*2))` →
  broadcast `serverBroke`, `isBroken=1`, `brokenServers+=1`, `calcServerEff`, pick a minigame. `fix` reverses.
  **Recovery is SILENT** — there is NO "server back online" email/toast; only the `fixed` delegate.

| Message | Trigger | Caller.fn | Channel | Coop |
|---|---|---|---|---|
| `AUTOMATIC SERVER ALERT SYSTEM … Server "{X}" is down!` (subj `Server Alert!`, type 2) | a server's `serverBroke` | panel_SATconsole.**Server Alert** → addEmail | EMAIL | **SUPPRESS+MIRROR** |
| `>Satellite server "{name}" requires maintenance` | same | panel_SATconsole.Server Alert → writeToLog | CONSOLE | **SUPPRESS+MIRROR** |
| `serverDown` alarm activated | same | panel_SATconsole.Server Alert → Activate | ALARM | **SUPPRESS+MIRROR** |
| `Server "{X}" is down!` / `has been damaged!` (subj `!!!Automatic Alert System!!!`, type 2) | break/damage w/ `dmg_notif` | ui_console.ExecuteUbergraph | EMAIL+CONSOLE | **SUPPRESS+MIRROR** |
| `Radio tower is down!` / `damaged!` | radio-tower break | ui_console | EMAIL+CONSOLE | **SUPPRESS+MIRROR** |
| `Coordinate radar "cr{id}" is down!` / `damaged!` | coord-radar break | ui_console | EMAIL+CONSOLE | **SUPPRESS+MIRROR** |
| `We cannot triangulate your location Dr.Kel…` (subj `Cannot determine location`) | drone called w/ servers/console/tower down | drone.ExecuteUbergraph_drone | EMAIL | **SUPPRESS+MIRROR** |
| `New signal added in glossary` / `…entry completed` / `…updated` (type 0) | signal glossed | lib.**addGloss** → addHint | TOAST | **SUPPRESS+MIRROR** (dish/save-driven) |

**SAT-console query responses** (all read host server state → wrong on a bare client): `sv.ping` →
`Server found`/`not found`/`not responding`/`recieved {n}`; `sv.check` → `health: [{h}]`; `sv.upgrades` →
`{u}/3`; `tw.check` → `Online`/`Disconnected`; `cr.check` → health/`OFFLINE`; `tr.check` → `Active|Power…`/
`Cannot access to the satellite`; `sd.cal`/`sd.calall`/`sv.request`/`sv.eject`/`sv.hash`/`sv.target reset`.
All **SUPPRESS+MIRROR** (or, better: the console reads a MIRRORED host server state → answers correctly).

## 2. addHint TOAST catalog (316 sites) — by category + coop verdict
The vast majority are **LOCAL per-player interaction feedback — FINE AS-IS on a client.** Only a handful
read host world state.
- **(b) SAVE** (mainGamemode): `Cannot save in mid air`(1), `SAVE ERROR: invalid mode`/`fridge door open`(2),
  `Error saving objects/triggers/primitives: main map does not match`(2), `Dupe detected! object deleted`(2).
  The `loadObjects`/`loadTriggers` `prohibited`/`main map doesnt match` are `debug=true` → **editor-only (won't
  show in shipping)**. Coop: saving is host-authoritative → local-to-whoever-triggers-a-save.
- **(c) network/online:** `List download error! No connection…`(2), `List download failed, another attempt`(1)
  (online content fetch — **LOCAL per peer**); `Custom Content is disabled!`; `Restart the game to apply the RHI!`.
- **(d) gameplay warnings — mostly LOCAL:** `Flashlight battery empty`, `I don't feel so good…`(3), `Too hot!`/
  `Too cold!`, sleep family, inventory/equipment, hundreds of prop-interaction hints (ATV/laptop/buckets/…).
  **Host-driven exceptions (ambiguous, per-player eval):** `radiationDetected: High levels…`(mainPlayer, host-
  driven source but per-player location), `Stop killing nisse…`(2, creature count), out-of-bounds `gay baby jail
  activate…`/`now leave`/`escape` (per-player position enforcement).
- **(e) tutorial/hints (LOCAL):** `_map_untitled_1` first-load tips (F5 quicksave, World Rules, custom content,
  disable popups, Hints&Tips, rebind controls). The `mainGamemode` `>…` lines are **Kel story monologue** (host
  story progression → host-driven).
- **(f) achievement/unlock:** none via addHint (closest = the `addGloss` glossary hints in §1).
- **(g) misc/dev (host-only by nature — client shouldn't run these):** `Error spawning Wisp: out of navmesh`,
  `Invalid event`(trigger_eventer/lib.setEvent), `Spawn error! Try again`(kerfurOmega), `Kerfur cannot find
  broken server`/`This kerfur is active`(findBrokenServer repair-bot), `respawnServer: Restart the game…`(dish).

## 3. EMAIL family (26 sites) — ALL host-authoritative
`mainGamemode.addEmail → laptop.addEmail` = Array_Add a `struct_email` row + recolor `button_tab_email` when
off-tab (unread badge). **No global HUD toast, no dedicated sound.** [V]
- **Task/reward:** `Daily task` / `Daily task results` (lib.add_task/setTaskNew/processTask), `Weekly results`×3
  (daynightCycle).
- **World-event story:** `Do not panic`/`Keep going` (newsky blackout), glitched `SORRY`(type 8), `Minor
  inconvenience`(trigger_jamDoor reactor/breaker), `Package recieved`(drone), data-driven radio `getResponse`.
- **Server:** the `Server Alert!` / `!!!Automatic Alert System!!!` emails (§1).

## 4. Coop sync surface + priority SUPPRESS+MIRROR list
**Mirror this host state (so the client's console/emails read TRUE, and it needn't self-compute):**
`mainGamemode.{brokenServers, serverEfficiency_calc, serverEfficiency_downl}` + per-`serverBox_C.{isBroken,
damaged, upgrades, health}` + `saveSlot.catchedSignals`; and forward the `serverBroke`/`fixed` delegate events
host→client instead of each peer authoring its own emails/hints/alarms.

**Priority (most-misleading first):**
1. `panel_SATconsole.Server Alert` — the `Server "{X}" is down!` EMAIL + `requires maintenance` CONSOLE + the
   `serverDown` ALARM. A client with an empty/diverged `servers[]` fires it wrongly (or misses it).
2. `ui_console` `!!!Automatic Alert System!!!` — server/radio-tower/coord-radar down/damaged emails+log.
3. SAT-console `sv.*`/`tw.*`/`cr.*`/`tr.*`/`sd.*` query responses (read host server state).
4. `lib.addGloss` signal-glossary hints (dish/signal sim + catchedSignals).
5. All 26 `addEmail` sites (host-only generation; mirror the host inbox / `struct_email`).
6. Host-side dev/spawn hints (`Error spawning Wisp…`, `Invalid event`, kerfur `findBrokenServer`).

**LOCAL / leave alone:** the ~280 prop-interaction/inventory/equipment/vehicle/build-tool `addHint` toasts,
first-load tutorial tips, RHI/settings hints, per-player sanity/hunger/flashlight warnings.

## 5. THE SCOPE QUESTION (for the design /qf + the user)
The user asked to "suppress the client's false SERVER-X-is-down and mirror the host's." The RE shows this is
**not a toast** but the tip of a **host-authoritative server-simulation** (break/fix ticker, efficiency,
emails, console, alarm). Two framings:
- **NARROW (symptom):** suppress the client's *self-authored* server emails/console/alarm/glossary + mirror the
  host's — a targeted per-channel intercept + a small `serverBroke/fixed` + email wire. Does NOT make the
  client's SAT-console query answers correct (they'd still read the diverged local `servers[]`).
- **ROOT (RULE-1):** mirror the host's server state (brokenServers/efficiency/per-server isBroken/upgrades/
  health + catchedSignals) so the client neither runs `ticker_serverBreaker` NOR self-authors any server
  notification, and its console/emails read the TRUE host state. Larger, but the faithful coop shape.
The design /qf pressures which increment is the least-crutches first step. Server-state sync may be its own
lane (docs/COOP_SCOPE amendment) — flag for the user before the big build.

## Flagged guesses / not byte-verified
- addHint + addEmail type-byte legends are **inferred from usage**, not decoded enums. **[?]**
- `serverBroke` delegate → `panel_SATconsole.Server Alert` binding is **inferred** (high conf; not the dumped
  `ComponentDelegateBinding` map). **[?]**
- `spaceRenderer.signalFound` is an 18-byte ubergraph stub; no user string (visible signal notices appear to
  be only the `addGloss` hints). **[?]**
- Dataset has **316** addHint sites disassembled (mainGamemode/daynightCycle/directionalWind in cfg/*.txt; the
  rest from bp_reflection JSON); full dump at the scratchpad `addhint_all.txt`.
- Whether `serverBox.active_downl` ALSO calls `addHint` (a floating toast) IN ADDITION to the email/console/
  alarm (system-RE agent asserted it; catalog agent centered on email/console/alarm) — **[?] reconcile with a
  runtime repro or a serverBox.active_downl bytecode read before designing the suppress mechanism per-channel.**
