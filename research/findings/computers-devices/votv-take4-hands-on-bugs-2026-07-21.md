# take-4 hands-on bug inventory -- signal workstation + drive disc + drone + world (2026-07-21)

**Point-in-time hands-on triage log.** DLL `multivoid-0.9.0n-122.dll` (md5 `96719c4230d531ba...`),
proto 122, both peers same build. HEAD at test time `635021c8`. NO code shipped this session -- this is
the diagnosis pass; each root gets its own `/qf` session next. One CONFIG fix applied (R12).

Evidence tags: **[V] measured** (log line / code file:line cited) vs **[H] hypothesis** (not yet proven;
needs a fresh log slice or a /qf).

The user drove both peers and reported symptoms live; log slices pulled from
`Game_0.9.0n_{HOST,CLIENT_1}/.../multivoid.log`. 21 reported symptoms collapse to **16 bug roots**
(some share one root) + 1 config (closed) + 1 improvement.

Source list of record (kept in sync during the session):
`scratchpad/take4_signal_desk_bugs.md`. This doc is the durable promotion.

---

## Severity 1 -- DATA LOSS (player state destroyed/lost)

### R14 + R15 + R16 -- the DRIVE DISC cluster (`prop_drive_C`) -- ONE root, ONE /qf  [V]
Symptoms #18 (disc "died"), #19 (content red->green late), #20 (green disc -> red/empty on the host).

**Unified root (measured):** a `prop_drive_C` disc's CONTENT is not bound to its IDENTITY/BIRTH, and its
identity CHURNS on every interaction. Each grab / hold (R-Hold) / drop / R-tap DESTROYS the disc actor and
RE-SPAWNS a fresh one under the SAME save-key with a NEW eid (or eid=0 while in hand). A freshly-spawned
`prop_drive_C` is born EMPTY (red lamp); the exported signal content must re-arrive LATE on the separate
`DrivePayload` chunk lane.

Measured evidence (client + host logs 13:49-13:56), disc key `3DO0UBQxDorLHVTYxroTHA` / `YtOeF48uLcuQDxqQ2q0TcA`:
- Respawn per interaction: `[PROP-DROP] CLIENT authored drop intent ... prop_drive_C` -> `[PROP-DROP] HOST
  spawned client-placed prop ... prop_drive_C` -> `grab_hook[Aprop.Init POST]: HOST broadcasting SPAWN
  cls='prop_drive_C' key=...`, a NEW eid each time (`CreateOrAdoptPropMirror eid=10796,10797,10798,10800,
  10803,10804,10809...`, all same key, ~9 respawns in 3 min).
- eid=0 in hand + destroy loop: dozens of `grab_hook[destroy-seam]: CLIENT broadcasting DESTROY ...
  key='YtOeF48...' eid=0` + `[ROCK-DROP] hand-edge: ex-hand actor ... DEAD (destroyed in-hand)`; host
  `remote_prop::OnDestroy: key '...' eid=0 -> destroying local actor` interleaved with `... has no local
  actor YET -- DEFERRING`.
- Content on a late lane: `drive_sync: payload applied eid=10797 (name='unknown [1]' size=1) from slot 0`
  fires AFTER the adopt -> red-then-green.
- Trash-collector churn: `trash_collect: BROADCAST held untracked item cls='prop_drive_C' key='3DO0...'
  eid=10804` -- the disc is also caught as an "untracked item".

**Consequences (all measured):**
- R14/#18: R-TAP-to-hotbar destroys the world actor + syncs DESTROY, but inventory POSSESSION isn't synced
  -> the disc "vanishes" on the host instead of being pocketed. (User: the action was a single R-tap; the
  disc went into the client inventory, but the world-actor disappearance desynced.)
- R15/#19: a freshly-adopted disc is red; content arrives late on DrivePayload -> visible red->green flash
  after every respawn.
- R16/#20: interacting with a green disc respawns a fresh EMPTY disc. **DIRECTION (user):** the disc goes
  red ONLY ON THE HOST; the CLIENT still sees GREEN + the signal ID. **Trigger is broad: ANY client
  grab/hold (R-Hold)/drop of a content disc**, not only drop. Since the HOST OWNS THE SAVE, the signal is
  lost on persist -> the worst data-loss facet.

