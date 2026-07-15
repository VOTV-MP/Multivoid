# Hands-on runbook — desk divergence repro + this session's fixes (2026-07-15, take 1)

DEPLOYED: `votv-coop.dll 65C0AC37E83BF40D` x4 (HOST + CLIENT_1/2/3), verified by hash.
kProtoVer **109** (no wire changes this session). HEAD `b67d7063`; the desk work + 3 fixes
below are UNCOMMITTED (held pending this hands-on). `[dev] desk_diag=1` set on HOST + CLIENT_1.

## What changed in this DLL (all BUILT + DEPLOYED, NONE hands-on VERIFIED yet)
1. **Weather exit-crash fix** — `weather_sync.cpp ResolveCycle()` now `IsLiveByIndex` (was plain
   `IsLive`, passed the recycled `daynightCycle`->`crematorDoor` slot -> `setRainParticles` fatal).
2. **desk_diag q-gate fix** — `q=Y` now also requires `!activeFrFilter && !activePoFilter` (offset
   integrates mid-ramp; the old gate would tag `q=Y` mid-ramp).
3. **Chat-leak-into-menu fix** — `FleeToMainMenu` now clears chat_input/bubbles/nameplate/voice_panel
   (was only chat_feed).
4. **Cursor WASD lag** — interp window 50ms -> 33ms (extrapolation queued if still lags).
5. **desk_diag probe** — the divergence instrument (4/5 symptoms; stationary-PC deferred, class unresolved).

## STEPS (host + client, both hands-on)
1. HOST loads a FRESH save (or New Game), creates session; CLIENT joins.
2. HOST sits at the signal desk (E), tunes a signal: move the coords cursor (WASD), start a
   download, **turn the frequency + polarity knobs**, aim dishes.
3. **PAUSE ~5 s at GENUINELY settled points**: filters OFF (not mid-ramp), decode idle, dishes stopped.
   These are the `q=Y` samples.
4. Let CLIENT repeat after HOST releases (both directions).
5. **Exit to menu** — confirm (a) NO crash (weather fix), (b) NO chat box / bubbles / stray HUD in
   the menu (chat fix), (c) cursor felt less laggy (33ms).

## WHAT TO READ IN THE LOG (order matters)
1. **VALIDATE the q-tag FIRST** — grep `[desk_diag]`, confirm `q=Y` lines cluster during the real
   pauses and NOT mid-drag / mid-ramp. If `q=Y` lands mid-motion, the gate is still wrong -> STOP,
   fix it before trusting any diff. ([[lesson-comparability-tag-inputs-need-measured-validation]])
2. **Grep each `lines=N` against a known-positive** before trusting a zero (`surface=coordLog2Text`
   vs `surface=sat_console` are near-identical names). ([[lesson-negative-grep-verify-against-known-positive]])
3. **DIFF `q=Y` HOST vs CLIENT**: `frData`/`poData` (NOT the `[jitter]` offsets), `needle` (exhaust),
   `decoded` at rest, per-dish `target`, coordLog line-set. The download-rate divergence is the
   mechanic-desync item (RNG in the rate — [[lesson-rng-in-rate-path-is-mechanic-desync]]).
4. **JOIN-ADOPT**: grep `JOIN-PRE-ADOPT` — the client's pre-adopt scalars vs the host seed (join-seed
   vs runtime drift).

## HONEST STATUS
- The 5 changes COMPILE + DEPLOY + hash-verify. NONE is hands-on verified. The weather fix targets the
  exact measured crash signature but the recur-on-a-timing-race nature means only a clean exit run
  confirms it. The q-gate fix is measure-derived (offset integrates while active) but the log must show
  `q=Y` actually avoids the ramp.
- Copy BOTH `votv-coop.log` (HOST + CLIENT_1) to scratchpad BEFORE relaunch (they truncate at boot).

## NEXT after the logs
Validate q-tag -> read divergence -> `/qf` the freq/polarity host-authoritative design (host owns
sim+RNG, client suppresses its tick, knobs intent-up, offsets INTERPOLATED like the cursor, screen
split by ownership). Then the ping (`coord_isPing` external setter) + PC power button each need their
own surface-resolution. RE foundation: `research/findings/computers-devices/votv-desk-download-machine-RE-2026-07-15.md`.

---
## UPDATE 2026-07-15 eve — probe SHIPPED + analyzed; pending hands-on is now CLOCK F
The desk_diag probe RAN (session 17:07-17:18) and produced valid census; committed `2de202ed`.
MEASURED divergences: `decoded`/`pol` (host 0.0064/1 vs client 0.0262/0), `coordCooldown`
(per-peer timer), `coordLog2Text` (host 55 vs client 34 lines), `coordIsPing` (local-only). The
freq/pol FILTER offsets stayed 0 (no signal caught) → that specific divergence still needs a
re-test with a LOCKED signal, but the rate divergence is proven. The host-auth freq/pol FIX is
UNBUILT (next, needs `/qf`). Details folded into `docs/COOP_RNG_AUTHORITY.md` T2-5b.

### CLOCK design F — the current pending hands-on (deployed `9a4a2ef8b5f3a142`+, v110)
NOTE: a proto bump to 110 + this doc pass will REDEPLOY a new hash — use whatever
`tools/deploy-all.ps1` last printed / `wc`-verified, NOT 9a4a2ef, for the clock test.
TAKE (2 peers, one look): host + client show the **same HH:MM at a paused moment**. Log cue
(client): one-shot `time_sync: applied CONNECT-EDGE host clock` on join, then
`time_sync: applied STREAM host clock` ~every 10 s; and NO `SendReliable(TimeSync)` spam on the
host. Design: `research/findings/computers-devices/votv-clock-sync-design-F-2026-07-15.md`.

### DESK download-SIM host-auth (v111) — pending hands-on (DLL `84e431bef0bd6982`, proto 111)
What changed: the freq/pol + download-rate sim is now host-authoritative (`desk_sim_sync` /
`MsgType::DeskSimPose=38`). Host owns the sim + streams the 8-float output vector ~10Hz; client
interpolates + overwrites. Knob intents stay occupant-authored.
STEPS: both peers to the signal desk; catch a signal; arm a download; on one peer tune the
frequency + polarity knobs while the OTHER peer watches the same desk (or a joiner enters after).
TAKE: (1) the **download % and the freq/polarity numbers MATCH on both peers** (were host 0.0064 vs
client 0.0262); (2) the **knob ramp is SMOOTH, not stepped** (offset rides the interp buffer);
(3) a **110-vs-111 peer pair HARD-CLOSEs** at the gate (proves the proto bump). Log cue: `sync:desk_sim`
in the walk-timer output. Design/as-built:
`research/findings/computers-devices/votv-desk-download-machine-RE-2026-07-15.md` (AS-BUILT section).
STATUS: built + deployed + audit READY (0 CRITICAL); NOT hands-on.
