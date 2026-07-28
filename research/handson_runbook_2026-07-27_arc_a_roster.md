# Hands-on runbook — arc A, the roster ledger (2026-07-27)

**Deployed:** `multivoid-0.9.0n-130.dll`, sha256 `51ec0c60cc698e681873ccc63757f4d06f0870b3736a29ce1dad4f858ed87cb9`,
**proto 130** (wire changed — RELAUNCH BOTH PEERS; a b128/proto-129 peer is refused at the join gate
by design, with a feed line saying so).
All four installs have it. HEAD `63a488a7`+.

**Why this runbook exists.** Arc A is fully drilled and audited and **no human has touched it**.
Four autonomous drills passed, but every one of them tests what I thought to test.

**THIS IS AN AUTO-RUNBOOK (rule, user 2026-07-27).** Where a check needs EYES, the drill takes the
SCREENSHOT and you give the verdict on the picture — you do not operate the game to produce it.
Checks that have a capture instrument name it below. Only what a picture genuinely cannot carry (a
felt hitch, a flicker between frames) is left as a play-it-yourself item, and it is marked as such.

**Two corrections to an earlier draft of this file, both measured:**
- **The player list is TILDE (`~`, `VK_OEM_3`, the physical key above TAB), not TAB.** An earlier
  draft said TAB throughout; pressing TAB does nothing. `imgui_overlay.cpp:162-171`.
- It is **hold-to-peek on a CLIENT** and **toggle on the HOST** (`imgui_overlay.cpp:26-27`) — the
  client list is a passive peek with no cursor, the host's is the clickable action board.

Design of record: `research/findings/join-identity/votv-nickname-arbitration-roster-id-DESIGN-2026-07-27.md`
(§3 AS-BUILT carries the drill evidence). Model: `include/coop/player/roster_ledger.h`.

---

## What changed, in one paragraph

