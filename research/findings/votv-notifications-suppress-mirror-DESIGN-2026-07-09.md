# Notifications (toast/email/console) suppress+mirror — DESIGN /qf convergence 2026-07-09

STATUS: **INCREMENT 1 (server-STATE mirror + client breaker-kill) BUILT + AGENT-AUDITED (0 CRITICAL/HIGH)
+ LAN-SMOKE-CLEAN + DEPLOYED 2026-07-09. NOT hands-on-verified** (the break->mirror->re-skin path needs a
broken server; the smoke world was all-healthy). Gates cleared: GATE-1 census DONE, GATE-2 scope RATIFIED,
M1+M2 build gates RESOLVED, design + mechanism + naming /qf DONE. RE docs: `docs/notifications/`.

## INCREMENT 1 — AS-BUILT (2026-07-09)
`coop/interactables/serverbox_sync.{h,cpp}` (317 LOC), the host-authoritative STATE mirror, modelled on the
proven `alarm_sync` poll-broadcast lane. Proto 106->107, DLL `3B2762CA`.
- **Wire:** `ReliableKind::ServerState=91` + `ServerStatePayload` (24B: brokenServers + effCalc/effDownl +
  serverCount + `uint64 isBrokenMask` -- a base's server farm ran **54** servers, smoke-caught, so the mask
  is 64-wide not 32). Router in `event_dispatch_world.cpp`; wired in `subsystems.cpp`.
- **HOST** (`Tick`, 1 Hz): read `mainGamemode.servers[]` (TArray @0x3F0) -> per-serverBox `IsBroken` bitmask
  + brokenServers/efficiency -> broadcast **on change** (edge-only; prime never sends). Late-join: current
  state to a world-ready joiner.
- **CLIENT** (`OnReliable`): DRIVE-REAL -- raw-write each `serverBox.IsBroken` (@0x378) then dispatch the
  box's own `check()` via reflected `CallFunction` (M2: check() re-skins PURELY from IsBroken) + mirror the
  gamemode aggregates. And **neutralize the local `ticker_serverBreaker`** (disable its actor tick -- the
  primary autonomous false-break source; smoke: "neutralized 1"). Client never sends (host-auth one-way).
- **Offsets resolved BY NAME + smoke-verified vs the CXXHeaderDump** (`servers=0x3F0 broken=0x8A0
  IsBroken=0x378 eff=0x400/0x404 check=yes`).
- **M2 clarification (resolves the audit MEDIUM-1 doc contradiction):** the census's "raw-write isBroken/
  damaged/health/upgrades" was the PRE-M2 inference; M2 MEASURED that `check()` keys on **IsBroken only**
  (it never reads `damaged`), so the isBroken mirror re-skins correctly. `damaged` feeds efficiency/health
  readouts, NOT the check() skin -- mirroring it (if a client-visible health readout diverges) is an Inc-2
  refinement, not needed for the visible break state.
- **Naming /qf (2 rounds):** renamed from `coop/world/server_sync` -> `coop/interactables/serverbox_sync`
  (engine-faithful `serverBox_C`; unambiguous vs the network server; co-located with its signal-device
  siblings signal_sync/console_state_sync -- world/ is a 14-file catch-all + would split the concept).
- **Audit: 0 CRITICAL/0 HIGH.** Fixed MEDIUM-2 (efficiency now in change-detection with an epsilon so it
  stays fresh if a download ramps it) + LOW-2 (write-side offset guards) + LOW-1 (comment). MEDIUM-1 = the
  M2 doc fix above.
- **NOT verified:** the actual break->client-re-skin (no broken server in the smoke). Hands-on: host breaks
  a server (or a server is saved-broken) -> client shows it broken + `sv.check` reads broken, and NO false
  autonomous "server down" on the client. Inc-2 = forward the host break EDGE for the true notice.

---
## (design / census / gates below -- the road to Inc-1)

Original design-pass status (superseded by AS-BUILT above): GATE-1 census DONE + GATE-2 scope RATIFIED,
design REFINED to a mirror-state + suppress-edge + forward-host-edge HYBRID. `/qf` design 2 rounds + census.

