# Desk-sim v111 coop bugs — root-cause audit (2026-07-16)

The user's first hands-on of the v111 `desk_sim_sync` build surfaced 5 concrete bugs. Read-only
audit root-caused ALL FIVE in our code (file:line cites against HEAD of 2026-07-16). Evidence:
code + `analogDScreenTest` bytecode (`[N]` = ubergraph statement indices) + the surviving CLIENT
log of the test (`Game_0.9.0n_CLIENT_1/.../votv-coop.log` 10:40-10:53; the HOST log was truncated
by a relaunch — `[[lesson-copy-peer-log-before-relaunch]]` strikes again). Status: DIAGNOSIS ONLY
— no fixes designed here (that is the next `/qf 15`).

## THE PREAMBLE FACT that shapes bugs 1/2/3 [MEASURED, client log]

**During the entire download-unit test the desk claim was NOT held.** The client held `desk` only
10:44:48-10:45:59 and 10:46:01-10:46:03; every knob/toggle event from 10:51:13 on logged
`console_state: desk button edge broadcast (...)` — a line that exists ONLY in the UNCLAIMED
branch (`console_state_sync.cpp:465-474`). Root: the claim engages only on the intComs
`activeInterface` edge (`device_occupancy.cpp:208-258`; widget map `ui_consolesAtlas_C -> "desk"`
in `ue_wrap/device_screen.cpp:44,53`), but the download unit's physical +1/+5/+15/toggle/dir
buttons are WORLD-SPACE presses that never set `activeInterface`. The v111 "occupant-intent
DeskState" lane therefore never engaged for the unit the design was built around. (The user's
report "only host input applies" is this, seen from the client seat.)

## Bug 1 — 1/2/3 + SHIFT cooldown killed on the CLIENT [MEASURED]

- The v111 `cooldown` stream channel maps to `coord_cooldown` = `g_fields[7]`
  (`ue_wrap/console_desk.cpp:36`); host reads (`:792`), client RAW-WRITES it every pump tick
  (`:808` <- `desk_sim_sync.cpp:115-120`).
- Native: a dot/scan press sets `coord_cooldown = maxCooldown` ([2507]); per-tick decay
  [2627-2629].
- On a CLIENT presser, the local charge is overwritten ~100 ms later by the interpolated HOST
  value (0 — the host never saw a press). BOTH uplinks discard the field: (a) the claimed
  DeskState carries `coordCooldown` but the host's v111 keep-local gate throws it away
  (`console_state_sync.cpp:601`, block :594-603); (b) the unclaimed edge lane excludes it as
  tick-derived (:206-207). The race is unwinnable BY DESIGN, not timing. Host presser unaffected
  (host never applies the stream, `desk_sim_sync.cpp:92-103`). => the user's "ты кулдаун удалил"
  = client-side cooldown-erase regression, shipped in v111.

## Bug 2 — client knob input does nothing [MEASURED]

1. The intent uplink for knob SPEEDS exists only on the CLAIMED lane (1 Hz `ScalarsDiffer`
   stream, `console_state_sync.cpp:460-464`) — never engaged (preamble). The unclaimed lane
   deliberately excludes the speed floats (`DiscreteDiffer` :208-218 — a v64-era comment
   "unclaimed knob write stays local by design", a premise v111 invalidated when it made the
   offsets host-owned).
2. The client's local knob effect is erased locally: its integrator outputs
   (`DL_FrFilterOffset/PoFilterOffset`) are overwritten every pump tick by `WriteSimOutputs`
   with host values (`console_desk.cpp:804-805`).
3. Toggles DID broadcast unclaimed (log 10:51:13) and the host applies them — but a toggle
   without a speed produces no motion. (One observed toggle edge may be the native auto-flip at
   frequency-found [1983] — [PLAUSIBLE which].)

**Reverse defect on the same lane [MEASURED]:** 31x `desk button edge broadcast
(dlDownloading>0)` in ~100 s. The client's UNSUPPRESSED native rate recompute ([2138-2175])
races the 10 Hz mirror write of the host's rate; the 1 Hz poll samples 0<->|>0 flips;
`DiscreteDiffer` misclassifies `(dlDownloading>0)` as player input (:217) -> each flap
rebroadcasts the client's FULL scalars -> the host APPLIES the client's stale zero speeds/toggles
(:589-604) -> **the HOST occupant's knobs get stomped every ~3 s too.**

## Bug 3 — desk sounds dead in coop [MEASURED mechanism]

