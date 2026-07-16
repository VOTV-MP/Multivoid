# Signal-Desk Download/Detector/Freq-Polarity State Machine — RE (2026-07-15)

Byte-by-byte RE of the `AanalogDScreenTest_C` download mechanic, produced to design the
host-authoritative sync (user 2026-07-15: "the way the user sets up frequency and polarity
CORRELATES to the download SPEED of the signal from space" — a LIVE MECHANIC desync, not
cosmetic). Sources: `research/bp_reflection/analogDScreenTest.json` (Kismet bytecode),
`CXXHeaderDump/analogDScreenTest.hpp` (offsets), `src/votv-coop/include/ue_wrap/console_desk.h`.
Statement indices `[N]` are into the flat top-level list of `ExecuteUbergraph_analogDScreenTest`
(the UAsset JSON has no per-statement byte offsets; the one byte anchor is `PUSH ->66736` at
`[2118]` = the `DL_downloading` rate block, matching the `@66736` note in console_desk.h).
`calculate_download`/`pingComp`/`process_coords`/`useSearch`/`used`/`player_use` are all THUNKS
into the ubergraph.

## A. THE DOWNLOAD-SPEED FORMULA (the core mechanic) [MEASURED]

Accumulator = `DL_SignalDownloadDLData.decoded` (member of `Fstruct_signalDataDynamic
DL_SignalDownloadDLData @0x0978`). Per-tick: `decoded += DL_downloading` `[1946]`; complete when
`decoded >= .size` `[1947]`. (`comp_progress`/`comp_downloading` are the SEPARATE refiner pane —
no freq/pol knobs — NOT this mechanic.)

The rate `DL_downloading @0x0A2C` is recomputed each tick (block `@66736`, `[2138]-[2175]`),
gated on `IsValid(DL_signalDownloadData.mesh)` `[2130]`:

```
DL_downloading = (DL_frData*8) * (DL_poData*8)          <-- FREQ x POLARITY MATCH = the driver
               * DL_downloadSpeed * DeltaSeconds
               * (1 - noise)                            <-- noise = RandomFloat-0.5 * DL_noiseScale, clamp01  [PER-PEER RNG]
               * (DL_precMult*4) * DL_downloadMultiplier
               * qualityMult(signal_qualiy)             <-- 0.5..1.5 by tier
               * DL_resDetecPercent                     <-- the detection needle  [PER-PEER RNG]
               * BoolToFloat(usesp_downl) * powerUsage
               * serverMult(serverEfficiency/ignoreServers)
               * Lerp(1,4, upg_downloadSpd/16*2)
```
If `DL_frData==0` OR `DL_poData==0` -> `DL_downloading=0` -> download HALTS.

### The freq/polarity error terms (the user-facing tuning)
- `DL_frData @0x0A0C` `[2059-2077]`: `closeness = (1000 - |signalFreq - DL_FrFilterOffset|)/1000`;
  `DL_frData = FClamp((closeness - frequencySpread) * 1/(1-freqSpread), 0, 1) * Lerp(20,1, filtSizeUpg)`.
  The explicit `|knob - signalFreq|` distance term — closer knob => faster download.
- `DL_poData @0x0A10` `[2000-2019]`: ANGULAR (dot-product) match of `GetRotated2D(DL_poFilterOffset)`
  vs the signal polarity vector, PLUS a HARD gate `DL_SignalDownloadDLData.polarity == DL_PolarityDir`
  (`[2000]`) — wrong polarity DIRECTION => `DL_poData=0` => stall.

### The detection needle `DL_resDetecPercent @0x0970` (a factor above) [MEASURED]
`[124-135]`: `inc = RandomFloatInRange(0.001, Lerp(0.004,0.02, upg_scanner)) * DL_detectorMultiplier`;
`resDetec = FClamp(resDetec + inc, 0, 1)`. At `>=1` -> canSaveSignal + beep. **RandomFloatInRange =
per-peer RNG.**