## The premise CORRECTION (why this is bigger than "suppress a toast")
The user asked to "suppress the client's false `SERVER "X" is down` toast + mirror the host's." The RE
proved that notice is **NOT a toast** — it is an **email** (`panel_SATconsole.Server Alert → addEmail`) +
a **console** `writeToLog` line + the **serverDown alarm**. And it is the tip of a **host-authoritative
server SIMULATION** (`ticker_serverBreaker` break/fix, `serverEfficiency`, per-`serverBox` state), none of
which is UE-replicated. So the real feature is **server-state sync**, not toast suppression.

## The /qf verdict (converged)
- **ROOT beats NARROW (RULE-1).** NARROW = per-channel suppress the client's self-authored notices +
  mirror the host's. ROOT = the **HOST owns server state; clients MIRROR + render and do NOT run the
  server sim** (neutralize the client `ticker_serverBreaker`). Under ROOT the client never fires
  `serverBroke` locally → never self-authors → **the entire suppress mechanism (poll-remove
  `ui_hints_C.verT_hinTs` / a `ProcessLocalScriptFunction` hook / hide-widget) is dead code** — a crutch
  NARROW only needs because it leaves the client running the sim.
- **Invariant, not a site-list:** "the client never runs the host-world server sim." NARROW leaves the
  SAT-console query lane (`sv.ping`/`sv.check`/`tw.check`…) reading the diverged local `servers[]` →
  permanently wrong; ROOT fixes it by construction. Counting that lane, ROOT is the *smaller* net blast
  radius.
- **State AND edge (likely two lanes, refuse-unification):** the server *condition* is STATE (poll
  `brokenServers` + per-serverBox struct — alarm_sync shape); the *down/damaged notice* is an EDGE
  (one-shot email/console/alarm author — event_fire shape). Probably a state-mirror lane + an
  edge-forward lane, not one.

## AUTHORING CENSUS — RUN 2026-07-09 (GATE-1 done; measured, corrects the /qf hypothesis)
Agent census over the disassembled serverBox/panel_SATconsole/ui_console/ticker_serverBreaker/mainGamemode/lib:
- **Q1 — the breaker is NOT the sole author.** ≥5 break paths call `serverBox.breakServer`: `ticker_serverBreaker`
  (host tick), serverBox self (in-box floppy/power/damage), **`dish.loadData` (WORLD-LOAD — a client save-loading
  the host world restores saved-broken servers)**, `trigger_breakDish`, `trigger_eventer.runSpecialEvent` (an
  EVENT). A 2nd authoring verb `break_type` is called by `analogDScreenTest` (the repair minigame). `fix` is
  called by `kerfurOmega`/`p_kerfus` (kerfur auto-repair). So **stopping the client ticker kills only 1 of ≥5**.
- **Q2 — drive-REAL is viable (no shadow needed).** `breakServer`/`fix`/`break_type` ENTANGLE state-set with
  authoring (broadcast `serverBroke`/`fixed`, spawn the repair minigame, `brokenServers±1`, `calcServerEff`,
  and `break_type` fires the Server Alert email/alarm). BUT **`check()` is a clean notify-free re-skin**
  (reads isBroken/damaged → SetMaterial/SetActive, no delegate/minigame). → mirror by raw-writing the fields
  (isBroken/damaged/health/upgrades) + `gamemode.brokenServers` then calling `check()`; NEVER call the verbs.
- **Q3 — the notice is an EDGE, two sinks, no state poll.** `ui_console.serverBroken` is bound to each
  `servers[].serverBroke` multicast delegate (one-shot per breakServer) → addEmail + writeToLog.
  `panel_SATconsole."Server Alert"` is a DIRECT call from `serverBox.break_type` (NOT a delegate subscription)
  → addEmail + writeToLog + `serverDown` alarm `Activate()`. Neither polls isBroken → a pure state-mirror does
  NOT re-author notices.