**Load-bearing fix (rule 1) -- /qf CONVERGED 2026-07-21 (8 rounds), design of record
`votv-drive-disc-content-birth-DESIGN-2026-07-21.md` (committed `d14b6644`, NOT built).** The converged
root is NARROWER than this section first proposed. The "keep ONE stable identity / no respawn per
interaction" (custody) idea was RETRACTED on measurement: a held/collected disc is per-player INVENTORY
data custodied SEPARATELY from the world save, so the pickup-destroy is CORRECT and a hidden host world
custody-actor would DOUBLE-WRITE (see `[[lesson-held-collected-prop-is-per-player-inventory-not-a-world-actor]]`).
A client drop is a genuine inventory->world BIRTH; `PropDropIntent` carried only a `savedScalar` FLOAT,
not the disc's signal content -> host born empty. Fix: generalize the birth channel float -> a per-class
CONTENT trailer (inline; reuse `DC::ReadDriveRow`/`WriteDriveRow` + `signal_wire`), retiring `savedScalar`
+ the drive birth-emission `NoteLocalDriveBirth`, KEEPING `drive_sync`'s steady-state lane (a disc
mutating in a rack != birth-state). Retirement is a PRECONDITION gated on hands-on reel+disc green. OPEN
residual: the churn adopt->destroy re-fire is UNEXPLAINED -> post-fix re-measure (design section 7).

### R9 -- garage DOOR (big shutter `garageDoor`) client-open DROPPED on host  [V]
Symptom #12. HARD divergence -- the door stays CLOSED on the host permanently.
- CLIENT 13:36:51 `garage: sent ON key='garageDoor'`.
- HOST 13:37:17 `garage: retry tick -- applied 0 deferred, dropped 1 expired, 0 still pending`.
- Distinct from the `door:` lane (interior doors): CLIENT `basedoor_garage` 13:36:48 DID apply on the host
  (`door: host opened+held key='basedoor_garage' slot=1`). Two subsystems; only the big shutter failed.

**ROOT (converged 2026-07-21 via `/qf` + kismet-analyzer bytecode + asset dig; NOT "key mismatch at
receive"):** the host garage is present but **UNKEYED** (`Key=None`). The garage is keyed by the
gamemode's **one-shot, sublevel-gated** pass (`mainGamemode::loadObjects` -> `GetAllActorsWithInterface`
+ `loadTriggers`, gated by `isSublevelAllowed`/`getCurrentLevelFromGamemode` -- kismet-analyzer bytecode).
A garage instance mid-recycle during the host's menu->save world transition (host garage index 1->0 at
13:15:35) is not in that snapshot and never gets keyed; `Channel::RebuildIndex` skips None-key instances
-> the host index has 0 garages FOREVER -> the client's OPEN edge defers into an empty index and expires
at `kPendingTTL=25s`. Sibling controls (SAME reload): 50 doors keyed on the host (same `triggerBase`
keying code, same 0x0260 Key) -> makeKeys ran; only the garage's tile was absent at that instant. B3
(actor absent) refuted: the host renders a CLOSED shutter + the menu garage was present+keyed.

**FIX (BUILT + smoke-verified, `multivoid-0.9.0n-123.dll` md5 `94b15d35...`, proto 122->123, NOT
hands-on):** key the garage by its **level-export FName** (`ue_wrap::garage::GetNameKey`, the
`door_box::GetNameKey` pattern) instead of the fragile save Key. The export FName is baked into the
cooked `untitled_1.umap` package -> deterministic + cross-peer stable BY CONSTRUCTION (both peers load
the identical package), and independent of the race-prone keying. The old Key path (GetKeyString,
g_keyOff, kKeyOffFallback, the triggerBase_C Key resolution) is DELETED (RULE 2). **Smoke proof
(2026-07-21 17:38, menu->save reload present = doors 19->50):** host garage index = **1, never 0**
(one state, name-hash `0x7414DA7199178EA1`), host==client keysHash identical, `garage: connect-snapshot
-- sent 1 full state(s) (of 1 indexed)` (take-4 sent 0-of-0). The identity/index layer -- the whole R9
root -- is fixed; the physical client-open -> host-apply is the hands-on take (never broken by anything
but the missing index). Empirical control: `door_box`'s name-based keysHash was byte-identical host vs
client through the SAME take-4 reload that broke the garage's Key identity.

