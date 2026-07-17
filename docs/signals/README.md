# docs/signals/ — the signal-processing subsystem coop knowledge base

**The concept (one folder = one domain, per the folder-per-domain-concept rule):** everything the
player does at the **signal desk / console workstation** to turn a signal from space into a decoded
recording — catch it on a dish, tune frequency + polarity to lock it, download it, watch the
detector needle, read the coord/log screens, drive the cursor. VOTV *is* signal processing; this is
its core loop, and it is a distinct concept from the generic base-computer devices (keypads, doors,
lockers, power) which stay in `research/findings/computers-devices/` and the device-occupancy layer.

This folder answers **"what exactly happens in the signal pipeline, and how does coop keep every
peer's screens + download progress identical."** The cross-cutting contracts stay where they are —
these docs LINK to them, never restate them (RULE 2):

- **`docs/COOP_RNG_AUTHORITY.md`** — the RNG host-authority tracker. The download-rate divergence is
  its row **T2-5b** (AS-BUILT **v112** `desk_sim_sync` + `desk_input_sync` — the BUGS-v111 fix,
  same day; smoke PASS, awaiting hands-on; + the T2-5c signal-chain RNG census). Signal
  RNG-authority PROGRESS is tracked THERE; this folder tracks subsystem COMPLETENESS.
- **`docs/COOP_SYNC_MAP.md`** — where each wire lane lives (rows: `console_state_sync`,
  `signal_sync`/`signal_catch_sync`/`signal_wire`, `desk_cursor_sync`, `comp_sync`). Find the file
  that owns a lane there; find the element's STATUS here.
- **`src/votv-coop/src/coop/interactables/`** + **`coop/world/`** — the shipped lanes
  (`device_occupancy`, `console_state_sync`, `desk_cursor_sync`, `signal_*`). Code is authority; a
  tracker row cites it.

## The native pipeline (ground truth — RE'd)

The signal-processing loop, source → decoded, and the RE finding that owns each stage:

1. **Generation** — the sky-signal director rolls signals into space (host-auth roller today).
   RE: `research/findings/computers-devices/votv-base-computers-RE-2026-06-11.md` §5.4.
2. **Catch** — a dish aimed at the signal's coords catches it; `coord_signalData` becomes the
   caught truth (v70 host-owned). RE: base-computers §5, `votv-connection-selector-events-pinecone`.
3. **Tune (freq + polarity)** — at `AanalogDScreenTest_C` the player animates a frequency-filter
   offset and a polarity-filter angle; how close each is to the signal's true freq/polarity sets the
   download SPEED. Wrong polarity DIRECTION or a zeroed filter HALTS the download. **This is a LIVE
   MECHANIC**, not cosmetic. Full byte-by-byte RE:
   `research/findings/computers-devices/votv-desk-download-machine-RE-2026-07-15.md`.
4. **Download** — `decoded += DL_downloading` per tick until `decoded >= size`; the rate formula
   folds freq-match × polarity-match × the detector needle × a noise term × upgrades/servers. **Two
   of those terms are per-peer RNG** (the detector needle `DL_resDetecPercent`, the `noise` term) —
   the divergence root. (The `comp_*` refiner pane is a SEPARATE mechanic, no freq/pol knobs.)
5. **Detect / classify** — the detector needle `DL_resDetecPercent` integrates to 1.0 → the signal
   becomes save-able (beep); coord screens (ping/scan/log) render the analysis.
6. **Screens + cursor** — all of the above renders through shared console widgets (one
   `ui_consolesAtlas`/`ui_console`/`ui_radar` instance — per-player screens are impossible without
   editing assets, A6), operated by the desk coords-panel cursor. Occupancy (`DeviceClaim=51`) is the
   per-player routing. **The desk keyboard verbs (SHIFT/1/2/3/ENTER/arrows) enter through
   `ui_consolesAtlas.OnKeyDown/Up` — the PE-visible key router on the occupant's machine** (RE'd
   2026-07-16; the panel actor's own input events are dead stubs). Screens design + gap list:
   `research/findings/computers-devices/votv-screens-panels-sync-DESIGN-2026-07-03.md`.

The chain does NOT end at the download. Downstream (all RE'd byte-level 2026-07-16 —
`votv-signal-chain-units-RE-2026-07-16.md`, the four desk units are panes of ONE
`AanalogDScreenTest_C`):

7. **Save** — unit-2 SAVE SIGNAL (gate: detector >= 1, NOT decoded>=size) mints the signal's ID
   (`lib::setSignalID` = GenerateRandomBytes(16), per-peer PRNG — an identity hazard) and appends
   to **`gamemode.savedSignals_0`** (the deck list). DELETE (under the lid) aborts the ACTIVE
   space signal instead. The red phone is a decorative RNG world event ("Doesn't work" is
   literal).
8. **Play deck (unit 3, ASO)** — plays the level-variant audio/image/text of a selected row
   (world-audible `signalSound`), SARV = display contrast remap, volume knob. IMPORT/EXPORT
   **MOVES** the row list<->drive (`prop_drive.Data_0`). SAVE SIGNAL here COPIES the row to the
   **Meadow DATABASE** (`saveSlot.savedSignals_0`, via `laptop.addSignal`).
9. **Drive chain** — `AdriveSlot_C` freezes+teleports the prop (never destroys — identity
   survives insertion); `prop_drive.Data_0` @0x550 is the payload (name/id/level/size/image);
   LED = level template + red/green/yellow color. `AsignalDriveEraser_C` = 3 s wipe of the whole
   payload.
10. **Processing (unit 4, comp)** — the CompState/CompData "refiner": per-tick RNG-noised rate,
    upgrade gate `level < upg_processLvl`, level-up re-mints the ID, level 3 fires per-signal
    world triggers; target-level knob + auto-continue need physMod byte 3.