### Why the knobs are "NOT INSTANT" (user finding #7) [MEASURED]
Buttons set a SPEED; the OFFSET integrates per tick:
`[2122-2128]`: `if(DL_activePoFilter) DL_poFilterOffset += DL_poFilterSpeed*dt` (wrap %360);
`if(DL_activeFrFilter) DL_FrFilterOffset += DL_FrFilterSpeed*dt` (clamp 0..1000). The animated ramp
IS the knob motion; a mirror that only gets a 1 Hz snapshot of the offset lags/steps.

### DIVERGENCE ROOT
The ENTIRE chain runs per-peer inside the tick: offsets integrate locally, frData/poData derive
locally, AND two RNG terms (needle `[131]`, noise `[2155]`) roll independently per peer. Nothing is
host-owned today -> two peers compute different download rates = the mechanic desync. (The caught
signal's true freq/polarity via `getSignalSData`->`coord_signalData @0x0A38` IS already v70
host-owned — the divergence is entirely the desk-local SIM, not the catch.)

## B. FIELD OWNERSHIP (the fix unit = who owns each field)

**host-authoritative (sim OUTPUT — host owns, client mirrors, client SUPPRESSES local sim):**
`DL_downloading @0x0A2C`, `DL_SignalDownloadDLData.decoded`, `DL_resDetecPercent @0x0970` (RNG),
`DL_frData @0x0A0C`, `DL_poData @0x0A10`.

**host-sim-input (player authors intent; host must OWN the value that feeds the sim):**
`DL_FrFilterOffset @0x09F8`, `DL_poFilterOffset @0x09F4` (integrated), `DL_FrFilterSpeed @0x0A00`,
`DL_poFilterSpeed @0x09FC`, `DL_activeFrFilter @0x0A04`, `DL_activePoFilter @0x0A05`,
`DL_PolarityDir @0x0A30` (the hard gate).

**already host-owned:** `coord_signalData @0x0A38` (v70 catch truth), `DL_signalDownloadData.mesh`
(arm gate, `formDownload`), `DL_SignalDownloadDLData.{size,polarity,frequency,quality}`
(`initDownloadSignal`, host rolls).

**sim-constant (from already-synced difficulty/upgrades/servers; NOT written in-asset — CDO/external):**
`DL_downloadSpeed/Multiplier/precMult/noiseScale/detectorMultiplier/ignoreServers`. `DL_difficultyDownload`
is computed `[527]` but NOT read in the rate formula.

**Dispatch:** every mutator here is inline `EX_Let`/`EX_LetBool`/`VictoryFloatPlusEquals` (native
store — PE-invisible AND Func-patch-invisible) or `addFilterParam`/`initDownloadSignal` (`EX_Local*`,
also invisible). => The fix CANNOT verb-hook; it must MIRROR STATE + SUPPRESS the local sim
(`[[lesson-votv-world-system-sync-mirror-state-not-verb]]`).

## C. THE VERBS (player actions)

- **Freq knob**: `button_downl_FF_spdAdd_{1,5,15}` -> `addFilterParam(±1)` -> `DL_FrFilterSpeed += ±{1,10,100}`
  clamp ±1000. Toggle `button_downl_FF_toggle` -> `DL_activeFrFilter=!` `[875]` (tag `toggleFrequencyPressed`).
- **Polarity knob**: `..._PF_spdAdd_{1,5,15}` -> `DL_poFilterSpeed += ±{1,5,15}` clamp ±360. Toggle
  `..._PF_toggle` -> `DL_activePoFilter=!` `[861]`. Dir toggle `..._pdir_toggle` -> `DL_PolarityDir=(x+1)%3` `[850-852]`.
- **SHIFT quick-scan (cursor screen)** *(CORRECTED 2026-07-16 — the original line here fused two
  distinct verbs)*: `useSearch`->uber(83010) = SHIFT: `playPingSound(newdesk_beepLong1)`,
  `spawnDirs()` (direction-arrow UMG widgets), `coord_cooldown=coord_maxCooldown`. **SHIFT does
  NOT touch the ping FSM.** The stage FSM (`coord_pingStage`, rings `coord_ping_ping/inner/outer`
  `[2546/2560/2574]`, gated `coord_isPing @0x0A7C`) belongs to the ENTER triangulation ping; its
  set-TRUE lives in **`ui_consolesAtlas.OnKeyDown` [45-56]** (the widget key router — resolved
  2026-07-16). Full unit-1 RE: `votv-signal-chain-units-RE-2026-07-16.md` §0-§1.