Native gauge volume is SPEED-gated (see the chain RE §2): polarity loop vol =
`|DL_poFilterSpeed| > 0 ? 0.2 : 0`; freq tune vol = `Lerp(0,1,frData) * snd_filtFreq * 0.5` where
`snd_filtFreq` interps to `clamp(|DL_FrFilterSpeed|)/10`. The DeskSimPose 8-vector does NOT carry
the speeds, and per bug 2 the speeds never rode ANY lane -> every non-pressing peer computes its
loops at |speed| = 0 -> **silence on the mirror side is data-starvation, not a missing audio
call** (we suppress no audio components; the only cue calls we make are the comp pane's
CompCueStart/Stop, `console_desk.cpp:547-560`). On the HOST side the 31 flap re-applies of the
client's zero speeds cut the host's own envelope mid-turn ~every 3 s (chain measured; audibility
= runtime).

## Bug 4 — stuck/looping sound at detector 100% [MEASURED, client-only]

Native: the detector block runs only while `DL_resDetecPercent < 1.0` ([124-126] — the "latch" IS
the value reaching exactly 1.0; no separate bool); at >=1 -> `beep_detecFinish.Play()` +
`canSaveSignal` ([144-152]). Our mirror pins the client's field permanently a hair BELOW 1.0:
the host streams exactly 1.0 at 10 Hz, but `SimInterp`'s 150 ms window (`desk_sim_sync.cpp:30`)
is reopened by every packet before `arrived` fires, so the exact-snap never happens; once
err ~1e-7 the per-tick `cur += err*dA` increments round below 1.0f ulp and `cur` freezes just
under 1.0 (`desk_sim_sync.cpp:43-60`); `WriteSimOutputs` writes that sub-1.0 value every pump
tick (`console_desk.cpp:807`). The client's OWN detector block is NOT suppressed (v111 =
overwrite-not-suppress): each BP tick sees <1 -> adds its own RNG inc -> clamps to 1.0 ->
**`beep_detecFinish.Play()` fires; next pump write drags the value back under 1.0; repeat every
frame** = the stuck beep loop (+ re-fired `canSaveSignal`/`setFullyProcessedSignalObject`).
Host latches natively at exactly 1.0 — unaffected.

## Bug 5 — SHIFT ping unmirrored; dot-press coordLog lines missing [MEASURED]

- The SHIFT quick-scan (`useSearch` -> `spawnDirs` + beepLong1 + cooldown charge) is on **no wire
  lane at all**: `useSearch`/`spawnDirs`/`coord_pingStage`/`coord_ping*` appear nowhere in
  src/votv-coop (negative grep validated against known-positive `coord_cooldown`). The only
  ping-adjacent wire state is the `coordIsPing` bool (DeskState, `console_state_sync.cpp:271/
  303`) and the streamed cooldown — neither reproduces arrows/sound/rings.
- coordLog: the `CR:[..]` cursor-readout family is written per tick ONLY while the presser holds
  a move key (gated on `master_spaceRenderer.move_*`, [1436-1450]) — a mirror peer's input bools
  are never true so it CANNOT regenerate them, AND the producer filter drops the family off the
  wire: `IsAnimatedLogLine` kPrefixes = `CR:[`, `CDOWN: [`, `AREA SCAN: [`, `APPROXIMATION:`,
  `ANALYSIS:` (`console_state_sync.cpp:328-335`, applied :381). The filter's premise
  ("regenerate per peer from mirrored scalars") holds for CDOWN (cooldown streams) but is FALSE
  for CR (input-gated) and APPROXIMATION/ANALYSIS (gated on `coord_pingStage`, never mirrored).
  The discrete dot LOCKS do mirror (DishAimState -> `WriteDishCommitted`,
  `console_desk.cpp:610-635`) — repaints dots but writes no log lines. Matches the user's "dots
  yes, chat screen no".

## File-size check (standing rule, `wc -l` live)

Cited files: desk_sim_sync.cpp 130, desk_cursor_sync.cpp 181, device_occupancy.cpp 418,
event_dispatch_state.cpp 636, console_state_sync.cpp 731 (approaching soft cap), console_desk.h
255. **FLAGGED >800:** `ue_wrap/console_desk.cpp` = 835 (extraction candidates: the v70
signal-catch consume surface ~651-833, or the comp-pane substrate ~418-566) and
`coop/net/session.cpp` = 1110 (known offender; candidate: per-MsgType send/receive scheduler
blocks). `protocol.h` 4216 = constants header, exempt.

## Where the fix `/qf` must start (facts, not the design)

- The claim model vs world-space buttons mismatch is the AXIS-level fact (preamble) — bugs 1/2/3
  are three symptoms of "the intent lane assumed a claim that physical buttons never create".
- Bug 4 is transport-shape: an exact-snap/latch question on the interp
  (`[[lesson-frozen-mirror-desync-is-transport-not-authority]]` neighborhood).
- Bug 5 is the OPEN-2 coordLog cluster + the never-synced SHIFT scan; the chain RE §1 gives the
  full native surface (arrows are UMG widgets on the shared atlas — mirroring is state-shaped,
  not actor-shaped).