11. **Tapes + the daily task** — `Awallunit_tapes_C` accrues both reels 1 Hz (`+= dt/speed`);
    reels ride the props; drone `sell` grades `saveSlot.taskNew.reel_big/small` (reward only if
    BOTH > 0). `votv-tape-caddy-daily-task-RE-2026-07-16.md`.
12. **The 24 big dishes** — every catch slews all of them to one shared target; rest pose is
    per-peer RNG at world load (never saved) + per-slew RNG everywhere; two ambient tickers add
    per-peer slews/decay. `votv-dish-rotation-RE-2026-07-16.md`.

## The four coop shapes (pick PER ELEMENT — never sync the desk as one blob)

From the screens design (verified against MTA); a signal element picks exactly one:

1. **Symmetric poll-delta** — state inert once set, applying locally is valid (screen on/off,
   discrete toggles). Zero local latency.
2. **Intent → host executes → broadcasts state** — once-only or host-simulated (signal CATCH,
   claim entry, saved-signal append).
3. **Host-auth roller / sim** — host owns the simulation + all RNG; client SUPPRESSES its own tick +
   RNG and mirrors the outputs (sky-signal generation today; the **download machine** is the next
   member — the whole rate/needle/noise sim moves host-side, freq/pol knobs become host-sim-inputs).
4. **Claim + owner-stream** — one occupant streams a continuous quantity 1-3 Hz while claimed; adopt
   at join, release snapshot for the next enterer (the desk cursor as an unreliable pose stream; the
   coord-log tail).

**The load-bearing rule this subsystem taught (2026-07-15):** the desk is **MIXED ownership** —
cursor (client-passthrough) beside freq/pol/download (host-auth). Sync each FIELD by its own owner;
a blob-sync of the desk fuses a passthrough field with a host-owned one and diverges. And once you
animate a host-auth MIRROR (the smooth cursor), you can WAKE dormant per-peer output generators that
were previously idle on the mirror side — after animating a mirror, owner-gate every per-peer
producer reading the moving field (`[[lesson-smooth-mirror-wakes-dormant-per-peer-generation]]`).

## The method (per element, like docs/events/)

Every signal element earns its row in `TRACKER.md` only when it reaches VERIFIED:

1. **RE** it (the desk-download RE is the template: byte-by-byte formula + a field-ownership table
   splitting host-authoritative / host-sim-input / already-owned / sim-constant).
2. **Design** the sync — pick ONE of the four shapes per field; for a host-auth sim, name every
   field the client must SUPPRESS and every one it mirrors. `/qf 15` before building (user rule).
3. **Build** the lane; bump `kProtocolVersion` on any wire change
   (`[[feedback-wire-format-change-bumps-protocol-version]]`).
4. **Verify** — hands-on or a matching live log; state the TAKE line (the download take: host +
   client show the same `decoded %` and detector needle with identical knobs). Autonomous smoke is
   NOT VERIFIED.

## Status at a glance → `TRACKER.md`

The living element-by-element ledger is **[TRACKER.md](TRACKER.md)**. Headline (2026-07-17 pm):
the transport-layer elements are SHIPPED (occupancy, desk scalars, dish-aim, saved signals, clock
v110, freq/pol + download-rate SIM v111, the BUGS-v111 fix v112, **L4 dish kinematics v113**
commit `f204c0f7`, **L7 tape caddy + daily task v114** commit `ba8ce297`, and **v115 desk AUDIO
mirror + cursor v2** commit `c5ff11a4` — the user's mid-v114-hands-on reports (missing
keypress/beep/loop sounds for observers; cursor jerks; the momentum tail lost at dismount) fixed
at the native audio seam (Func-patch Play/SetActive/Activate, relayed DeskSndFx=105) + a
claim-DECOUPLED cursor stream with a settle-gated momentum tail and an adaptive interp window;
smoke PASS x2 + the e2e audio self-test proven host->client; and **v115b the PHANTOM ping-FSM
fix** commit `de31889e` — the user's LIVE 14:46 ping test caught the v112 coordIsPing raw apply
WAKING a parallel ping sim on the host [the FSM is a latent tick machine gated on the flag,
analogd uber @82980] -> divergent verdicts + a phantom ARM raising the mirrored detector during
a local FAIL + double coordLog authorship + a false post-catch DISARM stomp; fixed same evening:
receivers never write the flag [bookkeeping only], a desk FSM-hold claim covers the pinger's
run, and the arm poll's re-init-window predicate; design
`votv-ping-fsm-phantom-v115b-DESIGN-2026-07-17.md`). **NOT hands-on: v112, v113, v114, v115 AND
v115b all await the user take** (batched — FIVE layers stack; runbook
`research/handson_runbook_2026-07-17_desk_v114.md` take 3; per-lane log prefixes keep
attribution: `desk_input:`/`dish_sync:`/`[reel]`/`[task]`/`desk_snd:`/`desk_cursor:`/
`FSM-hold`). The v111 hands-on FAILED on 5 fronts — all designed out in v112
(`votv-desk-input-lane-DESIGN-2026-07-16.md`). Remaining OPEN: OPEN-1 (cursor jerks — v115b
removed two more mechanisms [the phantom's per-tick machine on the observer + the double-author
text thrash]; apply cadence measured CLEAN 60 Hz; a smoke-env lead points at the client
npc_client walk ~30 ms/tick — re-judge at the take), OPEN-2 (coordLog cluster), OPEN-3
(upgrade-sync), OPEN-5/6/8/9 (L4+L7 are BUILT; the train order is L6 -> L8 -> L5 -> L9).