**DEFERRED (Part 2, no take-4 repro -- its own pass):** even with a perfect identity, the SYMMETRIC
channel loses a live edge to an actor UNLOADED at edge-time (host player away from the garage's
proximity-streamed tile -> pending expires at 25s, no re-assert). A reliable "shutter is OPEN" that can
silently expire is a rule-1 crutch. Repro to build against: host walks away from the garage -> client
opens it -> host returns -> does the OPEN survive? Fix-shape: pending never TTL-expires while the class
is resolvable-but-absent (apply on stream-in), or the sender re-asserts. Whole keyed-interactable class,
not garage-only. Design of record: the `/qf` thread + this section. Owner: `interactable_channel.h`.
**LATENT class exposure (documented, principle 4 -- don't broadly migrate blind):** doors/lights/
containers are also save-Key-based and could hit the same one-shot sublevel-gated miss; they came
through THIS reload keyed+working, so they stay Key-based. If one ever exhibits the miss, it gets the
same targeted FName treatment (the FName is strictly more race-proof for a placed actor).

### R11 -- drone delivery SACK contents empty on client, full on host  [H]
Symptom #14. Screenshots: host sack = MRE/Zip drive/Drive/Data tape A/B/case ~686 vol; client = 0.0.
- The sack prop exists on both peers; its container CONTENTS diverge. Candidate roots: (a) the daily FREE
  delivery is a HOST loot-RNG spawn (COOP_RNG_AUTHORITY) never spawned on the client; (b) items exist but
  the container INVENTORY isn't synced. `order_sync.cpp` handles player ORDERS (OrderData/items) but the
  daily free delivery may bypass it; `prop_container_extract.cpp` exists.

---

## Severity 2 -- HIGH (degrades every interaction / very visible)

### R4 -- CLIENT interaction-prompt flicker (GLOBAL) + local desk responsiveness  [H, strong clue]
Symptoms #3 (ALL client E-prompts flicker ~2 Hz), #7 (own knobs +1/+5/+15 feel like 300 ms ping).
- CRITICAL CLUE (user): the flicker is SYNCHRONIZED -- at the SAME instant BOTH the E-interaction GUI
  prompts AND the unit-2 TOGGLE POLARITY + TOGGLE FREQUENCY physical LAMPS flicker together, ~2 Hz,
  CLIENT-ONLY. Hits even NON-busy devices (the free drone console), so it is NOT the busy-path aim-clear
  (`device_occupancy.cpp:233` returns early when free).
- 2 Hz ~= 2x the 250 ms poll -> signature of a PING-PONG ECHO LOOP: client polls a wire-applied toggle as
  a local edge, ships it, host applies + its poll ships back, client re-applies -> lamps oscillate and the
  shared device widget (5 of 7 device families render ONE shared widget) rebuilds -> every prompt flickers.
  Prime suspect: `desk_input_sync::OnDeskInput` echo-prime baseline not covering the active-toggle re-apply.
- /qf must FIRST measure the ping-pong in the logs (repeating DeskInput active_* ships ~2 Hz), then decide.

---

## Severity 3 -- the "POLL-not-EDGE" family (discrete events discovered by a periodic array-diff)  [V]
All three: a discrete user EVENT is detected by a periodic full-array diff instead of captured at the
native verb -> the change lags by up to the poll interval. Same fix shape (hook the verb / edge-capture),
worth ONE shared /qf framing.

- **R2** (#4 spam Toggle polarity not mirrored): `desk_input_sync.cpp:25` polls every 250 ms and ships only
  the NET delta baseline<->current -> a toggle flipped back within one window reads `cur==baseline` ->
  nothing sent. Log: client SCAN + 2x DOT charges, host replayed only the SCAN.
- **R7** (#10 stationary PC power syncs very slowly): `laptop_sync.cpp:455` power edge on a 250 ms poll
  behind the laptop resolve/instance/prime gate (`:415-428`); receiver converges via `CallPowerToggle()`
  (`:387-400`) with open DEFERRED while unpowered (`:394`) -> multi-tick convergence.
- **R17** (#21 unit-3 signal LIST export/import lags): `signal_sync.cpp:27` AND `comp_sync.cpp:29` poll
  arrays at 1000 ms -> the list updates up to ~1 s late.

---

## Severity 3 -- "SNAP/absent-interp of a continuous value"

### R1 -- dish pose is a 4 Hz HARD SNAP, zero interpolation  [V]
Symptoms #6 (mini-radar arrow jumps during a turn), #9 (client dishes ~5 fps, should be smooth native).
The on-screen arrow IS the physical dish bearing.
- Receiver applies each pose row via `K2_SetRelativeRotation(bTeleport=true)` -- exact snap, NO lerp:
  `dish_sync.cpp:145-146` -> `dish.cpp:339-352` (`SetRelRot` `dish.cpp:165-173`). Stream 4 Hz movers-only
  (`dish_sync.cpp:26`). A full turn sampled at ~4 yaw values/sec = stepping.
- NOT an angle-wrap bug (nothing lerps; QuantDeg normalises to [0,360), `protocol.h:4616-4623`). Client
  self-sim is parked (`dish_sync.cpp:292-308`) -- not a local-vs-host fight (except own-ping handoff).
- Fix: receiver-side wrap-aware shortest-arc angle interpolation between the 4 Hz samples.

---

## Severity 3 -- MISSING / incomplete lanes (host-authored state not replicated)

- **R8** (#11 PC RT-screen tab switching not synced) [V]: grep of the whole coop tree for
  tab/page/screen-index navigation returns NOTHING -- no wire lane exists (scope gap). laptop_sync owns
  power+floppy+lid; laptop_buffer_sync the file-buffer quad; floppybox the LIFO. Needs a new lane + the
  minimum-subset decision + a mid-join answer (principle 8).
- **R10** (#13 drone red light too strong on client, weak on host) [V]: `DroneStatePayload` (`FillPayload`
  `drive... drone_sync.cpp:62-80`) carries only pose + `active` + `stateBits` (rotor dust, canTakeOff) +
  dust anchor -- NO light/emissive/beacon field. The red beacon is driven locally per peer -> diverges.
  Cheap fix once the driving field is found (one field on the host-authored lane).

---

## Severity 4 -- hypotheses needing a fresh log slice of the specific action

- **R13** (#17 client SAVE SIGNAL does nothing; host works) [H]: HOST 13:46:20 `signal_sync: row broadcast`
  -> client `applied row from slot 0`; the client's presses produced NO signal_sync send. RED HERRING
  RULED OUT: `save_button_disable` greys the PAUSE-MENU game-save (`ui_menu_C->button_Save`), NOT the
  desk. The `SavedSignalAppend` lane is producer-symmetric, so a client save WOULD sync -- it didn't
  happen because the saveable state (a completed download/decode on unit 2) is HOST-AUTHORED (DeskSimPose)
  -> the native saveSignal precondition is false client-side. /qf: input-gated or state-authored-away?
- **R3** (#2 triangulation not visible on host) [H]: committed dots c0/c1/c2 ride reliable `DishAimState`
  gated on desk-claim holder (`console_state_sync.cpp:413,431-440`), applied via `WriteDishCommitted`
  (`OnDishAim :528-547`). Client held the claim, so the path SHOULD fire; no send log to confirm, and
  unclear the host RENDERS triangulation from committed locks alone. Needs a fresh single-"enter" slice.
- **R5** (#8 no audio when client toggles polarity) [H]: desk audio mirrors via the v115 DeskSndFx native
  seam (AudioComponent:Play + ActorComponent:SetActive/Activate for the desk's 6 unit-1 comps,
  whitelist-gated). If the toggle sound isn't whitelisted / not through those comps, nothing rides. Unclear
  if the client is silent LOCALLY (native / ScopedWireApply over-suppress) or only the host is silent.
- **R6** (#1 desk cursor lag, LOW priority) [H]: host DID apply the cursor (`desk_cursor: applying slot=1`,
  cur~=target, ema 16-17 ms; one spike ema=63 ms). May be inherent interp delay; confirm on a fresh look.

---

## RESOLVED this session

### R12 -- health + stamina both refill to 100% mid-play  [V, CONFIG, closed]
Symptom #15. ROOT: deployed HOST `multivoid.ini` had `[dev] vitals_keepalive_sec=180` -- the dev keepalive
(`vitals_keepalive.cpp`) refills food/sleep(stamina)/health to max every 180 s and broadcasts RestoreVitals
to all peers. FIX APPLIED: set `vitals_keepalive_sec=0` (deployed HOST ini; the ini is gitignored). **Needs
a peer RESTART** to take effect (PeriodMs latches on first Tick). Not a code bug, not a /qf item.

---

## IMPROVEMENT (not a bug)

### I1 -- chat has NO scrollable history  [V]
Symptom #16. Chat is a transient overlay FEED (`chat_feed.cpp`: `g_lines` capped at `kMaxLines`, lines
expire, only a `g_expired[8]` ring kept). No persistent history store, no scroll UI. Improvement: a bounded
history ring + a scrollable panel on T-open. Own design.

---

## Cross-cutting framing for the /qf sessions
Three recurring shapes, worth holding across the per-root /qf sessions (MTA discipline: a continuous value
is interpolated, a discrete change is an EVENT captured at its edge -- never a low-rate poll of either):
1. **poll-not-edge** (R2/R7/R17): hook the native verb, don't diff an array on a timer.
2. **snap-not-interp of a continuous value** (R1): interpolate on the receiver, wrap-aware.
3. **content/possession not bound to identity/birth** (R14/R15/R16; and R8/R10/R11 as missing lanes):
   host-authored or content-bearing state must ride the BIRTH channel and a stable identity, not a late
   side lane that a churning respawn outruns.

**Suggested /qf order:** the data-loss trio first -- drive-disc cluster (R14/15/16) -> R9 (garage) -> R11
(sack) -- then R4 (global flicker), the poll-not-edge family (R2/R7/R17), R1 (dish interp), then the
hypothesis roots (R3/R5/R13/R6) which each want a fresh single-action log slice.