- The `buttonPressed_frequencyToggle/polarityToggle` class delegates are EMPTY stubs (RETURN only) — real
  logic is the inline button-switch.

## DESIGN IMPLICATIONS (for the /qf, NOT yet designed)
1. The download sim (rate + accumulator + needle + noise) is pure host-authoritative output -> host runs
   it, client MIRRORS + SUPPRESSES its own tick accrual + RNG. (RNG-authority item — see
   docs/COOP_RNG_AUTHORITY.md; host rolls needle+noise.)
2. The knob OFFSETS are host-sim-inputs: client's button presses = INTENT to the host; host applies to
   ITS offset/speed/dir state which drives ITS sim; host streams the offsets back for the mirror's
   animated display. (Split: client-authored intent up, host-owned value down.)
3. Because every mutator is EX_Local*/inline (dispatch-invisible), the client must SUPPRESS its local
   integrator + RNG (state-neutralize) and mirror the host outputs — cannot intercept the verb.
4. Screen is MIXED-ownership: cursor (client-passthrough, shipped) + freq/pol/download (host-auth /
   host-sim-input). Do NOT sync as one blob — split by field ownership (user rule 2026-07-15).
5. Transport: the animated offsets + the needle want an unreliable STREAM (user: "as a stream udp");
   the discrete toggles (active/dir) + arm/complete edges want reliable.

---

## AS-BUILT (v111, 2026-07-15, `33cd7404`) — hands-on 2026-07-16 FAILED (5 bugs root-caused: `votv-desk-sim-v111-coop-bugs-audit-2026-07-16.md` / signals TRACKER BUGS-v111)

The `/qf` design pass (5 measurement rounds, scratchpad qf_thread.md) CONVERGED BY SHRINKING: it
opened toward a host-authoritative sim MIGRATION and ended at a small fix, because each round deleted
an assumption by measurement:
- **Seed-sync REFUTED** — `noise` is UNSEEDED *and* TRANSIENT (a `RandomFloat` consumed in-expression,
  never stored to a member; grep-confirmed: 0 RandomStream in the asset, line 298395 is the audio
  asset not a field). So the client re-rolls it every tick inside the formula -> no seed and no field
  to inject -> `decoded += DL_downloading` can never agree while the client evaluates the formula.
  Host-authoritative was FORCED, not the pattern-default reach.
- **frData/poData do NOT "converge for free"** — they read a filter-size upgrade with NO live sync lane
  (grep-empty), so a mid-session upgrade purchase would silently diverge them. Fix: stream them
  host-auth (6->8 scalars), don't rely on native convergence. (The upgrade lane is its own workstream:
  signals OPEN-3.)
- **The client sim writes NOTHING that propagates** (display-local only), and the mirror OVERWRITES
  the diverging scalars -> the fix is "host owns + streams the output vector; client overwrites its
  local garbage", not a sim suppression. The one field the mirror can't overwrite (coordLog, an
  append-buffer) is kept SEPARATE (signals OPEN-2).
- **The host ALREADY consumes the occupant's DeskState** (`OnDeskState` applies on any non-holder; the
  "host never applies" at console_state_sync.cpp:517 is SkySignal-specific) -> no new host-consume seam.

AS-BUILT: `coop/interactables/desk_sim_sync` + `MsgType::DeskSimPose=38` (unreliable, host->all, ~10Hz,
newest-wins). Host reads `ReadSimOutputs` + `SetHostDeskSim`; client `TryGetHostDeskSim` + multi-channel
LerpWindow interp (cursor pattern) + `WriteSimOutputs` (raw every tick, screen repaint pulsed ~3Hz).
Gate 1: the live DeskState apply keeps the local sim-output fields (console_state_sync.cpp ~594); adopt
still seeds them. Proto 110->111. Audit READY (0 CRITICAL). DLL `84e431bef0bd6982` deployed x4.
Take (verify): freq/pol numbers match on both peers + the knob ramp is smooth (not stepped).
