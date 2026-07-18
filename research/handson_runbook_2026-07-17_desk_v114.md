# Hands-on runbook — v112..v122 (desk chain + meadow + laptop v2 + join identity), take 4

DEPLOYED: `votv-coop.dll 06b9e2d23c84037f...` x4, hash-verified 2026-07-18 night
(= v121 + the rack extraction trio + **v122 no-passive-mint stable-ID root fix** — the
join-identity layer: client census no longer mints keyed Elements (~2200 zombie
double-rows per join GONE, smoke A/B measured: sweep universe 2236->1, rack under ONE
host eid, digest circles cross-peer), host authority guards at the bind funnel + the
OnSpawn handback. Proto 121 unchanged. v122 is a TWELFTH layer but needs no dedicated
hands-on steps — it verifies as "the join feels identical, nothing duped/vanished";
its regressions would show as missing/duplicated props after a join or as loud
`HANDBACK`/`HOST-AUTHORITY` log lines (grep them post-take; expected 0 in normal play,
each firing is diagnostic evidence, not necessarily a bug).
Design: votv-stable-id-no-passive-mint-DESIGN-2026-07-18.md).
kProtocolVersion **121** (v121's LaptopBlob/LaptopQuad/FloppyBoxState lanes + the
LaptopState struct shrink; a 120-or-older peer HARD-CLOSEs at the gate — RELAUNCH BOTH
PEERS). TWELVE unverified layers v112..v122; prefixes attribute.

## What changed in v121 (2026-07-18 — OPEN-10 laptop v2) — proto 121, DLL a451fce7cb674d04 x4

Три оси: (A) ФАЙЛОВЫЙ БУФЕР ноутбука (вкладка floppy: erase / move-to-buffer / transfer /
remove-buffer) теперь синхронится ЖИВЬЁМ (раньше — только через сейв при джойне);
(B) КРЫШКА портативного PC (открыть/закрыть); (C) ЯЩИК ДИСКЕТ (prop_floppyBox — стопка
до 15). Grep prefixes: `laptop_buffer:` / `floppybox:` / `laptop_sync:` (lid = op=6).

**STEPS (v121):** (a) БУФЕР: peer A вставляет дискету в ноут, двигает файл в буфер
(move-to-buffer) — peer B открывает ноут и видит файл в буфере в ~1 s
(`laptop_buffer: canonical adopted`); erase/transfer/remove-buffer — так же; rw-счётчик
дискеты падает одинаково у обоих; (b) ОДНОВРЕМЕННО: A в UI ноута двигает буфер, B КИДАЕТ
дискету в слот — обе оси применяются, без затирания (edit-script ops не стомпают
floppyData); (c) КРЫШКА: открой/закрой портативный PC — у другого пира крышка
анимируется в ~1 s (`wire LID applied`); джойнер видит текущее состояние крышки;
(d) ЯЩИК: положи дискету в ящик (E с дискетой в руке) — у другого пира она появляется в
стопке (`floppybox: push applied` + canonical); возьми верхнюю (E) — у другого исчезает;
(e) ГОНКА ЯЩИКА: оба пира жмут E на ОДИН ящик почти одновременно — один получает диск,
второму диск реапается из руки с логом `pop DENIED -- held disc reaped` (окно <1 s;
дубликата НЕ должно остаться). KNOWN RESIDUALS: контент слота >4 KB режется (WARN,
OPEN-9-класс); canonical-квад >56 KB режет хвостовые строки буфера (WARN — практически
недостижимо без гигантских файлов).

## What changed in v120 (2026-07-19 — L9 meadow DB) — proto 120, DLL 452973c707d9cb8d x4

The laptop (Meadow OS) signal DATABASE now syncs: saves into it, deletes from it, AND the
row ORDER (стрелки вверх/вниз — синхронится по твоему rule-1 решению, host-canonical).
Grep prefixes: `meadow_db:` / `meadow_store:`.

