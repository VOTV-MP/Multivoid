# Hands-on runbook — arc A, the roster ledger (2026-07-27)

**Deployed:** `multivoid-0.9.0n-130.dll`, sha256 `c4d1d8d4bf1dcd3e02549b8fd6920cb019cc51d3bd84a507346f350f0c7db08d`,
**proto 130** (wire changed — RELAUNCH BOTH PEERS; a b128/proto-129 peer is refused at the join gate
by design, with a feed line saying so).
All four installs have it. HEAD `63a488a7`+.

**Why this runbook exists.** Arc A is fully drilled and audited and **no human has touched it**.
Four autonomous drills passed, but every one of them tests what I thought to test; the things below
are what only a person at the keyboard can see — whether the TAB list *reads* right, whether a
departure *looks* clean, and whether anything flickers.

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

## 1. TAB lists everyone, with IDs (the reported feature)

Four peers. On EACH peer press TAB.

- **Expect:** four rows on every peer, including on the clients. An **ID column** showing #1 for the
  host and #2/#3/#4 for the clients in join order.
- **Watch for:** a client showing only two rows (the old bug), a blank or `Remote player` name where
  a real nickname should be, an ID that repeats, or the host drawing #2.
- The `ping`/`link` columns for peer-to-peer rows on a CLIENT are new territory — they land on
  "VIA HOST" with no RTT. If that reads badly, say so; it is a presentation question nobody has
  looked at with human eyes.

## 2. A third peer leaves (the client-side half that never worked)

With four peers up, have CLIENT_3 quit — ALT+F4 or quit to menu, both are interesting.

- **Expect, on the host AND on both surviving clients, within a second:** the chat line
  `<Nick> left the game`, their puppet **gone from the world**, and their TAB row gone.
- **Watch for:** the body staying put (that is the pre-arc-A behaviour and would mean the teardown
  did not reach that peer), the row lingering, a duplicate leave line, or a leave line appearing on
  the peer that is itself quitting to the menu.

## 3. The seat changes hands (the case the token exists for)

After step 2, have that same player REJOIN — they will take the freed slot.

- **Expect:** the old body is gone BEFORE the new one appears; the new player's nickname, skin and
  nameplate colour are THEIRS, not the previous occupant's; one join line, not a leave/join pair
  that reads confusingly.
- **Watch for (this is the subtle one):** any frame where BOTH bodies exist, or the newcomer wearing
  the previous person's skin/colour/nameplate. The drill proved the ordering in the log; whether it
  is invisible on screen is a different question.

## 4. Moderation targets the person, not the seat

F1 -> Administration. Open the ban modal on a player, then — WITHOUT closing it — have that player
leave and someone else join into the freed slot. Now press Ban.

- **Expect:** the ban ABORTS. Nobody is banned, and the host log says
  `moderation: ban of #N ABORTED -- slot X now holds #M`.
- **Watch for:** the newcomer getting kicked or banned. That is the destructive bug arc A closes;
  if you see it, stop and say so immediately — the ban is an IP ban and it persists.
- `multivoid-banlist.txt` in the host's `Win64` folder should be unchanged (it does not exist at all
  unless something has been banned).

## 5. Just play for ten minutes

The roster is re-asserted on a repair pulse (fast for 10 s after any change, then every 5 s). It is
supposed to be invisible.

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