Peer slots RECYCLE — the lowest free slot is handed out — so slot 2 can go from person X to person Y
with no empty moment in between, and anything watching a per-slot boolean cannot see it. Each slot
now carries an occupancy TOKEN: a `playerNo` that travels on the wire (0 = the slot is empty) plus a
host-only generation. Per-person teardown is driven by that row changing, not by a connection edge.
The practical consequence you should be able to SEE: **a client can now list everyone in TAB** (it
used to list only itself and the host), and **a client now reacts to a third peer leaving** (the
old edge never rose there, so a departed peer's frozen puppet stayed in the world all session).

---

## 1. The player list shows everyone, with IDs (the reported feature) — AUTOMATED

**Instrument: `tools/net/roster_shot.ps1`.** Brings up four peers, waits until each client is
in-world (`ClientWorldReady`) AND knows the last row, then PostMessages the real tilde key to each
window and captures all four. Screens land in `research/roster_shots/`. Your job is the verdict on
them; re-run it any time.

- **Already answered on all four peers** (2026-07-27): `PLAYERS 4 online`, four rows, IDs 1-4. Before
  arc A a client's list had two rows.
- **Expect:** four rows on every peer, including on the clients. The **ID column is the RIGHTMOST**,
  right-aligned (user 2026-07-27); #1 is the host and #2/#3/#4 the clients in join order.
- **Watch for:** a client showing only two rows (the old bug), a blank or `Remote player` name where
  a real nickname should be, an ID that repeats, or the host drawing #2.
- **KNOWN-BAD, already diagnosed, fix queued (design item 7):** peer-to-peer rows on a CLIENT read
  `VIA HOST  --`. That column fuses TWO axes (the peer's transport vs MY route to them) and a client
  structurally cannot measure another client's transport. The fix is the host PUBLISHING each
  occupant's link + ping on `RosterRow` (wire change, protocol bump). Do not re-report it.
- **KNOWN-BAD, fix queued:** the table draws no header row at all (`TableHeadersRow()` is never
  called), so the columns are unlabelled.

## 2. A third peer leaves (the client-side half that never worked) — AUTOMATED

**Instrument: `tools/net/departure_drill.ps1`.** Already PASS: host + both survivors logged the
leave line, the puppet destroy and the row emptying in the same second. What is left for you is only
whether it LOOKS clean; quit to menu rather than ALT+F4 to see the path the drill does not take.

- **Expect, on the host AND on both surviving clients, within a second:** the chat line
  `<Nick> left the game`, their puppet **gone from the world**, and their TAB row gone.
- **Watch for:** the body staying put (that is the pre-arc-A behaviour and would mean the teardown
  did not reach that peer), the row lingering, a duplicate leave line, or a leave line appearing on
  the peer that is itself quitting to the menu.

## 3. The seat changes hands (the case the token exists for) — AUTOMATED, screens included

**Instrument: `tools/net/replacement_drill.ps1`.** It captures both observers at TWO moments —
`client1_before.png` / `client2_before.png` (predecessor alive and embodied) and `*_after.png`
(successor embodied) — in `research/roster_shots/`. A ghost (both bodies) or an inherited
skin/colour would be visible in the pair; no log line can show either.

**One thing the drill data already tells you:** the successor's body appears **~8 s after** the
predecessor's is destroyed (measured 23:42:21 → 23:42:29). That is the joiner's world-load window,
not a ghost — the risk was two bodies and what you get instead is a gap. Whether a roster entry with
no body for eight seconds looks broken is a product judgement.

- **Expect:** the old body is gone BEFORE the new one appears; the new player's nickname, skin and
  nameplate colour are THEIRS, not the previous occupant's; one join line, not a leave/join pair
  that reads confusingly.
- **Watch for (this is the subtle one):** any frame where BOTH bodies exist, or the newcomer wearing
  the previous person's skin/colour/nameplate. The drill proved the ordering in the log; whether it
  is invisible on screen is a different question.

## 4. Moderation targets the person, not the seat — CODE PATH AUTOMATED, the CLICK is yours

**Instrument: `[dev] roster_token_selftest` inside `tools/net/replacement_drill.ps1`.** Already PASS:
a token captured when the slot was first occupied, held across a real departure, fired at the
successor → `NEGATIVE(stale gen)=REJECTED POSITIVE(live gen)=ACCEPTED` and the REAL `BanPlayer`
logged `ABORTED`, banlist byte-identical before and after. What that does NOT cover is the modal's
own capture-and-pass — the token is in the signature so a tokenless call cannot compile, but only a
click proves the button hands over the right one.

F1 -> Administration. Open the ban modal on a player, then — WITHOUT closing it — have that player
leave and someone else join into the freed slot. Now press Ban.

- **Expect:** the ban ABORTS. Nobody is banned, and the host log says
  `moderation: ban of #N ABORTED -- slot X now holds #M`.
- **Watch for:** the newcomer getting kicked or banned. That is the destructive bug arc A closes;
  if you see it, stop and say so immediately — the ban is an IP ban and it persists.
- `multivoid-banlist.txt` in the host's `Win64` folder should be unchanged (it does not exist at all
  unless something has been banned).

## 5. Just play for ten minutes — GENUINELY MANUAL

This is the one a picture cannot carry. The roster is re-asserted on a repair pulse (fast for 10 s
after any change, then every 5 s) and it is supposed to be invisible; a flicker between frames or a
felt hitch has no screenshot. The counting half IS automated (4 transitions + 2 identity installs per
client across a full run, 0 HotPathGuard) — what remains is subjective smoothness.

- **Watch for:** a "joined the game" line repeating, nameplates flickering, TAB rows reordering on
  their own, a periodic hitch, or the chat feed filling with roster noise.

---

## Reading the log afterwards

Host and client logs are `Game_0.9.0n_*/WindowsNoEditor/VotV/Binaries/Win64/multivoid.log`
(rotated per boot; the previous run is `multivoid.prev.log`).

| grep for | means |
|---|---|
| `ledger: slot N occupied by #P` | someone took the seat |
| `ledger: slot N emptied (was #P '<nick>')` | the seat is now free |
| `ledger: slot N REPLACED -- #P '<nick>' -> #Q` | the seat changed hands with no gap |
| `net: peer slot N (#P) left -- puppet destroyed` | the teardown ran (should appear on CLIENTS too) |
| `moderation: ban of #P ABORTED` | the token guard fired — good |
| `HotPathGuard` | a threading violation; there should be ZERO |
| `roster:` `WARN` lines | a malformed/rejected row |

A count worth taking: `grep -c "joined the game"` should equal the number of real joins, not the
number of pulses.

---

## Known-open, so you can ignore them

- Per-peer `ping`/`link` on a client shows "VIA HOST" with no RTT (named in the design, §3 item 7).
- `session.h` is 817 LOC, 2% over the soft cap — flagged, deliberately not cut (the generation API
  is class members; a real extraction needs a mixin refactor of the core net class).
- The remaining OPEN drills in the design's drill list (successor voice silence, version-gate
  rejection, parked-row ordering, session-end counting) have no instrument yet.
- The settings-check popup on the smoke's clients reports `posinfo = '1' -- not a known setting`.
  Unrelated to arc A: a retired key left in the test installs' ini. Cosmetic, but it sits on top of
  every screenshot.