**STEPS (v120):** (a) on ONE peer, at the DECK, select a saved signal and press its
"save to DB" button — the OTHER peer opens the laptop DATABASE tab and sees the row within
~1 s (`meadow_db: applied append` + `digest n=+1`); (b) delete a row from the DB on one
peer (the laptop delete button) — vanishes on the other (`applied delete`); WATCH-FOR: if
the other peer was PLAYING that or a lower row, its selection may shift / audio stop —
that is native-parity (same as a local delete), report how it feels; (c) ORDER: move a row
up/down (sortSignal arrows) on one peer — the OTHER peer's list reorders within ~1-2 s
(`applied order`); move rows on BOTH peers quickly — both must converge to ONE order (the
host's arrival order decides; a brief flicker then convergence = correct); (d) double-save
the SAME deck row twice — TWO identical rows appear on both peers (multiset — native);
deleting one removes ONE on both. KNOWN RESIDUALS: wire-added rows show a BLANK image
until the shared bulk-image lane lands (v65-inherited, deck rows have the same); a
re-added identical row within ~20 s of a cross-peer delete race can be eaten (narrow,
v65-inherited).

## What changed in v119 (2026-07-19 night — L5 drive chain) — proto 119, DLL b9b0727e04d38e0e x4

Drive payloads (data_0), slot insert/eject (deck play/comp + eraser) and the drive RACK now
sync. Grep prefixes: `drive_sync:` / `drive_chain:`.

**STEPS (v119):** with a payload-carrying drive (import a downloaded signal to it first if
none): (a) HOST inserts the drive into the deck slot — CLIENT sees it freeze into the port
(watch `drive_sync: slot role=0 INSERT` on the client; the client-side self-sim may land
first — then the line logs a no-op, both fine); (b) HOST ejects (E-grab the slotted drive)
— CLIENT sees it leave (`slot role=0 EJECT applied`); (c) CLIENT repeats both (symmetric);
(d) EXPORT a deck row to the drive on one peer — the OTHER peer's hover/LED shows the
payload within ~1 s (`payload applied eid=...`); (e) ERASER: wipe a drive on one peer —
the other's LED goes red within ~1 s (poll-detected, no verb); (f) RACK (if placed/bought):
put a drive in on one peer — the other's rack shows the instance (`rack ... canonical
adopted`); take it out — crosses back; try a same-slot race if possible (expect ONE winner
+ a deny line + no item loss). WATCH-FOR: a phys-grabbed item dropping from your hand
exactly when a remote insert applies = the documented rare miss-path residual (report it);
signal appearing in BOTH the deck list and the drive (or neither) for <=1 s during
import/export = the two-lane transient, NOT a bug.

## What changed in v118 (2026-07-18 — L8 physMods)
The desk's 12-slot physical-modules array now syncs (design
votv-physmods-L8-impl-DESIGN-2026-07-18.md, 8-round /qf; value-ops + host-canonical
array; both audits caught + fixed ONE critical pre-handoff — the host self-op misroute).
**STEPS (v118):** with a module prop in hand (tape-compression etc.): (a) HOST plugs it
into a desk socket -> the module must appear plugged on the CLIENT's desk (socket visual +
its effect, e.g. tape speed) — logs: host `physmods: local PLUG byte=N -- canonical will
carry it`, client `canonical adopted`; (b) CLIENT plugs one -> host log `host applied
op=0 byte=N from slot 1 -- canonical broadcast`, both desks agree; (c) UNPLUG (hit the
socket): the module pops into the unplugger's HAND, the socket empties on BOTH; the
unplugged module dropped by the CLIENT must materialize for the host (the widened birth
whitelist) — logs: `FRESH-BIRTH intent` on the client, the host spawn; (d) hot-plug with
a unit powered (no coldswap rule): the presser explodes LOCALLY (native shape; the other
peer is unaffected — measured, not a bug); (e) a JOINER's desk must show the host's
modules (the `canonical -> joiner` line). Watch-fors: a phantom duplicate module
appearing near the desk (= the fixed CRITICAL regressing), `unplug ... denied (raced)`
lines outside an actual race, module effects (tape speed byte 21 / lamps byte 3)
diverging between peers.
**The take-3 test (17:00-17:09) found the v113-116 root this build fixes**: the client's
successful ping at 17:04:46 never crossed (the claim-gated catch detector raced the
FSM-hold release) -> host NO SIGNAL / frozen host dishes / diverged client dishes /
detector asymmetry. v116 retires the claim gates (the unprimed change-edge is the
authority) + adds the laptop PC lane + catch -> activity feed.
**NOTHING below is hands-on verified yet.** EIGHT unverified layers batch here; per-lane
log prefixes keep attribution:
`desk_input:`/`desk_sim:` = v112, `dish_sync:`/`[dish]` = v113, `[reel]`/`[task]` = v114,
`desk_snd:`/`desk_cursor:` = v115, `FSM-hold`/`ping attribution`/`re-init window` = v115b,
`signal_catch:`/`laptop_sync:`/`laptop:` = v116, `deck_play:` = v117, `physmods:` = v118, `drive_sync:` = v119, `meadow_db:` = v120.

## What changed in v117 (2026-07-18 — L6 deck playback)
The unit-3 play deck's PLAYBACK now mirrors (it was fully local: a non-occupant heard
NOTHING from the world-audible deck). Design votv-deck-play-L6-impl-DESIGN-2026-07-18.md
(7-round /qf); smoke x2 PASS with the e2e self-test chain proven (patch/routing/wire/gen);
the AUDIO half is exactly what only this take can verify.
**STEPS (v117):** save at least one fully-downloaded signal to the deck list first (or use
an existing one). (a) Peer A presses the deck PLAY button -> peer B (standing at the desk,
NOT occupying) must HEAR the same track from the desk + see the deck panes playing; logs:
A `deck_play: organic play idx=N gen=G`, B `deck_play: applied play idx=N gen=G`. (b) B
presses STOP (any peer may stop) -> playback stops for BOTH; logs: B `organic stop`, A
`applied stop`. (c) Let a track play to its NATURAL END -> it must stop on both with NO
`deck_play: applied stop` line (each peer self-terminates; a flood of stop lines here =
the fin bracket failed -- gen guard still keeps it correct, but report it). (d) Volume
knob + scroll while playing (rides v112 -- mirror volume must track). (e) IMPORT/EXPORT a
drive while playing -> playback stops on both (the seam covers those stops generically).
Watch-fors: double audio on a receiver (= routing bug), a play that never crosses
(`gates failed` WARN on the receiver names why), phantom stops killing a fresh playback
(= gen guard bug -- grep `stale stop`). NOTE: [dev] deck_selftest=1 is ON on the HOST --
at +25 s post-connect it fires one silent Activate/Deactivate pair (no sound, no panes);
ignore the matching gate-WARN pair on the client, or flip the ini key to 0.

## What changed in v116 (this build — the take-3 report fixes)
1. **The lost catch**: a successful ping now ALWAYS crosses — expect
   `signal_catch: local catch detected ('X' ...) -- relayed` on the CATCHER within ~1 s of
   the catch, `signal_catch: catch replay applied` on the host (or `catch identity
   applied` on observing clients), the host dishes slewing, and the observer's detector
   arming via the DishArm lane. The 17:04 symptom set (one peer detects, other NO SIGNAL,
   dishes frozen/diverged) must NOT reproduce.
2. **Activity feed**: every catch lands one chat line per peer — "You caught signal 'X'"
   for the catcher, "<nick> caught signal 'X'" for everyone else. A JOINER must NOT get a
   stale "caught" line (the connect seed is feed-silent).
3. **The stationary PC (laptop) syncs**: ACTIVATE (power/boot) crosses (`laptop_sync:
   local POWER edge` / `power replay dispatched`); floppy insert/eject crosses
   (`laptop_sync: local INSERT edge` / `wire INSERT applied` / `EJECT`); the observer's PC
   shows the same slot state + the mirrored disc content. The PC BUFFER (files copied
   onto the PC) + the portable PC are NOT in v1 (known-open, TRACKER row).
4. **Cursor/diag hygiene**: the closed-measurement diagnostics are OFF in the inis
   (kerfur_census's 8-25 ms walk every 10 s among them); the HOST now runs perf_probe —
   if the cursor still jerks, the host [perf] frames + desk_diag pacing + desk_cursor ema
   lines are the attribution set. Re-judge the cursor + right-screen text this take.

## STEPS (v116 half)
1. Repeat the 17:04 scenario EXACTLY: client sits at unit 1, 3 dots, Enter, and STAND UP
   mid-ping. On success watch BOTH logs for the chain in (1) above + the feed lines (2).
2. Host then catches one too (roles swapped) — the client must see the host's catch
   (detector arms, dishes move, feed line).
3. The stationary PC: host presses ACTIVATE — client's PC boots too (~6 s native lag is
   expected: the replay runs the native boot chain). Insert a floppy on one peer (both
   the throw-into-slot and the E-with-disc-in-hand paths if possible) — the other peer's
   PC shows the disc; eject on the OTHER peer — the disc pops out with its content.
4. Watch-fors (regressions): no `laptop_sync:` WARN spam; no double feed lines; no
   phantom ping behavior returning (v115b watch-fors still apply).
The v112+v113 steps live in `handson_runbook_2026-07-16_desk_v113.md` (still current for
those lanes — run its STEPS first or interleave); THIS file adds the v114 half + the v115
half + the v115b ping half (take-2 was superseded mid-take: the user's LIVE ping test at
14:46-14:48 surfaced the PHANTOM ping-FSM — the v112 coordIsPing raw apply woke a parallel
sim on the host; fixed same-evening, design
`votv-ping-fsm-phantom-v115b-DESIGN-2026-07-17.md`).

## What changed in v115b (the phantom ping FSM fix)
1. Only the PRESSER's machine runs a ping now. The observer's machine never wakes (no
   phantom stage sounds from the peer who did NOT press Enter; no phantom ARM).
2. While a ping runs, the desk is CLAIM-HELD for the pinger (host FSM-hold): another peer
   mounting the desk mid-ping gets the native deny + is seated back out. It releases by
   itself when the FSM ends.
3. A successful catch is no longer stomped a second later (the false DISARM after the
   catch replay is suppressed while the signal data lives).

## STEPS (v115b half; needs a signal in range — repeat the 14:46 scenario)
1. CLIENT at unit 1: place 3 dots, Enter. WATCH the HOST's log: there must be NO
   `desk_snd:` pingSound stage cues authored by slot 0 while the client pings, and NO
   `dish_sync: host ARM broadcast` before the client's own verdict.
2. While the client's ping runs (~20-60 s), HOST tries to mount the desk: expect the
   native deny click + forced exit; host log `device_occupancy: desk FSM-hold asserted
   for pinging slot 1`, then `... released (ping ended, slot 1)` when the FSM finishes.
3. If the ping FAILS ("inner circle too small" / "no signal in local area"): satellites
   moving + the re-Enter block are NATIVE (each attempt costs) — the unit-2 DETECTOR must
   NOT rise on a local fail anymore.
4. If the ping SUCCEEDS: `signal_catch: catch replay applied` on the host, and within ~30 s
   `dish_sync: host ARM broadcast` (the real one) with NO `DISARM applied (machine reset +
   signal actor deleted)` in between on the client. The host log may show ONE `dish_sync:
   mesh down with live signalData ... DISARM suppressed` line — that is the re-init window
   working (root-3); it repeating per-poll would be a bug (it is log-once per episode).
5. Right screen during the ping: coordLog lines appear ONCE each (no double-author thrash);
   the messages' cadence should be smooth now. Re-judge the CURSOR smoothness in the same
   sitting (residual R-c: the presser-side ema doubling had no root yet — the phantom was
   a per-tick machine on the observer and may have been it).

## What changed in v115 (desk AUDIO mirror + cursor v2 — design `votv-desk-audio-mirror-v115-DESIGN-2026-07-17.md`)
1. The OTHER peer now HEARS your desk unit-1 activity: keyboard clicks (down AND up),
   the button/verb sounds (switch-cursor, ping key, SHIFT scan, 1/2/3 dot), the outcome
   beeps (ok beepLong1 / cooldown-denied beep4 / broken-radar fail), the cursor-movement
   loop while you hold move keys, and the ping loop. Screen-button MOUSE clicks carry the
   same sounds.
2. Cursor momentum crosses: flick the cursor, get off the unit — every peer's screen shows
   the same glide to the same rest position (the screen dims at dismount, as native).
3. Cursor smoothness: the mirror no longer snaps on desk release/re-claim, and the interp
   window adapts to the sender's real frame cadence (fps dips no longer staircase).
4. NOT covered (known residuals): the "Satellites are active" error beep (PlaySound2D
   static); the LAPTOP's own keyboard clicks (same class, future whitelist extension);
   ping FSM stage sounds/rings stay presser-local (pre-existing).

## STEPS (v115 half; any point during the v112-v114 steps at the desk)
1. CLIENT sits at unit 1, types arrows/WASD + 1/2/3 + SHIFT (incl. on cooldown). HOST
   stands NEAR the desk (not using it): expect clicks per key down/up, buttonquick1 +
   beepLong1 on a dot, beep4 on a cooldown denial, buttonlong5 on SHIFT, and the
   cursor-glide loop sound WHILE keys are held (silent during pure momentum glide —
   native behavior). Swap roles and repeat.
2. HOST clicks the desk's physical SCREEN BUTTONS with the mouse (bring/switch coord):
   CLIENT nearby hears buttonquick1/buttonlong4 + the outcome beep.
3. Momentum: CLIENT flicks the cursor (hold a direction ~1s), releases the key AND
   immediately gets off the unit. HOST watches the screen: the cursor keeps gliding to a
   stop (dimmed), SAME rest position on both screens (compare CR readout if in doubt).
4. Smoothness: CLIENT drives the cursor in circles ~30s; HOST watches for jerks. Then
   CLIENT gets off/on repeatedly mid-motion — no snap-back on the HOST's screen.
5. Ping loop: CLIENT starts a triangulation ping (ENTER with a valid triangle) — HOST
   hears the ping loop start; it ends on the catch/fail (pingSuccess already crossed
   pre-v115; now the loop + start beep do too).

## WHAT TO READ IN THE LOGS (v115)
- Both peers at boot: `desk_audio: class-level resolve complete` + `desk_snd: audio seams
  installed (Play=1 SetActive=1 Activate=1)` (once).
- On the OBSERVER during the other peer's typing: `desk_snd: applied op=.. comp=.. cue=..`
  (1-in-32 throttle — a few lines per burst, not per click).
- `desk_snd: seam counters /60s: ...` — Play/SetActive/Activate rates; deskHits should
  roughly track the presser's key rate while at the desk.
- Cursor: `desk_cursor: applying slot=.. ema=..ms` (~5s throttle while mirroring);
  `desk_cursor: momentum tail ended (settled, ..ms)` on the sender after a glide;
  `desk_cursor: claim FLAP` = the occupancy-flicker attribution WARN (should NOT appear
  in normal play; if it does, that is the OPEN-1 attribution we wanted).
- BAD signs: any `desk_snd: apply failed`, `desk_snd: fx ring overflow`, a `claim FLAP`
  storm, loop sounds STUCK ON after the presser leaves/disconnects.

--- (v114 half below, unchanged) ---

## What changed in v114 (L7, all BUILT — design `votv-tape-caddy-L7-impl-DESIGN-2026-07-17.md`)
1. The STOLAS tape caddy's reel slots sync: insert/eject on either peer mirrors (slot meshes
   + hover progress); the recording accrual converges to the HOST's value (1 Hz corrector —
   both peers keep ticking natively, the corrector snaps; sawtooth <= 1%/s is by design).
2. A CLIENT-ejected reel now EXISTS on the host/others (it used to be a local-only ghost):
   the host authors it at the eject moment; it follows the client's hands.
3. Reel props carry their Progress % cross-peer at BIRTH (eject, container, join window,
   pocket->place) — hover shows the same % everywhere.
4. saveSlot.taskNew (the daily task: requirements, sigCompleted, reel grades, reward fields)
   mirrors host->client a second after any change (drone sell / day rollover).
5. The caddy ON/OFF toggle already synced (appliance lane, v45) — regression-check only.

## STEPS (v114 half; do the v112+v113 runbook first)
1. Both peers walk to the STOLAS caddy (base interior wall unit).
2. HOST ejects the BIG reel (E on the slot; refuse-while-active means toggle OFF first —
   the toggle itself must mirror, both peers see the unit stop). CLIENT watch: the slot mesh
   empties <= 0.5 s; the reel appears in the host's hands and its hover shows the SAME %.
3. HOST re-inserts it. CLIENT watch: slot mesh returns; hover Progress resumes accruing
   (toggle back ON) and the % matches host's within ~1%.
4. CLIENT ejects the SMALL reel. HOST watch: slot empties; a reel EXISTS in the client's
   hands on the host's screen (expect 2-4 one-shot `no local match` WARN lines in the host
   log at that moment — bounded, by design L7-R3). Client DROPS it on the desk: it lands on
   both peers at the same spot with the same hover %.
5. CLIENT picks it back up (E), pockets it (hold-R), then PLACES it from the hotbar: the
   placed reel keeps its Progress % on BOTH peers (the correctness-audit CRITICAL 1 case —
   a blank-tape reset here is the bug we fixed; verify it stays).
6. CLIENT throws a reel INTO the caddy funnel (route 2 insert): the slot fills on both peers
   with that reel's %, the thrown prop vanishes on both.
7. Let the unit record ~2 min with both reels in: hover % identical on both peers (+-1%).
8. Day flow (if in the session's plan anyway): drone-sell both reels HOST-side loaded ->
   CLIENT's tablet task page shows reel grades update <= 1-2 s ([task] line in the client
   log). KNOWN residual L7-R1: a sack loaded BY THE CLIENT does not grade (container
   inventory unsynced until the sack-contents lane) — not a v114 regression.

## WHAT TO READ IN THE LOGS
- `[reel] local INSERT/EJECT edge ... -- broadcast` on the presser; `[reel] wire INSERT/
  EJECT applied` on the mirror. NO repeating [reel] lines while nobody touches the caddy
  (a >=1 Hz repeat = a poll-echo bug — flag it).
- `[reel] wire INSERT onto OCCUPIED slot ... HOST keeps own` = the R5 tiebreak firing —
  expected ONLY on a genuine simultaneous insert; if it repeats, flag.
- `[task] taskNew changed -- broadcast` host-side at sell/rollover; `[task] taskNew
  mirrored` client-side. More than a few per game-day = the change-hash is unstable — flag.
- `[PROP-DROP] CLIENT authored REEL-EJECT intent ... +savedScalar` at a client eject;
  `[PROP-DROP] HOST spawned client-placed prop` right after.
- The v113 watch-fors (cue-reconciler repeat line, ARM pre-clear WARN) still apply.

## HONEST STATUS
v114 = BUILT + audited + smoke PASS. NOT hands-on. v112 and v113 are ALSO not hands-on —
three layers stack in this take; if a desk/dish regression appears, attribute by prefix
before assuming v114.