- **Q4 — `active_downl` does NOT toast.** "Server is down" = EMAIL (`!!!Automatic Alert System!!!`, body
  `Server "{serv}" is down!/has been damaged!`) + console writeToLog + the SAT-console alarm. serverBox's 4
  addHint sites are all floppy/upgrade prompts. (Corrects TOAST_SYSTEM_RE; confirms TOAST_CATALOG.)
- **Q5 — ROOT ALONE IS INSUFFICIENT (the decisive correction).** Stopping the client ticker kills path 1; but
  the EDGE authors fire from the client's OWN `breakServer`/`break_type` on non-ticker triggers — world-load
  dish (#2), events (#3), in-box (#4), repair-minigame (#5) — which survive both "stop ticker" and "mirror
  state". So the design MUST ALSO suppress the client's authoring. Narrowest seam (census): the **2 edge sinks**
  — `ui_console.serverBroken`, `panel_SATconsole."Server Alert"`, `serverDown.Activate()`.

## REFINED DESIGN (post-census — a mirror-state + suppress-edge + forward-host-edge HYBRID)
The pure "client never self-authors" ROOT was falsified (Q5). The measured design is host-authoritative with
THREE parts (still one invariant: *the host owns server state + its notices; the client renders*):
1. **STATE mirror (drive-real, alarm_sync shape):** host publishes `brokenServers` + per-serverBox
   {isBroken,damaged,upgrades,health} + serverEfficiency; the client raw-writes its real serverBox fields +
   calls `check()` to re-skin. Its SAT-console `sv.*`/`tw.*` queries then read TRUE state.
2. **GATE THE VERB, not the sinks (mechanism /qf refinement — invariant, not a site-list):** the notice +
   the state-mutation + the minigame all originate in the 3 verbs `breakServer`/`break_type`/`fix`. Gating
   THOSE once on the client puts all ≥5 break callers (ticker, world-load dish, events, in-box, minigame)
   under ONE authority-gate — timing-clean (unconditional on the client; no `world_load_episode` bracket
   needed, unlike the edge-suppress fallback). The client's serverBox becomes a pure puppet driven by (1).
   **BUILDABILITY GATE (M1):** the verbs dispatch via `EX_LocalVirtualFunction` (crude measure) → LIKELY
   invisible to BOTH our ProcessEvent detour AND the Func-patch (the `[[lesson-script-fn-invisible-to-func-patch]]`
   class). If confirmed, the verb-gate can't be a clean intercept → fall back to patching the NATIVE calls
   INSIDE the verb (the delegate broadcast / minigame spawn / Server Alert) or, last, the 2-edge-sink
   suppress (the site-list, with the world-load bracket). PRECISE schema-aware disasm of the verb call node
   is required to choose — NOT YET DONE.
3. **FORWARD the host notice EDGE (event_fire shape):** host forwards the real break/damaged edge ({server,
   damageType}); the client authors the true notice via a reflected addEmail/writeToLog/alarm (we are the
   dispatcher → visible), OR its now-gated edge sink fires only on the host-forwarded edge.
Two dispatch causes (STATE poll vs EDGE) → likely 2 wire lanes (refuse-unification confirmed).

## RESIDUAL RUNTIME PROBES (census-flagged — gate parts of the build)
- **P1 (drive-real gate):** confirm `check()` fully re-skins from a RAW-written `isBroken`/`damaged`, and locate
  the `EX_LetBool` that writes those bools (the census pinned upgrades/health writes, not the bool set). If
  check() doesn't re-skin from raw bools, the STATE applier needs another path. **Gates part 1.**
- **P2:** the exact in-box trigger of serverBox-internal breakServer (path #4 client-triggerability).
- **P3:** the player-repair→`fix` path (not in the dumps) + whether the repair minigame calls `fix` on success.
- **P4:** how to neutralize the 2 edge sinks on the client (they're BP delegate/direct calls — the suppress
  mechanism for part 2 must be RE'd: unbind the delegate, gate the handler, or suppress addEmail/writeToLog/
  alarm when client-locally-triggered).

## THE 2 GATES (before ANY build — GATE-1 now DONE)
1. **GATE-1 — the disasm AUTHORING CENSUS (a measurement, NOT YET RUN).** serverBox / panel_SATconsole /
   ui_console / ticker_serverBreaker are **absent from the bytecode dataset** (`research/pak_re/cfg/` has
   only mainGamemode/daynightCycle/directionalWind). Disassemble them (bp_reflect.py) to MEASURE, not infer:
   - Is `ticker_serverBreaker` the **SOLE** break/fix author, or do console-use / drone / damage / recovery
     ALSO author a server notice on the client? (If they do, neutralizing the breaker is insufficient —
     ROOT's key claim fails and a residual suppressor returns.)
   - Does writing the client's REAL `serverBox.{isBroken,upgrades,health}` **re-enter** the break/recovery
     sim (delegates, minigame spawn, `calcServerEff`)? → decides **drive-real-serverBox vs read-only-shadow**.
     (The alarm lane earned poll-a-bool only because `runTrigger` was first RE'd as idempotent; the
     equivalent property for server state is unmeasured.)
   - Does the down/damaged notice fire off the STATE or off the `serverBroke` delegate EDGE? → decides one
     lane vs two.
   - The exact channel per notice (which of serverBox's 4 addHint sites vs the Server Alert email/console/
     alarm fires on `active_downl`) + the exact localized string.
2. **GATE-2 — user COOP_SCOPE ratification.** Server-state sync is **not in `docs/COOP_SCOPE.md`** today —
   it's a NEW lane (the signal-server sim is core VOTV gameplay, unreplicated). ROOT is the right shape but
   it materially exceeds "suppress a toast." Needs the user's scope decision + a COOP_SCOPE amendment
   before the build investment.

## Valid Inc-1 (once the gates clear) — must exercise the RISKY half
NOT "mirror brokenServers + one isBroken so sv.ping reads true" (that proves only the cheap query half —
the F2-dupe trap of mapping one half). A valid first slice mirrors ONE server's break EDGE end-to-end: host
breaks a server → client's console/email/alarm shows the TRUE notice (mirrored, not self-authored) AND its
`sv.check` reads the true health — proving the state lane + the edge lane + the no-self-author invariant
together. Exact shape is designed AFTER the census (a MECHANISM /qf on the measured facts).

## Next steps (in order) — GATE-1 + GATE-2 + design/mechanism /qf all DONE; 2 build-gating measurements remain
1. ~~GATE-1 census~~ DONE. ~~GATE-2 scope~~ RATIFIED (COOP_SCOPE `4f2be8b7`). ~~Design + mechanism /qf~~ DONE
   (converged on the verb-gate hybrid above).
2. **M1 — precise verb-dispatch disasm** (schema-aware read of a `breakServer`/`break_type`/`fix` call node):
   is it `EX_LocalVirtualFunction` (invisible → verb-gate needs the native-inner-call patch or edge-suppress
   fallback) or a Func-visible route (verb-gate = a cancelling Func-patch)? This chooses the part-2 mechanism.
3. **M2 — P1 runtime probe:** does `serverBox.check()` re-skin from a RAW-written `isBroken`/`damaged` bool
   (+ locate the `EX_LetBool` field write)? Gates the whole part-1 STATE lane (fallback = re-implement the
   re-skin from SetMaterial/SetActive).
4. THEN: build **Inc-1** = the chosen verb-gate/suppress (part 2) + the drive-real STATE mirror (part 1) —
   host breaks a server → client `sv.check` reads broken + re-skins, ZERO false local notice. Then the
   IMPLEMENTATION /qf on the wiring, then Inc-2 = the FORWARD host notice edge (part 3).
Both M1 + M2 are read-only/probe measurements — the build cannot be correct without them (measure-don't-infer;
building the verb-gate on the inference that the verbs are interceptable is the exact trap the /qf caught).

Related: `docs/notifications/`, `docs/COOP_DISPATCH_VISIBILITY.md:126` (addHint invisibility),
`src/votv-coop/src/coop/world/alarm_sync.*` (state-poll lane precedent),
`coop/world/event_fire_sync` (edge-forward lane precedent), `reference/mtasa-blue/` (host-auth world state).
